/* streakcal.h - 0.4.0: the guided streak calibration's arithmetic (pure C; unit test streakcal_test on the hardware data of
 * 12 Oct, docs/re-progress.md s37 / s37b).
 *
 * The scanner software replaces the pixel pattern of every white line it measures by the stored "clean gain vector" of
 * the lens (cleangain.c). The vector is made here from several 256-line white measurements (RAW-type calibrations: a
 * preview's 16-line ones are noisier and carry a fixed pattern of their 16 sample spots - s37) as the per-photosite
 * MEDIAN, and checked on the pictures of an empty holder frame made with those calibrations before it is stored:
 *   the picture made with the vector in use (old) carries  path / old  per CCD photosite; with the new one it would carry
 *   path / new = picture x old / new. Both are compared after removing the smooth profile (high-pass per readout parity).
 * Dust darkens. A speck that only the white measurements see (dust on the "keep clean" strip) is in the new vector but not
 * in the light path of the pictures, so the corrected picture is locally BRIGHTER there (s37b: CCD 2856 - 2860 +0.6 -
 * 0.8 %, a 0.3 - 0.9 % dip in every scan line; CCD 4075 - 4083 +0.5 %). Those places are found (5-photosite mean of the
 * three colours above SC_BUMP) and repaired from the pictures: there the vector becomes new x (1 + the corrected
 * picture's pattern), which is the light path the pictures saw. Dark places are dust in the pictures' own path (on the
 * film-plane side) - not the vector's business, left alone. The repair is reported (clean the strip anyway). */

/* 0.5.0 (hardware 18 Oct): only GREEN is used for the result, the specks and the repair. In a RAW, green is the CCD's
   own samples, but the scanner moves red and blue sideways by a fraction of a photosite (its colour alignment
   interpolates them onto green), so "picture x old / new" at the same photosite does not hold for them: on 18 Oct the
   prediction said red / blue 0.9 / 1.0 % would be left, the RAW made with the stored vector had 0.08 % in all three;
   43 of the reported "specks" and red / blue repairs of up to 11 % came from that. The white line - and so the vector -
   is per photosite in all three colours, so the vector itself is right for red and blue; a speck on the strip darkens
   all three colours alike, so its repair (from green) is applied to all three. */
#ifndef STREAKCAL_H
#define STREAKCAL_H
#include <stdint.h>
#include "cleangain.h"

#define SC_KMAX 9            /* white measurements / pictures per calibration */
#define SC_BUMP 0.0035       /* a bright place in the corrected picture (5-photosite mean of GREEN) above this is a speck on the strip */
#define SC_MAXR 128          /* specks kept and repaired (0.4.0 kept 16 and left the rest unrepaired) */
#define SC_MAXFIX 0.03       /* largest repair of the vector at one photosite (fraction) */
#define SC_MARGIN 4          /* photosites added on both sides of a found place for the repair */

/* the picture of the empty frame as column means per channel, in CCD photosite order */
typedef struct {
    int c0, n;               /* photosites c0 .. c0 + n - 1 */
    double *p[3];            /* mean over the middle 90 % of the rows */
    double *h1[3], *h2[3];   /* the same over the first / second half of those rows (noise floor) */
} ScProf;

/* the per-photosite median of k white lines (each normalised per channel by its mean over the photosites 0..CG_LIGHT-1),
   scaled back to their mean level. Returns 0, or -1 for k < 1 or k > SC_KMAX. */
int sc_median_vector(const uint16_t *const *lines, int k, uint16_t *out);

/* column means of a RAW (w x h, ch channels, interleaved) into CCD order: RAW column x is CCD photosite ccd_x0 + dir * x
   (dir +1 or -1). ch 1 (green-only RAW) fills all three channels from it. Returns 0, or -1 (no memory / bad size). */
int sc_profile(const uint16_t *img, int w, int h, int ch, int ccd_x0, int dir, ScProf *pr);
void sc_profile_free(ScProf *pr);

typedef struct {
    double rms_old[3], rms_new[3], noise[3];     /* high-pass rms of the pictures (mean over them), the noise floor (fraction) */
    double ex_old[3], ex_new[3];                 /* above the noise floor: sqrt(rms^2 - noise^2) */
    int nflag;                                   /* specks found (bright places in the corrected picture) */
    int flag_c0[SC_MAXR], flag_c1[SC_MAXR];      /* their CCD ranges incl. SC_MARGIN (at most SC_MAXR kept) */
    double flag_d[SC_MAXR];                      /* their peak (fraction, 5-photosite mean of green) */
    int nfr;                                     /* ranges kept */
    int shift; double corr0, corr;               /* the mapping check: best extra shift (photosites) and correlations */
} ScEval;

/* k pictures (prof[i]) made with the vector vold, and their own white lines (line[i], may be NULL): evaluates vnew.
   The mapping is checked first: the pictures' pattern must follow (white line / vold) at the given photosites - the best
   shift within +-6 photosites is used when it is clearly better (corr > 0.3 and 0.05 above shift 0). Returns 0 or -1. */
int sc_eval(const ScProf *prof, const uint16_t *const *line, int k, const uint16_t *vold, const uint16_t *vnew, ScEval *e);

/* the repair of the found specks (e from sc_eval of vnew): vfix = vnew, and in each found range vnew x (1 + the green
   corrected picture's pattern, at most SC_MAXFIX) in all three colours. Returns the number of photosites changed, or -1. */
int sc_repair(const ScProf *prof, int k, const uint16_t *vold, const uint16_t *vnew, const ScEval *e, uint16_t *vfix);

/* the decision on green: 0 store (not worse, or already at the noise floor), 2 worse than the vector in use (keep it) */
int sc_decide(const ScEval *e);

#endif
