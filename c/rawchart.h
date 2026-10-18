/* rawchart.h - 0.3.0: the scanner's sharpness from the Scitex focus chart (Set up this scanner, optional step).
 * A RAW of the chart's two bar blocks (bars varying across, bars varying along; 8.3 lp/mm on the official chart) at the
 * lens's own resolution: the blocks are found in the picture, each block's square-wave harmonics are measured against
 * an ideal square wave of its duty cycle (scripts/sharpness/barmtf.py: the bars' slight tilt gives each line its own
 * phase, all samples are folded onto one period), and the sharpening model's residual blur is fitted to them
 * (scripts/sharpness/fit_chart.py, the per-axis halo variant the program uses since 0.1.24). */
#ifndef RAWCHART_H
#define RAWCHART_H
#include <stdint.h>

typedef struct { int x0, y0, x1, y1, ok; double score; } ChBox;   /* pixels, [x0, x1) x [y0, y1) */
/* the blocks of bars varying across (*bx) and along (*by) in a RAW (green channel; ch 1 or 3); returns how many found */
int chart_find(const uint16_t *img, int w, int h, int ch, ChBox *bx, ChBox *by);

#define CH_MAXH 12
typedef struct {
    int ok, nh;
    double f0;              /* fundamental, lp/mm on the film */
    double period_px, tilt, duty, michelson;
    double f[CH_MAXH], v[CH_MAXH];   /* harmonics n = 2.. (skipped where the square wave has almost none): lp/mm and
                                        contrast relative to the fundamental, against the ideal square wave */
} ChBars;
/* one block: axis 0 = bars vary across (along x), 1 = along (y); px_mm = the pixel pitch in mm along that axis */
int chart_bars(const uint16_t *img, int w, int h, int ch, const ChBox *b, int axis, double px_mm, ChBars *out);

/* the model fit: residual Gaussian at the CCD across / along (um), halo fractions across / along and its width (um at
   the CCD). mag = magnification, fnum = lens f-number, motion_um = carriage movement during a line on the film (0 with
   step-and-hold), lam_um = wavelength (green 0.54). Returns 0 ok; rms = residual of the fit. */
typedef struct { double sx, sy, ax, ay, sh, rms, maxres; int npts; } ChFit;
int chart_fit(const ChBars *across, const ChBars *along, double mag, double fnum, double motion_um, double lam_um, ChFit *fit);
#endif
