/* rawhdr_edge_test.c - HDR merge next to bright, clipped edges (cases from the independent review of 2026-10-08:
 * the merge used to put interpolation ringing from clipped highlights into neighbouring shadows - 365 rms against 8 -
 * and the registration was biased by bright edges). Scene 1: smooth slide-like field with hard-edged bright objects
 * (pixel-integrated, so a sub-pixel shift is exact), optionally blurred; scene 2: dark/bright stripes 40/24 px.
 * Build: cc -O2 -o rawhdr_edge_test rawhdr_edge_test.c rawhdr.c rawstack.c -lm */
#include "rawhdr.h"
#include "rawstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
static unsigned long long rng = 0x2545F4914F6CDD1DULL;
static double urand(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (rng >> 11) * (1.0 / 9007199254740992.0); }
static double grand(void) { double u = urand(), v = urand(); if (u < 1e-12) u = 1e-12; return sqrt(-2 * log(u)) * cos(2 * M_PI * v); }
static uint16_t q16(double v) { return v < 0 ? 0 : v > 65532 ? 65532 : (uint16_t)(v + 0.5); }
static int EDGES = 1;
static double dens(double x, double y, int c) {
    double d = 1.8 + 1.1 * sin(x * 0.011 + 0.5 * c) * cos(y * 0.007) + 0.6 * sin((x + 2 * y) * 0.023) + 0.12 * sin(x * 0.31 + c) * sin(y * 0.27);
    if (EDGES) {   /* bright hard-edged objects (windows / lamps): density 0.05 */
        static const double R[][4] = { { 100, 80, 160, 140 }, { 300.3, 200.7, 330.6, 420 }, { 500, 50, 620, 70.5 }, { 420.2, 300, 560, 330.8 } };
        for (int i = 0; i < 4; i++) if (x >= R[i][0] && x < R[i][2] && y >= R[i][1] && y < R[i][3]) d = 0.05;
    }
    return d;
}
static double scene1(double x, double y, int c) { return 65532.0 * pow(10, -dens(x, y, c)); }
static double BW = 1;
static double pix(double x, double y, int c) { double s = 0; for (int i = 0; i < 8; i++) for (int j = 0; j < 8; j++) s += scene1(x + BW * (-0.5 + (i + 0.5) / 8), y + BW * (-0.5 + (j + 0.5) / 8), c); return s / 64; }

static int fail;
static void scene1_case(double k, double sx, double sy, double bw) {
    BW = bw; EDGES = 1;
    int w = 700, h = 500, ch = 3; size_t ns = (size_t)w * h * ch; double sigma = 8, off = 120;
    uint16_t *n = malloc(ns * 2), *b = malloc(ns * 2), *out = malloc(ns * 2); float *al = malloc(ns * 4); double *S = malloc(ns * 8);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
        size_t i = ((size_t)y * w + x) * ch + c; double s = pix(x, y, c); S[i] = s;
        n[i] = q16(s + sigma * grand()); b[i] = q16(k * pix(x + sx, y + sy, c) + off + sigma * grand()); }
    RsInfo in; double g0; HdrInfo hi; int ra = hdr_align(n, b, w, h, ch, al, &in, &g0), rm = ra ? -9 : hdr_merge(n, al, w, h, ch, out, &hi);
    double se[2] = { 0 }, sn[2] = { 0 }; long long cnt[2] = { 0 };
    if (!rm) for (int y = 10; y < h - 10; y++) for (int x = 10; x < w - 10; x++) for (int c = 0; c < ch; c++) {
        size_t i = ((size_t)y * w + x) * ch + c; if (S[i] > 0.02 * 65532) continue;
        int near = 0; for (int dy = -6; dy <= 6 && !near; dy++) for (int dx = -6; dx <= 6; dx++) if (S[((size_t)(y + dy) * w + x + dx) * ch + c] > 0.3 * 65532) { near = 1; break; }
        double e = out[i] - S[i], e0 = n[i] - S[i]; se[near] += e * e; sn[near] += e0 * e0; cnt[near]++; }
    double ra0 = cnt[0] ? sqrt(se[0] / cnt[0]) : 0, rn0 = cnt[0] ? sqrt(sn[0] / cnt[0]) : 0, ra1 = cnt[1] ? sqrt(se[1] / cnt[1]) : 0, rn1 = cnt[1] ? sqrt(sn[1] / cnt[1]) : 0;
    int ok = !ra && !rm && fabs(in.dx + sx) < 0.02 && fabs(in.dy + sy) < 0.02 && fabs(hi.g[1] / k - 1) < 0.01 && ra0 < 0.7 * rn0 && ra1 < rn1 + 0.5;
    printf("%s bright objects k=%g shift %.2f %.2f blur %g: found %.3f %.3f, g %.3f; shadows: away %.1f (normal %.1f), within 6 px of a bright edge %.1f (normal %.1f)\n",
           ok ? "PASS" : "FAIL", k, -sx, -sy, bw, in.dx, in.dy, hi.g[1], ra0, rn0, ra1, rn1);
    if (!ok) fail = 1;
    free(n); free(b); free(out); free(al); free(S);
}
static double stripes(double x, double y, int c) {
    double dark = 65532.0 * pow(10, -(0.9 + 2.0 * y / 400.0)) * (1 + 0.3 * sin(y * 0.05 + c)) * (1 + 0.5 * sin(x * 0.07));
    return fmod(x + 1000.0, 64.0) < 40 ? dark : 65532.0 * 0.9;
}
static double spix(double x, double y, int c) { double s = 0; for (int i = 0; i < 16; i++) s += stripes(x - 0.5 + (i + 0.5) / 16, y, c); return s / 16; }
static void stripe_case(double k, double sx, double sy) {
    int w = 640, h = 400, ch = 3; size_t ns = (size_t)w * h * ch;
    uint16_t *n = malloc(ns * 2), *b = malloc(ns * 2), *out = malloc(ns * 2); float *al = malloc(ns * 4); double *S = malloc(ns * 8);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
        size_t i = ((size_t)y * w + x) * ch + c; double s = spix(x, y, c); S[i] = s;
        n[i] = q16(s + 8 * grand()); b[i] = q16(k * spix(x + sx, y + sy, c) + 120 + 8 * grand()); }
    RsInfo in; double g0; HdrInfo hi; int ra = hdr_align(n, b, w, h, ch, al, &in, &g0), rm = ra ? -9 : hdr_merge(n, al, w, h, ch, out, &hi);
    double se[2] = { 0 }, sn[2] = { 0 }, worst = 0; long long cnt[2] = { 0 };
    if (!rm) for (int y = 10; y < h - 10; y++) for (int x = 10; x < w - 10; x++) {
        double p = fmod(x + 1000.0, 64.0); if (p >= 40) continue;
        int far = fmin(p, 39 - p) >= 5;
        for (int c = 0; c < ch; c++) { size_t i = ((size_t)y * w + x) * ch + c; double e = out[i] - S[i], e0 = n[i] - S[i]; se[far] += e * e; sn[far] += e0 * e0; cnt[far]++; }
    }
    double rnear = sqrt(se[0] / cnt[0]), nnear = sqrt(sn[0] / cnt[0]), rfar = sqrt(se[1] / cnt[1]), nfar = sqrt(sn[1] / cnt[1]); (void)worst;
    int ok = !ra && !rm && fabs(hi.g[0] / k - 1) < 0.01 && fabs(hi.g[2] / k - 1) < 0.01 && rfar < 0.75 * nfar && rnear < nnear + 0.5;
    printf("%s stripes k=%g shift %.2f %.2f: found %.3f %.3f, g %.3f %.3f %.3f; dark side within 4 px of an edge %.1f (normal %.1f), further %.1f (normal %.1f)\n",
           ok ? "PASS" : "FAIL", k, sx, sy, in.dx, in.dy, hi.g[0], hi.g[1], hi.g[2], rnear, nnear, rfar, nfar);
    if (!ok) fail = 1;
    free(n); free(b); free(out); free(al); free(S);
}
int main(void) {
    scene1_case(4, 0.4, -0.3, 1); scene1_case(2, 0.4, -0.3, 1); scene1_case(4, 0.4, -0.3, 5); scene1_case(4, 0, 0, 1);
    scene1_case(2, 1.7, -2.6, 1); scene1_case(4, -0.5, 0.5, 3);
    stripe_case(2, 0.4, -0.3); stripe_case(4, 0.4, -0.3); stripe_case(4, 0, 0);
    printf(fail ? "SOME FAILED\n" : "ALL PASS\n");
    return fail;
}
