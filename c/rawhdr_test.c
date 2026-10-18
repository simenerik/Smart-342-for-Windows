/* rawhdr_test.c - checks rawhdr.c (with rawstack.c for the alignment) on a synthetic slide-like scene with a known
 * exposure ratio k, a dark-level offset, read noise and a sub-pixel shift of the brighter pass.
 * Scene: S = 65532 * 10^-D, density D from about 0 to 3.6 (smooth field + fine detail, analytic, so the shifted pass is
 * exact). normal = S + noise, brighter = k * S + offset + noise, both clipped at 65532.
 * Build: cc -O2 -o rawhdr_test rawhdr_test.c rawhdr.c rawstack.c -lm */
#include "rawhdr.h"
#include "rawstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static double grand(void) { double u = urand(), v = urand(); if (u < 1e-12) u = 1e-12; return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
static double dens(double x, double y, int c) {
    return 1.8 + 1.1 * sin(x * 0.011 + 0.5 * c) * cos(y * 0.007) + 0.6 * sin((x + 2 * y) * 0.023) + 0.12 * sin(x * 0.31 + c) * sin(y * 0.27)
         + 0.15 * sin(x * 1.1 + c) * sin(y * 0.9);   /* fine detail (about 0.16 cycles/pixel): alignment interpolation softens it a little */
}
static double scene(double x, double y, int c) { return 65532.0 * pow(10, -dens(x, y, c)); }
int main(void) {
    int w = 700, h = 500, ch = 3, fail = 0; size_t ns = (size_t)w * h * ch;
    double sigma = 8, ks[2] = { 2, 4 }, off = 120, sx = 0.4, sy = -0.3;
    uint16_t *n = malloc(ns * 2), *b = malloc(ns * 2), *reg = malloc(ns * 2), *out = malloc(ns * 2); float *al = malloc(ns * 4);
    for (int ki = 0; ki < 2; ki++) {
        double k = ks[ki], err[2][4] = { { 0 } }, ref_n[4] = { 0 }; HdrInfo hi; RsInfo in; double g0 = 0; int r = -9;
        /* run 0: no noise (the error left is the alignment interpolation of fine detail); run 1: with noise */
        for (int run = 0; run < 2; run++) {
            double sg = run ? sigma : 0;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
                size_t i = ((size_t)y * w + x) * ch + c;
                double v = scene(x, y, c) + sg * grand(); n[i] = v < 0 ? 0 : v > 65532 ? 65532 : (uint16_t)(v + 0.5);
                double u = k * scene(x + sx, y + sy, c) + off + sg * grand(); b[i] = u < 0 ? 0 : u > 65532 ? 65532 : (uint16_t)(u + 0.5);
            }
            int ra = hdr_align(n, b, w, h, ch, al, &in, &g0);
            if (ra) { printf("FAIL hdr_align %d\n", ra); fail = 1; break; }
            r = hdr_merge(n, al, w, h, ch, out, &hi);
            double se[4] = { 0 }, sn_[4] = { 0 }; long long cnt[4] = { 0 };
            for (int y = 10; y < h - 10; y++) for (int x = 10; x < w - 10; x++) for (int c = 0; c < ch; c++) {
                size_t i = ((size_t)y * w + x) * ch + c; double S = scene(x, y, c); int band = S < 0.02 * 65532 ? 0 : S < 0.06 * 65532 ? 3 : S > 0.5 * 65532 ? 2 : 1;   /* 3: 2-6 %, where the passes are blended */
                double e = out[i] - S, e0 = n[i] - S; se[band] += e * e; sn_[band] += e0 * e0; cnt[band]++;
            }
            for (int bnd = 0; bnd < 4; bnd++) { err[run][bnd] = sqrt(se[bnd] / cnt[bnd]); if (run) ref_n[bnd] = sqrt(sn_[bnd] / cnt[bnd]); }
        }
        /* expected shadow error: the noise of the brighter pass scaled back (sigma / k) together with the interpolation
           error measured without noise */
        double exp_sh = sqrt(pow(ref_n[0] / k, 2) + pow(err[0][0], 2));
        int ok = r == 0 && hi.spill_frac < 0.002 && fabs(in.dx + sx) < 0.02 && fabs(in.dy + sy) < 0.02
              && fabs(hi.g[0] - k) < 0.01 * k && fabs(hi.g[1] - k) < 0.01 * k && fabs(hi.g[2] - k) < 0.01 * k
              && fabs(hi.o[0] - off) < 5 && fabs(hi.o[1] - off) < 5 && fabs(hi.o[2] - off) < 5
              /* shadows: the brighter pass is used only where the whole 9 x 9 neighbourhood is dark (protection against
                 interpolation ringing next to bright edges, see rawhdr_edge_test), so part of the shadows next to brighter
                 detail keeps the normal pass: up to 1.4 x the ideal error, and clearly better than the normal pass */
              && err[1][0] < exp_sh * 1.4 && err[1][0] < ref_n[0] / (k == 2 ? 1.6 : 2.0) && err[1][1] < ref_n[1] * 1.05 && err[1][3] < ref_n[3] * 1.1;
        printf("%s k = %.0f: rough gain %.3f, shift found %.3f %.3f (true %.2f %.2f), fitted g %.3f %.3f %.3f, o %.1f %.1f %.1f (true %.0f)\n",
               ok ? "PASS" : "FAIL", k, g0, in.dx, in.dy, -sx, -sy, hi.g[0], hi.g[1], hi.g[2], hi.o[0], hi.o[1], hi.o[2], off);
        printf("     shadows (<2 %%): error %.2f (normal pass %.2f; expected %.2f = noise/k %.2f with interpolation %.2f); 2-6 %%: %.2f (%.2f); 6-50 %%: %.2f (%.2f); highlights %.2f (%.2f, scene clipped)\n",
               err[1][0], ref_n[0], exp_sh, ref_n[0] / k, err[0][0], err[1][3], ref_n[3], err[1][1], ref_n[1], err[1][2], ref_n[2]);
        printf("     from brighter %.1f %%, blended %.1f %%, normal %.1f %%; spill check rejected %.3f %% (no spill in this picture)\n", 100 * hi.frac_bright, 100 * hi.frac_blend, 100 * hi.frac_normal, 100 * hi.spill_frac);
        if (!ok) fail = 1;
    }
    (void)reg;

    /* 0.4.1: green spill-over in the brighter pass (as on the hardware, 13 Oct): short streaks of green far above k x
       the normal pass in dark parts, red and blue untouched. The merge must keep the normal pass there. */
    {
        double k = 4; int nsp = 0; double worst = 0, worst0 = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
            size_t i = ((size_t)y * w + x) * ch + c;
            double v = scene(x, y, c) + sigma * grand(); n[i] = v < 0 ? 0 : v > 65532 ? 65532 : (uint16_t)(v + 0.5);
            double u = k * scene(x, y, c) + off + sigma * grand(); b[i] = u < 0 ? 0 : u > 65532 ? 65532 : (uint16_t)(u + 0.5);
        }
        /* 40 streaks, 1 - 2 px wide and 3 - 12 px long along the CCD line, in places where the scene is dark */
        unsigned char *sp = calloc((size_t)w * h, 1);
        for (int t = 0; t < 4000 && nsp < 40; t++) {
            int x = 30 + (int)(urand() * (w - 60)), y = 30 + (int)(urand() * (h - 60));
            if (scene(x, y, 1) > 0.01 * 65532) continue;
            int len = 3 + (int)(urand() * 10), wid = 1 + (urand() < 0.5);
            for (int yy = y; yy < y + wid; yy++) for (int xx = x; xx < x + len; xx++) { size_t i = ((size_t)yy * w + xx) * ch + 1; b[i] = (uint16_t)(20000 + urand() * 15000); sp[(size_t)yy * w + xx] = 1; }
            nsp++;
        }
        for (size_t i = 0; i < ns; i++) al[i] = b[i];          /* no shift in this test: the passes are aligned as made */
        HdrInfo hi; int r = hdr_merge(n, al, w, h, ch, out, &hi);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (sp[(size_t)y * w + x]) {
            size_t i = ((size_t)y * w + x) * ch + 1; double S = scene(x, y, 1);
            double e = fabs(out[i] - S); if (e > worst) worst = e;
            double e0 = fabs(((double)b[i] - off) / k - S); if (e0 > worst0) worst0 = e0;
        }
        int ok = r == 0 && worst < 6 * sigma && hi.spill_frac < 0.02;
        printf("%s green spill-over: %d streaks; largest error in them %.1f (normal-pass noise %.0f; without the check up to %.0f); rejected %lld px = %.2f %% of the brighter-pass pixels\n",
               ok ? "PASS" : "FAIL", nsp, worst, sigma, worst0, hi.spill_px, 100 * hi.spill_frac);
        if (!ok) fail = 1;
        free(sp);
    }

    /* 0.4.1 review: a steady offset of the longer pass in the shadows only (level dependent - the fit's straight line
       cannot take it out): 30 units (0.05 % of white) where the scene is below 400. The spill check measures from each
       level's typical difference, so it must not reject noisy pixels there (against 0 it rejected ~23 %). */
    {
        double k = 4;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
            size_t i = ((size_t)y * w + x) * ch + c; double S = scene(x, y, c);
            double v = S + sigma * grand(); n[i] = v < 0 ? 0 : v > 65532 ? 65532 : (uint16_t)(v + 0.5);
            double u = k * (S + (S < 400 ? 30 : 0)) + off + sigma * grand(); b[i] = u < 0 ? 0 : u > 65532 ? 65532 : (uint16_t)(u + 0.5);
            al[i] = b[i];
        }
        HdrInfo hi; int r = hdr_merge(n, al, w, h, ch, out, &hi);
        int ok = r == 0 && hi.spill_px >= 0 && hi.spill_frac < 0.005;
        printf("%s steady shadow offset of the longer pass (30 units below level 400): spill check rejected %.3f %% (must stay small)\n", ok ? "PASS" : "FAIL", 100 * hi.spill_frac);
        if (!ok) fail = 1;
    }
    /* the brighter pass is not brighter (exposure not applied): -2 and a copy of the normal pass */
    {
        for (size_t i = 0; i < ns; i++) al[i] = n[i];
        HdrInfo hi; int r = hdr_merge(n, al, w, h, ch, out, &hi); int same = 1; for (size_t i = 0; i < ns; i++) if (out[i] != n[i]) { same = 0; break; }
        int ok = r == -2 && same;
        printf("%s same exposure twice: result %d (expected -2), g %.3f, output = normal pass: %s\n", ok ? "PASS" : "FAIL", r, hi.g[1], same ? "yes" : "no");
        if (!ok) fail = 1;
    }
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
