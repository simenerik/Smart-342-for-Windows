/* rawsharp.h - RAW sharpening: restores the contrast the scanner's optics and sensor take away, from a model of the
 * system response on the film plane (2026-10-09; reference: scripts/sharpness/wiener_proto.py, checked against it in
 * rawsharp_test.c).
 *
 * Model M(fx, fy) (fx across, fy along, cycles per um on the film):
 *   diffraction of a circular pupil, object-side working f-number N_obj = N (1 + m) / m, wavelength per channel
 *     (R 0.61, G 0.54, B 0.46 um);
 *   x the CCD cell's aperture: box of 7 um / m on the film in both directions (cell 0.007 mm: the scanner's flash,
 *     "0.007 ccd cell size"; m from the optical sampling: m = 0.007 mm x optical dpi / 25.4 mm);
 *   x the carriage moving during each line: box of one line pitch along;
 *   x a residual Gaussian: 0.1.22 sigma 1.83 um at the CCD both ways, fitted to the 35 mm lens's GRAIN response (s19) -
 *     which the Scitex focus chart (s21 / s23 / s24, 10 Oct) showed to be 2 - 2.5x too optimistic; 0.1.24: 1.693 um
 *     across / 3.573 um along at the CCD and a halo ((1 - hx) + hx g(fx)) ((1 - hy) + hy g(fy)), hx 0.458, hy 0.198,
 *     g a Gaussian of 7.66 um at the CCD - fitted to the chart's bars (scripts/sharpness/fit_chart.py, rms 0.012; the
 *     carriage-motion box checked: the 1x and 2x along scans fit with the motion = the line pitch x 1.02; along agrees
 *     with the s19 sprocket-hole edge);
 *   x, when the across samples are the scanner's interpolation of the CCD samples (a resolution other than the
 *     optical one), the response of its cubic kernel (Keys, a = -0.5 for steps < 1, a = -1 otherwise - driver
 *     piecewise_cubic 0x03cd50).
 * Restoration: Wiener W = M / (M^2 + K), normalised to 1 at zero frequency, as a 2-D FIR kernel (frequency sampling on
 * a 128 x 128 grid, centre 15 x 15 taps, Hann window, sum 1). K sets the strength (largest gain ~ 1 / (2 sqrt K)).
 * Only the 35 mm lens (L110) has been measured; the others use the same model with their f-number and magnification.
 */
#ifndef RAWSHARP_H
#define RAWSHARP_H
#include <stdint.h>

#define RSH_TAPS 15

typedef struct {
    double fnum;          /* lens f-number (L67 4.9, L89 5.0, L110 5.0, L60 5.6) */
    double mag;           /* lens magnification (CCD / film) */
    double cell_um;       /* CCD cell pitch (7.0) */
    double pitch_x_um;    /* sample pitch of the data across, on the film */
    double pitch_y_um;    /* along */
    double motion_um;     /* carriage movement during one line (the along pitch for a stepped scan) */
    double sigma_ccd_um;  /* residual Gaussian at the CCD (1.83) - both axes, when sigma_x/y_ccd_um are 0 */
    double interp_x;      /* 0: across samples are CCD samples (or pixel shift); else the scanner's interpolation step
                             (input CCD pixels per output pixel, e.g. 0.5 for 2x) */
    double K;             /* Wiener constant */
    /* 0.1.24 (fitted to the Scitex focus chart, s21 / s23 / s24): per-axis residual Gaussian and a halo
       ((1 - halo_x) + halo_x g(fx)) ((1 - halo_y) + halo_y g(fy)), g = Gaussian of halo_sigma_ccd_um; all 0 = the 0.1.22 model */
    double sigma_x_ccd_um, sigma_y_ccd_um, halo_x, halo_y, halo_sigma_ccd_um;
} RshModel;

typedef struct {
    double kern[3][RSH_TAPS * RSH_TAPS];
    double centre[3];         /* centre tap per channel */
    double noise_gain[3];     /* sqrt(sum of squared taps): gain for white noise */
    double resp_y[3][8], resp_x[3][8];   /* system response after restoration at 10..80 lp/mm (G used in logs) */
    double model_y[3][8], model_x[3][8]; /* before */
    double f_rep[4];                     /* lp/mm on the film: 0.4 / 0.6 / 0.8 / 1.0 x the CCD's Nyquist (half the optical sampling) */
    double rep_model_y[4], rep_resp_y[4], rep_model_x[4], rep_resp_x[4];   /* G at those, before / after */
} RshInfo;

extern const double RSH_LAMBDA[3];
double rsh_model(const RshModel *m, int ch, double fx, double fy);           /* M at (fx, fy), cycles / um */
int rsh_design(const RshModel *m, RshInfo *info);                          /* 0 ok */
/* img: w x h x ch (1 or 3) 16-bit; out may not alias img. Edges: nearest sample repeated. */
int rsh_apply(const RshInfo *info, const uint16_t *img, int w, int h, int ch, uint16_t *out);
/* the lens's f-number by lens index (0 L67, 1 L89, 2 L110, 3 L60); 0 if unknown */
double rsh_lens_fnum(int lens);
#endif
