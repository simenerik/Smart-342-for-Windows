/* rawreg.h - colour registration of a RAW in software (0.1.22): R and B are measured against G from the picture itself
 * and resampled onto G (Lanczos-3); G is not touched.
 *
 * Why (measured 2026-10-09, docs/inpol-research/REPORT.md): the driver registers R and B onto G with its lens table
 * (STR# 401 + lens, in CCD pixels) but applies the along part in scan LINES whatever the along step, so at a finer
 * along step than the CCD sampling (pixel shift and 2x-along RAWs; any RAW above the optical dpi) R is left about
 * 0.7 - 0.9 line and B about 0.2 line off (s18 / s19, both 2x along: R dy -0.70 .. -0.89, dx +0.18 .. +0.24; B dy
 * -0.21 .. -0.30). With the driver's alignment switched off (rawscan.c raw_noalign) the full table offsets remain
 * and are removed here the same way.
 *
 * Measurement: each channel Gaussian-blurred (sigma 1 px), R and B registered to G (rs_register2, NCC + sub-pixel) in
 * RR_BANDS vertical bands (full height). Each direction separately (0.1.23): bands with a match >= RR_MIN_SCORE, those
 * within RR_OUTLIER of the median kept; corrected only if >= RR_MIN_BANDS, and more than half of the measured bands, agree within RR_MAX_MAD of a line (on colour film the
 * three colours' pictures differ, and band shifts can scatter: s13's C-41 negative gave R dx -0.6 .. +2.1 across the
 * bands but R dy -0.54 .. -0.87 - only along was consistent); a straight line across with >= 4 bands (tilt limited),
 * else the median. A direction that does not agree is left as delivered.
 */
#ifndef RAWREG_H
#define RAWREG_H
#include <stdint.h>

#define RR_BANDS 8
#define RR_MIN_SCORE 0.80   /* band match needed (0.90 in 0.1.22 refused a whole C-41 frame, 10 Oct) */
#define RR_MIN_BANDS 3      /* bands that must agree for a direction to be corrected */
#define RR_MAX_MAD 0.08     /* px: median absolute deviation of the agreeing bands; more = the colours' pictures differ (colour film) */
#define RR_MAX_SHIFT 4.0
#define RR_MIN_MOVE 0.02  /* px: a channel whose shift stays below this everywhere is left as delivered */
#define RR_OUTLIER 0.25   /* px: a band this far from the median band, or off the fitted line, is left out */
#define RR_MAX_TILT 0.5   /* px: a fitted slope moving the picture ends more than this is not trusted (mean used) */

typedef struct {
    int ok[2];                      /* channel R (0) / B (1): at least one direction corrected */
    int corrx[2], corry[2];         /* across / along corrected (the bands agreed) */
    int whole[2];                   /* a picture narrower than RR_MIN_BANDS bands: the whole-picture shift was used (review 0.1.23) */
    double madx[2], mady[2];        /* their agreement (median absolute deviation of the bands kept, px) */
    double a_dx[2], b_dx[2], a_dy[2], b_dy[2];   /* shift at column x: a + b (x - w/2): c(x + dx, y + dy) ~ G(x, y); 0 if not corrected */
    int nb; double bdx[2][RR_BANDS], bdy[2][RR_BANDS], bsc[2][RR_BANDS]; int busx[2][RR_BANDS], busy[2][RR_BANDS];
    double gdx[2], gdy[2], gsc[2];  /* whole picture */
} RrInfo;

/* img: w x h x 3 interleaved 16-bit. Returns 0 = measured (see info->ok), -1 bad input / memory. */
int rr_measure(const uint16_t *img, int w, int h, RrInfo *info);
/* 1 = channel k (0 R, 1 B) is moved by rr_apply (measured, and somewhere at least RR_MIN_MOVE px) */
int rr_moves(const RrInfo *info, int k, int w);
/* out = img with R and B resampled by the measured shifts (G copied); out may not alias img. 0 ok. */
int rr_apply(const uint16_t *img, int w, int h, const RrInfo *info, uint16_t *out);
#endif
