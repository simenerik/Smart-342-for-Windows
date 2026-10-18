/* rawtile.h - putting several RAW blocks (tiles) of one crop together into one picture (registration in the overlaps,
 * per-channel gain match, Lanczos-3 resampling, feathered blending). Pure C, no emulator dependencies (used by app.c,
 * tested by rawtile_test.c). Needed because the scanner stops answering for RAW blocks beyond a certain size (session
 * s9, 2026-10-08): a large RAW is made of several smaller blocks. */
#ifndef RAWTILE_H
#define RAWTILE_H
#include <stdint.h>

typedef struct {
    int W, H, ch;        /* final picture size, channels */
    float *acc, *wsum;   /* weighted sum (W*H*ch) and weights (W*H) */
} RtMosaic;

typedef struct {
    int ovx0, ovy0, ovx1, ovy1, novl;   /* overlap with what was already placed (mosaic pixels), covered samples */
    int registered;      /* 1 = the shift was measured in the overlap; 0 = nominal position (no / too small overlap) */
    double dx, dy;       /* the tile's content is shifted by (dx, dy) pixels against its nominal place: tile(x - x0 + dx, y - y0 + dy) = mosaic(x, y) */
    double score;        /* registration score (NCC after the sub-pixel step; -2 = not measurable, -3 = whole-pixel result used) */
    int at_edge;         /* 1 = the best whole-pixel match lay on the edge of the search range (shift possibly larger) */
    double gain[3];      /* per channel, applied to the tile (mosaic / tile in the overlap); 1 = none */
    int gain_used;       /* 0 = gains outside 0.8 .. 1.25 - not applied */
    double rms_before, rms_after;   /* rms difference in the overlap, nominal place vs registered + gain (16-bit units) */
    double ov_mean;      /* mean level (registration channel) of what is placed, in the overlap */
} RtTileInfo;

int rtm_init(RtMosaic *m, int W, int H, int ch);   /* 0 = ok, -1 = out of memory */
void rtm_free(RtMosaic *m);
/* add tile t (tw x th x ch, interleaved) with its nominal top-left corner (x0, y0) in the picture. feather: width of
   the blending ramp in pixels at the tile's inner edges (edges on the picture border are not feathered). maxshift: the
   whole-pixel search range for the registration. 0 = ok, -1 = out of memory */
int rtm_add(RtMosaic *m, const uint16_t *t, int tw, int th, int x0, int y0, int feather, int maxshift, RtTileInfo *info);
/* the picture, rounded; samples no tile covered are 0. Returns the number of such samples (0 = complete). */
long long rtm_result(const RtMosaic *m, uint16_t *out);

/* tiling plan: n tiles of at most maxw x maxh covering W x H with at least ovl pixels overlap, evenly spaced.
   Returns the number of tiles (0 = impossible), writes up to maxn rectangles (x0, y0, w, h). */
int rt_plan(int W, int H, int maxw, int maxh, int ovl, int (*rects)[4], int maxn);
#endif
