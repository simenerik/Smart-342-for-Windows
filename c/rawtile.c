/* rawtile.c - see rawtile.h. */
#include "rawtile.h"
#include "rawstack.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int rtm_init(RtMosaic *m, int W, int H, int ch) {
    memset(m, 0, sizeof *m); m->W = W; m->H = H; m->ch = ch;
    m->acc = calloc((size_t)W * H * ch, sizeof(float)); m->wsum = calloc((size_t)W * H, sizeof(float));
    if (!m->acc || !m->wsum) { rtm_free(m); return -1; }
    return 0;
}
void rtm_free(RtMosaic *m) { free(m->acc); free(m->wsum); m->acc = m->wsum = NULL; }

static void plan_axis(int W, int maxw, int ovl, int *n, int *tw, int *pos, int maxn) {
    if (W <= maxw) { *n = 1; *tw = W; pos[0] = 0; return; }
    int o = ovl + 1;                                     /* +1: the rounding of the positions below */
    int k = (W - o + (maxw - o) - 1) / (maxw - o); if (k < 2) k = 2;
    int t = (W + (k - 1) * o + k - 1) / k;
    while (t > maxw && k < maxn) { k++; t = (W + (k - 1) * o + k - 1) / k; }
    *n = k; *tw = t;
    for (int i = 0; i < k && i < maxn; i++) pos[i] = (int)floor((double)i * (W - t) / (k - 1) + 0.5);
}
int rt_plan(int W, int H, int maxw, int maxh, int ovl, int (*rects)[4], int maxn) {
    if (W < 1 || H < 1 || maxw <= 2 * ovl + 2 || maxh <= 2 * ovl + 2) return 0;
    int nx, ny, tw, th, px[256], py[256];
    plan_axis(W, maxw, ovl, &nx, &tw, px, 256); plan_axis(H, maxh, ovl, &ny, &th, py, 256);
    if (tw > maxw || th > maxh || nx > 256 || ny > 256) return 0;
    int n = 0;
    for (int j = 0; j < ny; j++) for (int i = 0; i < nx; i++) {
        if (n < maxn) { rects[n][0] = px[i]; rects[n][1] = py[j]; rects[n][2] = tw; rects[n][3] = th; }
        n++; }
    return n;
}

/* Lanczos-3 weights for taps -2..3 around floor(x), fraction f, normalised to sum 1 */
static void lanczos6(double f, double w[6]) {
    double s = 0;
    for (int i = 0; i < 6; i++) {
        double t = (i - 2) - f, v;
        if (fabs(t) < 1e-9) v = 1; else if (fabs(t) >= 3) v = 0; else v = 3 * sin(M_PI * t) * sin(M_PI * t / 3) / (M_PI * M_PI * t * t);
        w[i] = v; s += v; }
    for (int i = 0; i < 6; i++) w[i] /= s;
    /* first-moment correction as in rawstack.c (review: without it a block is placed up to 0.02 px off) */
    double m1 = 0, mt = 0, st = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; m1 += w[i] * t; mt += t; }
    mt /= 6; for (int i = 0; i < 6; i++) { double t = (i - 2) - f; st += (t - mt) * t; }
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; w[i] -= m1 / st * (t - mt); }
}
/* the tile shifted by (dx, dy): out(x, y) = t(x + dx, y + dy), separable Lanczos-3; samples whose taps leave the tile
   are NaN (review: a negative marker turned real Lanczos undershoot next to black into holes) */
static float *shift_tile(const uint16_t *t, int tw, int th, int ch, double dx, double dy) {
    size_t n = (size_t)tw * th * ch;
    float *tmp = malloc(n * sizeof(float)), *out = malloc(n * sizeof(float));
    if (!tmp || !out) { free(tmp); free(out); return NULL; }
    int ix = (int)floor(dx), iy = (int)floor(dy); double wx[6], wy[6]; lanczos6(dx - ix, wx); lanczos6(dy - iy, wy);
    int exact_x = fabs(dx - ix) < 1e-9, exact_y = fabs(dy - iy) < 1e-9;
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < ch; c++) {
        float v;
        if (exact_x) { int sx = x + ix; v = (sx < 0 || sx >= tw) ? NAN : t[((size_t)y * tw + sx) * ch + c]; }
        else { int s0 = x + ix - 2; if (s0 < 0 || s0 + 5 >= tw) v = NAN; else { double a = 0; for (int k = 0; k < 6; k++) a += wx[k] * t[((size_t)y * tw + s0 + k) * ch + c]; v = (float)a; } }
        tmp[((size_t)y * tw + x) * ch + c] = v; }
    for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < ch; c++) {
        float v;
        if (exact_y) { int sy = y + iy; v = (sy < 0 || sy >= th) ? NAN : tmp[((size_t)sy * tw + x) * ch + c]; }
        else { int s0 = y + iy - 2; v = 0;
            if (s0 < 0 || s0 + 5 >= th) v = NAN;
            else { double a = 0; int bad = 0; for (int k = 0; k < 6; k++) { float q = tmp[((size_t)(s0 + k) * tw + x) * ch + c]; if (isnan(q)) { bad = 1; break; } a += wy[k] * q; } v = bad ? NAN : (float)a; } }
        out[((size_t)y * tw + x) * ch + c] = v; }
    free(tmp); return out;
}

int rtm_add(RtMosaic *m, const uint16_t *t, int tw, int th, int x0, int y0, int feather, int maxshift, RtTileInfo *info) {
    memset(info, 0, sizeof *info); info->gain[0] = info->gain[1] = info->gain[2] = 1; info->score = -2;
    int W = m->W, H = m->H, ch = m->ch;
    int gx0 = x0 < 0 ? 0 : x0, gy0 = y0 < 0 ? 0 : y0, gx1 = x0 + tw > W ? W : x0 + tw, gy1 = y0 + th > H ? H : y0 + th;
    /* overlap: the bounding box of the already covered samples inside the tile, minus a margin of 3 (resampling) */
    int ox0 = gx1, oy0 = gy1, ox1 = gx0, oy1 = gy0; long nov = 0;
    for (int y = gy0 + 3; y < gy1 - 3; y++) for (int x = gx0 + 3; x < gx1 - 3; x++) if (m->wsum[(size_t)y * W + x] > 0) {
        if (x < ox0) { ox0 = x; } if (x >= ox1) { ox1 = x + 1; } if (y < oy0) { oy0 = y; } if (y >= oy1) { oy1 = y + 1; } nov++; }
    double dx = 0, dy = 0;
    int gc = ch == 3 ? 1 : 0;
    if (nov >= 32 * 32 && ox1 - ox0 >= 16 && oy1 - oy0 >= 16) {
        info->ovx0 = ox0; info->ovy0 = oy0; info->ovx1 = ox1; info->ovy1 = oy1; info->novl = (int)nov;
        int rw = ox1 - ox0, rh = oy1 - oy0;
        /* reference (what is placed) and the tile over the same region, one channel, plus the covered mask */
        uint16_t *ref = malloc((size_t)rw * rh * 2), *img = malloc((size_t)rw * rh * 2); uint8_t *mk = malloc((size_t)rw * rh);
        if (!ref || !img || !mk) { free(ref); free(img); free(mk); return -1; }
        double sr = 0, si = 0; long ns = 0;
        for (int y = 0; y < rh; y++) for (int x = 0; x < rw; x++) {
            size_t g = (size_t)(oy0 + y) * W + ox0 + x; float ws = m->wsum[g];
            double r = ws > 0 ? m->acc[g * ch + gc] / ws : 0; int tx = ox0 + x - x0, ty = oy0 + y - y0;
            ref[(size_t)y * rw + x] = (uint16_t)(r < 0 ? 0 : r > 65535 ? 65535 : r + 0.5);
            img[(size_t)y * rw + x] = t[((size_t)ty * tw + tx) * ch + gc];
            mk[(size_t)y * rw + x] = ws > 0;
            if (ws > 0) { sr += r; si += img[(size_t)y * rw + x]; ns++; } }
        /* the sub-pixel step takes gradients from the neighbours of each point: only points whose neighbours (2 around)
           are covered too may be used (review: at the inner corner of an L-shaped overlap the uncovered 0 samples gave
           false gradients and biased the shift by up to 0.6 px on weak detail) */
        uint8_t *mk2 = calloc((size_t)rw * rh, 1); if (!mk2) { free(ref); free(img); free(mk); return -1; }
        for (int y = 2; y < rh - 2; y++) for (int x = 2; x < rw - 2; x++) {
            int ok = 1; for (int j = -2; j <= 2 && ok; j++) for (int i = -2; i <= 2; i++) if (!mk[(size_t)(y + j) * rw + x + i]) { ok = 0; break; }
            mk2[(size_t)y * rw + x] = (uint8_t)ok; }
        /* whole-pixel search +-maxshift on the covered samples: mean squared difference after matching the means */
        double gm = si > 0 ? sr / si : 1, best = 1e300, e00 = 1e300, esum = 0; int bx = 0, by = 0, ne = 0;
        for (int sy = -maxshift; sy <= maxshift; sy++) for (int sx = -maxshift; sx <= maxshift; sx++) {
            double e = 0; long n = 0;
            for (int y = maxshift; y < rh - maxshift; y += 1) for (int x = maxshift; x < rw - maxshift; x += 1) {
                size_t i = (size_t)y * rw + x; if (!mk[i]) continue;
                double d = ref[i] - gm * img[(size_t)(y + sy) * rw + x + sx]; e += d * d; n++; }
            if (n && sx == 0 && sy == 0) e00 = e / n;
            if (n) { esum += e / n; ne++; }
            if (n && e / n < best) { best = e / n; bx = sx; by = sy; } }
        /* no clear structure (a plain area, e.g. sky or film base, only noise): a whole-pixel minimum elsewhere that is
           not clearly better than the nominal place is noise - stay at the nominal place */
        if (!(best < 0.9 * e00)) { bx = 0; by = 0; }
        /* the match must stand out against the other shifts (detail); otherwise the overlap has nothing to measure
           (noise only: every shift fits about equally) - nominal place, no sub-pixel step */
        int structure = ne > 1 && best < 0.7 * (esum / ne);
        /* sub-pixel: Lucas-Kanade from the best whole-pixel shift (rawstack.c), covered samples only */
        RsImg a = { ref, rw, rh, 1, 0, mk2 }, b = { img, rw, rh, 1, 0, NULL };
        RsRect win = { maxshift + 3, maxshift + 3, rw - maxshift - 3, rh - maxshift - 3 };
        double rx = bx, ry = by; int weak = 0;
        double sc = (structure && win.x1 - win.x0 >= 8 && win.y1 - win.y0 >= 8) ? rs_register2(a, b, &win, 1, &rx, &ry, &weak) : -2;
        if (sc > -2 && weak == 0 && fabs(rx - bx) <= 1.5 && fabs(ry - by) <= 1.5) { dx = rx; dy = ry; }
        else { dx = bx; dy = by; if (sc > -2) sc = -3; }   /* -3: the sub-pixel result was not used (whole-pixel place) */
        info->score = sc; info->registered = 1; info->dx = dx; info->dy = dy;
        info->at_edge = structure && (abs(bx) == maxshift || abs(by) == maxshift);   /* the best match on the edge of the search: the true shift may be larger */
        free(ref); free(img); free(mk); free(mk2);
    }
    float *s = shift_tile(t, tw, th, ch, dx, dy); if (!s) return -1;
    /* gain per channel in the overlap (what is placed / the shifted tile) and the rms difference before/after */
    if (info->registered) {
        double sr[3] = { 0 }, si[3] = { 0 }; long n = 0;
        for (int y = info->ovy0; y < info->ovy1; y++) for (int x = info->ovx0; x < info->ovx1; x++) {
            size_t g = (size_t)y * W + x; float ws = m->wsum[g]; if (!(ws > 0)) continue;
            size_t ti = ((size_t)(y - y0) * tw + (x - x0)) * ch; if (isnan(s[ti])) continue;
            for (int c = 0; c < ch; c++) { sr[c] += m->acc[g * ch + c] / ws; si[c] += s[ti + c]; } n++; }
        int ok = n > 0;
        for (int c = 0; c < ch; c++) { info->gain[c] = si[c] > 0 ? sr[c] / si[c] : 1; if (info->gain[c] < 0.8 || info->gain[c] > 1.25) ok = 0; }
        info->gain_used = ok; if (!ok) info->gain[0] = info->gain[1] = info->gain[2] = 1;
        double e0 = 0, e1 = 0, sm = 0; long n0 = 0, n1 = 0;
        for (int y = info->ovy0; y < info->ovy1; y++) for (int x = info->ovx0; x < info->ovx1; x++) {
            size_t g = (size_t)y * W + x; float ws = m->wsum[g]; if (!(ws > 0)) continue;
            double r = m->acc[g * ch + gc] / ws, raw = t[((size_t)(y - y0) * tw + (x - x0)) * ch + gc];
            e0 += (r - raw) * (r - raw); n0++; sm += r;
            size_t ti = ((size_t)(y - y0) * tw + (x - x0)) * ch + gc; if (!isnan(s[ti])) { double v = s[ti] * info->gain[gc]; e1 += (r - v) * (r - v); n1++; } }
        info->rms_before = n0 ? sqrt(e0 / n0) : 0; info->rms_after = n1 ? sqrt(e1 / n1) : 0; info->ov_mean = n0 ? sm / n0 : 0;
    }
    /* feathered accumulation: the weight rises from the tile's inner edges over 'feather' pixels; edges that lie on the
       picture border count as far away (nothing to blend with there) */
    for (int y = gy0; y < gy1; y++) for (int x = gx0; x < gx1; x++) {
        int tx = x - x0, ty = y - y0; size_t ti = ((size_t)ty * tw + tx) * ch;
        if (isnan(s[ti])) continue;
        int dl = x0 <= 0 ? 1 << 20 : tx, dr = x0 + tw >= W ? 1 << 20 : tw - 1 - tx, dt = y0 <= 0 ? 1 << 20 : ty, db = y0 + th >= H ? 1 << 20 : th - 1 - ty;
        int d = dl; if (dr < d) d = dr; if (dt < d) d = dt; if (db < d) d = db;
        double w = feather > 0 ? (d + 1.0) / feather : 1; if (w > 1) w = 1; if (w < 1e-3) w = 1e-3;
        size_t g = (size_t)y * W + x;
        for (int c = 0; c < ch; c++) m->acc[g * ch + c] += (float)(w * s[ti + c] * info->gain[c]);
        m->wsum[g] += (float)w; }
    free(s); return 0;
}
long long rtm_result(const RtMosaic *m, uint16_t *out) {
    long long miss = 0; int W = m->W, H = m->H, ch = m->ch;
    for (size_t g = 0; g < (size_t)W * H; g++) {
        float ws = m->wsum[g];
        for (int c = 0; c < ch; c++) { double v = ws > 0 ? m->acc[g * ch + c] / ws : 0; out[g * ch + c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5); }
        if (!(ws > 0)) miss++; }
    if (!miss) return 0;
    /* samples no tile covered (only possible at the picture border, when a tile there was shifted and no margin was
       scanned): the nearest covered sample of the same row, else of the same column */
    for (int y = 0; y < H; y++) {
        int last = -1;
        for (int x = 0; x < W; x++) if (m->wsum[(size_t)y * W + x] > 0) { last = x; break; }
        if (last < 0) continue;
        for (int x = 0; x < W; x++) {
            size_t g = (size_t)y * W + x;
            if (m->wsum[g] > 0) { last = x; continue; }
            int nx = -1; for (int k = x + 1; k < W; k++) if (m->wsum[(size_t)y * W + k] > 0) { nx = k; break; }
            int src = (nx >= 0 && (x - last > nx - x || last > x)) ? nx : last;
            memcpy(out + g * ch, out + ((size_t)y * W + src) * ch, ch * sizeof(uint16_t)); } }
    for (int x = 0; x < W; x++) {
        int first = -1; for (int y = 0; y < H; y++) { int any = 0; for (int k = 0; k < W && !any; k++) any = m->wsum[(size_t)y * W + k] > 0; if (any) { first = y; break; } }
        if (first < 0) break;
        for (int y = 0; y < first; y++) memcpy(out + ((size_t)y * W + x) * ch, out + ((size_t)first * W + x) * ch, ch * sizeof(uint16_t));
        int lastr = H - 1; while (lastr > 0) { int any = 0; for (int k = 0; k < W && !any; k++) any = m->wsum[(size_t)lastr * W + k] > 0; if (any) break; lastr--; }
        for (int y = lastr + 1; y < H; y++) memcpy(out + ((size_t)y * W + x) * ch, out + ((size_t)lastr * W + x) * ch, ch * sizeof(uint16_t)); }
    return miss;
}
