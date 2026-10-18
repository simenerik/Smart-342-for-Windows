/* cleangain.h - 0.3.1: this scanner's own "clean gain vectors" (the cause of the streaks along the scan, see cleangain.c) */
#ifndef CLEANGAIN_H
#define CLEANGAIN_H
#include <stdint.h>
#include <stddef.h>

#define CG_NPIX 5137                 /* the gain line: 5137 positions (smart_env +0x14 - 1), RGB interleaved */
#define CG_N (CG_NPIX * 3)
#define CG_LIGHT 5004                /* positions 0 .. 5003 are photosites (VERIFIED on s19's calibration lines) */
#define CG_OUTLIER 0.0015            /* a white line whose shape differs from this scanner's average by more is not used
                                        (s34: each of 12 lines 0.015 - 0.016 % from the others' average) */
#define CG_KEEP 64                   /* the average follows the most recent lines: at 64 the weights are halved */
#define CG_MARK "Smart342 for Windows:"   /* the start of the text at byte 100 of the GAIN resources this program makes */

#define CG_LOCAL 0.002               /* ... or whose 30-photosite average differs locally by more (dust, a dark patch;
                                        s34: 0.015 - 0.022 % between lines; 60 cells 1 % darker = 0.8 %) */
/* the lit photosites of a white line (green): the longest run whose 31-sample average is above half the line's
   95th percentile, 20 positions inside its ends unless it reaches the array's end; returns the number of positions (0 = none) */
int cg_lit(const uint16_t *v, int *lo, int *hi);
/* the pixel pattern of a white line: green, each readout parity on its own (the gain vector's odd / even step is a
   separate effect), relative deviation from a 15-sample moving average, rms over the lit photosites (fraction) */
double cg_pattern(const uint16_t *v);
/* what the scanner software's correction leaves in every scan: the white line as measured (before) against the same
   line after the clean-gain correction (after) - the gain is K / after, the picture therefore carries before / after.
   Same measure as cg_pattern, over before's lit photosites. */
double cg_residual(const uint16_t *before, const uint16_t *after);

/* how a white line v differs from a reference line (an average): *rms as cg_residual, *local the largest
   30-photosite deviation after removing the smooth profile (fractions) */
void cg_diff(const uint16_t *v, const uint16_t *ref, double *rms, double *local);
/* the same local measure on one line alone (its largest 30-photosite deviation from its smooth profile) */
double cg_local(const uint16_t *v);

/* running sum per lens (0 L67, 1 L89, 2 L110, 3 L60, 4 L80) and mount (0 transparent, 1 reflective): each white line
   is normalised by its own channel means before it is added, so lines of different light levels average properly.
   Lines that differ from the average (CG_OUTLIER / CG_LOCAL) are not used; nref counts them in a row (Fix streaks does
   not store a lens whose latest 2 lines were refused). They are kept apart as candidates (csum): candidates that agree
   with each other replace the average after 3 lines when the difference is broad (rms over CG_OUTLIER, local under
   3 x rms: another lamp / CCD), after CG_REPLACE_LOCAL lines when it is local (a speck
   that stays in the light path; dust on the calibration glass is usually cleaned before). A disagreeing candidate
   starts them again. total = the lines in the average's history. */
#define CG_REPLACE_LOCAL 8
#define CG_MAGIC 0x43474134u          /* "CGA4" */
typedef struct { uint32_t magic, count, total, ccount, nref, clocal; double level[3], clevel[3]; double sum[CG_N], csum[CG_N]; } CgAcc;
/* 1 = a sane accumulator (magic, counts, finite positive levels and sums) */
int cg_acc_valid(const CgAcc *a);
/* adds the line: returns 1 added, 0 not (a candidate; d[0] rms, d[1] local difference), 2 the candidates replaced the
   average, -1 out of memory. The first line is taken as it is; every later one is compared with the average.
   An invalid accumulator starts again. */
int cg_acc_add(CgAcc *a, const uint16_t *v, double *d);
/* the clean vector (CG_N values, the measured lines' average shape at their average level); *pattern = its cg_pattern.
   0 = ok; -1 no usable accumulator; -2 the result is not a plausible white line (level or pattern) */
int cg_acc_vector(const CgAcc *a, uint16_t *out, double *pattern);
/* the GAIN resource of Smart342.Prefs: 200-byte header (the original's: 0, day, month, year, 0, hour, minute, second as
   big-endian words; from byte 100 the text mark) + the vector as big-endian words; id = 601 + 2 x lens + reflective;
   name as the original's */
int cg_resource(const uint16_t *v, int lens, int reflective, const int when[6], const char *mark, uint8_t *out, size_t outmax, int *id, char *name, int nname);
/* 1 = a GAIN resource this program made (its mark copied to mark) */
int cg_resource_ours(const uint8_t *res, size_t n, char *mark, int markmax);
#endif
