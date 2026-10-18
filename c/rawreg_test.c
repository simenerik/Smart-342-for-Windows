/* rawreg_test.c - rawreg.c on a synthetic picture with known channel shifts; with arguments IN.bin W H OUT.bin it
 * also registers a real RAW (16-bit interleaved RGB, little-endian) and writes the result (for scripts/sharpness). */
#include "rawreg.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); } else printf("ok: "); printf(__VA_ARGS__); printf("\n"); } while (0)
static double tex(double x, double y) {   /* smooth texture with detail at several scales (periods 7 .. 60 px) */
    return 20000 + 6000 * sin(x * 0.11 + 0.3 * sin(y * 0.05)) + 5000 * sin(y * 0.17 + x * 0.04) + 3000 * sin((x + 1.7 * y) * 0.31) + 2500 * sin((2.1 * x - y) * 0.43) + 1500 * cos(x * 0.83 + y * 0.29);
}
int main(int argc, char **argv) {
    if (argc == 5) {
        int w = atoi(argv[2]), h = atoi(argv[3]); size_t n = (size_t)w * h * 3;
        uint16_t *a = malloc(n * 2), *o = malloc(n * 2); FILE *f = fopen(argv[1], "rb"); if (!f || fread(a, 2, n, f) != n) { printf("read failed\n"); return 2; } fclose(f);
        RrInfo in; rr_measure(a, w, h, &in);
        for (int k = 0; k < 2; k++) { printf("%c: ok %d  dx %+.3f %+.5f/px  dy %+.3f %+.5f/px  (whole %+.3f %+.3f score %.4f) bands:", k ? 'B' : 'R', in.ok[k], in.a_dx[k], in.b_dx[k], in.a_dy[k], in.b_dy[k], in.gdx[k], in.gdy[k], in.gsc[k]);
            for (int i = 0; i < in.nb; i++) printf(" [%+.3f%s %+.3f%s %.3f]", in.bdx[k][i], in.busx[k][i] ? "" : "x", in.bdy[k][i], in.busy[k][i] ? "" : "x", in.bsc[k][i]); printf("  corrected across %d (MAD %.3f) along %d (MAD %.3f)\n", in.corrx[k], in.madx[k], in.corry[k], in.mady[k]); }
        rr_apply(a, w, h, &in, o); f = fopen(argv[4], "wb"); fwrite(o, 2, n, f); fclose(f); return 0;
    }
    int w = 300, h = 240; uint16_t *img = malloc((size_t)w * h * 6), *out = malloc((size_t)w * h * 6);
    double rdx = 0.21, rdy = -0.74;                 /* R(x + rdx, y + rdy) = G(x, y) */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double xc = x - w / 2.0, bdx = -0.10 + 0.0013 * xc, bdy = -0.25 + 0.0004 * xc;   /* B: changes across, as the lens table does */
        uint16_t *p = img + ((size_t)y * w + x) * 3;
        p[1] = (uint16_t)(tex(x, y) + 0.5);
        p[0] = (uint16_t)(0.8 * tex(x - rdx, y - rdy) + 0.5);
        p[2] = (uint16_t)(1.1 * tex(x - bdx, y - bdy) + 0.5);
    }
    RrInfo in; CHECK(rr_measure(img, w, h, &in) == 0, "measure");
    CHECK(in.corrx[0] && in.corry[0] && fabs(in.a_dx[0] - rdx) < 0.03 && fabs(in.a_dy[0] - rdy) < 0.03 && fabs(in.b_dx[0]) < 2e-4 && fabs(in.b_dy[0]) < 2e-4,
          "R shift found: dx %+.3f (%+.2f) dy %+.3f (%+.2f), slopes %+.5f %+.5f (0)", in.a_dx[0], rdx, in.a_dy[0], rdy, in.b_dx[0], in.b_dy[0]);
    CHECK(in.corrx[1] && in.corry[1] && fabs(in.a_dx[1] + 0.10) < 0.03 && fabs(in.b_dx[1] - 0.0013) < 3e-4 && fabs(in.a_dy[1] + 0.25) < 0.03 && fabs(in.b_dy[1] - 0.0004) < 3e-4,
          "B shift changing across found: dx %+.3f %+.5f/px (-0.10 +0.0013), dy %+.3f %+.5f/px (-0.25 +0.0004)", in.a_dx[1], in.b_dx[1], in.a_dy[1], in.b_dy[1]);
    CHECK(rr_apply(img, w, h, &in, out) == 0, "apply");
    /* after: R / 0.8 and B / 1.1 equal G in the interior */
    double er = 0, eb = 0, e0r = 0; int n = 0;
    for (int y = 10; y < h - 10; y++) for (int x = 10; x < w - 10; x++) {
        const uint16_t *p = out + ((size_t)y * w + x) * 3, *q = img + ((size_t)y * w + x) * 3;
        er += pow(p[0] / 0.8 - p[1], 2); eb += pow(p[2] / 1.1 - p[1], 2); e0r += pow(q[0] / 0.8 - q[1], 2); n++; }
    er = sqrt(er / n); eb = sqrt(eb / n); e0r = sqrt(e0r / n);
    CHECK(er < 0.05 * e0r && eb < 0.05 * e0r, "after registration R and B match G: rms %.1f / %.1f (R before %.1f)", er, eb, e0r);
    { int same = 1; for (size_t i = 1; i < (size_t)w * h * 3; i += 3) if (out[i] != img[i]) same = 0; CHECK(same, "G untouched"); }
    RrInfo in2; rr_measure(out, w, h, &in2);
    CHECK(fabs(in2.a_dx[0]) < 0.02 && fabs(in2.a_dy[0]) < 0.02 && fabs(in2.a_dx[1]) < 0.02 && fabs(in2.a_dy[1]) < 0.02, "measured again: R %+.3f %+.3f, B %+.3f %+.3f (0)", in2.a_dx[0], in2.a_dy[0], in2.a_dx[1], in2.a_dy[1]);
    /* an outlier band at the edge (review M4: debris / clipping in one band): B of the first sixth shifted 0.6 px more */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double xc = x - w / 2.0, bdx = -0.10 + 0.0013 * xc + (x < w / 6 ? 0.6 : 0), bdy = -0.25 + 0.0004 * xc;
        img[((size_t)y * w + x) * 3 + 2] = (uint16_t)(1.1 * tex(x - bdx, y - bdy) + 0.5); }
    { RrInfo io; rr_measure(img, w, h, &io); int dropped = !io.busx[1][0];
      CHECK(dropped && fabs(io.a_dx[1] + 0.10) < 0.05 && fabs(io.b_dx[1] - 0.0013) < 4e-4, "edge outlier band left out (%s), B dx %+.3f %+.5f/px (-0.10 +0.0013)", dropped ? "yes" : "no", io.a_dx[1], io.b_dx[1]); }
    /* colour film (0.1.23): across, the bands scatter (each band's B shifted differently, +-0.2 .. 0.4 px), along they agree:
       only along is corrected */
    { static const double sc8[8] = { 0.35, -0.30, 0.05, 0.40, -0.25, 0.20, -0.40, 0.10 };
      for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
          int band = (x - 8) * 8 / (w - 16); if (band < 0) band = 0; if (band > 7) band = 7;
          double bdx = sc8[band], bdy = -0.25;
          img[((size_t)y * w + x) * 3 + 2] = (uint16_t)(1.1 * tex(x - bdx, y - bdy) + 0.5); }
      RrInfo is; rr_measure(img, w, h, &is);
      /* the measured bands mix neighbouring scatter (smoother than made): across must be left, or moved only by about the
         average (no large shift from a few bands); along corrected */
      CHECK((!is.corrx[1] || (fabs(is.a_dx[1]) < 0.1 && fabs(is.b_dx[1]) * w / 2 < 0.1)) && is.corry[1] && fabs(is.a_dy[1] + 0.25) < 0.04,
            "scattered bands across: across %s (MAD %.3f, dx %+.3f), along corrected %+.3f (-0.25)", is.corrx[1] ? "only the average" : "left", is.madx[1], is.a_dx[1], is.a_dy[1]); }
    /* narrow picture (under RR_MIN_BANDS x 40 px): the whole-picture shift is used (review 0.1.23: 0.1.23 first left it) */
    { int wn = 100, hn = 120; uint16_t *a = malloc((size_t)wn * hn * 6);
      for (int y = 0; y < hn; y++) for (int x = 0; x < wn; x++) { uint16_t *p = a + ((size_t)y * wn + x) * 3;
          p[1] = (uint16_t)(tex(x, y) + 0.5); p[0] = (uint16_t)(0.8 * tex(x - rdx, y - rdy) + 0.5); p[2] = (uint16_t)(1.1 * tex(x + 0.1, y + 0.25) + 0.5); }
      RrInfo iw; rr_measure(a, wn, hn, &iw);
      CHECK(iw.whole[0] && iw.corrx[0] && iw.corry[0] && fabs(iw.a_dx[0] - rdx) < 0.05 && fabs(iw.a_dy[0] - rdy) < 0.05 && fabs(iw.a_dx[1] + 0.1) < 0.05 && fabs(iw.a_dy[1] + 0.25) < 0.05,
            "narrow picture (%d px, %d band): whole-picture shift R %+.3f %+.3f (%+.2f %+.2f), B %+.3f %+.3f (-0.10 -0.25)", wn, iw.nb, iw.a_dx[0], iw.a_dy[0], rdx, rdy, iw.a_dx[1], iw.a_dy[1]);
      free(a); }
    /* flat picture: nothing to measure -> nothing changed */
    for (size_t i = 0; i < (size_t)w * h * 3; i++) img[i] = 30000;
    RrInfo in3; rr_measure(img, w, h, &in3); CHECK(!in3.ok[0] && !in3.ok[1], "flat picture: no shift measured (ok %d %d), nothing changed", in3.ok[0], in3.ok[1]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
