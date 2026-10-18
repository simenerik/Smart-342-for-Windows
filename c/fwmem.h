/* fwmem.h - the scanner controller's memory rule for image-processor block commands (SCSI 0x80), as found in its
 * firmware (SCSI.EXE 1.C8, 2026-10-09; addresses in docs/re-progress.md "The RAW (Max Detail) size limit" and
 * "Streaming RAW"). Pure C, no emulator dependencies (used by guard.c and sim.c, tested by fwmem_test.c).
 *
 *   - CDB byte 3 says which block fields the firmware takes from the host (0x80 path 0x720b..0x72c9): bit 0 clear ->
 *     w26 := 1; bit 2 clear -> word +0x22 := 0; bit 3 clear -> end code w28 := default (cmd 1: 0x602, cmd 4: 0x702,
 *     else 0xffff; 0x1fec); bit 4 clear -> rate := 0x7cf, w32 := 0xffff.
 *   - bytes per line = pixels x channels x (bits > 8 ? 2 : 1), whole image T = bpl x ceil(lines / w26) (0x1b18).
 *   - cmd 1 and 2 empty the memory pool first (0x1ade -> 0x4366); cmd 3 and 4 keep what the last cmd 1/2 reserved
 *     (0x438e). Free = 0x6C0000 - reserved (0x4302).
 *   - cmd 4 sizes R = min(T, (free / A) x (A / bpl) x bpl), cmd 1..3 R = T (cmd 2: A taken as 0); the need is
 *     max(3 x A, R) (A = the block size the host gives, bytes) (0xc3c). need > free (or need 0) -> the controller
 *     program prints "Non availble mem for BIP activity", masks all interrupts and exits (0x96f6) - the scanner
 *     stops answering until power-off.
 *   - a successful cmd 1/2 reserves the need (pool pointer saved, 0x4318 with flag 0); cmd 3/4 do not change it.
 *   - the reserve survives SCSI 0xD0 and the end of a host program: it is only known after a cmd 1/2 (review).
 * Also refused here (the firmware would crash or hang): cmd outside 1..4, a zero divisor, for cmd 1/4 an end code
 * without a counter. Firmware arithmetic: 32-bit products, rows rounded up with a 16-bit addition. */
#ifndef FWMEM_H
#define FWMEM_H
#include <stdint.h>

#define FW_POOL 0x6C0000u          /* 7,077,888 bytes */
#define FW_RESERVE_START 6120000u  /* guard.c: the reserve assumed until a cmd 1 passes */

typedef struct {
    int cmd;                       /* 1 prescan / Max Detail, 2 load prescan, 3 processing, 4 final scan */
    uint32_t bpl_host, block;      /* the host's bytes per line and block size (A) */
    uint32_t lines, pixels, channels, bits, w26, w28;
    uint64_t bpl, T;               /* as the firmware computes them */
    uint64_t free_before;          /* free memory when the check runs */
    uint64_t need;                 /* what the check compares with free_before */
    int ok;                        /* 1 = the firmware accepts it; 0 = it would stop */
    uint32_t reserve_after;        /* reserve after the command (cmd 1/2: need; else unchanged) */
} FwBip;

/* decode a 0x80 command (cdb[3] = flags, data = the 34 bytes as sent, words little-endian, longs high word first)
   and apply the rule with the current reserve. Returns 0 = decoded, -1 = too short. */
int fw_bip_check(const uint8_t *cdb, int ncdb, const uint8_t *data, int ndata, uint32_t reserve, FwBip *r);
/* the same 34 bytes with the command word changed (cmd 1 -> 4 for a streamed RAW) */
void fw_bip_set_cmd(uint8_t *data, int cmd);
#endif
