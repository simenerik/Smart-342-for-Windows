/* guard.c - safety interlock between the driver and the scanner.
 * Simen's rules for this scanner: never copy parameters Mac -> Scanner, no calibrations. The original driver
 * writes the scanner's stored state on its own in several places (verified in its code, 2026-10-07 audit):
 *   - motion-board NV-RAM: indexer command 2406 (one NV byte, word 2 = address<<8 | value) from save_NV /
 *     set_nv_scanner_no and init_scanner (the 70-byte parameter block from the Mac-side prefs, addresses 2..0x47),
 *     write_scanner_status (status bytes 0/1: A3/A5/A7), set_light_level (address 0x22); and the parameter
 *     commands 241d 241e 244c 243f 341a 3421 3429 6423 6424 6425 6426. Indexer commands are written one 16-bit
 *     word per WRITE BUFFER (3B) to the command port 0xA09040, little-endian, word count in the top nibble of
 *     the first word (send_motor_command);
 *   - the motion board's firmware: 4F (Accept Indexer's SW fragment) from LoadNVProgram/LoadIndx when its
 *     version differs from the driver's copy;
 *   - the scanner's flash disk: SCP packets (42) 0x11 FileOpen, 0x14 FileData, 0x12 FileClose, 0x15 PgmRun,
 *     0x17 FlErase, 0x18 FlWrite (save_to_flash_disk, and the controller reflash in scsi_init).
 * The guard is fail-closed: only the opcodes this driver uses in normal operation pass, the indexer NV/parameter
 * writes and every SCP packet except 0x13 (open a file for reading) are held back. A held-back command is answered
 * GOOD, so the driver continues (it verifies the NV checksum itself and reports "Loading to NV_ram failed").
 * This first version also holds back the routine NV writes (status byte, light level, lamp timer); allowing those
 * is a later, deliberate decision. */
#include "sim.h"
#include "fwmem.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

volatile int scsi_gone, guard_gone_cmds;
typedef struct { ScsiTarget t; ScsiTarget *in; int idx_left, idx_block; long blocked; char last[300]; long repeats;
                 uint32_t reserve; int sense_pending, held;
                 int st_phase; uint64_t st_T, st_read; } Guard;   /* streamed RAW in progress: 1 reading, 2 read and ended */
/* image-processor block commands (0x80) and the controller's memory (fwmem.h, found in its firmware 2026-10-09):
   - a block the firmware would not have memory for is not sent: the controller program would stop (session s9);
     it is answered CHECK CONDITION with the sense 0x00e3, which makes the driver abort the operation (see below);
   - streamed RAW (experimental): when the window arms it, the next Max Detail block (cmd 1 while the driver's
     current_OperationMode is 3 = Max Detail; previews use the same end code 0xa02, so the code alone does not tell)
     is sent as cmd 4, so the firmware streams it through as many blocks as fit (as for final scans) instead of
     keeping the whole picture; nothing else in the block changes. One block per arming. */
volatile int raw_stream_armed, raw_stream_sent;      /* sent: the number of Max Detail blocks sent as cmd 4 */
volatile int guard_capture;   /* sim.h */
volatile long fw_last_need, fw_last_free; volatile int fw_last_cmd, fw_refused; volatile unsigned long fw_reserve_now;
/* the controller's wait counter after a streamed RAW (SCSI.EXE 1.C8, verified in its code 2026-10-09):
   - SCSI 0x48 with CDB byte 5 = 2 returns 4 bytes from 0xfcb:0xa09a (0x1e95): word a09a, word a09c (little-endian,
     the controller's memory as it is); both are cleared when a transfer loop starts (0x12b8) and by 0x1fbf;
   - a09c is incremented (0x1697) when a line arrives while the ring has no room for it (0x49dc refused it: the
     scanner had to wait for the computer to read) and a host read is served at that moment - once per wait
     (latch a098, cleared at 0x15df when a line is written again); the stalled line is not copied and its event is
     kept for later (0x16a9 -> 0x520); nothing is overwritten, no error; the console prints "Recovery from
     stop-spiral" when the ring has drained below one block (0x4d53);
   - a09a: incremented in the read service 0xf5e under a flag set by the interrupt queue code (0x580);
   - the driver's names (SCIF_get_bip_intrp_violation 0x07d5c0, final_scan_report 0x048f14..0x048f44: word 0 ->
     "Num of controler - bip timing violation", word 1 -> "Num of host - controler timing violation"): a09a = the
     controller was late for the image processor, a09c = the host was late for the controller (buffers full);
   - the motion board keeps its own count of spiral stops (motor_num_stop_spiral 0x03aed4, indexer command 0x140d),
     which the driver logs as "Num stop_spiral" after final scans AND after prescans (previews, Max Detail) made with
     the spiral method (prescan_report 0x048d38: method field 0xa, the same test that prints "prescan method:
     Spiral") - the driver asks for it after prescans too (that a prescan can stop and resume is inferred, not
     verified);
   - the driver reads it once after every final scan (all traced scans: 00000000), with exactly this CDB, after the
     image processor's end handshake (83, 3b ae0020, 83, 82, 3b ae0038, 3b ae0000), status polls (3c a08c), an
     indexer command (3b a09040) and 3c a08a. For a streamed RAW the guard sends the same CDB after all of the picture
     was read and the same end handshake was seen, right before the driver's next status poll - earlier than the
     driver does after a final scan (before its indexer command; review), but like it after the end handshake, when
     the transfer loop is over. Read only (0x1e95 copies 4 bytes); no state of the scanner changes.
   - a count of 0 means the controller never counted a wait; whether every wait is counted (only when a host read is
     served while the ring is full) is not fully established (review) - the window says "counter 0", not more. */
volatile long raw_stream_wait = -1, raw_stream_cnt1 = -1; volatile int raw_stream_cnt_seq;
static const uint8_t cnt_cdb[10] = { 0x48, 0x01, 0x01, 0x00, 0x00, 0x02, 0x00, 0x00, 0x04, 0x00 };

static int nv_write_op(uint32_t w) {
    static const uint16_t ops[] = { 0x2406, 0x241d, 0x241e, 0x244c, 0x243f, 0x341a, 0x3421, 0x3429, 0x6423, 0x6424, 0x6425, 0x6426 };
    for (size_t i = 0; i < sizeof ops / sizeof *ops; i++) if (ops[i] == w) return 1;
    return 0;
}
static void gnote(Guard *g, const char *fmt, ...) {
    char m[300]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    if (g->t.log) TLOG(g->t.log, "!! INTERLOCK: %s\n", m);      /* the trace gets every one */
    if (g->t.note) g->t.note(m);      /* every one; the receiver groups repeats for display */
}
static int must_block(Guard *g, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out) {
    switch (cdb[0]) {
    case 0x00: case 0x03: case 0x12: case 0x3C: case 0x43: case 0x48: case 0x4A: case 0x4E: case 0x80: case 0x82:
    case 0x83: case 0x85: case 0x86: case 0xD0: case 0xD4: case 0xD5: case 0xED: case 0xEE:
        return 0;
    case 0x3B: {
        if (ncdb < 9) { g->blocked++; gnote(g, "held back a short WRITE BUFFER command"); return 1; }
        uint32_t addr = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        uint32_t n = (uint32_t)cdb[6] << 16 | (uint32_t)cdb[7] << 8 | cdb[8];
        if (addr < 0xA00000 || addr + n > 0xB00000) { g->blocked++; gnote(g, "held back a WRITE BUFFER outside the scanner's register space (address %06x)", addr); return 1; }
        if (addr != 0xA09040) {
            if (addr < 0xA09042 && addr + n > 0xA09040) { g->blocked++; gnote(g, "held back a WRITE BUFFER overlapping the indexer port (address %06x)", addr); return 1; }
            return 0;
        }
        int block = 0;
        for (size_t k = 0; k + 1 < nout; k += 2) {
            uint32_t w = out[k] | (out[k + 1] << 8);          /* the scanner's 386 side is little-endian */
            if (g->idx_left == 0) {
                int nw = (int)((w >> 12) & 0xF); g->idx_left = (nw > 1 ? nw : 1) - 1;
                g->idx_block = nv_write_op(w);
                if (g->idx_block) { g->blocked++; gnote(g, "held back a motion-board NV-RAM write (indexer command %04x)", w); }
            } else g->idx_left--;
            if (g->idx_block) block = 1;
        }
        if (!has_out) return 0;
        return block;
    }
    case 0x42: {
        if (!has_out || nout < 4) { g->blocked++; gnote(g, "held back a malformed flash-disk SCP packet"); return 1; }
        uint32_t cmd = out[2] | (out[3] << 8);
        if (cmd != 0x13) { g->blocked++; gnote(g, "held back a flash-disk write (SCP command %02x); only reading the flash disk is allowed", cmd); return 1; }
        return 0;
    }
    case 0x4F: g->blocked++; gnote(g, "held back a motion-board FIRMWARE load (4F) - the driver wanted to reprogram the motion board"); return 1;
    default: g->blocked++; gnote(g, "held back SCSI command %02x (not used by the driver in normal operation)", cdb[0]); return 1;
    }
}
static int g_execute(ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out,
                     uint32_t alloc, uint8_t **din, size_t *nin) {
    Guard *g = (Guard *)t;
    /* 0.2.0: a scanner that no longer answers (switched off or disconnected; the simulator's stopped or "switched
       off" controller): every command fails at once, and the driver's error handling asks for the sense again and
       again - each failed REQUEST SENSE goes one level deeper (Check_Sense_SCSI -> Check_SCSIComplete). Hardware s36
       (quit with the scanner switched off): about 23,800 commands in 3 s, then the emulated stack ran out ("unmapped
       write at 3fbfffe0" in SCSI_Data_Phase_In). Nothing useful can follow, so after GONE_LIMIT commands the scanner
       software is stopped here with a message the window recognises (GONE_TAG). The driver's own first reactions
       still run (s36: 25 commands and three alerts before the deep recursion). */
    if ((scsi_gone || sim_fw_dead) && t->log) {
        guard_gone_cmds++;
        if (guard_gone_cmds >= GONE_LIMIT) {
            TLOG(t->log, "%s: %d commands to a scanner that no longer answers - the scanner software is stopped here (last op %02x)\n", GONE_TAG, guard_gone_cmds, cdb[0]);
            emu_fail(t->log, "%s: the scanner no longer answers (%d commands failed)", GONE_TAG, guard_gone_cmds);
        }
    }
    /* the sense of a command answered CHECK CONDITION here (a held-back image-processor block, or a data poll/read
       while held): one Scitex sense word 0x00e3 (count 1, little-endian). The driver's Check_SCSIComplete passes it
       to scsi_interupt_treat (0x07ce3c), which returns -1 for 0xe3 ("Motion board S/W loading failed", an alert)
       without the paths other codes take (FRQU / ExitToShell for 0x90/0x91/0xe1..0xe8, write_scanner_status for
       0xf7..0xff, the emergency-stop text for low nibble 0xf; the 0xe3 test at 0x07d194 also skips
       write_scanner_status) - found by the independent review. In the simulator the driver then ends the operation
       with its own abort (FABT: motors stopped, scanner re-initialised) before any scan motion, and the next RAW and
       preview work. A zero sense (count 0) made it continue as if nothing happened; "not ready" polls kept it
       polling for ever (both tried). The window explains the misleading alert. */
    if (g->sense_pending && cdb[0] == 0x03) {
        g->sense_pending = 0; size_t n = alloc ? alloc : 8; *din = calloc(n > 4 ? n : 4, 1); *nin = n;
        if (n >= 4) { (*din)[0] = 1; (*din)[2] = 0xe3; }
        t->has_dt = g->in->has_dt; t->last_dt = 0.001; return 0;
    }
    g->sense_pending = 0;
    if (g->st_phase && (cdb[0] == 0x80 || cdb[0] == 0xD0)) {      /* a new block or a reset: the counters would be cleared */
        if (g->t.log) {
            if (g->st_phase == 2) TLOG(g->t.log, "STREAMCNT not read: all of the streamed RAW was read, but a new block or a reset came before the next status poll\n");
            else TLOG(g->t.log, "STREAMCNT not read: the streamed RAW ended before all of it was read (%llu of %llu bytes)\n", (unsigned long long)g->st_read, (unsigned long long)g->st_T);
        }
        g->st_phase = 0;
    }
    if (g->st_phase == 2 && cdb[0] == 0x3C) {
        g->st_phase = 0;
        uint8_t *cd = NULL; size_t cn = 0;
        g->in->log = t->log; g->in->world = t->world;
        int cst = g->in->execute(g->in, cnt_cdb, 10, NULL, 0, 0, 4, &cd, &cn);
        if (cst == 0 && cd && cn >= 4) {
            raw_stream_cnt1 = cd[0] | cd[1] << 8; raw_stream_wait = cd[2] | cd[3] << 8; raw_stream_cnt_seq++;
            if (g->t.log) TLOG(g->t.log, "STREAMCNT %02x%02x%02x%02x: host - controller timing violations (a09c) %ld, controller - bip timing violations (a09a) %ld\n", cd[0], cd[1], cd[2], cd[3], raw_stream_wait, raw_stream_cnt1);
        } else if (g->t.log) TLOG(g->t.log, "STREAMCNT read failed (status %d, %zu bytes)\n", cst, cn);
        free(cd);
    }
    /* after a block was held back the controller runs no transfer: a data poll or read sent to it would never be
       answered (60 s time-out; review). Until the next block passes they are answered here the same way. */
    if (g->held && cdb[0] == 0x48) { t->has_dt = g->in->has_dt; t->last_dt = 0.001; g->sense_pending = 1; *din = NULL; *nin = 0; return 2; }
    if (must_block(g, cdb, ncdb, out, nout, has_out)) { *din = NULL; *nin = 0; t->has_dt = g->in->has_dt; t->last_dt = 0.001; return 0; }
    uint8_t blk[64]; const uint8_t *send = out; FwBip fb; int is_bip = cdb[0] == 0x80;
    if (is_bip) {
        /* only the form the driver uses (every 0x80 in the hardware sessions: 6-byte CDB, 34 data bytes = CDB byte 4)
           is checked and sent; anything else is held back (review) */
        int form = ncdb >= 5 && has_out && nout >= 34 && nout <= 36 && nout == cdb[4];
        if (form) { memcpy(blk, out, nout); send = blk; fw_bip_check(cdb, ncdb, blk, (int)nout, g->reserve, &fb); }
        if (form && raw_stream_armed && fb.cmd == 1 && raw_op_is_maxdet(g->t.log)) {
            raw_stream_armed = 0;
            /* the RAW shape only (review): 3 channels, word +0x22 not taken from the host, whole lines per block,
               the host's bytes per line = the firmware's, the Max Detail end code */
            if (fb.channels == 3 && !(cdb[3] & 4) && fb.bpl_host && fb.block % fb.bpl_host == 0 && fb.bpl_host == fb.bpl && fb.w28 == 0xa02) {
                fw_bip_set_cmd(blk, 4);
                fw_bip_check(cdb, ncdb, blk, (int)nout, g->reserve, &fb);
                raw_stream_sent++;
                g->st_phase = fb.T > 0; g->st_T = fb.T; g->st_read = 0;   /* T 0 (never from the driver): no read (review) */
                gnote(g, "streamed RAW: the Max Detail block (%u x %u pixels) goes to the scanner as a streaming request (cmd 4): %ld KB of buffers for a %ld KB picture, %ld KB free",
                      fb.pixels, fb.lines, (long)(fb.need / 1024), (long)(fb.T / 1024), (long)(fb.free_before / 1024));
            } else gnote(g, "streamed RAW: this Max Detail block does not have the RAW shape (%u channels, %u bytes per line, block %u) - sent as it is", fb.channels, fb.bpl_host, fb.block);
        }
        if (form) { fw_last_cmd = fb.cmd; fw_last_need = (long)fb.need; fw_last_free = (long)fb.free_before; }
        /* 0.1.21: the counters are read after a normal Max Detail too (the same end handshake on the scanner, s12), so the
           "controler - bip timing violations" of normal and streamed RAWs can be compared (s12: 38 and 218 in the
           streamed S and L, no line lost, repeated or changed - checked row by row) */
        if (form && fb.ok && fb.cmd == 1 && fb.T > 0 && !g->st_phase && raw_op_is_maxdet(g->t.log)) { g->st_phase = 1; g->st_T = fb.T; g->st_read = 0; }
        if (!form || !fb.ok) {
            g->blocked++; fw_refused++; g->sense_pending = 1; g->held = 1;
            if (!form) gnote(g, "held back an image-processor block of an unexpected form (%zu data bytes, CDB byte 4 = %d)", nout, ncdb >= 5 ? cdb[4] : -1);
            else gnote(g, "held back an image-processor block (cmd %d, %u x %u pixels): it needs %llu bytes of the scanner's memory, %llu are free - the scanner's controller program would stop",
                       fb.cmd, fb.pixels, fb.lines, (unsigned long long)fb.need, (unsigned long long)fb.free_before);
            *din = NULL; *nin = 0; t->has_dt = g->in->has_dt; t->last_dt = 0.001; return 2;
        }
        g->held = 0;
    }
    g->in->log = t->log; g->in->world = t->world;
    int st = g->in->execute(g->in, cdb, ncdb, send, nout, has_out, alloc, din, nin);
    /* 0.4.0: the picture phase (hardware s39, session 19:40: a Stop during it left the scanner mid-capture and the next
       capture got "CCD timeout" B01018; every Stop during calibrations was fine) */
    if (is_bip && st == 0) guard_capture = 1;
    else if (cdb[0] == 0xD0) guard_capture = 0;
    else if (guard_capture && st == 0 && cdb[0] == 0x3B && ncdb >= 6 && cdb[2] == 0 && cdb[3] == 0xAE && cdb[4] == 0x00 && cdb[5] == 0x38) guard_capture = 0;
    if (g->st_phase == 1 && st == 0) {
        if (cdb[0] == 0x48 && ncdb >= 10 && cdb[5] == 0) g->st_read += *nin;
        else if (cdb[0] == 0x3B && ncdb >= 9 && cdb[2] == 0 && cdb[3] == 0xAE && cdb[4] == 0 && cdb[5] == 0 && g->st_read >= g->st_T) g->st_phase = 2;   /* end handshake */
    }
    if (is_bip) {
        /* the reserve of the last cmd 1 (the controller keeps it until the next cmd 1, also across 0xD0 and program
           restarts - unknown at start, so the worst case until a cmd 1 has passed); a cmd 1 that failed may have
           reserved its memory or not: the larger value (review) */
        if (fb.cmd == 1) g->reserve = st == 0 ? fb.reserve_after : (g->reserve > fb.need ? g->reserve : (uint32_t)fb.need);
        fw_reserve_now = g->reserve;
    }
    t->has_dt = g->in->has_dt; t->last_dt = g->in->last_dt;     /* timing only (replay); not part of the interlock */
    return st;
}
static void g_destroy(ScsiTarget *t) { Guard *g = (Guard *)t; if (g->in->destroy) g->in->destroy(g->in); free(g); }
ScsiTarget *guard_new(ScsiTarget *inner) {
    Guard *g = calloc(1, sizeof *g); g->in = inner;
    /* the controller keeps the reserve of its last cmd 1 until the next one, also across program restarts (review):
       until a cmd 1 passes here, assume the largest reserve this program can leave - a RAW block of at most
       1,020,000 pixels x 6 bytes (app.c RAW_AREA_MAX + 2 % slack); the original front end (Max Detail <= 500 x 500,
       previews 512 x 512) and the released earlier versions (RAW up to 512 x 698) leave less; the RAW size test of
       0.1.15-0.1.18 (never run on the scanner as far as known) and the developer option --raw-limit could leave more
       (the window warns; the README asks to switch the scanner off and on once before using 0.1.19 - review).
       With it a final scan (3 blocks of at most 307,200 bytes) still fits. */
    g->reserve = FW_RESERVE_START; fw_reserve_now = FW_RESERVE_START;
    g->t.execute = g_execute; g->t.destroy = g_destroy; g->t.log = inner->log; g->t.world = inner->world; g->t.note = inner->note;
    return &g->t;
}
