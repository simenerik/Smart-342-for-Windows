/* negconv.c - 0.4.0: negatives -> a positive (see negconv.h) */
#include "negconv.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

void nc_defaults(NcParams *p) { p->gamma = 0.6; p->plo = 0.05; p->phi = 99.95; p->margin = 0.02; p->out_lo = 0.02; p->out_hi = 0.98; }

static int key(int ch) { return ch == 3 ? 1 : 0; }   /* the channel that tells holder from film */

int nc_area(const uint16_t *img, int w, int h, int ch, int *x0, int *y0, int *x1, int *y1) {
    if (!img || w < 1 || h < 1 || (ch != 1 && ch != 3)) return -1;
    int k = key(ch), a0 = 0, a1 = w - 1, b0 = 0, b1 = h - 1;
    /* twice: rows within the columns found, then columns within the rows found */
    for (int pass = 0; pass < 2; pass++) {
        /* rows from the top / bottom while more than half of their pixels (within a0..a1) are holder */
        while (b0 <= b1) { int n = 0; for (int x = a0; x <= a1; x++) n += img[((size_t)b0 * w + x) * ch + k] < NC_HOLDER; if (2 * n > a1 - a0 + 1) b0++; else break; }
        while (b1 >= b0) { int n = 0; for (int x = a0; x <= a1; x++) n += img[((size_t)b1 * w + x) * ch + k] < NC_HOLDER; if (2 * n > a1 - a0 + 1) b1--; else break; }
        if (b1 < b0) return -1;
        while (a0 <= a1) { int n = 0; for (int y = b0; y <= b1; y++) n += img[((size_t)y * w + a0) * ch + k] < NC_HOLDER; if (2 * n > b1 - b0 + 1) a0++; else break; }
        while (a1 >= a0) { int n = 0; for (int y = b0; y <= b1; y++) n += img[((size_t)y * w + a1) * ch + k] < NC_HOLDER; if (2 * n > b1 - b0 + 1) a1--; else break; }
        if (a1 < a0) return -1;
    }
    *x0 = a0; *x1 = a1; *y0 = b0; *y1 = b1; return 0;
}

static double dens(double v) { return log10(65535.0 / (v < 1 ? 1 : v)); }

int nc_levels(const uint16_t *img, int w, int h, int ch, int x0, int y0, int x1, int y1, const NcParams *p, NcLevels *lv) {
    memset(lv, 0, sizeof *lv);
    if (!img || (ch != 1 && ch != 3) || w < 1 || h < 1) return -1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > w - 1) x1 = w - 1; if (y1 > h - 1) y1 = h - 1;
    if (x1 < x0 || y1 < y0) return -1;
    int mx = (int)((x1 - x0 + 1) * p->margin), my = (int)((y1 - y0 + 1) * p->margin);
    x0 += mx; x1 -= mx; y0 += my; y1 -= my;
    if (x1 < x0 || y1 < y0) return -1;
    uint32_t *hist = calloc((size_t)3 * 65536, sizeof(uint32_t)); if (!hist) return -1;
    int k = key(ch); double clear = ch == 3 ? NC_WHITE / 2 : 0.85 * NC_WHITE; long n = 0;
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) {
        const uint16_t *q = img + ((size_t)y * w + x) * ch;
        if (q[k] < NC_OPAQUE || q[k] > clear) continue;
        for (int c = 0; c < ch; c++) hist[c * 65536 + q[c]]++;
        n++;
    }
    lv->ch = ch; lv->n = n;
    if (n < 1000) { free(hist); return -1; }
    for (int c = 0; c < ch; c++) {
        /* density falls as the value rises: the low density percentile is the high value one */
        long lim_hi = (long)ceil(n * (1 - p->plo / 100)), lim_lo = (long)ceil(n * (1 - p->phi / 100)); if (lim_lo < 1) lim_lo = 1;
        long acc = 0; int vlo = 0, vhi = 65535, got_lo = 0;
        for (int v = 0; v < 65536; v++) { acc += hist[c * 65536 + v];
            if (!got_lo && acc >= lim_lo) { vlo = v; got_lo = 1; }
            if (acc >= lim_hi) { vhi = v; break; } }
        lv->lo[c] = dens(vhi); lv->hi[c] = dens(vlo);
    }
    free(hist);
    int g = ch == 3 ? 1 : 0;
    for (int c = 0; c < ch; c++) if (!(lv->hi[c] - lv->lo[c] > 0.05)) return -1;
    lv->range = (lv->hi[g] - lv->lo[g]) / (p->gamma > 0.1 ? p->gamma : 0.6);
    lv->ok = 1; return 0;
}

int nc_convert(const uint16_t *img, size_t npix, int ch, const NcLevels *lv, const NcParams *p, uint16_t *out) {
    if (!lv || !lv->ok || lv->ch != ch || (ch != 1 && ch != 3)) return -1;
    uint16_t *lut = malloc((size_t)ch * 65536 * sizeof(uint16_t)); if (!lut) return -1;
    double R = lv->range, blk = pow(10, -R);
    for (int c = 0; c < ch; c++) {
        double lo = lv->lo[c], d = lv->hi[c] - lv->lo[c];
        for (int v = 0; v < 65536; v++) {
            double t = (dens(v) - lo) / d, lin = (pow(10, (t - 1) * R) - blk) / (1 - blk);
            if (lin < 0) lin = 0;
            double e = p->out_lo + (p->out_hi - p->out_lo) * pow(lin, 1 / 2.2);
            if (e < 0) e = 0; if (e > 1) e = 1;
            lut[c * 65536 + v] = (uint16_t)(e * 65535 + 0.5);
        }
    }
    for (size_t i = 0; i < npix; i++) for (int c = 0; c < ch; c++) out[i * ch + c] = lut[c * 65536 + img[i * ch + c]];
    free(lut); return 0;
}

uint16_t *nc_rotate(const uint16_t *img, int w, int h, int ch, int k, int *ow, int *oh) {
    k &= 3; int W = (k & 1) ? h : w, H = (k & 1) ? w : h;
    uint16_t *o = malloc((size_t)w * h * ch * sizeof(uint16_t)); if (!o) return NULL;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int X, Y;
        switch (k) { case 0: X = x; Y = y; break;
                     case 1: X = h - 1 - y; Y = x; break;          /* 90 clockwise: the left column becomes the top row */
                     case 2: X = w - 1 - x; Y = h - 1 - y; break;
                     default: X = y; Y = w - 1 - x; break; }      /* 90 counter-clockwise */
        memcpy(o + ((size_t)Y * W + X) * ch, img + ((size_t)y * w + x) * ch, ch * sizeof(uint16_t));
    }
    *ow = W; *oh = H; return o;
}
