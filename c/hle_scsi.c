/* hle_scsi.c - SCSI Manager HLE: original 'old' interface with TIB interpreter, and SCSI Manager 4.3
 * SCSIAction (Inside Macintosh: Devices, ch. 3). Port of scripts/mac_scsi.py. The bus drives
 * ScsiTarget objects (the simulator now, the SPTI pass-through on Windows). */
#include "mac.h"
#include "sim.h"
#include <stdlib.h>
#include <string.h>

#define W (E->W)
#define REG(name) static int64_t h_##name(Emu *E)
enum { scInc = 1, scNoInc = 2, scAdd = 3, scMove = 4, scLoop = 5, scNop = 6, scStop = 7, scComp = 8 };
#define scsiNonZeroStatus (-7932)

typedef struct Bus {
    World *w; ScsiTarget *tg[8]; int log;
    int sel;                           /* -1 = None */
    uint8_t cdb[256]; int ncdb, has_cdb;
    Bytes out;
    uint8_t *inbuf; size_t nin, inpos;
    int status, executed, ncmd;
    double rate, overhead, frac;
    double rec_s; int64_t rec_base; int rec_started;     /* replay timing */
    uint8_t *last_key; size_t nlast; int ncdb_last, have_last; int repeat, max_repeat;
} Bus;

Bus *bus_new(World *Wd, ScsiTarget **targets, int log) {
    Bus *b = calloc(1, sizeof *b);
    b->w = Wd; for (int i = 0; i < 8; i++) b->tg[i] = targets[i];
    b->log = log; b->sel = -1; b->rate = 3.3e6; b->overhead = 0.0004; b->frac = 0.0; b->max_repeat = 200000;
    Wd->bus = b; return b;
}
int bus_ncmd(Bus *b) { return b->ncmd; }

static void hex(const uint8_t *d, size_t n, char *o) { for (size_t i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", d[i]); o[2 * n] = 0; }

/* 0.3.0: the scanner's parameter file ('flash14') as the scanner software reads it at every start (SCP 42 cmd 0x13
   open-for-read, 43 '4' data packets <HHHHH 0,0,0,seq,len> + data, 43 '2' close - hwtools/flash_from_trace.py). Kept
   for the Scanner(Flash Disk) -> Smart342.Prefs copy when the prefs belong to another scanner (app.c). Nothing extra is
   sent to the scanner: this only watches the software's own read. */
uint8_t *scsi_flash_cap; size_t scsi_nflash_cap;
static struct { int on; char name[64]; Bytes pk[256]; int have[256]; } fcap;
static void flash_cap_feed(const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int st, const uint8_t *din, size_t nin) {
    if (ncdb < 2) return;
    if (cdb[0] == 0x42 && nout >= 12 && (out[2] | out[3] << 8) == 0x13) {
        for (int i = 0; i < 256; i++) { bytes_free(&fcap.pk[i]); fcap.have[i] = 0; }
        size_t L = 0; while (12 + L < nout && out[12 + L] && L < sizeof fcap.name - 1) L++;
        memcpy(fcap.name, out + 12, L); fcap.name[L] = 0; fcap.on = 1;
    } else if (cdb[0] == 0x43 && cdb[1] == '4' && fcap.on && st == 0 && din && nin >= 10) {
        int seq = din[6] | din[7] << 8, len = din[8] | din[9] << 8;
        if (seq < 256 && (size_t)len + 10 <= nin) { bytes_set(&fcap.pk[seq], din + 10, (size_t)len); fcap.have[seq] = 1; }
    } else if (cdb[0] == 0x43 && cdb[1] == '2' && fcap.on) {
        fcap.on = 0;
        if (strcmp(fcap.name, "flash14")) return;
        int k = 0; size_t tot = 0; while (k < 256 && fcap.have[k]) tot += fcap.pk[k++].n;
        for (int i = k; i < 256; i++) if (fcap.have[i]) return;    /* a packet missing in between: not complete */
        uint8_t *f = malloc(tot + 1); size_t o = 0;
        for (int i = 0; i < k; i++) { memcpy(f + o, fcap.pk[i].b, fcap.pk[i].n); o += fcap.pk[i].n; }
        free(scsi_flash_cap); scsi_flash_cap = f; scsi_nflash_cap = tot;
    }
}

static void bus_execute(Emu *E, Bus *b, uint32_t alloc_in) {
    if (b->executed) return;
    if (b->sel < 0 || b->sel > 7 || !b->tg[b->sel]) emu_fail(E, "SCSI: no target %d", b->sel);
    ScsiTarget *t = b->tg[b->sel];
    uint8_t *din = NULL; size_t nin = 0;
    int st = t->execute(t, b->cdb, b->ncdb, b->out.b, b->out.n, b->out.n > 0, alloc_in, &din, &nin);
    free(b->inbuf); b->inbuf = din; b->nin = din ? nin : 0; b->inpos = 0; b->status = st; b->executed = 1;
    flash_cap_feed(b->cdb, b->ncdb, b->out.b, b->out.n, st, din, din ? nin : 0);
    /* virtual time: transfer + per-command overhead */
    if (t->has_dt && world_extclock) {          /* replay of a recorded session: the clock follows its timing */
        if (!b->rec_started) { b->rec_started = 1; b->rec_base = ticks(b->w); }
        b->rec_s += t->last_dt; world_set_ext_clock(b->w, b->rec_base + (int64_t)(b->rec_s * 60.0));
    } else {
        b->frac += (b->overhead + (double)(b->out.n + b->nin) / b->rate) * 60.0;
        if (b->frac >= 1.0) { int64_t k = (int64_t)b->frac; tick(b->w, k); b->frac -= (double)k; }
    }
    /* watchdog: identical (cdb, out) repeated */
    size_t nk = b->ncdb + b->out.n; uint8_t *key = malloc(nk + 1);
    memcpy(key, b->cdb, b->ncdb); if (b->out.n) memcpy(key + b->ncdb, b->out.b, b->out.n);
    /* python compares the tuple (cdb, out) */
    int same = b->have_last && b->nlast == nk && b->ncdb_last == b->ncdb && !memcmp(b->last_key, key, nk);
    if (same) {
        b->repeat++;
        if (b->repeat > b->max_repeat) { char h[600]; hex(b->cdb, b->ncdb, h); free(key); emu_fail(E, "watchdog: %d identical SCSI commands %s", b->repeat, h); }
        free(key);
    } else { free(b->last_key); b->last_key = key; b->nlast = nk; b->ncdb_last = b->ncdb; b->have_last = 1; b->repeat = 0; }
    if (b->log && E->trace) {
        char wc[200], h[600]; hex(b->cdb, b->ncdb, h); emu_where_caller(E, wc);
        TLOG(E, "SCSI#%d id%d CDB %s out=%zu in=%zu st=%d  [%s]\n", b->ncmd, b->sel, h, b->out.n, b->nin, st, wc);
        if (b->out.n && b->out.n <= 64) { char o[200]; hex(b->out.b, b->out.n, o); TLOG(E, "    out: %s\n", o); }
        if (b->nin && b->nin <= 64) { char o[200]; hex(b->inbuf, b->nin, o); TLOG(E, "    in : %s\n", o); }
    }
    b->ncmd++;
}

/* Data-in length the TIB will transfer, computed without side effects (writes go to an overlay).
 * The simulator ignores it; a real device (SPTI) needs it as the transfer length up front. */
static uint32_t tib_dry_run(Emu *E, uint32_t tib) {
    uint32_t ka[64], va[64]; int nk = 0;
#define OVR(a) ({ uint32_t _a = (a), _v = rd32(E, _a); for (int _i = 0; _i < nk; _i++) if (ka[_i] == _a) _v = va[_i]; _v; })
#define OVW(a, v) do { uint32_t _a = (a), _v = (v); int _i = 0; for (; _i < nk; _i++) if (ka[_i] == _a) break; if (_i == nk) { if (nk == 64) return total; ka[nk] = _a; nk++; } va[_i] = _v; } while (0)
    uint32_t pc = tib, total = 0;
    for (long steps = 0; steps < 1000000; steps++) {
        uint32_t op = rd16(E, pc), p1 = OVR(pc + 2), p2 = OVR(pc + 6);
        if (op == scInc || op == scNoInc) { total += p2; if (op == scInc) OVW(pc + 2, p1 + p2); pc += 10; }
        else if (op == scAdd) { OVW(p1, OVR(p1) + p2); pc += 10; }
        else if (op == scMove) { OVW(p2, OVR(p1)); pc += 10; }
        else if (op == scLoop) { p2 = p2 - 1; OVW(pc + 6, p2); if (p2 != 0 && p2 < 0x80000000u) pc += p1; else pc += 10; }
        else if (op == scNop) pc += 10;
        else break;     /* scStop, scComp or invalid: the real run reports it */
    }
#undef OVR
#undef OVW
    return total;
}

static uint32_t run_tib(Emu *E, Bus *b, uint32_t tib, int dir_in) {
    uint32_t pc = tib, total = 0;
    uint32_t want = 0;
    if (dir_in) {
        if (b->executed && getenv("SCSI_DEBUG")) fprintf(stderr, "second SCSIRead for cmd %d cdb %02x\n", b->ncmd - 1, b->cdb[0]);
        if (!b->executed) { want = tib_dry_run(E, tib); bus_execute(E, b, want); }
    }
    for (;;) {
        uint32_t op = rd16(E, pc), p1 = rd32(E, pc + 2), p2 = rd32(E, pc + 6);
        if (op == scInc || op == scNoInc) {
            if (dir_in) {
                uint8_t *chunk = calloc((size_t)p2 + 1, 1);
                if (b->inpos < b->nin) { size_t m = b->nin - b->inpos; if (m > p2) m = p2; memcpy(chunk, b->inbuf + b->inpos, m); }
                emu_write(E, p1, chunk, p2); free(chunk); b->inpos += p2;
            } else {
                uint8_t *d = malloc((size_t)p2 + 1); emu_read(E, p1, d, p2); bytes_put(&b->out, d, p2); free(d);
            }
            total += p2;
            if (op == scInc) wr32(E, pc + 2, p1 + p2);
            pc += 10;
        } else if (op == scAdd) { wr32(E, p1, rd32(E, p1) + p2); pc += 10; }
        else if (op == scMove) { wr32(E, p2, rd32(E, p1)); pc += 10; }
        else if (op == scLoop) {
            p2 = p2 - 1; wr32(E, pc + 6, p2);
            if (p2 != 0 && p2 < 0x80000000u) pc += p1; else pc += 10;
        } else if (op == scNop) pc += 10;
        else if (op == scStop) break;
        else if (op == scComp) emu_fail(E, "scComp not modelled");
        else emu_fail(E, "bad TIB opcode %u at %08x", op, pc);
    }
    if (dir_in && getenv("SCSI_DEBUG") && (total != want || total != b->nin))
        fprintf(stderr, "TIB cmd %d cdb %02x: dry %u real %u device %zu\n", b->ncmd - 1, b->cdb[0], want, total, b->nin);
    return total;
}

static Bus *BUS(Emu *E) { if (!W->bus) emu_fail(E, "SCSI call without a bus"); return W->bus; }
REG(SCSIGet) { (void)E; return 0; }
REG(SCSISelect) {
    Bus *b = BUS(E); int tid = SA(E, 0);
    if (tid < 0 || tid > 7 || !b->tg[tid]) return 2;   /* scCommErr: no device */
    b->sel = tid; b->has_cdb = 0; b->ncdb = 0; b->out.n = 0; b->executed = 0; b->status = -1;
    return 0;
}
REG(SCSICmd) {
    Bus *b = BUS(E); int n = SA(E, 1); if (n < 0 || n > 255) emu_fail(E, "SCSICmd length %d", n);
    emu_read(E, A(E, 0), b->cdb, n); b->ncdb = n; b->has_cdb = 1; return 0;
}
REG(SCSIRead) { run_tib(E, BUS(E), A(E, 0), 1); return 0; }
REG(SCSIWrite) { run_tib(E, BUS(E), A(E, 0), 0); return 0; }
REG(SCSIComplete) {
    Bus *b = BUS(E);
    if (b->sel < 0 || !b->has_cdb) return 5;   /* scSequenceErr */
    bus_execute(E, b, 0);
    wr16(E, A(E, 0), (uint32_t)b->status); wr16(E, A(E, 1), 0);
    b->sel = -1; return 0;
}
/* SCSIHdr: 0 qLink, 6 scsiPBLength, 8 scsiFunctionCode, 10 scsiResult, 12 scsiDevice{rsv,bus,target,lun},
 *   16 scsiCompletion, 20 scsiFlags, 24 driverStorage, 28 XPTprivate, 32 reserved (36 bytes)
 * SCSIExecIOPB: 36 scsiResultFlags, 40 scsiDataPtr, 44 scsiDataLength, 48 scsiSensePtr, 52 scsiSenseLength,
 *   53 scsiCDBLength, 54 scsiSGListCount, 60 scsiSCSIstatus, 61 scsiSenseResidual, 64 scsiDataResidual,
 *   68 scsiCDB[16], 84 scsiTimeout, 94 scsiIOFlags, 102 scsiDataType, 103 scsiTransferType, 112 scsiHandshake */
REG(SCSIAction) {
    Bus *b = BUS(E); uint32_t pb = A(E, 0); int fn = rd8(E, pb + 8);
    if (fn == 0x80) {   /* SCSIGetVirtualIDInfo */
        int oid = rds16(E, pb + 36); int ex = (oid >= 0 && oid <= 7 && b->tg[oid]) ? 1 : 0;
        uint8_t dev[4] = { 0, 0, (uint8_t)(oid & 0xFF), 0 }; emu_write(E, pb + 12, dev, 4); wr8(E, pb + 38, ex); wr16(E, pb + 10, 0);
        T(E, "SCSIAction", "GetVirtualIDInfo id %d -> exists %d", oid, ex); return 0;
    }
    if (fn == 0x03) {   /* SCSIBusInquiry */
        wr16(E, pb + 36, 1); wr16(E, pb + 38, 1); wr32(E, pb + 40, 1);
        wr16(E, pb + 44, 176); wr16(E, pb + 46, 176); wr8(E, pb + 52, 1); wr8(E, pb + 69, 7);
        wr16(E, pb + 84, 7); wr16(E, pb + 86, 7); wr16(E, pb + 10, 0);
        T(E, "SCSIAction", "%s", "BusInquiry"); return 0;
    }
    if (fn == 0x01) {   /* SCSIExecIO */
        int tid = rd8(E, pb + 14); uint32_t flags = rd32(E, pb + 20);
        int cdblen = rd8(E, pb + 53); uint8_t cdb[256]; emu_read(E, pb + 68, cdb, cdblen);
        if (rd8(E, pb + 102) != 0) emu_fail(E, "scsiDataType %d not modelled", rd8(E, pb + 102));
        uint32_t dptr = rd32(E, pb + 40), dlen = rd32(E, pb + 44), d = flags & 0xC0000000u;
        b->sel = tid; memcpy(b->cdb, cdb, cdblen); b->ncdb = cdblen; b->has_cdb = 1; b->out.n = 0; b->executed = 0;
        if (d == 0x80000000u) { uint8_t *t = malloc((size_t)dlen + 1); emu_read(E, dptr, t, dlen); bytes_put(&b->out, t, dlen); free(t); }
        bus_execute(E, b, d == 0x40000000u ? dlen : 0);
        uint32_t got = 0;
        if (d == 0x40000000u && dlen) {
            got = b->nin < dlen ? (uint32_t)b->nin : dlen;
            if (got) emu_write(E, dptr, b->inbuf, got);
        } else if (d == 0x80000000u) got = dlen;
        wr32(E, pb + 64, dlen - got); wr8(E, pb + 60, (uint32_t)b->status);
        int res = 0;
        if (b->status != 0) {
            res = scsiNonZeroStatus;
            if (b->status == 2 && !(flags & 0x20000000u)) {
                uint32_t sp = rd32(E, pb + 48); int sl = rd8(E, pb + 52);
                ScsiTarget *t = b->tg[tid]; uint8_t rq[6] = { 3, 0, 0, 0, (uint8_t)sl, 0 }; uint8_t *sense = NULL; size_t ns = 0;
                t->execute(t, rq, 6, NULL, 0, 0, (uint32_t)sl, &sense, &ns);
                if (!sense) ns = 0; if (ns > (size_t)sl) ns = (size_t)sl;
                if (sp && ns) emu_write(E, sp, sense, (uint32_t)ns);
                wr8(E, pb + 61, (uint32_t)(sl - (int)ns)); wr16(E, pb + 36, rd16(E, pb + 36) | 0x0004);   /* scsiAutosenseValid */
                if (b->log && E->trace) { char h[600]; hex(sense, ns, h); TLOG(E, "    autosense: %s\n", h); }
                free(sense);
            }
        }
        wr16(E, pb + 10, (uint32_t)res);
        b->sel = -1;
        uint32_t comp = rd32(E, pb + 16);
        char h[600]; hex(cdb, cdblen, h);
        T(E, "SCSIAction", "ExecIO id %d cdb %s len %u st %d comp %08x", tid, h, dlen, b->status, comp);
        if (comp) { uint32_t a = pb; emu_call_nested(E, comp, &a, 1); return 0; }
        return 0;
    }
    emu_fail(E, "SCSIAction function 0x%02x not modelled", fn);
}

void hle_scsi_register(void) {
#define R(n) emu_reg(#n, h_##n)
    R(SCSIGet); R(SCSISelect); emu_reg("SCSISelAtn", h_SCSISelect); R(SCSICmd); R(SCSIRead); emu_reg("SCSIRBlind", h_SCSIRead);
    R(SCSIWrite); emu_reg("SCSIWBlind", h_SCSIWrite); R(SCSIComplete); R(SCSIAction);
#undef R
}
