/* rawcmp.h - RAW check comparison (app.c rc_measure; tested by rawcmp_test.c with the RAW data of hardware session
 * s10, 2026-10-09). A reference RAW (a normal request) is found in a streamed RAW at its expected place:
 *   - the whole reference is registered (rs_register2): match >= 0.995 and |shift| <= max_off. Two RAWs of the same
 *     place differ by a fraction of a pixel to about a pixel along on this scanner (s10: S against N -0.30 across,
 *     +0.76 along; N at (199.93, 300.52) of Q, S at (200.21, 299.76) - expected (200, 300)): the scanner's
 *     repeatability, so the limit is generous (2.5 px).
 *   - then bands of 50 rows are registered one by one, starting from that shift: a lost or repeated line inside the
 *     reference moves every band below it by one line, so the shift along jumps by about 1 px between bands. The
 *     spread of the band shifts (largest - smallest, along and across) must stay <= 0.4 px (s10: 0.05 / 0.17 / 0.15
 *     along for S-N, N-Q, S-Q; one line cut out of S at row 290 or repeated at row 120: 1.03). Bands of 25 rows were
 *     too noisy (0.20 - 0.32 for the good pairs). Bands without measurable detail are skipped; each measured band
 *     must match >= 0.95 (a single band of the s10 negative: >= 0.987).
 * Not detectable: a lost line outside the reference's rows, or within about its first 10 rows (s10: a line cut out at
 * row 10 gave a spread of 0.14 - the first band is mostly shifted already; the absolute shift then changes by up to
 * 1 px, within max_off). */
#ifndef RAWCMP_H
#define RAWCMP_H
#include <stdint.h>

typedef struct {
    double dx, dy, ncc, bright;          /* whole reference: shift (img(x+dx, y+dy) ~ ref(x, y)), match, brightness ratio */
    int bands, measured;                 /* bands of rows, and those with measurable detail */
    double bdx_min, bdx_max, bdy_min, bdy_max, bncc_min;
    int jump_row;                        /* first row of the band after the largest step along (-1 none) */
    double jump;                         /* that step, px */
    int ok;
} RcCmp;

#define RC_SPREAD_MAX 0.4
#define RC_BAND 50
#define RC_BAND_NCC_MIN 0.95   /* per band (noise of a single band of a negative: 0.987 on s10; another place: < 0.5) */

/* ref (rw x rh) and img (w x h): 16-bit interleaved RGB; the reference is expected at (x0, y0) of img.
   Returns 0 = compared (r->ok tells), -1 = the reference area is outside img, -2 = out of memory. */
int rc_compare(const uint16_t *ref, int rw, int rh, const uint16_t *img, int w, int h, int x0, int y0, double max_off, RcCmp *r);
#endif
