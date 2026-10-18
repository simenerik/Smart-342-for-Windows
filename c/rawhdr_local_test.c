/* rawhdr_local_test.c - 0.4.1: the HDR alignment per band along the scan and per channel (rawhdr.c local_align).
 * The brighter pass is made with a shift that changes along the scan (rows) - a slow drift plus a step, as measured on
 * the hardware on 13 Oct (1.13 -> 1.00 px with a 0.12 px jump) - and differs per channel (red 0.08 px across from
 * green). Checks: the per-band shifts found match the true ones, the aligned brighter pass matches the true picture
 * better than with one whole-picture shift, a constant shift is still found as constant, and a picture without dark
 * detail falls back to the whole-picture shift.
 * Build: cc -O2 -o rawhdr_local_test rawhdr_local_test.c rawhdr.c rawstack.c -lm */
#include "rawhdr.h"
#include "rawstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static unsigned long long rng = 0x9E3779B97F4A7C15ULL;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static double grand(void) { double u = urand(), v = urand(); if (u < 1e-12) u = 1e-12; return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
/* a dark slide-like scene (most of it in the shadows, where the merge uses the brighter pass) with detail at many scales */
static double dens(double x, double y, int c) {
    return 2.2 + 0.7 * sin(x * 0.013 + 0.5 * c) * cos(y * 0.009) + 0.4 * sin((x + 2 * y) * 0.031) + 0.2 * sin(x * 0.29 + c) * sin(y * 0.23)
         + 0.12 * sin(x * 0.9 + 0.3 * c) * sin(y * 0.7 + c);
}
static double scene(double x, double y, int c) { return 65532.0 * pow(10, -dens(x, y, c)); }
static int H = 1600;
/* true shift of the brighter pass at row y, channel c: b(x, y) = k S(x + sx, y + sy) -> found shift = (-sx, -sy) */
static int mode;   /* 0 drifting + step + per-channel, 1 constant */
static void truth(double y, int c, double *sx, double *sy) {
    if (mode == 1) { *sx = 0.4; *sy = -0.3; return; }
    *sy = -(1.13 - 0.13 * y / H + (y > 0.45 * H ? -0.12 : 0)) + (c == 0 ? 0.05 : 0);
    *sx = 0.25 + (c == 0 ? 0.08 : c == 2 ? -0.03 : 0) - 0.05 * y / H;
}
int main(void) {
    int w = 640, h = H, ch = 3, fail = 0; size_t ns = (size_t)w * h * ch; double k = 4, off = 60;
    uint16_t *n = malloc(ns * 2), *b = malloc(ns * 2); float *al = malloc(ns * 4), *alg = malloc(ns * 4);
    for (mode = 0; mode < 2; mode++) {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
            size_t i = ((size_t)y * w + x) * ch + c; double sx, sy; truth(y, c, &sx, &sy);
            double v = scene(x, y, c) + 3 * grand(); n[i] = v < 0 ? 0 : v > 65532 ? 65532 : (uint16_t)(v + 0.5);
            double u = k * scene(x + sx, y + sy, c) + off + 3 * grand(); b[i] = u < 0 ? 0 : u > 65532 ? 65532 : (uint16_t)(u + 0.5);
        }
        RsInfo in; double g0; HdrAlignInfo la;
        int r = hdr_align2(n, b, w, h, ch, al, &in, &g0, &la);
        RsInfo in2; double g2;
        hdr_align2(n, b, w, h, ch, alg, &in2, &g2, NULL);      /* whole-picture shift only (as 0.4.0) */
        double maxe = 0; int bad = 0;
        for (int c = 0; c < ch && la.local; c++) for (int q = 0; q < la.nbands; q++) {
            /* compare with the band's mean true shift (a band across the step is an average of both sides) */
            double mx = 0, my = 0; int m = 0, y0 = h * q / la.nbands, y1 = h * (q + 1) / la.nbands;
            for (int y = y0; y < y1; y++) { double a, bb; truth(y, c, &a, &bb); mx += a; my += bb; m++; }
            mx /= m; my /= m;
            double ex = fabs(la.bdx[c][q] + mx), ey = fabs(la.bdy[c][q] + my);
            int straddle = mode == 0 && y0 <= 0.45 * H && y1 > 0.45 * H;
            if (ex > (straddle ? 0.08 : 0.03) || ey > (straddle ? 0.08 : 0.03)) bad++;
            if (!straddle) { if (ex > maxe) maxe = ex; if (ey > maxe) maxe = ey; }
        }
        /* error of the aligned brighter pass in the shadows (where the merge uses it), away from the step rows */
        double se = 0, seg = 0; long long cnt = 0;
        for (int y = 40; y < h - 40; y++) {
            if (mode == 0 && fabs(y - 0.45 * H) < 120) continue;
            for (int x = 20; x < w - 20; x++) for (int c = 0; c < ch; c++) {
                size_t i = ((size_t)y * w + x) * ch + c; double S = scene(x, y, c);
                if (S > 0.03 * 65532 || al[i] < 0 || alg[i] < 0) continue;
                double e = (al[i] - off) / k - S, eg = (alg[i] - off) / k - S; se += e * e; seg += eg * eg; cnt++;
            }
        }
        double rmsg = sqrt(seg / (cnt ? cnt : 1));
        double rms = sqrt(se / (cnt ? cnt : 1));
        if (mode == 0) {
            int ok = r == 0 && la.local && bad == 0 && rms < 0.6 * rmsg;
            printf("%s drifting shift with a step, red offset: %d bands, measured per channel %d/%d/%d; largest band error %.3f px (bands across the step excluded); shadows error %.2f with bands, %.2f with one shift (noise/k %.2f)\n",
                   ok ? "PASS" : "FAIL", la.nbands, la.nvalid[0], la.nvalid[1], la.nvalid[2], maxe, rms, rmsg, 3.0 / k);
            printf("     green along %+.3f .. %+.3f, red across %+.3f .. %+.3f, green across %+.3f .. %+.3f (whole-picture shift %+.3f %+.3f)\n",
                   la.dymin[1], la.dymax[1], la.dxmin[0], la.dxmax[0], la.dxmin[1], la.dxmax[1], in.dx, in.dy);
            if (!ok) fail = 1;
        } else {
            double spread = 0; for (int c = 0; c < ch; c++) { if (la.dymax[c] - la.dymin[c] > spread) spread = la.dymax[c] - la.dymin[c]; if (la.dxmax[c] - la.dxmin[c] > spread) spread = la.dxmax[c] - la.dxmin[c]; }
            int ok = r == 0 && la.local && bad == 0 && spread < 0.04 && rms < 1.1 * rmsg;
            printf("%s constant shift: whole-picture %+.3f %+.3f, bands within %.3f px of each other; shadows error %.2f with bands, %.2f with one shift\n", ok ? "PASS" : "FAIL", in.dx, in.dy, spread, rms, rmsg);
            if (!ok) fail = 1;
        }
    }
    /* without dark detail (a flat dark field): the bands cannot be measured -> the whole-picture shift is used */
    {
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
            size_t i = ((size_t)y * w + x) * ch + c;
            double base = (y > h / 2 - 200 && y < h / 2 + 200 && x > 150 && x < 490) ? scene(x, y, c) : 300;   /* detail only in the middle */
            double v = base + 3 * grand(); n[i] = (uint16_t)(v < 0 ? 0 : v + 0.5);
            double u = k * (base == 300 ? 300 : scene(x + 0.4, y - 0.3, c)) + off + 3 * grand(); b[i] = (uint16_t)(u < 0 ? 0 : u > 65532 ? 65532 : u + 0.5);
        }
        RsInfo in; double g0; HdrAlignInfo la;
        int r = hdr_align2(n, b, w, h, ch, al, &in, &g0, &la);
        /* bands outside the middle cannot be measured; they take the neighbours' values - the result must stay near the true shift */
        double worst = 0; for (int c = 0; c < ch && la.local; c++) for (int q = 0; q < la.nbands; q++) {
            double e = fabs(la.bdy[c][q] - 0.3); if (e > worst) worst = e; e = fabs(la.bdx[c][q] + 0.4); if (e > worst) worst = e; }
        int ok = r == 0 && (!la.local ? la.why == 2 : worst < 0.05);
        printf("%s detail only in the middle: %s (%d bands, measured %d/%d/%d), worst band %.3f px from the true shift\n", ok ? "PASS" : "FAIL",
               la.local ? "per band (unmeasured bands filled from neighbours)" : "whole-picture shift", la.nbands, la.nvalid[0], la.nvalid[1], la.nvalid[2], worst);
        if (!ok) fail = 1;
    }
    free(n); free(b); free(al); free(alg);
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
