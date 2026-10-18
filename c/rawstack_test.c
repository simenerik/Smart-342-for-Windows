/* rawstack_test.c - checks rawstack.c with synthetic passes whose true shift is known exactly.
 * The picture is a band-limited random texture (sum of 300 sinusoids up to 0.3 cycles/pixel, random phases and
 * directions), evaluated analytically at x - sx, y - sy, so a shifted pass is exact (no interpolation in the test
 * data), plus Gaussian noise per pass. Build: cc -O2 -o rawstack_test rawstack_test.c rawstack.c -lm */
#include "rawstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#define NS 300
static double fq[NS][2], ph[NS], am[NS], fmax_ = 0.3;
#include <time.h>
static unsigned long long rng = 88172645463325252ULL;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static double grand(void) { double u = urand(), v = urand(); if (u < 1e-12) u = 1e-12; return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
static double tex(double x, double y, int c) {
    double s = 0; for (int i = 0; i < NS; i++) s += am[i] * cos(2 * M_PI * (fq[i][0] * x + fq[i][1] * y) + ph[i] + c);
    return 30000 + 9000 * s;
}
static void make(uint16_t *p, int w, int h, double sx, double sy, double noise) {
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++) {
        double v = tex(x - sx, y - sy, c) + noise * grand();
        p[((size_t)y * w + x) * 3 + c] = v < 0 ? 0 : v > 65535 ? 65535 : (uint16_t)(v + 0.5);
    }
}
int main(void) {
    double tot = 0;
    unsigned long long seed = rng;
again:
    rng = seed; tot = 0;
    for (int i = 0; i < NS; i++) {
        double f = fmax_ * sqrt(urand()), a = 2 * M_PI * urand();
        fq[i][0] = f * cos(a); fq[i][1] = f * sin(a); ph[i] = 2 * M_PI * urand(); am[i] = 1.0 / (1 + 20 * f); tot += am[i];
    }
    for (int i = 0; i < NS; i++) am[i] /= tot / 3;
    static int w = 600, h = 400, fail = 0, bias_pass = 0;
    if (bias_pass) {   /* the sub-pixel bias against the texture's bandwidth (reported, not a pass/fail check) */
        uint16_t *a = malloc((size_t)w * h * 6), *b = malloc((size_t)w * h * 6);
        make(a, w, h, 0, 0, 0); make(b, w, h, 3.3, -1.7, 0);
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in);
        clock_t t0 = clock(); rs_add(&s, b, &in); double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        printf("INFO bandwidth %.2f cycles/pixel: shift 3.30 -1.70 found %.4f %.4f (error %.4f %.4f), rs_add %.0f ms for %d x %d x 3\n",
               fmax_, in.dx, in.dy, in.dx - 3.3, in.dy + 1.7, ms, w, h);
        rs_free(&s); free(a); free(b);
        if (fmax_ > 0.1) { fmax_ /= 2; goto again; }
        printf(fail ? "SOME FAILED\n" : "ALL PASS\n"); return fail;
    }
    uint16_t *a = malloc((size_t)w * h * 6), *b = malloc((size_t)w * h * 6), *out = malloc((size_t)w * h * 6);
    /* 1. registration accuracy */
    static const double S[][2] = { { 0, 0 }, { 3.3, -1.7 }, { 0.5, 0.25 }, { -12.6, 7.1 }, { 90.4, -60.8 }, { 0.1, -0.05 }, { -2, 3 } };
    static const double N[] = { 0, 200 };
    for (int ni = 0; ni < 2; ni++) {
        make(a, w, h, 0, 0, N[ni]);
        for (size_t k = 0; k < sizeof S / sizeof *S; k++) {
            make(b, w, h, S[k][0], S[k][1], N[ni]);
            RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in); rs_add(&s, b, &in);
            double ex = in.dx - S[k][0], ey = in.dy - S[k][1], tmax = 0;
            for (int t = 0; t < 9; t++) { double e = hypot(in.tdx[t] - S[k][0], in.tdy[t] - S[k][1]); if (e > tmax) tmax = e; }
            double lim = 0.02;   /* measured bias of the Lanczos/Lucas-Kanade step on these textures: below 0.01 pixel */ int ok = fabs(ex) < lim && fabs(ey) < lim && tmax < 5 * lim;
            printf("%s noise %3.0f shift %7.2f %7.2f -> found %8.4f %8.4f (error %.4f %.4f, worst tile %.4f) score %.4f %s\n",
                   ok ? "PASS" : "FAIL", N[ni], S[k][0], S[k][1], in.dx, in.dy, ex, ey, tmax, in.score, in.resampled ? "resampled" : "copied");
            if (!ok) fail = 1;
            rs_free(&s);
        }
    }
    /* 2. whole-pixel shift without noise: the mean equals the original exactly where both passes cover */
    {
        make(a, w, h, 0, 0, 0); make(b, w, h, -2, 3, 0);
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in); rs_add(&s, b, &in); rs_result(&s, out);
        long bad = 0; for (size_t i = 0; i < (size_t)w * h * 3; i++) if (out[i] != a[i]) bad++;
        printf("%s whole-pixel shift: %ld samples differ from the original (cover %d..%d x %d..%d)\n", bad ? "FAIL" : "PASS", bad,
               s.cover[1].x0, s.cover[1].x1, s.cover[1].y0, s.cover[1].y1);
        if (bad) fail = 1;
        rs_free(&s);
    }
    /* 3. averaging: N passes with random sub-pixel shifts and noise sigma; error of the mean against the clean picture */
    for (int np = 1; np <= 16; np *= 2) {
        double sig = 400; RawStack s; rs_init(&s, w, h, 3); RsInfo in; double first_rms = 0;
        for (int k = 0; k < np; k++) {
            double sx = k ? (urand() - 0.5) * 6 : 0, sy = k ? (urand() - 0.5) * 6 : 0;
            make(b, w, h, sx, sy, sig); rs_add(&s, b, &in);
            if (k == 1) first_rms = in.drms[1][1] + in.drms[1][2];
        }
        rs_result(&s, out); make(a, w, h, 0, 0, 0);
        double se = 0; long n = 0;
        for (int y = 20; y < h - 20; y++) for (int x = 20; x < w - 20; x++) for (int c = 0; c < 3; c++) {
            double d = (double)out[((size_t)y * w + x) * 3 + c] - a[((size_t)y * w + x) * 3 + c]; se += d * d; n++; }
        double rms = sqrt(se / n), ideal = sig / sqrt(np);
        int ok = rms < ideal * 1.25 + 30;
        printf("%s %2d passes, noise %.0f: error of the mean %.1f (ideal sigma/sqrt(N) %.1f)%s\n", ok ? "PASS" : "FAIL", np, sig, rms, ideal,
               np > 1 ? "" : "");
        (void)first_rms; if (!ok) fail = 1;
        rs_free(&s);
    }
    /* 3b. detail kept: 4 noise-free passes with sub-pixel shifts; error of the mean against the clean picture,
       relative to the texture's own rms (resampling softens fine detail a little; pass 1 is never resampled) */
    {
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; static const double sh[4][2] = { { 0, 0 }, { 0.5, 0.5 }, { -1.25, 0.75 }, { 2.5, -0.5 } };
        for (int k = 0; k < 4; k++) { make(b, w, h, sh[k][0], sh[k][1], 0); rs_add(&s, b, &in); }
        rs_result(&s, out); make(a, w, h, 0, 0, 0);
        double se = 0, ss = 0; long n = 0;
        for (int y = 20; y < h - 20; y++) for (int x = 20; x < w - 20; x++) for (int c = 0; c < 3; c++) {
            size_t o = ((size_t)y * w + x) * 3 + c; double d = (double)out[o] - a[o], v = a[o] - 30000.0; se += d * d; ss += v * v; n++; }
        double rel = sqrt(se / ss); int ok = rel < 0.03;
        printf("%s detail: 4 noise-free passes at half/quarter-pixel shifts, error of the mean %.2f %% of the texture rms\n", ok ? "PASS" : "FAIL", 100 * rel);
        if (!ok) fail = 1;
        rs_free(&s);
    }
    /* 5. pictures the registration cannot or must not align */
    {
        /* like the simulator's test pattern: R = ramp across, G = sawtooth along (period 512 rows), B constant;
           two identical passes -> no shift (across is not measurable: a ramp matches at every shift) */
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint16_t *p = a + ((size_t)y * w + x) * 3;
            p[0] = (uint16_t)(x * 65535 / (w - 1)); p[1] = (uint16_t)(((y * 8) & 4095) * 16); p[2] = 32768; }
        memcpy(b, a, (size_t)w * h * 6);
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in); rs_add(&s, b, &in); rs_result(&s, out);
        int ok = in.used && fabs(in.dx) < 1e-6 && fabs(in.dy) < 1e-6 && !memcmp(out, a, (size_t)w * h * 6);
        printf("%s ramp/sawtooth pattern (simulator-like), identical passes: shift %.4f %.4f, weak directions %d, used %d, mean == pass 1: %s\n",
               ok ? "PASS" : "FAIL", in.dx, in.dy, in.weak, in.used, !memcmp(out, a, (size_t)w * h * 6) ? "yes" : "no");
        if (!ok) fail = 1;
        rs_free(&s);
        /* a pass of a different picture: left out */
        make(a, w, h, 0, 0, 0); for (size_t i = 0; i < (size_t)w * h * 3; i++) b[i] = (uint16_t)(30000 + 1000 * grand());
        rs_init(&s, w, h, 3); rs_add(&s, a, &in); rs_add(&s, b, &in); rs_result(&s, out);
        ok = !in.used && s.n == 1 && !memcmp(out, a, (size_t)w * h * 6);
        printf("%s unrelated pass: score %.3f, used %d, passes in the mean %d\n", ok ? "PASS" : "FAIL", in.score, in.used, s.n);
        if (!ok) fail = 1;
        rs_free(&s);
        /* blank passes (no detail at all): added without shift */
        for (size_t i = 0; i < (size_t)w * h * 3; i++) a[i] = b[i] = 20000;
        rs_init(&s, w, h, 3); rs_add(&s, a, &in); rs_add(&s, b, &in); rs_result(&s, out);
        ok = in.used && in.dx == 0 && in.dy == 0 && !memcmp(out, a, (size_t)w * h * 6);
        printf("%s blank passes: score %.0f, shift %.1f %.1f, used %d\n", ok ? "PASS" : "FAIL", in.score, in.dx, in.dy, in.used);
        if (!ok) fail = 1;
        rs_free(&s);
    }
    /* 6. repetitive detail (period 40 pixels across and 24 along, plus a little texture): the nearest match */
    {
        for (int k = 0; k < 2; k++) {
            double sx = k ? 3.3 : 0, sy = k ? -1.7 : 0; uint16_t *p = k ? b : a;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 3; c++)
                p[((size_t)y * w + x) * 3 + c] = (uint16_t)(30000 + 8000 * cos(2 * M_PI * (x - sx) / 40) * cos(2 * M_PI * (y - sy) / 24) + 0.1 * (tex(x - sx, y - sy, c) - 30000));
        }
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in); rs_add(&s, b, &in);
        int ok = fabs(in.dx - 3.3) < 0.05 && fabs(in.dy + 1.7) < 0.05;
        printf("%s repetitive detail: shift 3.30 -1.70 found %.4f %.4f (score %.4f)\n", ok ? "PASS" : "FAIL", in.dx, in.dy, in.score);
        if (!ok) fail = 1;
        rs_free(&s);
    }
    /* 7. cases found by the independent review (2026-10-08): fine broadband texture on a large picture, where a
       small pyramid top level ranked a far shift first; and a 3-sinusoid texture (exact near-periods) in 3 channels,
       where the channel sum cancelled detail */
    {
        int W = 1100, H = 1700; uint16_t *A = malloc((size_t)W * H * 2), *B = malloc((size_t)W * H * 2);
        static double gfx[60], gfy[60], gph[60], gam[60]; unsigned r = 7;
        #define RND() ((r = r * 1103515245u + 12345u, (r >> 8) / 16777216.0))
        for (int i = 0; i < 60; i++) { double f = 0.02 + RND() * 0.8, an = RND() * 6.283; gfx[i] = f * cos(an); gfy[i] = f * sin(an); gph[i] = RND() * 6.283; gam[i] = 1500 * (0.3 + RND()) / (1 + f * 3); }
        static const double SH[2][2] = { { 5.5, -6.5 }, { 8.1, 8.1 } };
        for (int k = 0; k < 2; k++) {
            for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) for (int p = 0; p < 2; p++) {
                double sx = p ? SH[k][0] : 0, sy = p ? SH[k][1] : 0, v = 25000;
                for (int i = 0; i < 60; i++) v += gam[i] * sin(gfx[i] * (x + sx) + gfy[i] * (y + sy) + gph[i]);
                (p ? B : A)[(size_t)y * W + x] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
            }
            RawStack s; rs_init(&s, W, H, 1); RsInfo in; rs_add(&s, A, &in); rs_add(&s, B, &in);
            int ok = in.used && fabs(in.dx + SH[k][0]) < 0.02 && fabs(in.dy + SH[k][1]) < 0.02;
            printf("%s review case: broadband texture 1100 x 1700, shift %.1f %.1f -> found %.4f %.4f (score %.4f)\n", ok ? "PASS" : "FAIL", -SH[k][0], -SH[k][1], in.dx, in.dy, in.score);
            if (!ok) fail = 1;
            rs_free(&s);
        }
        free(A); free(B);
        int w3 = 200, h3 = 240; uint16_t *a3 = malloc((size_t)w3 * h3 * 6), *b3 = malloc((size_t)w3 * h3 * 6);
        for (int y = 0; y < h3; y++) for (int x = 0; x < w3; x++) for (int c = 0; c < 3; c++) for (int p = 0; p < 2; p++) {
            double X = x + (p ? 1 : 0), Y = y + (p ? 0.5 : 0);
            (p ? b3 : a3)[((size_t)y * w3 + x) * 3 + c] = (uint16_t)(20000 + 8000 * sin(X * 0.37 + c) + 6000 * cos(Y * 0.29 + X * 0.11) + 4000 * sin((X + Y) * 0.71 + c * 2)); }
        RawStack s; rs_init(&s, w3, h3, 3); RsInfo in; rs_add(&s, a3, &in); rs_add(&s, b3, &in);
        int ok = in.used && fabs(in.dx + 1) < 0.02 && fabs(in.dy + 0.5) < 0.02;
        printf("%s review case: 3-sinusoid texture 200 x 240 x 3, shift -1 -0.5 -> found %.4f %.4f (channel %d, score %.4f)\n", ok ? "PASS" : "FAIL", in.dx, in.dy, in.chan, in.score);
        if (!ok) fail = 1;
        rs_free(&s); free(a3); free(b3);
    }
    /* 4. difference statistics: two passes, no shift, noise sigma -> rms difference ~ sigma * sqrt(2) */
    {
        make(a, w, h, 0, 0, 300); make(b, w, h, 0, 0, 300);
        RawStack s; rs_init(&s, w, h, 3); RsInfo in; rs_add(&s, a, &in); rs_add(&s, b, &in);
        double sq = 0; long long n = 0; for (int bb = 0; bb < 4; bb++) { sq += in.drms[1][bb] * in.drms[1][bb] * in.dn[1][bb]; n += in.dn[1][bb]; }
        double r = sqrt(sq / n); int ok = fabs(r / (300 * sqrt(2)) - 1) < 0.05;
        printf("%s difference statistics: rms %.1f, expected %.1f (%s)\n", ok ? "PASS" : "FAIL", r, 300 * sqrt(2), in.resampled ? "resampled" : "copied");
        if (!ok) fail = 1;
        rs_free(&s);
    }
    free(a); free(b); free(out);
    bias_pass = 1; goto again;
}
