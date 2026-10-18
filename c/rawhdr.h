/* rawhdr.h - merging a normal RAW pass with a longer-exposed one (experimental HDR). Pure C (app.c, rawhdr_test.c). */
#ifndef RAWHDR_H
#define RAWHDR_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    double g[3], o[3];        /* fitted per channel: brighter = g * normal + o (16-bit units), over unclipped pixels */
    double fit_rms[3];        /* rms of the fit's residual */
    long long nfit[3];        /* 8 x 8 blocks used for the fit */
    double clip;              /* level above which the brighter pass is not used (16-bit) */
    double frac_bright, frac_blend, frac_normal;   /* share of pixels taken from the brighter pass, blended, normal */
    long long spill_px;       /* 0.4.1: pixels (incl. 2 around) where the brighter pass was brighter than the normal pass allows - normal pass used; -1 = the check could not run */
    double spill_frac;        /* the same as a share of the pixels that would have used the brighter pass */
} HdrInfo;

/* n: normal pass (w*h*ch, 16-bit), b: brighter pass already aligned to n (floats, -1 = no data there).
   out: merged picture in the normal pass' scale. Returns 0 = merged; -1 = too few pixels to fit; -2 = the brighter pass
   is not brighter (g < 1.2 in every channel: the longer exposure did not happen) - out is then a copy of n. */
int hdr_merge(const uint16_t *n, const float *b, int w, int h, int ch, uint16_t *out, HdrInfo *hi);
/* rough brightness ratio of the brighter pass b (not aligned yet) to n, from the sums over pixels well inside the range
   of both - only to scale a copy of b for the registration; 0 when there are too few such pixels */
double hdr_rough_gain(const uint16_t *n, const uint16_t *b, size_t nsamples);
/* align the brighter pass b to n: for the registration both go through the same smooth compression (the brighter
   pass clips earlier; its clipped highlights must not pull the sub-pixel result - found in testing), b scaled by the
   rough gain; then b itself is shifted into dst (floats, -1 = no data). info: the registration (see rawstack.h).
   Returns 0, -1 = memory, -3 = no rough gain (too few usable pixels), -4 = did not match (left out). */
struct RsInfoTag;
int hdr_align(const uint16_t *n, const uint16_t *b, int w, int h, int ch, float *dst, void *rsinfo, double *gain0);
/* 0.4.1: hdr_align now also measures the shift again in bands along the scan (rows) for each channel and resamples the
   brighter pass with a shift interpolated per row and channel (see rawhdr.c). hdr_align2 does the same and reports it
   in la; with la == NULL it uses the whole-picture shift only (as 0.4.0's hdr_align). */
#define HDR_MAXBANDS 16
typedef struct {
    int local;                 /* 1 = the per-band, per-channel shift was used; 0 = the whole-picture shift */
    int why;                   /* when not: 1 picture too small, 2 too few measurable bands, 3 memory */
    int nbands; double band_y[HDR_MAXBANDS];
    double gdx, gdy;           /* the whole-picture shift */
    int nvalid[3], borrowed[3];  /* bands measured per channel; 1 = too few, the best channel's profile used */
    double bscore[3][HDR_MAXBANDS], bdx[3][HDR_MAXBANDS], bdy[3][HDR_MAXBANDS];   /* per band (after filling and the median) */
    double dxmin[3], dxmax[3], dymin[3], dymax[3];
} HdrAlignInfo;
int hdr_align2(const uint16_t *n, const uint16_t *b, int w, int h, int ch, float *dst, void *rsinfo, double *gain0, HdrAlignInfo *la);
#endif
