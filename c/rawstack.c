/* rawstack.c - averaging several RAW passes of the same crop (see rawstack.h).
 *
 * Registration of pass k against pass 1 (on the channel with the most detail in the centre of pass 1):
 *   1. whole pixels: normalised cross-correlation (NCC) on a pyramid of the central part (up to 2048 x 2048 pixels,
 *      halved while both sides stay >= 96): every shift within +-(a third of the top level's shorter side, at most 48)
 *      at the top level (+-672 pixels at full size for a 2048 crop), the 8 best local maxima and the centre are
 *      each followed down (+-2 per level) and the full-size mean squared difference decides; among candidates
 *      within 10 % of the best the one nearest the centre is taken (repetitive detail);
 *   2. sub-pixel: Lucas-Kanade (Gauss-Newton on the brightness-normalised difference) on the central 512 x 512
 *      pixels, pass k sampled with Lanczos-3 interpolation (6 x 6 taps), until the step is below 0.001 pixel, for
 *      the best 3 whole-pixel candidates and the best one within +-3 of the centre; the smallest mean squared difference wins (within 10 %: the one nearest
 *      the centre); a result more than 1.5 pixels from its start is not used;
 *   3. the same sub-pixel step in a 3 x 3 grid of tiles, only reported (to see whether the shift is the same
 *      everywhere, i.e. a plain translation).
 * Pass k is then shifted by the global (dx, dy) - samples copied for a whole-pixel shift, Lanczos-3 interpolation
 * for a fractional one - and added; each pixel's mean is taken over the passes that cover it. */
#include "rawstack.h"
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline float px(RsImg a, int x, int y) {     /* off = -1: sum of the three channels (brightness) */
    const uint16_t *q = a.p + ((size_t)y * a.w + x) * a.stride;
    return a.off >= 0 ? q[a.off] : (float)q[0] + q[1] + q[2];
}
/* Lanczos-3 weights for taps -2..3 around floor(x), fraction f, normalised to sum 1. Chosen over cubic (Keys) for
   keeping detail: at a half-pixel shift the response at 0.3 cycles/pixel is 0.98 instead of 0.78 (computed). */
void rs_lanczos6(double f, double w[6]) {
    double s = 0;
    for (int i = 0; i < 6; i++) {
        double t = (i - 2) - f, v;
        if (fabs(t) < 1e-9) v = 1; else if (fabs(t) >= 3) v = 0;
        else { double a = M_PI * t; v = 3 * sin(a) * sin(a / 3) / (a * a); }
        w[i] = v; s += v;
    }
    for (int i = 0; i < 6; i++) w[i] /= s;
    /* first-moment correction: normalised Lanczos does not reproduce a straight line exactly (sum w*t != 0), which
       is a fractional-position error of about 0.015 pixel on smooth detail (measured, rawstack_test). A linear
       term with zero sum removes it: sum w = 1 and sum w*t = 0 afterwards. */
    double m1 = 0, mt = 0, st = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; m1 += w[i] * t; mt += t; }
    mt /= 6; for (int i = 0; i < 6; i++) { double t = (i - 2) - f; st += (t - mt) * t; }
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; w[i] -= m1 / st * (t - mt); }
}
/* ---- whole-pixel search on a float pyramid ---- */
typedef struct { float *p; int w, h; } FImg;
static double ncc_f(const FImg *r, const FImg *m, int sx, int sy, int margin) {
    int xa = margin, xb = r->w - margin, ya = margin, yb = r->h - margin;
    if (xa < -sx) xa = -sx;
    if (xb > m->w - sx) xb = m->w - sx;
    if (ya < -sy) ya = -sy;
    if (yb > m->h - sy) yb = m->h - sy;
    if (xb - xa < 8 || yb - ya < 8) return -2;
    int step = 1; while ((long long)((xb - xa) / step) * ((yb - ya) / step) > 65536) step++;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; long n = 0;
    for (int y = ya; y < yb; y += step) for (int x = xa; x < xb; x += step) {
        double a = r->p[(size_t)y * r->w + x], b = m->p[(size_t)(y + sy) * m->w + x + sx];
        sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; n++;
    }
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va <= 0 || vb <= 0) return -2;
    return (sab - sa * sb / n) / sqrt(va * vb);
}
/* mean squared difference of m shifted by (sx, sy) against r over the overlap (2-pixel border left out), relative to
   the variance of r there; 1e300 when the overlap is too small or r has no variance */
static double msd_f(const FImg *r, const FImg *m, int sx, int sy) {
    int xa = 2, xb = r->w - 2, ya = 2, yb = r->h - 2;
    if (xa < -sx) xa = -sx;
    if (xb > m->w - sx) xb = m->w - sx;
    if (ya < -sy) ya = -sy;
    if (yb > m->h - sy) yb = m->h - sy;
    if (xb - xa < 8 || yb - ya < 8) return 1e300;
    int step = 1; while ((long long)((xb - xa) / step) * ((yb - ya) / step) > 262144) step++;
    double sa = 0, saa = 0, sd = 0; long n = 0;
    for (int y = ya; y < yb; y += step) for (int x = xa; x < xb; x += step) {
        double a = r->p[(size_t)y * r->w + x], b = m->p[(size_t)(y + sy) * m->w + x + sx];
        sa += a; saa += a * a; sd += (a - b) * (a - b); n++;
    }
    double va = saa / n - (sa / n) * (sa / n);
    if (va <= 1e-9) return 1e300;
    return sd / n / va;
}
static FImg half(const FImg *a) {
    FImg b = { NULL, a->w / 2, a->h / 2 }; b.p = malloc((size_t)b.w * b.h * sizeof(float)); if (!b.p) return b;
    for (int y = 0; y < b.h; y++) for (int x = 0; x < b.w; x++) {
        const float *s = a->p + (size_t)(2 * y) * a->w + 2 * x;
        b.p[(size_t)y * b.w + x] = 0.25f * (s[0] + s[1] + s[a->w] + s[a->w + 1]);
    }
    return b;
}
/* whole-pixel shift of m against r, central part; returns 0 when memory was short */
/* whole-pixel candidates for the shift of m against r, best first (at most 3); returns their number, 0 when memory
   was short */
static int coarse(RsImg r, RsImg m, int out[5][2], double outmsd[5]) {
    int cw = r.w < 2048 ? r.w : 2048, chh = r.h < 2048 ? r.h : 2048, cx = (r.w - cw) / 2, cy = (r.h - chh) / 2;
    FImg R[12], M[12]; int L = 0;
    R[0].w = M[0].w = cw; R[0].h = M[0].h = chh;
    R[0].p = malloc((size_t)cw * chh * sizeof(float)); M[0].p = malloc((size_t)cw * chh * sizeof(float));
    if (!R[0].p || !M[0].p) { free(R[0].p); free(M[0].p); return 0; }
    for (int y = 0; y < chh; y++) for (int x = 0; x < cw; x++) {
        R[0].p[(size_t)y * cw + x] = px(r, cx + x, cy + y); M[0].p[(size_t)y * cw + x] = px(m, cx + x, cy + y); }
    while (L < 11 && R[L].w / 2 >= 96 && R[L].h / 2 >= 96) {       /* the top level keeps >= 96 pixels a side */
        R[L + 1] = half(&R[L]); M[L + 1] = half(&M[L]);
        if (!R[L + 1].p || !M[L + 1].p) { free(R[L + 1].p); free(M[L + 1].p); break; }
        L++;
    }
    /* top level: every shift within +-range; the best local maxima (and the centre) are all followed down the
       pyramid, and the full-size match decides - a small top level can rank a wrong shift first (aliasing, little
       overlap), the full size does not */
    int range = (R[L].w < R[L].h ? R[L].w : R[L].h) / 3; if (range > 48) range = 48;
    int nside = 2 * range + 1; double *map = malloc((size_t)nside * nside * sizeof(double));
    if (!map) { for (int l = 0; l <= L; l++) { free(R[l].p); free(M[l].p); } return 0; }
    for (int sy = -range; sy <= range; sy++) for (int sx = -range; sx <= range; sx++)
        map[(size_t)(sy + range) * nside + sx + range] = ncc_f(&R[L], &M[L], sx, sy, range);
    enum { K = 8 }; int cand[K + 2][2], nc = 0; double cv[K + 2];
    cand[nc][0] = 0; cand[nc][1] = 0; cv[nc++] = 9;           /* the centre always */
    {   /* and the best shift within +-2 of the centre (small shifts are the likely ones; with repetitive detail
           there can be more than K equally good maxima further out) */
        int nx = 0, ny = 0; double nv = -3;
        for (int sy = -2; sy <= 2; sy++) for (int sx = -2; sx <= 2; sx++) {
            if (sx < -range || sx > range || sy < -range || sy > range) continue;
            double v = map[(size_t)(sy + range) * nside + sx + range]; if (v > nv) { nv = v; nx = sx; ny = sy; } }
        if (nx || ny) { cand[nc][0] = nx; cand[nc][1] = ny; cv[nc++] = 8; }
    }
    for (int sy = -range; sy <= range; sy++) for (int sx = -range; sx <= range; sx++) {
        double v = map[(size_t)(sy + range) * nside + sx + range]; int peak = v > -1.5;
        for (int j = -1; j <= 1 && peak; j++) for (int i = -1; i <= 1; i++) {
            int yy = sy + j, xx = sx + i; if ((i || j) && yy >= -range && yy <= range && xx >= -range && xx <= range && map[(size_t)(yy + range) * nside + xx + range] > v) { peak = 0; break; } }
        if (!peak || (sx == 0 && sy == 0)) continue;
        if (sx >= -2 && sx <= 2 && sy >= -2 && sy <= 2) continue;   /* the near one is in already */
        if (nc < K + 2) { cand[nc][0] = sx; cand[nc][1] = sy; cv[nc++] = v; }
        else { int w = 0; for (int k = 0; k < nc; k++) if (cv[k] < 8 && (cv[w] >= 8 || cv[k] < cv[w])) w = k; if (v > cv[w]) { cand[w][0] = sx; cand[w][1] = sy; cv[w] = v; } }
    }
    free(map);
    int bx = 0, by = 0;
    double fin[K + 2]; int fx_[K + 2], fy_[K + 2];
    for (int k = 0; k < nc; k++) {
        int x = cand[k][0], y = cand[k][1]; double c = -3;
        for (int l = L - 1; l >= 0; l--) {
            x *= 2; y *= 2; int cxx = x, cyy = y; c = ncc_f(&R[l], &M[l], cxx, cyy, 2);
            for (int sy = cyy - 2; sy <= cyy + 2; sy++) for (int sx = cxx - 2; sx <= cxx + 2; sx++) {
                if (sx == cxx && sy == cyy) continue;
                double v = ncc_f(&R[l], &M[l], sx, sy, 2); if (v > c + 1e-4) { c = v; x = sx; y = sy; } }
        }
        (void)c;
        fin[k] = msd_f(&R[0], &M[0], x, y); fx_[k] = x; fy_[k] = y;
    }
    /* the full-size decision is the plain mean squared difference (not brightness-normalised: the passes have the
       same exposure, and a normalised match cannot tell a smooth ramp at one shift from the same ramp at another);
       repetitive detail matches again one period away: among the candidates within 10 % (+0.002) of the best, the
       one nearest the centre */
    double best = 1e300; for (int k = 0; k < nc; k++) if (fin[k] < best) best = fin[k];
    double bd = 1e18;
    for (int k = 0; k < nc; k++) if (fin[k] <= best * 1.1 + 0.002) { double d = (double)fx_[k] * fx_[k] + (double)fy_[k] * fy_[k]; if (d < bd) { bd = d; bx = fx_[k]; by = fy_[k]; } }
    for (int l = 0; l <= L; l++) { free(R[l].p); free(M[l].p); }
    /* the chosen one first, then the next two by mean squared difference (distinct): the sub-pixel step decides
       between them (a whole-pixel comparison can rank a wrong position first when the true shift is half a pixel) */
    int no = 0; out[no][0] = bx; out[no][1] = by;
    for (int k = 0; k < nc; k++) if (fx_[k] == bx && fy_[k] == by) outmsd[no] = fin[k];
    no++;
    while (no < 3) {
        int bk = -1;
        for (int k = 0; k < nc; k++) {
            int dup = 0; for (int j = 0; j < no; j++) if (out[j][0] == fx_[k] && out[j][1] == fy_[k]) dup = 1;
            if (!dup && fin[k] < 1e299 && (bk < 0 || fin[k] < fin[bk])) bk = k;
        }
        if (bk < 0) break;
        out[no][0] = fx_[bk]; out[no][1] = fy_[bk]; outmsd[no] = fin[bk]; no++;
    }
    /* and the best one near the centre (within +-3 pixels), if it is not among them yet: small shifts are the likely
       ones, and with nearly repeating detail a half-pixel true shift can rank behind far whole-pixel matches */
    { int bk = -1;
      for (int k = 0; k < nc; k++) if (abs(fx_[k]) <= 3 && abs(fy_[k]) <= 3 && fin[k] < 1e299 && (bk < 0 || fin[k] < fin[bk])) bk = k;
      if (bk >= 0) { int dup = 0; for (int j = 0; j < no; j++) if (out[j][0] == fx_[bk] && out[j][1] == fy_[bk]) dup = 1;
                     if (!dup) { out[no][0] = fx_[bk]; out[no][1] = fy_[bk]; outmsd[no] = fin[bk]; no++; } } }
    return no;
}

/* NCC of m shifted by whole pixels (ix, iy) against r in the window (full size, brightness) - for passes whose
   sub-pixel step could not run; -2 = no variance */
static double ncc_whole(RsImg r, RsImg m, RsRect win, int ix, int iy) {
    int xa = win.x0, xb = win.x1, ya = win.y0, yb = win.y1;
    if (xa < -ix) xa = -ix;
    if (xb > m.w - ix) xb = m.w - ix;
    if (ya < -iy) ya = -iy;
    if (yb > m.h - iy) yb = m.h - iy;
    if (xb - xa < 4 || yb - ya < 4) return -2;
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; long n = 0;
    for (int y = ya; y < yb; y++) for (int x = xa; x < xb; x++) {
        double a = px(r, x, y), b = px(m, x + ix, y + iy); sa += a; sb += b; saa += a * a; sbb += b * b; sab += a * b; n++; }
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n;
    if (va <= 1e-9 * n || vb <= 1e-9 * n) return -2;
    return (sab - sa * sb / n) / sqrt(va * vb);
}

/* weak (out): directions without enough detail to measure (0, 1 or 2; the shift is not changed along them).
   The shift is the same for every point, so pass k is interpolated separably with one set of Lanczos weights per
   iteration (6 + 6 multiplications per point). */
static double lk(RsImg r, RsImg m, RsRect win, double *dx, double *dy, int *weak, double *msd) {
    *weak = 0; *msd = 1e300;
    win.x0 = win.x0 < 1 ? 1 : win.x0; win.y0 = win.y0 < 1 ? 1 : win.y0;
    win.x1 = win.x1 > r.w - 1 ? r.w - 1 : win.x1; win.y1 = win.y1 > r.h - 1 ? r.h - 1 : win.y1;
    int ww = win.x1 - win.x0, wh = win.y1 - win.y0;
    if (ww < 8 || wh < 8) return -2;
    size_t np = (size_t)ww * wh;
    float *R = malloc(np * sizeof(float)), *GX = malloc(np * sizeof(float)), *GY = malloc(np * sizeof(float));
    float *I = malloc(np * sizeof(float)), *HB = malloc((size_t)ww * (wh + 6) * sizeof(float));
    uint8_t *MK = r.mask ? malloc(np) : NULL;
    double score = -2;
    if (!R || !GX || !GY || !I || !HB || (r.mask && !MK)) goto done;
    for (int y = 0; y < wh; y++) for (int x = 0; x < ww; x++) {
        int X = win.x0 + x, Y = win.y0 + y; size_t o = (size_t)y * ww + x;
        R[o] = px(r, X, Y); GX[o] = 0.5f * (px(r, X + 1, Y) - px(r, X - 1, Y)); GY[o] = 0.5f * (px(r, X, Y + 1) - px(r, X, Y - 1));
        if (MK) MK[o] = r.mask[(size_t)Y * r.w + X];
    }
    for (int it = 0; it < 30; it++) {
        int ox = (int)floor(*dx), oy = (int)floor(*dy); double wx[6], wy[6];
        rs_lanczos6(*dx - ox, wx); rs_lanczos6(*dy - oy, wy);
        /* points whose 36 taps are inside pass k: x + ox - 2 >= 0, x + ox + 3 <= w - 1 */
        int vx0 = win.x0 > 2 - ox ? win.x0 : 2 - ox, vx1 = win.x1 < m.w - 3 - ox ? win.x1 : m.w - 3 - ox;
        int vy0 = win.y0 > 2 - oy ? win.y0 : 2 - oy, vy1 = win.y1 < m.h - 3 - oy ? win.y1 : m.h - 3 - oy;
        if (vx1 - vx0 < 8 || vy1 - vy0 < 8) { score = -2; break; }
        int vw = vx1 - vx0, vh = vy1 - vy0;
        for (int sr = 0; sr < vh + 5; sr++) {            /* source rows vy0 + oy - 2 .. vy1 - 1 + oy + 3 */
            int Y = vy0 + oy - 2 + sr; float *hb = HB + (size_t)sr * ww;
            for (int x = 0; x < vw; x++) { int X = vx0 + x + ox - 2; double v = 0; for (int i = 0; i < 6; i++) v += wx[i] * px(m, X + i, Y); hb[x] = (float)v; }
        }
        double sr_ = 0, si = 0, srr = 0, sii = 0; long n = 0;
        for (int y = 0; y < vh; y++) for (int x = 0; x < vw; x++) {
            double v = 0; for (int j = 0; j < 6; j++) v += wy[j] * HB[(size_t)(y + j) * ww + x];
            I[(size_t)y * ww + x] = (float)v;
            size_t o = (size_t)(vy0 - win.y0 + y) * ww + (vx0 - win.x0 + x);
            if (MK && !MK[o]) continue;
            double a = R[o];
            sr_ += a; si += v; srr += a * a; sii += v * v; n++;
        }
        if (n < 64) { score = -2; break; }
        double mr = sr_ / n, mi = si / n, vr = srr / n - mr * mr, vi = sii / n - mi * mi;
        if (vr <= 1e-9 || vi <= 1e-9) { score = -2; break; }
        double qr = 1 / sqrt(vr), qi = 1 / sqrt(vi);
        double h11 = 0, h12 = 0, h22 = 0, b1 = 0, b2 = 0, sc = 0, sd = 0;
        for (int y = 0; y < vh; y++) for (int x = 0; x < vw; x++) {
            size_t o = (size_t)(vy0 - win.y0 + y) * ww + (vx0 - win.x0 + x);
            if (MK && !MK[o]) continue;
            double a = (R[o] - mr) * qr, b = (I[(size_t)y * ww + x] - mi) * qi, gx = GX[o] * qr, gy = GY[o] * qr;
            h11 += gx * gx; h12 += gx * gy; h22 += gy * gy; b1 += gx * (a - b); b2 += gy * (a - b); sc += a * b;
            double dd = R[o] - I[(size_t)y * ww + x]; sd += dd * dd;
        }
        score = sc / n; *msd = sd / n / vr;      /* plain mean squared difference, relative to the variance of r */
        /* solve along the eigenvectors of H; a direction whose mean squared gradient (brightness normalised to
           variance 1) is below 1e-4 per pixel - detail coarser than about 600 pixels - is not measured */
        double tr = h11 + h22, dd = sqrt((h11 - h22) * (h11 - h22) / 4 + h12 * h12), l1 = tr / 2 + dd, l2 = tr / 2 - dd;
        double v1x, v1y; if (fabs(h12) > 1e-12) { v1x = l1 - h22; v1y = h12; } else if (h11 >= h22) { v1x = 1; v1y = 0; } else { v1x = 0; v1y = 1; }
        double nv = sqrt(v1x * v1x + v1y * v1y); v1x /= nv; v1y /= nv;
        double v2x = -v1y, v2y = v1x, thr = 1e-4 * n, ex = 0, ey = 0;
        *weak = (l1 < thr) + (l2 < thr);
        if (*weak == 2) { score = -2; break; }
        { double c = (v1x * b1 + v1y * b2) / l1; ex += c * v1x; ey += c * v1y; }
        if (l2 >= thr) { double c = (v2x * b1 + v2y * b2) / l2; ex += c * v2x; ey += c * v2y; }
        ex = ex > 1 ? 1 : ex < -1 ? -1 : ex; ey = ey > 1 ? 1 : ey < -1 ? -1 : ey;
        *dx += ex; *dy += ey;
        if (fabs(ex) < 1e-3 && fabs(ey) < 1e-3) break;
    }
done:
    free(R); free(GX); free(GY); free(I); free(HB); free(MK);
    return score;
}
static RsRect central(int w, int h, int size) {
    RsRect c; int cw = w < size ? w : size, ch = h < size ? h : size;
    c.x0 = (w - cw) / 2; c.y0 = (h - ch) / 2; c.x1 = c.x0 + cw; c.y1 = c.y0 + ch; return c;
}
double rs_register(RsImg ref, RsImg img, const RsRect *win, int start, double *dx, double *dy) {
    int weak; return rs_register2(ref, img, win, start, dx, dy, &weak);
}
double rs_register2(RsImg ref, RsImg img, const RsRect *win, int start, double *dx, double *dy, int *weak) {
    RsRect w = win ? *win : central(ref.w, ref.h, ref.mask ? 1024 : 512);   /* with a mask: a larger window (fewer usable points) */
    if (start) {
        double md, sx = *dx, sy = *dy, sc = lk(ref, img, w, dx, dy, weak, &md);
        if (sc == -2) { *dx = sx; *dy = sy; }    /* nothing measurable: keep the start */
        return sc;
    }
    int cand[5][2]; double cmsd[5] = { 1e300, 1e300, 1e300, 1e300, 1e300 }; int n = coarse(ref, img, cand, cmsd); if (!n) return -3;
    /* each candidate refined; the smallest plain mean squared difference after refinement wins (a candidate whose
       refinement could not run keeps its whole-pixel difference), and among those within 10 % (+0.002) of it the one
       nearest the centre (a smooth ramp, or nearly repeating detail, matches equally well at several positions) */
    double rx[5], ry[5], rs_[5], md[5]; int rw[5], bi = 0;
    for (int k = 0; k < n; k++) {
        rx[k] = cand[k][0]; ry[k] = cand[k][1];
        rs_[k] = lk(ref, img, w, &rx[k], &ry[k], &rw[k], &md[k]);
        if (rs_[k] == -2 || fabs(rx[k] - cand[k][0]) > 1.5 || fabs(ry[k] - cand[k][1]) > 1.5) {   /* not measurable, or walked off */
            rx[k] = cand[k][0]; ry[k] = cand[k][1]; rs_[k] = -2; md[k] = cmsd[k]; }
    }
    double best = 1e300; for (int k = 0; k < n; k++) if (md[k] < best) best = md[k];
    double bd = 1e18;
    for (int k = 0; k < n; k++) if (md[k] <= best * 1.1 + 0.002) { double d = rx[k] * rx[k] + ry[k] * ry[k]; if (d < bd) { bd = d; bi = k; } }
    *dx = rx[bi]; *dy = ry[bi]; *weak = rw[bi];
    return rs_[bi];
}

/* ---- the stack ---- */
int rs_init(RawStack *s, int w, int h, int ch) {
    memset(s, 0, sizeof *s); s->w = w; s->h = h; s->ch = ch;
    s->acc = calloc((size_t)w * h * ch, sizeof(float)); s->first = malloc((size_t)w * h * ch * sizeof(uint16_t));
    if (!s->acc || !s->first) { rs_free(s); return -1; }
    return 0;
}
void rs_free(RawStack *s) { free(s->acc); free(s->first); s->acc = NULL; s->first = NULL; }

/* reg: the picture used for the registration; src: the picture that is shifted (the same for averaging; for HDR the
   brighter pass is registered as a copy scaled to pass 1's brightness and the unscaled pass is shifted). dst == NULL:
   added to the mean; dst != NULL: the shifted picture is written to dst (-1 where not covered), nothing is added. */
static int add_core(RawStack *s, const uint16_t *reg, const uint16_t *src, RsInfo *info, float *dst) {
    const uint16_t *img = src;
    memset(info, 0, sizeof *info); info->used = 1;
    int w = s->w, h = s->h, ch = s->ch;
    if (!dst && s->n >= RS_MAXPASS) return -2;
    if (dst && s->n == 0) return -2;
    if (s->n == 0) {
        memcpy(s->first, img, (size_t)w * h * ch * sizeof(uint16_t));
        for (size_t i = 0; i < (size_t)w * h * ch; i++) s->acc[i] = img[i];
        s->cover[0] = (RsRect){ 0, 0, w, h }; s->n = 1; info->score = 1; return 0;
    }
    /* registration channel: the one with the most detail (mean squared gradient) in the central part of pass 1 -
       not the sum of the channels, which can cancel detail that is in one channel only (found in testing) */
    int gc = 0;
    if (ch == 3) {
        RsRect c = central(w, h, 512); double bestg = -1;
        for (int k = 0; k < 3; k++) {
            double g = 0; for (int y = c.y0 + 1; y < c.y1 - 1; y++) for (int x = c.x0 + 1; x < c.x1 - 1; x++) {
                const uint16_t *q = s->first + ((size_t)y * w + x) * ch + k;
                double gx = (double)q[ch] - q[-ch], gy = (double)q[(size_t)w * ch] - q[-(ptrdiff_t)((size_t)w * ch)]; g += gx * gx + gy * gy; }
            if (g > bestg) { bestg = g; gc = k; }
        }
    }
    info->chan = gc;
    RsImg R = { s->first, w, h, ch, gc, NULL }, M = { reg, w, h, ch, gc, NULL };
    if (s->regmask) {          /* use the mask only if the central window holds enough of its points */
        RsRect c = central(w, h, 1024); long cnt = 0;
        for (int y = c.y0; y < c.y1; y++) for (int x = c.x0; x < c.x1; x++) cnt += s->regmask[(size_t)y * w + x] != 0;
        if (cnt >= 4096) { R.mask = s->regmask; info->masked = 1; }
    }
    double dx = 0, dy = 0; info->score = rs_register2(R, M, NULL, 0, &dx, &dy, &info->weak);
    if (info->score < -2.5) return -1;
    info->dx = dx; info->dy = dy;
    /* the same in 3 x 3 tiles (reported only) */
    for (int ty = 0; ty < 3; ty++) for (int tx = 0; tx < 3; tx++) {
        int k = ty * 3 + tx, cx = w * (2 * tx + 1) / 6, cy = h * (2 * ty + 1) / 6, hw = w / 6 < 128 ? w / 6 : 128, hh = h / 6 < 128 ? h / 6 : 128;
        RsRect t = { cx - hw, cy - hh, cx + hw, cy + hh };
        double tdx = dx, tdy = dy; info->tscore[k] = rs_register(R, M, &t, 1, &tdx, &tdy);
        info->tdx[k] = tdx; info->tdy[k] = tdy;
    }
    info->ntiles = 9;
    /* a pass that does not match pass 1 (NCC below 0.5) is left out: averaging it would blur the picture. A picture
       without measurable detail (score -2) is added with the whole-pixel shift (0 when nothing matches better). */
    if (info->score == -2) {   /* no sub-pixel step: check the whole-pixel shift at full size before using it */
        RsRect c = central(w, h, 512); double v = ncc_whole(R, M, c, (int)floor(dx + 0.5), (int)floor(dy + 0.5));
        info->wscore = v;
        if (v > -1.5 && v < 0.5) { info->used = 0; return 0; }
    }
    if (info->score > -1.5 && info->score < 0.5) { info->used = 0; return 0; }
    info->used = 1;
    if (dst) for (size_t i = 0; i < (size_t)w * h * ch; i++) dst[i] = -1;
    /* shift and add */
    double rx = floor(dx + 0.5), ry = floor(dy + 0.5);
    int whole = fabs(dx - rx) < 0.01 && fabs(dy - ry) < 0.01;
    double sq[3][4] = { { 0 } }; long long nn[3][4] = { { 0 } };
    RsRect cv;
    if (whole) {
        int ix = (int)rx, iy = (int)ry;
        cv.x0 = ix < 0 ? -ix : 0; cv.x1 = ix > 0 ? w - ix : w; cv.y0 = iy < 0 ? -iy : 0; cv.y1 = iy > 0 ? h - iy : h;
        for (int y = cv.y0; y < cv.y1; y++) for (int x = cv.x0; x < cv.x1; x++) for (int c = 0; c < ch; c++) {
            size_t o = ((size_t)y * w + x) * ch + c; double v = img[((size_t)(y + iy) * w + x + ix) * ch + c];
            if (dst) { dst[o] = (float)v; continue; }
            s->acc[o] += (float)v; double f = s->first[o], d = v - f; int b = (int)f >> 14;
            sq[c][b] += d * d; nn[c][b]++;
        }
    } else {
        int ox = (int)floor(dx), oy = (int)floor(dy); double wx[6], wy[6];
        rs_lanczos6(dx - ox, wx); rs_lanczos6(dy - oy, wy);
        /* all 36 taps inside: x + ox - 2 >= 0 and x + ox + 3 <= w - 1 */
        cv.x0 = 2 - ox; if (cv.x0 < 0) cv.x0 = 0; cv.x1 = w - 3 - ox; if (cv.x1 > w) cv.x1 = w;
        cv.y0 = 2 - oy; if (cv.y0 < 0) cv.y0 = 0; cv.y1 = h - 3 - oy; if (cv.y1 > h) cv.y1 = h;
        if (cv.x1 > cv.x0 && cv.y1 > cv.y0) {
            /* separable: each source row filtered across once (ring of 6 rows), then 6 rows combined */
            int cw = cv.x1 - cv.x0; float *ring = malloc((size_t)6 * cw * ch * sizeof(float)); int ring_row[6] = { -1, -1, -1, -1, -1, -1 };
            if (!ring) return -1;
            for (int y = cv.y0; y < cv.y1; y++) {
                float *rr[6];
                for (int j = 0; j < 6; j++) {
                    int Y = y + oy - 2 + j, slot = ((Y % 6) + 6) % 6; float *hb = ring + (size_t)slot * cw * ch;
                    if (ring_row[slot] != Y) {
                        const uint16_t *src = img + (size_t)Y * w * ch;
                        for (int x = 0; x < cw; x++) { const uint16_t *q = src + (size_t)(cv.x0 + x + ox - 2) * ch;
                            for (int c = 0; c < ch; c++) hb[x * ch + c] = (float)(wx[0] * q[c] + wx[1] * q[ch + c] + wx[2] * q[2 * ch + c] + wx[3] * q[3 * ch + c] + wx[4] * q[4 * ch + c] + wx[5] * q[5 * ch + c]); }
                        ring_row[slot] = Y;
                    }
                    rr[j] = hb;
                }
                for (int x = 0; x < cw; x++) for (int c = 0; c < ch; c++) {
                    int k = x * ch + c;
                    double v = wy[0] * rr[0][k] + wy[1] * rr[1][k] + wy[2] * rr[2][k] + wy[3] * rr[3][k] + wy[4] * rr[4][k] + wy[5] * rr[5][k];
                    size_t o = ((size_t)y * w + cv.x0 + x) * ch + c;
                    if (dst) { dst[o] = (float)v; continue; }
                    s->acc[o] += (float)v; double f = s->first[o], d = v - f; int b = (int)f >> 14;
                    sq[c][b] += d * d; nn[c][b]++;
                }
            }
            free(ring);
        }
        info->resampled = 1;
    }
    if (cv.x1 <= cv.x0 || cv.y1 <= cv.y0) cv = (RsRect){ 0, 0, 0, 0 };
    info->cover = cv;
    if (dst) return 0;
    s->cover[s->n++] = cv;
    for (int c = 0; c < ch; c++) for (int b = 0; b < 4; b++) { info->dn[c][b] = nn[c][b]; info->drms[c][b] = nn[c][b] ? sqrt(sq[c][b] / nn[c][b]) : 0; }
    return 0;
}
int rs_add(RawStack *s, const uint16_t *img, RsInfo *info) { return add_core(s, img, img, info, NULL); }
int rs_align(RawStack *s, const uint16_t *reg, const uint16_t *src, RsInfo *info, float *dst) { return add_core(s, reg, src, info, dst); }
void rs_result(const RawStack *s, uint16_t *out) {
    int w = s->w, h = s->h, ch = s->ch;
    int *cnt = malloc((size_t)(w + 1) * sizeof(int));
    for (int y = 0; y < h; y++) {
        memset(cnt, 0, (size_t)(w + 1) * sizeof(int));
        for (int k = 0; k < s->n; k++) { const RsRect *r = &s->cover[k]; if (y >= r->y0 && y < r->y1 && r->x1 > r->x0) { cnt[r->x0]++; cnt[r->x1]--; } }
        int c = 0;
        for (int x = 0; x < w; x++) {
            c += cnt[x];
            for (int k = 0; k < ch; k++) {
                size_t o = ((size_t)y * w + x) * ch + k;
                double v = c > 0 ? s->acc[o] / c : s->first[o];   /* c >= 1 always: pass 1 covers everything */
                out[o] = v <= 0 ? 0 : v >= 65535 ? 65535 : (uint16_t)(v + 0.5);
            }
        }
    }
    free(cnt);
}
