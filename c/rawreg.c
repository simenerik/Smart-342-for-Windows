/* rawreg.c - see rawreg.h. */
#include "rawreg.h"
#include "rawstack.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void lanczos6(double f, double w[6]) {   /* as rawsr.c: Lanczos-3 weights for taps -2..+3 around floor, first moment corrected */
    double s = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f, v;
        if (fabs(t) < 1e-9) v = 1; else if (fabs(t) >= 3) v = 0; else v = 3 * sin(M_PI * t) * sin(M_PI * t / 3) / (M_PI * M_PI * t * t);
        w[i] = v; s += v; }
    for (int i = 0; i < 6; i++) w[i] /= s;
    double m1 = 0, mt = 0, st = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; m1 += w[i] * t; mt += t; }
    mt /= 6; for (int i = 0; i < 6; i++) { double t = (i - 2) - f; st += (t - mt) * t; }
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; w[i] -= m1 / st * (t - mt); }
}

static uint16_t *blur1(const uint16_t *p, int w, int h, int c, double sigma) {
    int r = (int)ceil(3 * sigma); double k[32], ks = 0; if (r > 15) r = 15;
    for (int i = -r; i <= r; i++) { k[i + r] = exp(-0.5 * i * i / (sigma * sigma)); ks += k[i + r]; }
    for (int i = 0; i <= 2 * r; i++) k[i] /= ks;
    float *t = malloc((size_t)w * h * sizeof(float)); uint16_t *o = malloc((size_t)w * h * sizeof(uint16_t));
    if (!t || !o) { free(t); free(o); return NULL; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double a = 0;
        for (int i = -r; i <= r; i++) { int xx = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i; a += k[i + r] * p[((size_t)y * w + xx) * 3 + c]; }
        t[(size_t)y * w + x] = (float)a; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double a = 0;
        for (int i = -r; i <= r; i++) { int yy = y + i < 0 ? 0 : y + i >= h ? h - 1 : y + i; a += k[i + r] * t[(size_t)yy * w + x]; }
        o[(size_t)y * w + x] = (uint16_t)(a < 0 ? 0 : a > 65535 ? 65535 : a + 0.5); }
    free(t); return o;
}

/* one direction (dir 0 = across / dx, 1 = along / dy) of channel k from the band measurements: median, MAD, line */
static void fit_dir(RrInfo *info, int k, int dir, const double *xcs, int nb, int w) {
    double *bv = dir ? info->bdy[k] : info->bdx[k]; int *use = dir ? info->busy[k] : info->busx[k];
    double *a = dir ? &info->a_dy[k] : &info->a_dx[k], *b = dir ? &info->b_dy[k] : &info->b_dx[k];
    *a = *b = 0; int *corr = dir ? &info->corry[k] : &info->corrx[k]; *corr = 0;
    double v[RR_BANDS]; int n = 0;
    for (int i = 0; i < nb; i++) if (use[i]) v[n++] = bv[i];
    if (n < RR_MIN_BANDS) return;
    int measured = n;   /* the bands that agree must be a majority of the measured ones (a few agreeing by chance is not agreement) */
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    double med = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    n = 0; for (int i = 0; i < nb; i++) if (use[i]) { if (fabs(bv[i] - med) > RR_OUTLIER) use[i] = 0; else v[n++] = fabs(bv[i] - med); }
    if (n < RR_MIN_BANDS) return;
    *a = med; *corr = 1;
    for (int it = 0; it < RR_BANDS; it++) {   /* a line across when >= 4 bands agree, else the median */
        double sw = 0, sx = 0, sxx = 0, sv = 0, sxv = 0; int nu = 0;
        for (int i = 0; i < nb; i++) if (use[i]) { double ww = info->bsc[k][i], xc = xcs[i]; sw += ww; sx += ww * xc; sxx += ww * xc * xc; sv += ww * bv[i]; sxv += ww * xc * bv[i]; nu++; }
        if (nu < RR_MIN_BANDS || sw <= 0) { *corr = 0; *a = *b = 0; return; }
        double den = sw * sxx - sx * sx;
        if (nu >= 4 && den > 1e-9) { *b = (sw * sxv - sx * sv) / den; *a = (sv - *b * sx) / sw; if (fabs(*b) * w / 2 > RR_MAX_TILT) { *b = 0; *a = sv / sw; } }
        else { *b = 0; *a = sv / sw; }
        int drop = 0;
        for (int i = 0; i < nb; i++) if (use[i] && fabs(bv[i] - (*a + *b * xcs[i])) > RR_OUTLIER) { use[i] = 0; drop++; }
        if (!drop) break;
    }
    /* agreement: median absolute deviation of the kept bands from the fitted line (a slow change across is real - the
       lens table's offsets change across the CCD - scatter is not) */
    n = 0; for (int i = 0; i < nb; i++) if (use[i]) v[n++] = fabs(bv[i] - (*a + *b * xcs[i]));
    if (n < RR_MIN_BANDS || 2 * n <= measured) { *corr = 0; *a = *b = 0; return; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    double mad = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    if (dir) info->mady[k] = mad; else info->madx[k] = mad;
    if (mad > RR_MAX_MAD) { *corr = 0; *a = *b = 0; }   /* the bands disagree: the colours' pictures differ too much here (colour film) */
}

int rr_measure(const uint16_t *img, int w, int h, RrInfo *info) {
    memset(info, 0, sizeof *info);
    if (!img || w < 32 || h < 32) return -1;
    uint16_t *b[3];
    for (int c = 0; c < 3; c++) { b[c] = blur1(img, w, h, c, 1.0); if (!b[c]) { for (int k = 0; k < c; k++) free(b[k]); return -1; } }
    RsImg G = { b[1], w, h, 1, 0, NULL };
    int nb = w >= RR_BANDS * 40 ? RR_BANDS : w / 40 < 1 ? 1 : w / 40; info->nb = nb;
    int m = 8;                       /* margin kept free of the edges (the shift and the blur) */
    double xcs[RR_BANDS];
    for (int k = 0; k < 2; k++) {
        RsImg C = { b[k ? 2 : 0], w, h, 1, 0, NULL };
        RsRect all = { m, m, w - m, h - m }; double gx = 0, gy = 0; int weak = 0;
        double gs = rs_register2(G, C, &all, 0, &gx, &gy, &weak);
        info->gdx[k] = gx; info->gdy[k] = gy; info->gsc[k] = gs;
        int gok = gs >= RR_MIN_SCORE && !weak && fabs(gx) <= RR_MAX_SHIFT && fabs(gy) <= RR_MAX_SHIFT;
        for (int i = 0; i < nb; i++) {
            int x0 = m + (w - 2 * m) * i / nb, x1 = m + (w - 2 * m) * (i + 1) / nb; xcs[i] = (x0 + x1) / 2.0 - w / 2.0;
            RsRect r = { x0, m, x1, h - m }; double bx = gx, by = gy; int bw = 0;
            double sc = rs_register2(G, C, &r, gok, &bx, &by, &bw);
            info->bdx[k][i] = bx; info->bdy[k][i] = by; info->bsc[k][i] = sc;
            int u = sc >= RR_MIN_SCORE && !bw && fabs(bx) <= RR_MAX_SHIFT && fabs(by) <= RR_MAX_SHIFT;
            info->busx[k][i] = info->busy[k][i] = u;
        }
        fit_dir(info, k, 0, xcs, nb, w); fit_dir(info, k, 1, xcs, nb, w);
        if (nb < RR_MIN_BANDS && gok) {   /* too narrow for bands to agree (under 120 px): the whole picture's shift, as 0.1.22 */
            info->a_dx[k] = gx; info->a_dy[k] = gy; info->b_dx[k] = info->b_dy[k] = 0; info->corrx[k] = info->corry[k] = 1; info->whole[k] = 1; }
        info->ok[k] = info->corrx[k] || info->corry[k];
    }
    for (int c = 0; c < 3; c++) free(b[c]);
    return 0;
}

int rr_moves(const RrInfo *info, int k, int w) {
    if (!info->ok[k]) return 0;
    double h = w / 2.0, mx = fmax(fabs(info->a_dx[k] - info->b_dx[k] * h), fabs(info->a_dx[k] + info->b_dx[k] * h)), my = fmax(fabs(info->a_dy[k] - info->b_dy[k] * h), fabs(info->a_dy[k] + info->b_dy[k] * h));
    return mx >= RR_MIN_MOVE || my >= RR_MIN_MOVE;
}
int rr_apply(const uint16_t *img, int w, int h, const RrInfo *info, uint16_t *out) {
    if (!img || !out || w < 1 || h < 1) return -1;
    memcpy(out, img, (size_t)w * h * 3 * sizeof(uint16_t));
    double (*wx)[6] = malloc(sizeof(double[6]) * w), (*wy)[6] = malloc(sizeof(double[6]) * w);
    int *ix = malloc(sizeof(int) * w), *iy = malloc(sizeof(int) * w);
    if (!wx || !wy || !ix || !iy) { free(wx); free(wy); free(ix); free(iy); return -2; }
    for (int k = 0; k < 2; k++) {
        if (!rr_moves(info, k, w)) continue;
        int c = k ? 2 : 0;
        for (int x = 0; x < w; x++) {   /* per column: c(x + dx, y + dy) */
            double xc = x - w / 2.0, dx = info->a_dx[k] + info->b_dx[k] * xc, dy = info->a_dy[k] + info->b_dy[k] * xc;
            double sx = x + dx, sy = dy; int fx = (int)floor(sx), fy = (int)floor(sy);
            lanczos6(sx - fx, wx[x]); lanczos6(sy - fy, wy[x]); ix[x] = fx - 2; iy[x] = fy - 2;
        }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            double a = 0;
            for (int j = 0; j < 6; j++) {
                int yy = y + iy[x] + j; yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy;
                const uint16_t *row = img + (size_t)yy * w * 3; double r = 0;
                for (int i = 0; i < 6; i++) { int xx = ix[x] + i; xx = xx < 0 ? 0 : xx >= w ? w - 1 : xx; r += wx[x][i] * row[(size_t)xx * 3 + c]; }
                a += wy[x][j] * r;
            }
            out[((size_t)y * w + x) * 3 + c] = (uint16_t)(a < 0 ? 0 : a > 65535 ? 65535 : a + 0.5);
        }
    }
    free(wx); free(wy); free(ix); free(iy);
    return 0;
}
