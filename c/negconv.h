/* negconv.h - 0.4.0: colour (C-41) and black-and-white negatives -> a positive (pure C; unit test negconv_test on the
 * hardware data of 12 Oct, docs/re-progress.md s38 / s39).
 *
 * Works in density: D = log10(65535 / v) of the RAW's linear 16-bit values (the preview's samples are brought to the same
 * scale: 14-bit x 4, s39 RAW / preview 1.001). Per channel the levels lo / hi are the 0.05 / 99.95 % points of D over the
 * picture (holder and clear film left out, a margin at the edges): lo is the film's thinnest part (the scene's darkest,
 * or the unexposed base), hi its densest (the scene's brightest). Each channel is mapped on its own (t = (D - lo) / (hi -
 * lo): the orange mask and the layers' different contrasts are taken out, blacks and whites come out neutral), then to
 * light with one scene range R = (hi - lo of green) / film gamma: linear = 10^((t - 1) R), black at 10^-R removed, encoded
 * with gamma 2.2 into out_lo .. out_hi of the 16-bit range (room left for an editor). The film base itself cancels out.
 * The prototype (scripts/neg/negconv.py) on s39's frame: natural skin, neutral background. Levels can be kept from one
 * frame for a whole roll (the same lo / hi applied to every frame). */
#ifndef NEGCONV_H
#define NEGCONV_H
#include <stdint.h>
#include <stddef.h>

#define NC_WHITE 50000.0     /* an empty frame in the RAW's 16-bit scale (hardware s37b: 49400 - 55400) */
#define NC_OPAQUE 40.0       /* below this a pixel is holder, not film (s39: holder G 8, the densest image G 650) */
#define NC_HOLDER 120.0      /* a row / column with most pixels below this (green) is holder */

typedef struct {
    double gamma;            /* the film's gamma (contrast of the negative): 0.6 normal, 0.7 softer result, 0.5 harder */
    double plo, phi;         /* percentiles of the density for the levels (0.05, 99.95) */
    double margin;           /* fraction of the box left out at each edge (0.02) */
    double out_lo, out_hi;   /* the output range (0.02, 0.98) */
} NcParams;

typedef struct {
    int ch;                  /* 1 or 3 */
    double lo[3], hi[3];     /* density levels per channel */
    double range;            /* the scene's log10 range R */
    long n;                  /* pixels the levels were taken from */
    int ok;
} NcLevels;

void nc_defaults(NcParams *p);
/* the image area: rows and columns at the edges that are mostly holder (green / the channel below NC_HOLDER) are left
   out. Box inclusive. Returns 0, or -1 when nothing but holder is found */
int nc_area(const uint16_t *img, int w, int h, int ch, int *x0, int *y0, int *x1, int *y1);
/* the levels from the box less the margin, leaving out opaque pixels (holder) and clear ones (no film: green above
   NC_WHITE / 2 for colour, the channel above 0.85 x NC_WHITE for black and white). Returns 0, or -1 (too few pixels,
   no density range) */
int nc_levels(const uint16_t *img, int w, int h, int ch, int x0, int y0, int x1, int y1, const NcParams *p, NcLevels *lv);
/* the positive of npix pixels (ch interleaved) -> out (16-bit, gamma 2.2 encoded). Returns 0 or -1 */
int nc_convert(const uint16_t *img, size_t npix, int ch, const NcLevels *lv, const NcParams *p, uint16_t *out);
/* rotated by k x 90 degrees clockwise (k 0..3): a new buffer (malloc), its size in *ow x *oh; NULL without memory */
uint16_t *nc_rotate(const uint16_t *img, int w, int h, int ch, int k, int *ow, int *oh);
#endif
