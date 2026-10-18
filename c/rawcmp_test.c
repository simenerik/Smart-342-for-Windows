/* rawcmp_test.c - the RAW check comparison (rawcmp.c) on the RAW data of hardware session s10 (2026-10-09 16:47,
 * C-41 negative, round T 35mm holder, 2000 dpi), taken from the session's SCSI trace (every 0x48 data read after each
 * image-processor block, complete): N = normal 600 x 400, Q = normal 1000 x 1000, S = the area of N STREAMED (cmd 4,
 * 1.4 times round a 1019 KB ring). Run from the work folder. Data: 12-bit little-endian interleaved RGB, as read;
 * converted here as the window does (driver's 14->12 table undone, x 4).
 * Expected (measured by hand with an independent Python Lucas-Kanade on the green channel, sigma 1.5):
 *   S against N: shift -0.40 across, +0.76 along, the same in all 8 bands (spread 0.11 / 0.06) -> passes;
 *   N in Q at (199.85, 300.51), S in Q at (200.25, 299.76) -> both pass (the scanner's repeatability: two scans of
 *   the same place differ by up to 0.76 px along here);
 *   S with one line cut out (rows below move up one line; last line repeated) -> fails (jump ~1 px);
 *   S with one line repeated -> fails; S of other content (Q's corner) -> fails. */
#include "rawcmp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint16_t *load(const char *path, int w, int h) {
    FILE *f = fopen(path, "rb"); if (!f) { printf("FAIL cannot open %s\n", path); exit(1); }
    size_t n = (size_t)w * h * 3; uint8_t *b = malloc(n * 2); uint16_t *o = malloc(n * 2);
    if (fread(b, 2, n, f) != n) { printf("FAIL short file %s\n", path); exit(1); }
    fclose(f);
    for (size_t i = 0; i < n; i++) {
        double v = b[2 * i] | b[2 * i + 1] << 8;
        double e = v < 255 ? v : 255 + (v - 255 + 0.5) / 0.238095239; if (e > 16383) e = 16383;
        o[i] = (uint16_t)floor(e * 4 + 0.5);
    }
    free(b); return o;
}
static void show(const char *what, const RcCmp *r) {
    printf("     %s: shift %+.3f across %+.3f along, match %.4f, bands %d (%d measured), spread along %.3f across %.3f, band match >= %.4f, largest step %+.3f at row %d\n",
           what, r->dx, r->dy, r->ncc, r->bands, r->measured, r->bdy_max - r->bdy_min, r->bdx_max - r->bdx_min, r->bncc_min, r->jump, r->jump_row);
}
int main(void) {
    uint16_t *N = load("ref/hw/s10_N_600x400.bin", 600, 400), *Q = load("ref/hw/s10_Q_1000x1000.bin", 1000, 1000), *S = load("ref/hw/s10_S_600x400.bin", 600, 400);
    RcCmp r; size_t L = 600 * 3;
    rc_compare(N, 600, 400, S, 600, 400, 0, 0, 2.5, &r); show("S against N", &r);
    CHECK(r.ok && fabs(r.dy - 0.76) < 0.1 && fabs(r.dx + 0.40) < 0.1 && r.bands == 8 && r.measured == 8, "streamed S = normal N (hardware s10): constant shift, no lost or repeated line");
    rc_compare(N, 600, 400, Q, 1000, 1000, 200, 300, 2.5, &r); show("N in Q", &r);
    CHECK(r.ok && fabs(r.dy - 0.515) < 0.1, "normal N found in normal Q at (200, 300.5): the scanner's own repeatability");
    rc_compare(S, 600, 400, Q, 1000, 1000, 200, 300, 2.5, &r); show("S in Q", &r);
    CHECK(r.ok && fabs(r.dy + 0.24) < 0.1 && fabs((r.dy - 0.515) + 0.76) < 0.1, "streamed S found in normal Q at (200, 299.76): consistent with S against N (+0.76 = 0.515 + 0.24)");
    /* one line cut out at row 290 (where S's ring wrapped first: 1,043,xxx bytes / 3600 per line) */
    uint16_t *X = malloc(600 * 400 * 3 * 2);
    memcpy(X, S, 290 * L * 2); memcpy(X + 290 * L, S + 291 * L, (400 - 291) * L * 2); memcpy(X + 399 * L, S + 399 * L, L * 2);
    rc_compare(N, 600, 400, X, 600, 400, 0, 0, 2.5, &r); show("S, line 290 cut out", &r);
    CHECK(!r.ok && r.bdy_max - r.bdy_min > 0.8 && r.jump_row == 300, "a lost line at row 290 is found (step %+.2f px at the band from row %d)", r.jump, r.jump_row);
    /* one line repeated at row 120 */
    memcpy(X, S, 121 * L * 2); memcpy(X + 121 * L, S + 120 * L, (400 - 121) * L * 2);
    rc_compare(N, 600, 400, X, 600, 400, 0, 0, 2.5, &r); show("S, line 120 repeated", &r);
    CHECK(!r.ok && r.bdy_max - r.bdy_min > 0.8, "a repeated line at row 120 is found");
    /* a lost line near the top (row 10): the first band is mixed; still a step */
    memcpy(X, S, 10 * L * 2); memcpy(X + 10 * L, S + 11 * L, (400 - 11) * L * 2); memcpy(X + 399 * L, S + 399 * L, L * 2);
    rc_compare(N, 600, 400, X, 600, 400, 0, 0, 2.5, &r); show("S, line 10 cut out", &r);
    printf("     (a line lost at row 10: %s - see rawcmp.h, near the first row it is only partly visible)\n", r.ok ? "not found" : "found");
    /* other content: N against Q's top-left corner */
    rc_compare(N, 600, 400, Q, 1000, 1000, 0, 0, 2.5, &r); show("N against Q's corner", &r);
    CHECK(!r.ok, "a picture of another place is refused");
    printf("%s (%d failed)\n", fails ? "FAIL" : "ALL PASS", fails);
    free(N); free(Q); free(S); free(X);
    return fails != 0;
}
