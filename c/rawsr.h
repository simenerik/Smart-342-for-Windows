/* rawsr.h - pixel shift across: several RAW passes of the same crop, each with the picture moved across by a different
 * fraction of a CCD cell (lens carriage steps), combined on a grid F times finer across. Pure C, no emulator
 * dependencies (tested by rawsr_test.c). Along the scan nothing is refined (the carriage already steps there); each
 * pass is only aligned along. No sharpening: the result is the cells' samples placed where they were taken, so it
 * keeps the RAW principle; the finer sampling is what pixel shift adds. */
#ifndef RAWSR_H
#define RAWSR_H
#include <stdint.h>

typedef struct {
    int n;                 /* passes used */
    double dx[16], dy[16]; /* measured offsets of each pass against pass 1 (pass(x + dx, y + dy) = pass1(x, y)), pixels */
    double phase[16];      /* fractional part of -dx: where the pass samples between pass 1's cells (0 .. 1) */
    double maxgap;         /* largest gap between neighbouring sample phases (in cells) - for the log */
    double griddist;       /* for the F given to sr_measure: the largest distance (in fine columns) from a fine column to
                              the nearest pass's samples - what sr_combine's nearest-column placement sees. A pass at
                              phase p puts all its samples at fine positions = F x p (mod F), so fine column class r
                              (0 .. F-1) is covered at distance min over passes of the circular distance |F p - r|. 0 =
                              every column hit exactly; 0.5 = some columns get no sample (weight (1 - 2d)^2 = 0): only
                              linear filling. Found in review: phases 0, .25, .75 have maxgap 0.51 but griddist 0.5. */
    double score[16];      /* registration scores */
    int used[16];          /* 0.1.23: 1 = the pass's samples were placed (within SR_NEAR of the best pass for its columns) */
    double gain[16][3];    /* brightness factor applied to each pass and channel (pass 1 = 1): the lens carriage
                              steps change the light a little (hardware s11: 98.9 % at +2 steps, 97.5 % at +4) - unmatched,
                              neighbouring fine columns from different passes would differ: a stripe pattern */
} SrInfo;

/* passes: n pictures w x h x ch (interleaved); F: refinement across (2 .. 4). dx_hint (optional, n values, [0]
   unused): the expected offset across of each pass (steps x pixels per step); the registration then starts there and
   a result more than 1.5 pixels away is refused (score -4). out: (w*F) x h x ch; the common area only is fully
   supported - columns outside it come from fewer passes. Returns 0 ok, -1 memory/arguments, -2 registration failed
   for some pass (info->score < 0.5). */
int sr_combine(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, uint16_t *out, SrInfo *info);
/* step 1 of sr_combine alone: the offsets, phases and largest phase gap of the passes so far (the window uses it after
   each pass to decide whether another lens step is needed). Same returns. */
int sr_measure(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, SrInfo *info);
#define SR_NEAR 0.05         /* fine columns: passes farther than this beyond the nearest one feeding the same columns are left out */
#define SR_GRIDDIST_OK 0.35   /* fine columns at most this far (in fine columns) from a sample: weight >= 0.09 - covered */
/* griddist of a set of phases (0 .. 1) on the F-times-finer grid (as sr_measure computes it) */
double sr_griddist(const double *phase, int n, int F);
/* the next lens step (0.1.22; reworked 0.1.23): steps[0..n-1] taken so far, dx[0..n-1] their measured offsets across
   (dx[0] = 0). The step-to-picture relation is not linear and changes between runs (hardware: 35 mm lens +1 step
   -0.71 / -0.89 / -0.99 px in three runs; 6x6 lens first step 0.44, the next ones ~1.15).
   - grid not yet covered (griddist > SR_STEP_TARGET): explore - the offset of a further step is predicted from the
     local slope of the two largest steps, and the smallest step in (last, last + SR_STEP_RANGE] whose predicted phase
     gives griddist <= SR_STEP_TARGET is taken, else the one with the smallest predicted griddist.
   - covered: the step 0 .. min(largest + SR_STEP_RANGE, SR_STEP_MAX) with the lowest EXPECTED sr_grid_cost - the cost
     averaged over the uncertainty of the predicted position (a step taken before: SR_REPEAT_SIGMA; between measured
     steps or beyond them: SR_STEP_SIGMA per step of distance to the nearest measured step). So a far extrapolation,
     whose phase cannot be predicted, is not preferred over a near one on a guess (review 0.1.23: on the 6x6 lens's
     curve the straight-line guess chose +6, really 4.2 px further than predicted). A repeat is allowed.
   Candidates whose predicted offset exceeds max_off px (> 0; the registration's search range) are not taken; no step
   above SR_STEP_MAX. Returns the step, or -1 when no slope can be measured (|slope| < 0.05 px per step: the carriage
   does not move the picture) or no candidate is allowed; pred_gd / pred_phase (optional): the prediction. */
#define SR_STEP_RANGE 6
#define SR_STEP_TARGET 0.2    /* predicted griddist good enough to take the smallest such step (exploring) */
#define SR_STEP_MAX 24        /* steps 0 .. 24: with the 8-step approach from above the carriage stays within +32 */
#define SR_NOISE_W 0.01       /* the weight of noise against position error (fine columns^2): one sample's noise counts
                                 like a 0.1-column position error */
#define SR_STEP_SIGMA 0.35    /* px of unpredictability per step away from a measured step (6x6 lens: 0.44 then 1.15) */
#define SR_REPEAT_SIGMA 0.1   /* px, a step taken before (its position is measured; repeat error) */
int sr_next_step(const double *dx, const int *steps, int n, int F, double *pred_gd, double *pred_phase);
/* the expected error of the finer columns for passes at these sample positions (cells; = -dx): per class bias^2 +
   SR_NOISE_W / effective samples, with sr_combine's choice of passes */
double sr_grid_cost(const double *pos, int n, int F);
/* the same; *maxbias = the largest position error (|weighted mean offset|, fine columns) of a column class (0.5: a class
   without samples) */
double sr_grid_cost2(const double *pos, int n, int F, double *maxbias);
#define SR_BIAS_OK 0.1         /* normal setting: combine once every column class is within this (fine columns) */
/* balance: unused (kept for the callers; both settings choose the same way since 0.1.23) */
int sr_next_step2(const double *dx, const int *steps, int n, int F, int balance, double *pred_gd, double *pred_phase);
int sr_next_step3(const double *dx, const int *steps, int n, int F, int balance, double max_off, double *pred_gd, double *pred_phase);
/* 0.1.24: the same with a start for every pass along as well (dy_hint; NULL = as sr_measure); with both hints each pass
   is only refined from its start (no whole-pixel search) and refused (score -4) when it moves more than 1.5 px. */
int sr_measure2(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, const double *dy_hint, SrInfo *info);
int sr_combine2(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, const double *dy_hint, uint16_t *out, SrInfo *info);
/* one pass against pass 1 as sr_measure registers it (green, blurred); start: 0 = full search, 1 = refine from *dx, *dy
   only. Returns the match score (< 0.5: no match; -1 memory). */
double sr_register1(const uint16_t *p0, const uint16_t *pk, int w, int h, int ch, int start, double *dx, double *dy);
/* 0.1.24: is a newly measured offset (mx, my) of a pass at lens step `step` plausible, given the n passes accepted so far
   (dx/dy/steps[0..n-1], pass 1 = 0/0)? Hardware 10 Oct: periodic bars made the registration lock whole bar periods off
   (s22: +3.458 / +27.861 px for -2.84 / -3.64; s26: -52.6 lines along for ~-2.2), and such passes were combined.
   Along: within along_tol of the median of the accepted passes (scan start repeats within ~4 lines at 5324 dpi).
   Across, relative to the accepted steps: the same step - within SR_CHECK_SLACK of their mean; between two measured
   steps - between their offsets (+- slack); beyond them - SR_PXSTEP_MIN .. SR_PXSTEP_MAX px per step from the nearest
   (+- slack; the first step from rest can be short: 0.44 - 0.99 px, later ones up to 1.73 px), in the direction `sign`
   (-1 / +1: the RAW's orientation; 0 = unknown, both) - and, when the local step is known from two measured steps,
   within SR_CHECK_SLACK + SR_CHECK_PERSTEP x |D| of its extrapolation (review: the band alone admits one-period aliases
   on long jumps). Slack covers the carriage's slips (up to ~1.1 px, s25) and the
   warm-up drift (<= 0.3 px). Returns 1 plausible; ex[0 .. *nex-1] (1 or 2) and *ey: where to re-measure from. */
#define SR_PXSTEP_MIN 0.3
#define SR_PXSTEP_MAX 2.2
#define SR_PXSTEP_TYP 1.25   /* a step from above, 35 mm lens (s21 / s22 / s25: 1.20 - 1.27) - only a start for re-measuring */
#define SR_CHECK_SLACK 1.5
#define SR_CHECK_PERSTEP 0.75 /* beyond the measured steps, with the local step known: allowed distance from its extrapolation per step */
int sr_check_offset(const double *dx, const double *dy, const int *steps, int n, int step, int sign, double mx, double my,
                    double along_tol, double ex[2], int *nex, double *ey);
/* 0.1.24: the whole check for a newly made pass (what the window does): register it against pass 1 (full search); if
   the result is not plausible (sr_check_offset) or does not match, refine from each expected place and take a
   plausible result with the best score. Returns 1 accepted (*how 0 = as first measured, 1 = re-measured near the
   expected place), 0 refused (no plausible match: leave the pass out), -1 memory. *dx, *dy: the accepted offset, or the
   first measurement when refused; *fdx, *fdy, *fsc (optional): the first (full-search) measurement and its match. */
int sr_accept_pass(const uint16_t *p0, const uint16_t *pk, int w, int h, int ch, const double *adx, const double *ady,
                   const int *asteps, int n, int step, int sign, double along_tol, double *dx, double *dy, double *score,
                   int *how, double *fdx, double *fdy, double *fsc);
/* 0.1.25 (lens shift test): a straight line x = a + slope * k through n positions (n >= 3), and the best single jump
   (the positions from index j on offset by *jump - the carriage landing about a motor step off and staying there,
   hardware s21 / s25 / s27). Returns 0 ok. *rms, *dev: scatter about the line (rms over n - 2, largest); *jpos (1 ..
   n-1), *jump, *slope_j, *rms_j: the best one-jump fit (n - 3 dof; *jpos 0 when n < 4). */
int sr_fit_series(const double *x, const int *k, int n, double *slope, double *rms, double *dev,
                  int *jpos, double *jump, double *slope_j, double *rms_j);
/* 0.3.0 (Set up this scanner): the lens-step series of one lens - n pictures in the order made, lens step k[i], measured
   offset across dx[i] / along dy[i] against picture 1. Fit dx = a + slope x k + drift x i (least squares, i = picture
   index: the slow drift of the picture between scans seen on the hardware, s21 / s22 / s32); rms / maxdev: scatter
   about the fit (rms over n - 3); first: the 0 -> +1 step as a fraction of the slope (1 = a full step; straight
   approaches gave 0.1 - 0.6, from above ~1); repeat: the largest difference between two pictures at the same step after
   the fit (repeatability); along: max - min of dy. ok: n >= 6, 0.3 <= |slope| <= 2.5 px and rms <= 0.35 x |slope|. */
typedef struct { int n, ok; double slope, a, drift, rms, maxdev, first, repeat, along, slope_nd, rms_nd; } SrCalib;
/* 0.3.0: the step chooser's predictor. on = 0: as before (pred_offset); on = 1: the ridge model (rawsr.c model_fit)
   with prior = the expected slope in px per step, signed as dx (0 = none: from Set up this scanner, else the thin-lens
   estimate); sigma0 / sigma_step: the uncertainty of a prediction at d steps from a measured one = sqrt(sigma0^2 +
   (sigma_step d)^2) (0 = 0.3 / 0.1); the plausibility check starts from |prior| and allows up to 1.5 x it. */
typedef struct { int on; double prior, sigma0, sigma_step; } SrPred;
extern SrPred sr_pred;
double sr_model_pred(const double *dx, const int *steps, int n, int k, double *slope);   /* NAN when not solvable */
int sr_calib_fit(const double *dx, const double *dy, const int *k, int n, SrCalib *c);
#endif
