/* spti.c - the real scanner as a ScsiTarget, through Windows SCSI pass-through (SPTI).
 * The pass-through call is the one proven on Simen's PC (smart342-tracerelay / smart342-relay.ps1):
 * IOCTL_SCSI_PASS_THROUGH_DIRECT, 32-byte sense at offset 56, DataIn = (len==0||dir==0)?UNSPECIFIED:(dir==1?IN:OUT).
 * CDBs are passed with the length the Mac driver uses (6 or 10 bytes; it never sends 16-byte CDBs).
 *
 * Sense: the adapter fetches sense data automatically with a CHECK CONDITION (autosense), which clears it
 * in the scanner. The Mac driver then asks with its own REQUEST SENSE (Check_Sense_SCSI reads the
 * Scitex-format words: count + codes), so the autosense bytes are kept and returned for that request.
 *
 * Every command is also written to a binary trace in the smart342-tracerelay format (S342TRC1), so
 * the existing trace tools read it. */
#include "sim.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

SptiCaps spti_caps;   /* 0.5.0: the scanner's SCSI card limits, read when the scanner was found (Windows) */
const char *spti_fake;   /* developer option --fake-scsi: a device list for tests ("port:bus:id:VENDOR:PRODUCT;..."); nothing is sent */
static void dev_fill(SptiDev *d, const uint8_t id[36]) {
    d->type = id[0] & 0x1F;
    memcpy(d->vendor, id + 8, 8); d->vendor[8] = 0; memcpy(d->product, id + 16, 16); d->product[16] = 0; memcpy(d->rev, id + 32, 4); d->rev[4] = 0;
    for (char *c = d->vendor; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
    for (char *c = d->product; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
    for (char *c = d->rev; *c; c++) if ((unsigned char)*c < 32 || (unsigned char)*c > 126) *c = '?';
    for (int k = 7; k >= 0 && d->vendor[k] == ' '; k--) d->vendor[k] = 0;
    for (int k = 15; k >= 0 && d->product[k] == ' '; k--) d->product[k] = 0;
    for (int k = 3; k >= 0 && d->rev[k] == ' '; k--) d->rev[k] = 0;
    d->scitex = !memcmp(id + 8, "SCITEX", 6);
}
static int fake_scan(SptiDev *out, int max) {
    int n = 0; const char *p = spti_fake;
    while (p && *p && n < max) {
        const char *e = strchr(p, ';'); size_t L = e ? (size_t)(e - p) : strlen(p);
        char item[128]; if (L > 127) L = 127; memcpy(item, p, L); item[L] = 0;
        int port, path, tgt; char ven[64] = "", prod[64] = "";
        if (sscanf(item, "%d:%d:%d:%63[^:]:%63[^\n]", &port, &path, &tgt, ven, prod) >= 4) {
            uint8_t id[36]; memset(id, ' ', 36); id[0] = 6; memcpy(id + 8, ven, strlen(ven) < 8 ? strlen(ven) : 8); memcpy(id + 16, prod, strlen(prod) < 16 ? strlen(prod) : 16); memcpy(id + 32, "TEST", 4);
            SptiDev *d = &out[n++]; memset(d, 0, sizeof *d); d->port = port; d->path = path; d->target = tgt; d->listed = 1; d->initiator = 7; dev_fill(d, id);
        }
        p = e ? e + 1 : NULL;
    }
    return n;
}

#ifdef _WIN32
#include <windows.h>
#include <ntddscsi.h>
#ifndef IOCTL_SCSI_PASS_THROUGH_DIRECT
#define IOCTL_SCSI_PASS_THROUGH_DIRECT 0x4D014
#endif
#define SPTD_SIZE 56
#define SENSE_SIZE 32
#pragma pack(push, 8)
typedef struct {
    USHORT Length; UCHAR ScsiStatus; UCHAR PathId; UCHAR TargetId; UCHAR Lun; UCHAR CdbLength;
    UCHAR SenseInfoLength; UCHAR DataIn; ULONG DataTransferLength; ULONG TimeOutValue; PVOID DataBuffer;
    ULONG SenseInfoOffset; UCHAR Cdb[16];
} SPTD;
#pragma pack(pop)
_Static_assert(sizeof(SPTD) <= SPTD_SIZE, "SPTD layout");
_Static_assert(offsetof(SPTD, DataTransferLength) == 12, "xferlen offset");
_Static_assert(offsetof(SPTD, DataBuffer) == 24, "databuf offset");
_Static_assert(offsetof(SPTD, SenseInfoOffset) == 32, "senseoff offset");
_Static_assert(offsetof(SPTD, Cdb) == 36, "cdb offset");

typedef struct {
    ScsiTarget t;
    HANDLE h; int port, path, id;
    uint8_t sense[SENSE_SIZE]; int nsense;          /* autosense of the last CHECK CONDITION */
    FILE *trc; uint32_t seq; LARGE_INTEGER qpf;
    uint32_t crc_table[256];
    int timeout_s;
    int dead;                                       /* the scanner stopped answering: later commands fail at once */
    int fails;                                      /* failed commands in a row */
    uint32_t maxbytes;                              /* 0.5.0: largest transfer the card's driver allows (page-aligned buffer); 0 = unknown */
} Spti;
volatile int spti_dead;                             /* read by the window (RAW size test) */


/* 0.5.0: the data buffer is page-aligned (VirtualAlloc) and kept between commands. Windows refuses a pass-through
   whose buffer spans more memory pages than the card's driver allows (IO_SCSI_CAPABILITIES MaximumPhysicalPages) or is
   not aligned to its AlignmentMask (error 87). Aligned to a page, the Scitex driver's largest read (307,200 bytes)
   spans exactly 75 pages, and every alignment rule up to a page is met - for any SCSI card. */
static _Thread_local uint8_t *xbuf; static _Thread_local SIZE_T xcap;
static uint8_t *xfer_buf(uint32_t len) {
    SIZE_T need = ((SIZE_T)(len > 16 ? len : 16) + 4095) & ~(SIZE_T)4095;
    if (need > xcap) {   /* the new buffer first: the old one stays usable if this fails (review) */
        uint8_t *nb = VirtualAlloc(NULL, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!nb) return NULL;
        if (xbuf) VirtualFree(xbuf, 0, MEM_RELEASE);
        xbuf = nb; xcap = need;
    }
    memset(xbuf, 0, need);
    return xbuf;
}
static int exec_raw(HANDLE h, int path, int id, const uint8_t *cdb, int cdbLen, int dir, uint8_t *data, uint32_t len, int timeout,
                    int *ok, DWORD *err, uint8_t *status, uint8_t *sense, int *nsense) {
    uint8_t blk[SPTD_SIZE + SENSE_SIZE]; memset(blk, 0, sizeof blk);
    uint8_t *buf = xfer_buf(len);
    if (!buf) { *ok = 0; *err = ERROR_NOT_ENOUGH_MEMORY; *status = 0; *nsense = 0; memset(sense, 0, SENSE_SIZE); return 0; }
    if (dir == 2 && len) memcpy(buf, data, len);
    SPTD *s = (SPTD *)blk;
    s->Length = SPTD_SIZE; s->PathId = (UCHAR)path; s->TargetId = (UCHAR)id; s->CdbLength = (UCHAR)cdbLen; s->SenseInfoLength = SENSE_SIZE;
    s->DataIn = (UCHAR)((len == 0 || dir == 0) ? 2 : (dir == 1 ? 1 : 0));
    s->DataTransferLength = dir == 0 ? 0 : len;
    s->TimeOutValue = (ULONG)timeout; s->DataBuffer = buf; s->SenseInfoOffset = SPTD_SIZE;
    memcpy(s->Cdb, cdb, cdbLen);
    DWORD ret = 0;
    BOOL r = DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, blk, sizeof blk, blk, sizeof blk, &ret, NULL);
    *ok = r ? 1 : 0; *err = r ? 0 : GetLastError(); *status = s->ScsiStatus;
    int got = (*ok && dir != 0) ? (int)s->DataTransferLength : 0;
    if (got < 0 || (uint32_t)got > len) got = (int)len;
    memcpy(sense, blk + SPTD_SIZE, SENSE_SIZE);
    *nsense = 0; for (int i = 0; i < SENSE_SIZE; i++) if (sense[i]) *nsense = i + 1;
    if (*ok && dir == 1 && got > 0) memcpy(data, buf, got);
    return dir == 1 ? got : 0;
}
static uint32_t crc32b(Spti *s, const uint8_t *p, size_t n) { uint32_t c = 0xFFFFFFFFu; while (n--) c = s->crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8); return c ^ 0xFFFFFFFFu; }
static uint64_t us_now(Spti *s) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)(t.QuadPart * 1000000.0 / s->qpf.QuadPart); }
static void w8(FILE *f, uint8_t v) { fwrite(&v, 1, 1, f); }
static void w32(FILE *f, uint32_t v) { uint8_t b[4] = { v, v >> 8, v >> 16, v >> 24 }; fwrite(b, 4, 1, f); }
static void w64(FILE *f, uint64_t v) { w32(f, (uint32_t)v); w32(f, (uint32_t)(v >> 32)); }

static int spti_execute(ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out,
                        uint32_t alloc, uint8_t **din, size_t *nin) {
    Spti *s = (Spti *)t; *din = NULL; *nin = 0;
    if (ncdb < 1 || ncdb > 16) { if (t->log) TLOG(t->log, "!! SPTI: bad CDB length %d\n", ncdb); return 2; }
    /* REQUEST SENSE right after a CHECK CONDITION: answer from the autosense data */
    if (cdb[0] == 0x03 && s->nsense) {
        uint32_t n = alloc ? alloc : (ncdb >= 5 ? cdb[4] : 0); uint8_t *d = calloc(n + 1, 1);
        memcpy(d, s->sense, n < SENSE_SIZE ? n : SENSE_SIZE);
        s->nsense = 0; *din = d; *nin = n;
        if (s->trc) {   /* recorded like a device command (flags bit1: served from autosense) */
            uint64_t t0 = us_now(s);
            w32(s->trc, 0x52435453); w32(s->trc, s->seq++); w64(s->trc, t0); w64(s->trc, t0);
            w8(s->trc, (uint8_t)ncdb); w8(s->trc, 1); w8(s->trc, 1); w8(s->trc, 0); w32(s->trc, 0); w32(s->trc, n); w32(s->trc, n);
            uint8_t c16[16] = { 0 }; memcpy(c16, cdb, ncdb); fwrite(c16, 16, 1, s->trc);
            uint8_t z[SENSE_SIZE] = { 0 }; w8(s->trc, 0); fwrite(z, SENSE_SIZE, 1, s->trc); w8(s->trc, 2);
            w32(s->trc, 0); w32(s->trc, n); w32(s->trc, crc32b(s, d, n)); fwrite(d, n, 1, s->trc);
        }
        return 0;
    }
    int dir = has_out ? 2 : (alloc ? 1 : 0);
    uint32_t len = has_out ? (uint32_t)nout : alloc;
    uint8_t *data = calloc(len + 1, 1); if (has_out && nout) memcpy(data, out, nout);
    int ok = 0, nsense = 0; DWORD err = 0; uint8_t status = 0, sense[SENSE_SIZE];
    uint64_t t0 = us_now(s);
    /* once the scanner has stopped answering (a command ran into the time-out, or Windows reports the device gone),
       every later command fails at once instead of waiting the time-out again (session s9: 60 s, 61 s, 30 s ...) */
    int got = 0, notsent = s->dead;
    if (s->dead) { ok = 0; err = 1167; status = 0; nsense = 0; memset(sense, 0, sizeof sense); }
    else got = exec_raw(s->h, s->path, s->id, cdb, ncdb, dir, data, len, s->timeout_s, &ok, &err, &status, sense, &nsense);
    uint64_t t1 = us_now(s);
    if (s->trc) {
        int scanpix = (cdb[0] == 0x48 && ncdb > 2 && cdb[2] == 3);
        uint32_t stored = (uint32_t)got; uint8_t flags = notsent ? 4 : 0;   /* bit 2: not sent (the scanner had stopped answering) */
        if (scanpix && stored > 256) { stored = 256; flags |= 1; }
        w32(s->trc, 0x52435453); w32(s->trc, s->seq++); w64(s->trc, t0); w64(s->trc, t1);
        w8(s->trc, (uint8_t)ncdb); w8(s->trc, (uint8_t)dir); w8(s->trc, (uint8_t)ok); w8(s->trc, status); w32(s->trc, err);
        w32(s->trc, len); w32(s->trc, (uint32_t)got);
        uint8_t c16[16] = { 0 }; memcpy(c16, cdb, ncdb); fwrite(c16, 16, 1, s->trc);
        w8(s->trc, (uint8_t)nsense); fwrite(sense, SENSE_SIZE, 1, s->trc); w8(s->trc, flags);
        w32(s->trc, has_out ? (uint32_t)nout : 0); if (has_out && nout) fwrite(out, nout, 1, s->trc);
        w32(s->trc, stored); w32(s->trc, got > 0 ? crc32b(s, data, (size_t)got) : 0); if (stored) fwrite(data, stored, 1, s->trc);
        if ((s->seq & 255) == 0) fflush(s->trc);
    }
    if (!ok) {
        if (t->log) TLOG(t->log, "!! SPTI: Windows error %lu on op %02x%s\n", (unsigned long)err, cdb[0], s->dead ? " (not sent: the scanner stopped answering)" : "");
        if (!s->dead) {
            double secs = (double)(t1 - t0) / 1e6;
            int timed_out = secs >= s->timeout_s - 5;
            s->fails++;                       /* two failed commands in a row also mean the scanner no longer answers (review) */
            char m[400];
            /* error 87 (refused as a parameter) and 8 (no memory here) happen before anything reaches the scanner: never a
               sign that the scanner is gone, so they do not count toward "not answering" (review) */
            int not_sent = err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_ENOUGH_MEMORY;
            if (not_sent) {
                s->fails = 0;
                if (err == ERROR_INVALID_PARAMETER && s->maxbytes && len > s->maxbytes)
                    snprintf(m, sizeof m, "SCSI command %02x of %lu bytes was refused by Windows (error 87): your SCSI card's driver allows at most %lu bytes per command. "
                             "This scan needs a larger limit - see \"SCSI card limits\" in README.txt. Smaller scans and previews still work.", cdb[0], (unsigned long)len, (unsigned long)s->maxbytes);
                else if (err == ERROR_INVALID_PARAMETER)
                    snprintf(m, sizeof m, "SCSI command %02x of %lu bytes was refused by Windows (error 87) before it was sent - most likely your SCSI card's driver does not accept a command this large. "
                             "See \"SCSI card limits\" in README.txt; please send the Logs folder.", cdb[0], (unsigned long)len);
                else snprintf(m, sizeof m, "SCSI command %02x could not be sent: not enough memory on this PC (Windows error 8).", cdb[0]);
            } else if (timed_out || err == 1167 || s->fails >= 2) {
                s->dead = 1; spti_dead = 1; scsi_gone = 1;
                snprintf(m, sizeof m, "THE SCANNER IS NOT ANSWERING (SCSI command %02x, Windows error %lu after %.0f s): it may be switched off, "
                         "or its cable or adapter came loose. Switch the scanner off, wait 10 seconds, switch it on, wait for the steady "
                         "green light, then start this program again.", cdb[0], (unsigned long)err, secs);
            } else snprintf(m, sizeof m, "SCSI command %02x failed: Windows error %lu after %.1f s.", cdb[0], (unsigned long)err, secs);
            if (t->note) t->note(m);
        }
        /* reported to the driver as CHECK CONDITION, sense key ABORTED COMMAND */
        memset(s->sense, 0, SENSE_SIZE); s->sense[0] = 0x70; s->sense[2] = 0x0B; s->sense[7] = 10; s->nsense = 18;
        free(data); return 2;
    }
    s->fails = 0;
    if (status == 2) { memcpy(s->sense, sense, SENSE_SIZE); s->nsense = nsense ? nsense : SENSE_SIZE; }
    else s->nsense = 0;
    if (dir == 1) { *din = data; *nin = (size_t)got; } else free(data);
    return status;
}
static void spti_destroy(ScsiTarget *t) { Spti *s = (Spti *)t; if (s->trc) fclose(s->trc); CloseHandle(s->h); free(s); }

/* INQUIRY at one place (port, bus, ID, LUN 0); 1 = a device answered; d36 = its 36 inquiry bytes */
static int inquiry_at(HANDLE h, int path, int id, uint8_t d36[36], int timeout) {
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 }, sense[SENSE_SIZE], st; int ok, ns; DWORD err;
    memset(d36, 0, 36);
    exec_raw(h, path, id, cdb, 6, 1, d36, 36, timeout, &ok, &err, &st, sense, &ns);
    return ok && st == 0 && (d36[0] & 0xE0) == 0;     /* peripheral qualifier 0: a device is connected there */
}
static HANDLE open_port(int p) {
    char name[32]; snprintf(name, sizeof name, "\\\\.\\Scsi%d:", p);
    return CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}
ScsiTarget *spti_open_at(int port, int path, int id, Emu *log, const char *trace_path, char *msg, int msgmax) {
    if (spti_fake) { snprintf(msg, msgmax, "(test list of SCSI devices: no SCSI command is sent)"); return NULL; }
    HANDLE h = open_port(port);
    if (h == INVALID_HANDLE_VALUE) { snprintf(msg, msgmax, "SCSI port %d (\\\\.\\Scsi%d:) could not be opened (Windows error %lu). Is the program running as Administrator?", port, port, (unsigned long)GetLastError()); return NULL; }
    uint8_t d[36];
    if (!inquiry_at(h, path, id, d, 5) || memcmp(d + 8, "SCITEX", 6)) {
        snprintf(msg, msgmax, "No Scitex scanner answers at SCSI port %d, bus %d, ID %d%s%.8s %.16s%s.", port, path, id,
                 d[8] ? " (found there: " : "", d[8] ? (const char *)d + 8 : "", d[8] ? (const char *)d + 16 : "", d[8] ? ")" : "");
        CloseHandle(h); return NULL; }
    Spti *s = calloc(1, sizeof *s); s->h = h; s->port = port; s->path = path; s->id = id; s->timeout_s = 60;
    s->t.execute = spti_execute; s->t.destroy = spti_destroy; s->t.log = log;
    QueryPerformanceFrequency(&s->qpf);
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; s->crc_table[i] = c; }
    if (trace_path) { s->trc = u_fopen(trace_path, "wb"); if (s->trc) fwrite("S342TRC1", 8, 1, s->trc); }
    /* 0.5.0: the card driver's limits (a question to Windows' port driver - nothing is sent to the scanner) */
    memset(&spti_caps, 0, sizeof spti_caps);
    { IO_SCSI_CAPABILITIES c; DWORD ret = 0; memset(&c, 0, sizeof c); c.Length = sizeof c;
      if (DeviceIoControl(h, IOCTL_SCSI_GET_CAPABILITIES, NULL, 0, &c, sizeof c, &ret, NULL) && ret >= offsetof(IO_SCSI_CAPABILITIES, AlignmentMask) + sizeof(ULONG)) {
          spti_caps.known = 1; spti_caps.max_transfer = c.MaximumTransferLength; spti_caps.max_pages = c.MaximumPhysicalPages; spti_caps.align_mask = c.AlignmentMask;
          spti_caps.max_bytes = spti_caps_limit(c.MaximumTransferLength, c.MaximumPhysicalPages);
          s->maxbytes = spti_caps.max_bytes;
      } }
    snprintf(msg, msgmax, "Scanner found on \\\\.\\Scsi%d: bus %d, ID %d (%.8s %.16s %.4s)", port, path, id, d + 8, d + 16, d + 32);
    return &s->t;
}
/* the devices on every SCSI port: first Windows' own list (made when Windows found the devices - nothing is sent to
   them), then, with probe, an INQUIRY at every other ID of each bus (a scanner switched on after Windows started is
   not in Windows' list). LUN 0 only (the scanner answers on LUNs 0 - 7). */
int spti_scan(SptiDev *out, int max, int probe) {
    if (spti_fake) return fake_scan(out, max);
    int n = 0;
    for (int p = 0; p < 16 && n < max; p++) {
        HANDLE h = open_port(p); if (h == INVALID_HANDLE_VALUE) continue;
        static uint8_t buf[65536]; DWORD ret = 0; memset(buf, 0, sizeof buf);
        int nbus = 1, init[8]; for (int b = 0; b < 8; b++) init[b] = -1;
        int n0 = n;
        if (DeviceIoControl(h, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0, buf, sizeof buf, &ret, NULL) && ret >= 1) {
            PSCSI_ADAPTER_BUS_INFO bi = (PSCSI_ADAPTER_BUS_INFO)buf;
            nbus = bi->NumberOfBuses; if (nbus < 1) nbus = 1; if (nbus > 8) nbus = 8;
            for (int b = 0; b < bi->NumberOfBuses && b < 8; b++) {
                if ((size_t)((uint8_t *)&bi->BusData[b + 1] - buf) > ret) break;
                init[b] = bi->BusData[b].InitiatorBusId;
                ULONG off = bi->BusData[b].InquiryDataOffset; int guard = 0;
                while (off && off + sizeof(SCSI_INQUIRY_DATA) <= ret && guard++ < 256 && n < max) {
                    PSCSI_INQUIRY_DATA q = (PSCSI_INQUIRY_DATA)(buf + off);
                    if (q->Lun == 0) {
                        SptiDev *d = &out[n++]; memset(d, 0, sizeof *d);
                        d->port = p; d->path = q->PathId; d->target = q->TargetId; d->claimed = q->DeviceClaimed; d->listed = 1; d->initiator = init[b];
                        uint8_t id[36] = { 0 }; ULONG L = q->InquiryDataLength; if (L > 36) L = 36;
                        if (off + offsetof(SCSI_INQUIRY_DATA, InquiryData) + L <= ret) memcpy(id, q->InquiryData, L);
                        dev_fill(d, id);
                    }
                    if (q->NextInquiryDataOffset <= off) break;
                    off = q->NextInquiryDataOffset;
                }
            }
        }
        if (probe) for (int b = 0; b < nbus && n < max; b++) for (int t = 0; t < 16 && n < max; t++) {
            if (t == init[b]) continue;
            int known = 0; for (int k = n0; k < n; k++) if (out[k].path == b && out[k].target == t) known = 1;
            if (known) continue;
            uint8_t id[36];
            if (!inquiry_at(h, b, t, id, 2)) continue;
            SptiDev *d = &out[n++]; memset(d, 0, sizeof *d);
            d->port = p; d->path = b; d->target = t; d->initiator = init[b]; dev_fill(d, id);
        }
        CloseHandle(h);
    }
    return n;
}
ScsiTarget *spti_open(int id, Emu *log, const char *trace_path, char *msg, int msgmax) {
    for (int p = 0; p < 16; p++) for (int b = 0; b < 4; b++) {
        HANDLE h = open_port(p); if (h == INVALID_HANDLE_VALUE) break;
        uint8_t d[36]; int hit = inquiry_at(h, b, id, d, 5) && !memcmp(d + 8, "SCITEX", 6); CloseHandle(h);
        if (hit) return spti_open_at(p, b, id, log, trace_path, msg, msgmax);
    }
    snprintf(msg, msgmax, "Scanner not found at SCSI ID %d. Is the scanner on (steady green) and is the program running as Administrator?", id);
    return NULL;
}
#else
ScsiTarget *spti_open(int id, Emu *log, const char *trace_path, char *msg, int msgmax) {
    (void)id; (void)log; (void)trace_path; snprintf(msg, msgmax, "SCSI pass-through is only available on Windows"); return NULL;
}
ScsiTarget *spti_open_at(int port, int path, int id, Emu *log, const char *trace_path, char *msg, int msgmax) {
    (void)port; (void)path; (void)id; (void)log; (void)trace_path; snprintf(msg, msgmax, "SCSI pass-through is only available on Windows"); return NULL;
}
int spti_scan(SptiDev *out, int max, int probe) { (void)probe; return spti_fake ? fake_scan(out, max) : 0; }
#endif
