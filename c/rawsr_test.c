/* rawsr_test.c - checks of rawsr.c (developer use; run by regress.sh): pixel shift across, on a synthetic scene whose
 * cell-integrated values are known exactly.
 * Scene: value noise at several scales with detail up to and above the single-pass limit (0.5 cycles per cell
 * across), plus a probe sinusoid at 0.7 cycles per cell. A CCD cell is a box 1 cell wide and 1 pixel high; a cell's
 * value is the scene averaged over that box (numerically; the probe exactly: amplitude * sinc(0.7)).
 * Pass k sees the scene moved across by dx_k (lens carriage steps x 1.2445 cells: the L89 prediction) and along by a
 * small dy_k (carriage repeatability), with optional noise.
 * Truth: the cell-integrated scene on the fine grid (x = j / F). The combined result is compared against it, and
 * against one pass interpolated to the same grid (what one pass can give at best without inventing detail).
 * Criteria: offsets measured within 0.05 cells; error clearly lower than the one-pass baseline; and a component above
 * the old limit (0.7 cycles/cell, 1.4x) present in the result with at least half its true cell amplitude, while one
 * pass shows it at under 15 % (it aliases to 0.3 cycles/cell there). */
#include "rawsr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint64_t rs = 88172645463325252ULL;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (rs >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(6.283185307 * v); }
static double sinc(double t) { return fabs(t) < 1e-12 ? 1 : sin(M_PI * t) / (M_PI * t); }

/* the scene: value noise at several scales (not periodic, so registration has a unique answer, like film) - one
   isotropic coarse layer and fine layers whose cells are 1.3 and 2.1 cells across (detail up to and above the
   single-pass limit) and coarser along - plus the probe sinusoid. A cell's value: the scene averaged over the cell
   (12 x 3 sub-samples, midpoint rule; the same rule for passes and truth). */
static double hsh(int64_t x, int64_t y, int c) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ULL ^ (uint64_t)y * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(c + 1) * 0x165667B19E3779F9ULL;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32; return (double)(h >> 11) * (2.0 / 9007199254740992.0) - 1;
}
static double vn(double x, double y, double cx, double cy, int c) {
    x /= cx; y /= cy; double fx = floor(x), fy = floor(y), tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty); int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double a = hsh(ix, iy, c), b = hsh(ix + 1, iy, c), d = hsh(ix, iy + 1, c), e = hsh(ix + 1, iy + 1, c);
    return (a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty;
}
static double scene(double x, double y, int c) {
    return 30000 + 7000 * vn(x, y, 23, 23, c) + 4000 * vn(x, y, 1.3, 7, c + 7) + 3000 * vn(x, y, 2.1, 5, c + 13);
}
static const double PU = 0.70, PA = 3000;          /* the probe: 0.7 cycles per cell, purely across */
static double cell(double x, double y, int c, int with_probe) {
    double v = 0; const int NX = 12, NY = 3;
    for (int j = 0; j < NY; j++) for (int i = 0; i < NX; i++) v += scene(x - 0.5 + (i + 0.5) / NX, y - 0.5 + (j + 0.5) / NY, c);
    v /= NX * NY;
    if (with_probe) v += PA * sinc(PU) * cos(2 * M_PI * PU * x + 0.3 * c);
    return v;
}
static uint16_t q16(double v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5); }

/* one pass interpolated across to the fine grid (Lanczos-3), the baseline */
static void upsample(const uint16_t *p, int w, int h, int ch, int F, double *out) {
    int W = w * F;
    for (int y = 0; y < h; y++) for (int j = 0; j < W; j++) for (int c = 0; c < ch; c++) {
        double x = (double)j / F; int i0 = (int)floor(x); double s = 0, ws = 0;
        for (int i = i0 - 2; i <= i0 + 3; i++) { if (i < 0 || i >= w) continue; double t = x - i, k;
            k = fabs(t) < 1e-9 ? 1 : fabs(t) >= 3 ? 0 : 3 * sin(M_PI * t) * sin(M_PI * t / 3) / (M_PI * M_PI * t * t);
            s += k * p[((size_t)y * w + i) * ch + c]; ws += k; }
        out[((size_t)y * W + j) * ch + c] = s / ws;
    }
}
/* amplitude of the probe frequency (across, cycles per cell) in a fine-grid picture minus the truth without the probe
   (tnp), interior only, channel c */
static double probe_amp(const double *img, const double *tnp, int W, int h, int ch, int F, int c, int m) {
    double sc = 0, ss = 0; long n = 0;
    for (int y = m; y < h - m; y++) for (int j = m * F; j < W - m * F; j++) {
        double x = (double)j / F, d = img[((size_t)y * W + j) * ch + c] - tnp[((size_t)y * W + j) * ch + c];
        sc += d * cos(2 * M_PI * PU * x + 0.3 * c); ss += d * sin(2 * M_PI * PU * x + 0.3 * c); n++; }
    return 2 * sqrt(sc * sc + ss * ss) / n;
}

typedef struct { const char *name; int n; int steps[8]; int F; double sigma; int hint; double hrate; int along; double fall; double dxs[8]; double expect_gd; } Case;
/* hint: 0 none, 1 = steps x hrate (0: 1.2445 + 0.01, a measured rate slightly off); along: 1 = the lens carriage moves
   the picture along the scan instead of across (the case the hardware lens shift test must rule out) - then the
   combine must refuse; fall: brightness lost per lens step (hardware s11: about 0.55 % per step for the first steps) -
   the combine must match it (gain), or neighbouring fine columns from different passes would stripe */

static double run_case(const Case *cs) {   /* returns the combined rms error (-1 if not computed) */
    double ret = -1;
    const int w = 240, h = 96, ch = 3, F = cs->F, W = w * F, m = 16;
    const double ppstep = 1.2445;
    uint16_t *pass[8]; double tdx[8], tdy[8];
    for (int k = 0; k < cs->n; k++) {
        tdx[k] = cs->dxs[1] != 0 ? cs->dxs[k] : cs->steps[k] * ppstep; tdy[k] = k ? 0.1 * sin(1.7 * k) : 0;
        if (cs->along) { tdy[k] += tdx[k]; tdx[k] = 0; }
        pass[k] = malloc((size_t)w * h * ch * 2);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++)
            pass[k][((size_t)y * w + x) * ch + c] = q16(cell(x - tdx[k], y - tdy[k], c, 1) * (1 - cs->fall * cs->steps[k]) + cs->sigma * gauss());
    }
    uint16_t *out = malloc((size_t)W * h * ch * 2); SrInfo info;
    double hint[8]; for (int k = 0; k < cs->n; k++) hint[k] = cs->steps[k] * (cs->hrate ? cs->hrate : ppstep + 0.01);
    int r = sr_combine((const uint16_t *const *)pass, cs->n, w, h, ch, F, cs->hint ? hint : NULL, out, &info);
    if (cs->along) {
        CHECK(r == -2, "%s: refused (returned %d, score %.2f, measured dx %.3f dy %.3f)", cs->name, r, info.score[1], info.dx[1], info.dy[1]);
        goto done;
    }
    CHECK(r == 0, "%s: combine returned %d (phase gap %.3f cells; scores %.3f %.3f %.3f %.3f)", cs->name, r, info.maxgap, info.score[0], info.score[1], info.score[2], info.score[3]);
    if (r) goto done;
    double worst = 0; for (int k = 1; k < cs->n; k++) { double e = fmax(fabs(info.dx[k] - tdx[k]), fabs(info.dy[k] - tdy[k])); if (e > worst) worst = e; }
    if (getenv("SR_VERBOSE")) for (int k = 1; k < cs->n; k++) printf("     pass %d: dx %.3f (true %.3f) dy %.3f (true %.3f) phase %.3f score %.3f\n", k + 1, info.dx[k], tdx[k], info.dy[k], tdy[k], info.phase[k], info.score[k]);
    CHECK(worst < 0.05, "%s: offsets measured within %.3f cells (limit 0.05)", cs->name, worst);
    { double ge = 0; for (int k = 1; k < cs->n; k++) for (int c = 0; c < ch; c++) ge = fmax(ge, fabs(info.gain[k][c] * (1 - cs->fall * cs->steps[k]) - 1));
      CHECK(ge < 0.003, "%s: brightness of each pass matched within %.4f (limit 0.003)", cs->name, ge); }
    { double rmin = 1e9; for (int y = 0; y < h; y++) { double sm = 0; for (int j = 0; j < W; j++) sm += out[((size_t)y * W + j) * ch + 1]; rmin = fmin(rmin, sm / W); }
      CHECK(rmin > 10000, "%s: every row filled, also the first and last (lowest row mean %.0f)", cs->name, rmin); }
    double *res = malloc((size_t)W * h * ch * sizeof(double)), *base = malloc((size_t)W * h * ch * sizeof(double));
    for (size_t i = 0; i < (size_t)W * h * ch; i++) res[i] = out[i];
    upsample(pass[0], w, h, ch, F, base);
    /* the common area: all passes cover it (largest shift + margin) */
    double mxs = 0; for (int k = 0; k < cs->n; k++) if (fabs(tdx[k]) > mxs) mxs = fabs(tdx[k]);
    int mx = m + (int)ceil(mxs); double e1 = 0, e0 = 0; long n = 0;
    double *tnp = malloc((size_t)W * h * ch * sizeof(double));
    for (int y = 0; y < h; y++) for (int j = 0; j < W; j++) for (int c = 0; c < ch; c++) tnp[((size_t)y * W + j) * ch + c] = cell((double)j / F, y, c, 0);
    for (int y = m; y < h - m; y++) for (int j = mx * F; j < W - mx * F; j++) for (int c = 0; c < ch; c++) {
        size_t i = ((size_t)y * W + j) * ch + c; double t = tnp[i] + PA * sinc(PU) * cos(2 * M_PI * PU * j / F + 0.3 * c);
        e1 += (res[i] - t) * (res[i] - t); e0 += (base[i] - t) * (base[i] - t); n++; }
    e1 = sqrt(e1 / n); e0 = sqrt(e0 / n);
    double a1 = probe_amp(res, tnp, W, h, ch, F, 1, mx), a0 = probe_amp(base, tnp, W, h, ch, F, 1, mx), at = PA * sinc(PU);
    ret = e1;
    printf("     %s: rms error %.0f combined vs %.0f one pass (16-bit units); 0.7 cyc/cell probe %.0f %% vs %.0f %% of its cell amplitude\n",
           cs->name, e1, e0, 100 * a1 / at, 100 * a0 / at);
    printf("     %s: largest phase gap %.3f cells, grid distance %.3f fine columns (combine when <= %.2f)\n", cs->name, info.maxgap, info.griddist, SR_GRIDDIST_OK);
    if (cs->expect_gd >= 0) CHECK(fabs(info.griddist - cs->expect_gd) < 0.02, "%s: grid distance %.3f as worked out by hand (%.3f)", cs->name, info.griddist, cs->expect_gd);
    if (cs->n > 1 && info.griddist <= SR_GRIDDIST_OK) {
        CHECK(e1 < 0.6 * e0, "%s: error below 60 %% of one pass (%.0f vs %.0f)", cs->name, e1, e0);
        CHECK(a1 > 0.5 * at && a0 < 0.15 * at, "%s: detail at 1.4x the old limit recovered (%.0f %% vs one pass %.0f %%)", cs->name, 100 * a1 / at, 100 * a0 / at);
    } else {
        /* passes that do not fill the fine grid: no claim of more detail, but not worse than one pass */
        CHECK(e1 < 1.15 * e0 + 2 * cs->sigma, "%s: phases do not fill the grid - result no worse than one pass (%.0f vs %.0f)", cs->name, e1, e0);
    }
    free(res); free(base); free(tnp);
done:
    free(out); for (int k = 0; k < cs->n; k++) free(pass[k]);
    return ret;
}

int main(void) {
    /* steps -> phases with 1.2445 cells per step: 1 -> .756, 2 -> .511, 3 -> .267, 4 -> .022, 8 -> .044 */
    Case cases[] = {   /* name, passes, steps, F, noise, hint, hint rate, along */
        { "F2 steps 0,2", 2, { 0, 2 }, 2, 0, 1, 0, 0 },
        { "F2 steps 0,2 no hint", 2, { 0, 2 }, 2, 0, 0, 0, 0 },
        { "F2 steps 0,2 noise 40", 2, { 0, 2 }, 2, 40, 1, 0, 0 },
        { "F3 steps 0,3,1", 3, { 0, 3, 1 }, 3, 0, 1, 0, 0 },
        { "F4 steps 0,3,2,1", 4, { 0, 3, 2, 1 }, 4, 0, 1, 0, 0 },
        { "F2 steps 0,2,4,6 (two per phase) noise 40", 4, { 0, 2, 4, 6 }, 2, 40, 1, 0, 0 },
        { "F2 steps 0,4 (same phase)", 2, { 0, 4 }, 2, 0, 1, 0, 0 },
        { "F2 one pass", 1, { 0 }, 2, 0, 1, 0, 0 },
        { "F4 steps 0,3,2,1, hint rate 1.35 (0.1 per step off)", 4, { 0, 3, 2, 1 }, 4, 0, 1, 1.35, 0 },
        { "F2 steps 0,2, picture moves along instead", 2, { 0, 2 }, 2, 0, 1, 0, 1 },
        { "F2 steps 0,2 no hint, light falls 0.55 % per step, noise 40", 2, { 0, 2 }, 2, 40, 0, 0, 0, 0.0055 },
        { "F2 steps 0,1,2,3 no hint, light falls 0.55 % per step, noise 40", 4, { 0, 1, 2, 3 }, 2, 40, 0, 0, 0, 0.0055 },
        /* the offsets the scanner gave (hardware s11, L89: steps 0, 1, 2 -> 0, 0.444, 1.593 px; phases 0, .556, .407) */
        /* grid distance by hand: phase p = frac(-dx); fine position 2p mod 2; class 1 nearest: 0.556 -> 1.112, 0.407 -> 0.814 */
        { "F2 scanner's offsets 0, 0.444, 1.593 no hint, light 100/99.6/98.9 %, noise 40", 3, { 0, 1, 2 }, 2, 40, 0, 0, 0, 0.0055, { 0, 0.444, 1.593 }, 0.112 },
        { "F2 scanner's offsets 0, 0.444 only (2 passes), noise 40", 2, { 0, 1 }, 2, 40, 0, 0, 0, 0.0055, { 0, 0.444 }, 0.112 },
        /* review case: phases 0, .25, .75 - largest gap 0.5, but .25 and .75 sit at 0.5 / 1.5 fine positions: halfway
           between columns, so the odd columns get no sample (grid distance 0.5) - must not count as filled */
        { "F2 phases 0, .25, .75 (review case)", 3, { 0, 1, 2 }, 2, 0, 0, 0, 0, 0, { 0, -0.25, -0.75 }, 0.5 },
        { "F2 phases 0, .5", 2, { 0, 1 }, 2, 0, 0, 0, 0, 0, { 0, -0.5 }, 0.0 },
    };
    double rms_s11 = -1;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) { if (cases[i].dxs[1] == 0 && cases[i].expect_gd == 0) cases[i].expect_gd = -1;
        double e = run_case(&cases[i]); if (!strncmp(cases[i].name, "F2 scanner's offsets 0, 0.444, 1.593", 36)) rms_s11 = e; }
    CHECK(rms_s11 > 0 && rms_s11 < 60, "the scanner's s11 offsets, 3 passes: rms %.0f (0.1.21: 46; a dropped third pass: 165)", rms_s11);
    /* 8 passes on the scanner's own step curve (s11, 6x6 lens: +1 0.444, +2 1.593, +4 3.885, +8 9.901 px; beyond, 1.5 px per
       step): the steps chosen as the app does (sr_next_step2, balance), then combined - must not be worse than 3 passes,
       and with noise 80 better */
    {
        static const double KX[5] = { 0, 1, 2, 4, 8 }, KY[5] = { 0, 0.444, 1.593, 3.885, 9.901 };
        double off[40]; for (int k = 0; k < 40; k++) { if (k >= 8) off[k] = 9.901 + 1.5 * (k - 8); else { int i = 0; while (KX[i + 1] < k) i++; off[k] = KY[i] + (KY[i + 1] - KY[i]) * (k - KX[i]) / (KX[i + 1] - KX[i]); } }
        int st[8] = { 0, 1 }; double dx[8] = { 0, off[1] };
        for (int n = 2; n < 8; n++) { int k = sr_next_step2(dx, st, n, 2, 1, NULL, NULL); if (k < 0 || k > 32) k = st[n - 1] + 1; st[n] = k; dx[n] = off[k]; }
        printf("     8 passes on the s11 curve: steps");
        for (int k = 0; k < 8; k++) printf(" %+d (%.3f)", st[k], dx[k]); printf("\n");
        Case c8 = { "F2 s11 curve, 8 passes, noise 80", 8, { 0 }, 2, 80, 0, 0, 0, 0.0055, { 0 }, -1 };
        Case c3 = { "F2 s11 curve, 3 passes (0, 1, 2), noise 80", 3, { 0, 1, 2 }, 2, 80, 0, 0, 0, 0.0055, { 0, 0.444, 1.593 }, -1 };
        for (int k = 0; k < 8; k++) { c8.steps[k] = st[k]; c8.dxs[k] = dx[k]; }
        double e8 = run_case(&c8), e3 = run_case(&c3);
        CHECK(e8 > 0 && e3 > 0 && e8 < 0.9 * e3, "8 passes on the s11 curve: rms %.0f against %.0f for 3 passes (less noise)", e8, e3);
    }
    /* the 35 mm lens's curves (s18: +1..+4 = 0.89, 1.98, 3.14, 5.31 px, then 2.17 per step; s20: +1 0.991, +7 11.393, about
       1.734 per step): the normal setting (steps chosen until covered, at most 5) against the 8-pass setting */
    {
        static const struct { const char *nm; int np; double kx[6], ky[6], tail; } CV[3] = {
            { "s18", 5, { 0, 1, 2, 3, 4 }, { 0, 0.89, 1.98, 3.14, 5.31 }, 2.17 },
            { "s20", 3, { 0, 1, 7 }, { 0, 0.991, 11.393 }, 1.734 },
            { "s11 (6x6 lens)", 5, { 0, 1, 2, 4, 8 }, { 0, 0.444, 1.593, 3.885, 9.901 }, 1.5 } };
        for (int cv = 0; cv < 3; cv++) {
            double off[40];
            for (int k = 0; k < 40; k++) { int last = (int)CV[cv].kx[CV[cv].np - 1];
                if (k >= last) off[k] = CV[cv].ky[CV[cv].np - 1] + CV[cv].tail * (k - last);
                else { int i = 0; while (CV[cv].kx[i + 1] < k) i++; off[k] = CV[cv].ky[i] + (CV[cv].ky[i + 1] - CV[cv].ky[i]) * (k - CV[cv].kx[i]) / (CV[cv].kx[i + 1] - CV[cv].kx[i]); } }
            double ev[2] = { -1, -1 };
            for (int mode = 0; mode < 2; mode++) {   /* 0 normal (up to 5, stop when covered), 1 the 8-pass setting */
                int st[8] = { 0, 1 }; double dx[8] = { 0, off[1] }; int n = 2;
                for (; n < (mode ? 8 : 5); n++) {
                    double ph[8]; for (int i = 0; i < n; i++) { double p = -dx[i]; p -= floor(p); ph[i] = p >= 1 ? 0 : p; }
                    double pos[8], mb = 1; for (int i = 0; i < n; i++) pos[i] = -dx[i]; sr_grid_cost2(pos, n, 2, &mb); (void)ph;
                    if (!mode && n >= 3 && mb <= SR_BIAS_OK) break;   /* the app's normal-setting rule (0.1.23) */
                    int k = sr_next_step2(dx, st, n, 2, mode, NULL, NULL); if (k < 0 || k > 32) k = st[n - 1] + 1; st[n] = k; dx[n] = off[k]; }
                printf("     %s curve, %s: steps", CV[cv].nm, mode ? "8 passes" : "normal"); for (int k = 0; k < n; k++) printf(" %+d (%.3f)", st[k], dx[k]); printf("\n");
                Case c = { mode ? "8-pass setting" : "normal setting", n, { 0 }, 2, 80, 0, 0, 0, 0.0055, { 0 }, -1 };
                for (int k = 0; k < n; k++) { c.steps[k] = st[k]; c.dxs[k] = dx[k]; }
                ev[mode] = run_case(&c);
            }
            CHECK(ev[0] > 0 && ev[1] > 0 && ev[1] < ev[0], "%s curve: 8 passes rms %.0f, normal setting %.0f", CV[cv].nm, ev[1], ev[0]);
        }
    }
    /* a pass of something else must be refused */
    {
        const int w = 200, h = 100, ch = 3; uint16_t *a = malloc((size_t)w * h * ch * 2), *b = malloc((size_t)w * h * ch * 2), *o = malloc((size_t)2 * w * h * ch * 2);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
            a[((size_t)y * w + x) * ch + c] = q16(cell(x, y, c, 1)); b[((size_t)y * w + x) * ch + c] = q16(30000 + 4000 * gauss()); }
        const uint16_t *pp[2] = { a, b }; SrInfo info;
        int r = sr_combine(pp, 2, w, h, ch, 2, NULL, o, &info);
        CHECK(r == -2, "a pass of other content refused (returned %d, score %.2f)", r, info.score[1]);
        free(a); free(b); free(o);
    }
    /* next lens step (0.1.22) */
    {
        double gd, ph;
        /* the scanner's offsets of s19 (35 mm lens): +1 step -> -0.707 px; the fixed choice was +2 (-1.379, griddist 0.24) */
        { double dx[2] = { 0, -0.707 }; int st[2] = { 0, 1 }; int k = sr_next_step(dx, st, 2, 2, &gd, &ph);
          CHECK(k == 2 && fabs(gd - 0.172) < 0.01, "s19 offsets: next step +%d, predicted griddist %.3f (by hand: phase .414 -> class 1 at 0.172, the first <= 0.2)", k, gd); }
        /* s18 (35 mm lens): +1 -> -0.89; the fixed +2, +3, +4 gave griddist 0.38 after 5 passes */
        { double dx[2] = { 0, -0.89 }; int st[2] = { 0, 1 }; int k = sr_next_step(dx, st, 2, 2, &gd, &ph);
          CHECK(k == 4 && fabs(gd - 0.12) < 0.01, "s18 offsets: next step +%d, predicted griddist %.3f (steps 2..7 by hand: .56 .34 .12 .10 .32 .46 -> +4, the first <= 0.2)", k, gd); }
        /* s11 (L89): +1 -> -0.444: fine positions 0 and 0.888 - the grid is covered already (griddist 0.112), so the
           normal setting chooses the step most likely to bring every class within SR_BIAS_OK over the uncertainty of
           the prediction (0.1.23 after review: before, the straight-line guess alone chose +6, which on the real curve
           lands 4.2 px further). Worked independently (scratch cost.py, the same rules in Python): chance / expected
           cost +2 .247/.0288 (u 0.35 px), +3 .244/.0285, +4..+6 .268 (phase unknown) -> all within 0.05 and 2 %: the
           best-known position, +2 (on the real curve +2 = 1.593 px: bias 0.006 - done after 3 passes) */
        { double dx[2] = { 0, -0.444 }; int st[2] = { 0, 1 }; int k = sr_next_step(dx, st, 2, 2, &gd, &ph);
          double pos[3] = { 0, 0.444, 1.593 }, mb = 9; sr_grid_cost2(pos, 3, 2, &mb);
          CHECK(k == 2 && mb < 0.01, "s11 offsets: next step +%d (by hand +2; with the real +2 the largest class bias %.3f)", k, mb); }
        /* the registration's search range: candidates predicted beyond max_off are not taken */
        { double dx[2] = { 0, -0.89 }; int st[2] = { 0, 1 }; int k = sr_next_step3(dx, st, 2, 2, 0, 3.0, &gd, &ph);
          CHECK(k >= 2 && 0.89 * k <= 3.0, "max_off 3 px: next step +%d (predicted %.2f px, not beyond 3)", k, 0.89 * k); }
        /* no step above SR_STEP_MAX, also when exploring */
        { double dx[2] = { 0, -0.02 * 23 }; int st[2] = { 0, 23 }; int k = sr_next_step(dx, st, 2, 2, &gd, &ph);
          CHECK(k <= SR_STEP_MAX, "after 0, +23: next step %+d (at most +%d)", k, SR_STEP_MAX); }
        /* the local slope after three passes: 0, 1, 5 -> 0, -0.89, -5.31 (s18's real +4 was -5.31: the relation is not linear) */
        { double dx[3] = { 0, -0.89, -5.31 }; int st[3] = { 0, 1, 5 }; int k = sr_next_step(dx, st, 3, 2, &gd, &ph);
          CHECK(k > 5 && k <= 5 + SR_STEP_RANGE, "after 0, +1, +5: next step +%d, increasing (predicted griddist %.3f)", k, gd); }
        /* no movement: no slope */
        { double dx[2] = { 0, -0.01 }; int st[2] = { 0, 1 }; CHECK(sr_next_step(dx, st, 2, 2, NULL, NULL) == -1, "the carriage does not move the picture: no step chosen"); }
        { double phs[3] = { 0, 0.25, 0.75 }; CHECK(fabs(sr_griddist(phs, 3, 2) - 0.5) < 1e-9, "griddist of phases 0, .25, .75 = 0.5 (review case)"); }
        /* balance (8 passes): offsets 0, -0.6, -1.2 -> phases 0, .6, .2 -> fine positions 0, 1.2, 0.4 (griddist 0.2:
           covered). Samples within 0.1 of a column: class 0 has one (0.0), class 1 none (1.2 is 0.2 off) -> class 1.
           Candidates (0.6 px per step): +3 -> 1.6 (0.6 off), +4 -> 0.8 (0.2), +5 -> 0.0 (1.0), +6 -> 1.2 (0.2), +7 -> 0.4
           (0.6), +8 -> 1.6 (0.6): the nearest is +4 (0.2, first of the two) */
        { double dx[3] = { 0, -0.6, -1.2 }; int st[3] = { 0, 1, 2 }; int k = sr_next_step2(dx, st, 3, 2, 1, &gd, &ph);
          CHECK(k == 4 && fabs(ph - 0.4) < 1e-9, "balance: covered grid -> next step +%d towards the class without a near sample (phase %.3f; by hand +4, 0.4)", k, ph); }
        { double dx[2] = { 0, -0.707 }; int st[2] = { 0, 1 }; CHECK(sr_next_step2(dx, st, 2, 2, 1, NULL, NULL) == 2, "balance mode before the grid is covered = the normal choice (+2)"); }
    }
    /* 0.1.24: the pass check on every offset the scanner gave (10 sessions, logs of 9 / 10 Oct): every real pass must be
       accepted, the three periodic mis-locks (s22 passes 3 and 5, s26 pass 4 along) refused; refused passes are then
       taken as re-measured (the values rawsr_hwcheck found on the real passes). Sign -1 (transparent, not mirrored);
       along tolerance 0.04 mm in lines (5324 dpi: 8.4; L89 at 2698 dpi: 4.25). */
    {
        static const struct { const char *nm; double tol; int n; int st[8]; double dx[8], dy[8], fixx[8], fixy[8]; } HS[] = {
            { "s13", 4.25, 3, { 0, 1, 2 }, { 0, -0.565, -1.580 }, { 0, 0.388, 0.205 } },
            { "s14", 4.25, 3, { 0, 1, 2 }, { 0, -0.437, -1.592 }, { 0, 2.063, 0.883 } },
            { "s16", 8.4, 3, { 0, 1, 2 }, { 0, -0.667, -1.483 }, { 0, 1.730, 2.189 } },
            { "s18", 8.4, 5, { 0, 1, 2, 3, 4 }, { 0, -0.890, -1.975, -3.142, -5.310 }, { 0, -1.651, 0.840, -1.565, 0.820 } },
            { "s19", 8.4, 3, { 0, 1, 2 }, { 0, -0.707, -1.379 }, { 0, -2.928, -0.262 } },
            { "s20", 8.4, 3, { 0, 1, 7 }, { 0, -0.991, -11.393 }, { 0, -1.976, -4.843 } },
            { "s21", 8.4, 5, { 0, 1, 3, 2, 4 }, { 0, -1.439, -4.120, -3.385, -6.971 }, { 0, -0.944, 2.256, -0.521, 2.712 } },
            { "s22", 8.4, 8, { 0, 1, 2, 0, 2, 0, 0, 0 }, { 0, -1.422, 3.458, -0.895, 27.861, -1.193, -1.328, -1.670 }, { 0, -1.346, 1.591, -1.333, 1.555, -1.400, 1.085, -1.619 },
              { 0, 0, -2.839, 0, -3.640 }, { 0, 0, 1.536, 0, 1.421 } },
            { "s25", 8.4, 8, { 0, 1, 0, 1, 1, 0, 1, 0 }, { 0, -1.528, -0.234, -1.430, -2.464, -1.297, -2.333, -1.410 }, { 0, -2.410, 0.604, -2.007, 0.811, -2.410, 0.357, -2.040 } },
            { "s26", 8.4, 8, { 0, 1, 5, 6, 0, 0, 1, 1 }, { 0, -0.670, -7.679, -9.402, -0.532, -0.565, -1.159, -1.449 }, { 0, -2.345, 0.529, -52.621, 0.493, -2.242, 1.126, -2.204 },
              { 0, 0, 0, -9.449 }, { 0, 0, 0, -2.281 } } };
        int bad_ok = 0, good_refused = 0, refused = 0; char rl[200] = "";
        for (unsigned si = 0; si < sizeof HS / sizeof HS[0]; si++) {
            double ax[8] = { 0 }, ay[8] = { 0 }; int as[8] = { HS[si].st[0] }, na = 1;
            for (int k = 1; k < HS[si].n; k++) {
                double ex[2], ey; int nex;
                int ok = sr_check_offset(ax, ay, as, na, HS[si].st[k], -1, HS[si].dx[k], HS[si].dy[k], HS[si].tol, ex, &nex, &ey);
                int mislock = HS[si].fixx[k] != 0 || HS[si].fixy[k] != 0;
                if (mislock && ok) bad_ok++;
                if (!mislock && !ok) good_refused++;
                if (!ok) { refused++; snprintf(rl + strlen(rl), sizeof rl - strlen(rl), " %s/%d (start %+.2f/%+.2f)", HS[si].nm, k + 1, ex[0], ey); }
                double ux = mislock ? HS[si].fixx[k] : HS[si].dx[k], uy = mislock ? HS[si].fixy[k] : HS[si].dy[k];
                if (mislock) { double e2[2], y2; int n2; if (!sr_check_offset(ax, ay, as, na, HS[si].st[k], -1, ux, uy, HS[si].tol, e2, &n2, &y2)) good_refused++; }
                ax[na] = ux; ay[na] = uy; as[na] = HS[si].st[k]; na++;
            }
        }
        CHECK(bad_ok == 0 && good_refused == 0 && refused == 3, "pass check on the hardware offsets: %d refused (%s), real passes refused %d, mis-locks let through %d", refused, rl, good_refused, bad_ok);
        /* review 0.1.24: a long jump with one-period aliases (6.3 px bars): steps 0 / +1 known (-1.25), +7 true -8.75 */
        { double ax[3] = { 0, -1.25 }, ay[3] = { 0, 0 }; int as[3] = { 0, 1 }; double ex[2], ey; int nex;
          int t = sr_check_offset(ax, ay, as, 2, 7, -1, -8.75, 0, 8.4, ex, &nex, &ey), a = sr_check_offset(ax, ay, as, 2, 7, -1, -2.45, 0, 8.4, ex, &nex, &ey), b = sr_check_offset(ax, ay, as, 2, 7, -1, -15.05, 0, 8.4, ex, &nex, &ey);
          CHECK(t && !a && !b, "long jump +1 -> +7: the true -8.75 accepted (%d), the one-period aliases -2.45 / -15.05 refused (%d %d)", t, !a, !b); }
        /* sign unknown (0): both directions allowed; the s22 mis-lock is still refused (beyond 2.2 px per step + slack) */
        { double ax[3] = { 0, -1.422 }, ay[3] = { 0, -1.346 }; int as[3] = { 0, 1 }; double ex[2], ey; int nex;
          int a = sr_check_offset(ax, ay, as, 2, 2, 0, 27.861, 1.555, 8.4, ex, &nex, &ey), b = sr_check_offset(ax, ay, as, 2, 2, 0, -2.839, 1.536, 8.4, ex, &nex, &ey);
          CHECK(!a && b && nex == 2, "sign unknown: +27.86 at step +2 refused, -2.84 accepted, two starts (%+.2f, %+.2f)", ex[0], ex[1]); }
    }
    /* 0.1.25: the lens shift test's series fit on the hardware's own series (s27, pictures 1 - 6 straight and 7 - 12 from
       above, across, rel. to the series' first; worked independently in Python with numpy lstsq: straight step -0.933 rms
       0.168 (n-2 dof: 0.206); from above no jump: -1.538, rms 0.534 (n-2: 0.654); one jump before picture 3 of the
       series: step -1.272, jump -1.169) */
    {
        int k[6] = { 0, 1, 2, 3, 0, 2 }; double st[6] = { 0, -0.621, -1.466, -2.872, 0.164, -1.603 }, ab[6] = { 0, -1.086, -3.252, -5.317, -1.241, -3.400 };
        double sl, rms, dev, jump, slj, rmsj; int jp;
        sr_fit_series(st, k, 6, &sl, &rms, &dev, &jp, &jump, &slj, &rmsj);
        CHECK(fabs(sl + 0.933) < 0.002 && fabs(rms - 0.206) < 0.002, "series fit, s27 straight: %.3f px/step, scatter %.3f (by hand -0.933, 0.206)", sl, rms);
        sr_fit_series(ab, k, 6, &sl, &rms, &dev, &jp, &jump, &slj, &rmsj);
        CHECK(fabs(sl + 1.538) < 0.002 && fabs(rms - 0.654) < 0.002 && jp == 2 && fabs(jump + 1.169) < 0.002 && fabs(slj + 1.272) < 0.002,
              "series fit, s27 from above: %.3f px/step (scatter %.3f); one jump before picture %d: %+.3f px, step %.3f (by hand -1.538 / 0.654; 3, -1.169, -1.272)", sl, rms, jp + 1, jump, slj);
    }
    /* 0.3.0: sr_calib_fit - an exact synthetic series, then the hardware's from-above series (35 mm lens) */
    { int k[10] = { 0, 1, 2, 3, 4, 6, 8, 0, 2, 5 }; double dx[10], dy[10];
      for (int i = 0; i < 10; i++) { dx[i] = 0.2 - 1.4 * k[i] + 0.03 * i; dy[i] = 0.01 * i; }
      dx[1] += 0.0; SrCalib c; sr_calib_fit(dx, dy, k, 10, &c);
      CHECK(fabs(c.slope + 1.4) < 1e-9 && fabs(c.drift - 0.03) < 1e-9 && c.rms < 1e-9 && fabs(c.first - 1) < 1e-9 && c.repeat < 1e-9 && fabs(c.along - 0.09) < 1e-9 && c.ok,
            "calib fit exact: slope %.3f drift %.3f rms %.3g first %.3f repeat %.3g along %.3f ok %d", c.slope, c.drift, c.rms, c.first, c.repeat, c.along, c.ok);
      dx[1] = dx[0] - 0.3 * 1.4 + 0.03; sr_calib_fit(dx, dy, k, 10, &c);   /* a short first step (0.3 of a step) */
      CHECK(c.first > 0.2 && c.first < 0.5 && c.ok, "calib fit short first step: first %.3f (0.3 put in), slope %.3f, rms %.3f", c.first, c.slope, c.rms);
      double z[10] = { 0 }; sr_calib_fit(z, dy, k, 10, &c); CHECK(!c.ok && fabs(c.slope) < 1e-9, "calib fit no movement: not ok (slope %.3f)", c.slope); }
    { /* s32 (11 Oct, 0.1.27) lens shift test, from above, SLOW moves: pictures 8, 10, ..., 18 (log) */
      int k[6] = { 0, 1, 2, 3, 0, 2 }; double dx[6] = { -0.483, -2.119, -3.450, -5.478, -0.476, -4.298 }, dy[6] = { -0.250, -0.300, -0.357, -0.269, -0.333, -0.288 };
      SrCalib c; sr_calib_fit(dx, dy, k, 6, &c);
      CHECK(c.slope < -1.5 && c.slope > -1.8 && c.ok, "calib fit s32 slow: %.3f px/step (thin lens 1.573), drift %+.3f/picture, rms %.3f, repeat %.3f, first %.2f, along %.3f, ok %d", c.slope, c.drift, c.rms, c.repeat, c.first, c.along, c.ok);
      /* s30 (10 Oct) from above, normal moves: pictures 7 - 12 */
      double dx2[6] = { -0.736, -2.435, -4.205, -5.744, -1.296, -4.110 }; sr_calib_fit(dx2, NULL, k, 6, &c);
      CHECK(c.slope < -1.4 && c.slope > -1.9 && c.ok, "calib fit s30 normal from above: %.3f px/step, drift %+.3f, rms %.3f, first %.2f", c.slope, c.drift, c.rms, c.first); }
    /* 0.3.0: the model predictor (sr_pred) on real runs, one pass ahead: s33 (6x6 lens, slow, prior = thin lens 1.2445)
       and s31 (35 mm lens, slow, prior 1.54 = the mean of the slow runs); the offline replay gave 0.13 / 0.22 and 0.13 / 0.56 */
    { struct { const char *nm; int n; int k[8]; double dx[8]; double prior; double lim_med; } R[2] = {
        { "s33", 8, { 0, 1, 4, 5, 7, 10, 12, 14 }, { 0, -1.378, -5.168, -6.610, -8.927, -12.818, -15.150, -17.768 }, -1.2445, 0.2 },
        { "s31", 8, { 0, 1, 5, 0, 5, 2, 2, 2 }, { 0, -1.905, -8.524, -0.749, -8.585, -3.976, -4.016, -4.051 }, -1.54, 0.3 } };
      for (int r = 0; r < 2; r++) {
          double em[8]; int ne = 0;
          for (int n = 2; n < R[r].n; n++) {
              sr_pred.on = 1; sr_pred.prior = R[r].prior; double pm = sr_model_pred(R[r].dx, R[r].k, n, R[r].k[n], NULL);
              em[ne++] = fabs(pm - R[r].dx[n]); }
          double t[8]; memcpy(t, em, sizeof t); for (int i = 1; i < ne; i++) for (int j = i; j > 0 && t[j] < t[j - 1]; j--) { double q = t[j]; t[j] = t[j - 1]; t[j - 1] = q; }
          double med = ne % 2 ? t[ne / 2] : 0.5 * (t[ne / 2 - 1] + t[ne / 2]);
          CHECK(med <= R[r].lim_med, "model predictor %s: median one-pass-ahead error %.3f px (largest %.3f) over %d passes", R[r].nm, med, t[ne - 1], ne);
      }
      sr_pred.on = 0; sr_pred.prior = 0; }
    printf("%s (%d failed)\n", fails ? "FAIL" : "ALL PASS", fails);
    return fails != 0;
}
