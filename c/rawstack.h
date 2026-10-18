/* rawstack.h - averaging several RAW passes of the same crop (sub-pixel registration, resampling, mean). Pure C, no
 * emulator dependencies (used by app.c, tested by rawstack_test.c). */
#ifndef RAWSTACK_H
#define RAWSTACK_H
#include <stdint.h>

#define RS_MAXPASS 64
typedef struct { int x0, y0, x1, y1; } RsRect;     /* pixels [x0, x1) x [y0, y1) */
typedef struct {
    int w, h, ch, n;          /* picture size, channels (1 or 3), passes added */
    float *acc;               /* sum of the aligned passes, w*h*ch */
    uint16_t *first;          /* pass 1 as delivered (reference for registration and the difference statistics) */
    RsRect cover[RS_MAXPASS]; /* the area each aligned pass covers (edges lost to the shift and the resampling) */
    const uint8_t *regmask;   /* optional (w*h): points the sub-pixel registration may use (HDR: the dark parts), else NULL */
} RawStack;

typedef struct {
    double dx, dy;            /* this pass is shifted by (dx, dy) pixels against pass 1: pass(x+dx, y+dy) = pass1(x, y) */
    double score;             /* normalised cross-correlation at the found shift (1 = identical) */
    int ntiles; double tdx[9], tdy[9], tscore[9];   /* the same, measured separately in a 3 x 3 grid of tiles */
    int masked;               /* 1 = the sub-pixel step used only the points of RawStack.regmask */
    int used;                 /* 0 = left out (did not match pass 1: NCC below 0.5) */
    RsRect cover;             /* where the shifted pass has data */
    int chan;                 /* channel used for the registration (0 R, 1 G, 2 B; 0 for grey) */
    double wscore;            /* when the sub-pixel step could not run: NCC of the whole-pixel shift (-2 = no variance) */
    int weak;                 /* directions without measurable detail in the central window (0..2; -2 score if 2) */
    int resampled;            /* 1 = fractional shift, Lanczos-3 resampling used; 0 = whole-pixel shift, samples copied */
    /* pass-to-pass difference after alignment (this pass - pass 1), per channel, in 4 level bins of pass 1
       (quarters of 0..65535): rms difference and number of samples. With exact alignment and without resampling,
       the noise of one pass is about rms / sqrt(2). */
    double drms[3][4]; long long dn[3][4];
} RsInfo;

/* one channel of an interleaved 16-bit picture (off = -1: the sum of three channels) */
typedef struct { const uint16_t *p; int w, h, stride, off; const uint8_t *mask; } RsImg;   /* mask (w*h, optional): 1 = the sub-pixel step may use this point */

int rs_init(RawStack *s, int w, int h, int ch);           /* 0 = ok, -1 = out of memory */
int rs_add(RawStack *s, const uint16_t *img, RsInfo *info); /* img: w*h*ch interleaved; 0 = ok, -1 = memory, -2 = too many */
void rs_result(const RawStack *s, uint16_t *out);
/* registration of reg against pass 1 and the shifted src written to dst (w*h*ch floats, -1 where not covered); nothing
   is added to the mean. For HDR: reg = the brighter pass scaled to pass 1's brightness, src = that pass as made. */
int rs_align(RawStack *s, const uint16_t *reg, const uint16_t *src, RsInfo *info, float *dst);         /* mean of the passes covering each pixel, rounded, w*h*ch */
void rs_free(RawStack *s);

/* registration of img against ref (same size): (dx, dy) with img(x+dx, y+dy) ~ ref(x, y); returns the NCC score.
   Window: the given rectangle of ref (or the central part if win == NULL); dx, dy in: start (used when *start=1). */
double rs_register(RsImg ref, RsImg img, const RsRect *win, int start, double *dx, double *dy);
double rs_register2(RsImg ref, RsImg img, const RsRect *win, int start, double *dx, double *dy, int *weak);
/* the resampling kernel used here (Lanczos-3, 6 taps for -2..3 around floor(x), fraction f; sum 1, first moment 0) */
void rs_lanczos6(double f, double w[6]);
#endif
