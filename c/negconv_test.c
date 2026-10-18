/* negconv_test.c - negconv.c on the hardware data of 12 Oct (s39: C-41 frame 2, 4 T35mm H; the RAW box-averaged 4 x 4,
 * the preview as the driver's PM file holds it - 12-bit after its 14 -> 12 table). The expected levels and output are
 * those of the prototype scripts/neg/negconv.py on the same data.
 * usage: negconv_test REFDIR (ref/neg) */
#include "negconv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails, passes;
#define CHECK(c, ...) do { if (c) { passes++; printf("PASS "); } else { fails++; printf("FAIL "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static uint16_t *load(const char *dir, const char *name, size_t n) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", dir, name); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    uint16_t *d = malloc(n * 2); size_t got = fread(d, 2, n, f); fclose(f);
    if (got != n) { free(d); return NULL; }
    return d;
}
static double pct(const uint16_t *o, size_t n, int ch, int c, double q) {
    static uint32_t h[65536]; memset(h, 0, sizeof h); for (size_t i = 0; i < n; i++) h[o[i * ch + c]]++;
    size_t lim = (size_t)(q / 100 * n), acc = 0; for (int v = 0; v < 65536; v++) { acc += h[v]; if (acc > lim) return v / 65535.0; }
    return 1;
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "ref/neg";
    const int W = 906, H = 635, PW = 514, PH = 354;
    uint16_t *raw = load(dir, "s39_raw_q4_906x635.bin", (size_t)W * H * 3), *pv = load(dir, "s39_preview_v12_514x354.bin", (size_t)PW * PH * 3);
    if (!raw || !pv) { printf("FAIL reference data not found in %s\n", dir); return 1; }
    NcParams p; nc_defaults(&p);
    /* 1. the RAW: levels and positive as the prototype's */
    int x0, y0, x1, y1; CHECK(nc_area(raw, W, H, 3, &x0, &y0, &x1, &y1) == 0 && x0 == 0 && y0 == 0 && x1 == W - 1 && y1 == H - 1, "RAW crop: no holder in it (%d %d - %d %d)", x0, y0, x1, y1);
    NcLevels lv; int r = nc_levels(raw, W, H, 3, x0, y0, x1, y1, &p, &lv);
    const double plo[3] = { 0.6110, 1.0106, 1.2651 }, phi[3] = { 1.5120, 1.9537, 2.0096 };
    double dmax = 0; for (int c = 0; c < 3; c++) { dmax = fmax(dmax, fabs(lv.lo[c] - plo[c])); dmax = fmax(dmax, fabs(lv.hi[c] - phi[c])); }
    CHECK(r == 0 && dmax < 0.005 && fabs(lv.range - 1.572) < 0.01, "levels as the prototype: lo %.4f %.4f %.4f, hi %.4f %.4f %.4f, range %.3f (%.1f stops), %ld pixels; largest difference %.4f D",
          lv.lo[0], lv.lo[1], lv.lo[2], lv.hi[0], lv.hi[1], lv.hi[2], lv.range, lv.range / log10(2), lv.n, dmax);
    uint16_t *out = malloc((size_t)W * H * 3 * 2);
    CHECK(nc_convert(raw, (size_t)W * H, 3, &lv, &p, out) == 0, "converted");
    const double ex[3][3] = { { 0.0939, 0.6553, 0.8819 }, { 0.0739, 0.5336, 0.8471 }, { 0.1013, 0.3820, 0.8889 } }; double pm = 0;
    for (int c = 0; c < 3; c++) { const double q[3] = { 1, 50, 99 }; for (int k = 0; k < 3; k++) pm = fmax(pm, fabs(pct(out, (size_t)W * H, 3, c, q[k]) - ex[c][k])); }
    CHECK(pm < 0.006, "positive as the prototype's: R %.3f / %.3f / %.3f, G %.3f / %.3f / %.3f, B %.3f / %.3f / %.3f (1 / 50 / 99 %%); largest difference %.4f",
          pct(out, (size_t)W * H, 3, 0, 1), pct(out, (size_t)W * H, 3, 0, 50), pct(out, (size_t)W * H, 3, 0, 99), pct(out, (size_t)W * H, 3, 1, 1), pct(out, (size_t)W * H, 3, 1, 50),
          pct(out, (size_t)W * H, 3, 1, 99), pct(out, (size_t)W * H, 3, 2, 1), pct(out, (size_t)W * H, 3, 2, 50), pct(out, (size_t)W * H, 3, 2, 99), pm);
    /* 2. holder and clear film are left out of the levels: a picture with a patch of holder and a patch of clear film
          (sprocket hole) has the levels of the same picture with both patches holder (1 % of it each; counted, either
          would move a level by 1 - 3 D) */
    { uint16_t *t = malloc((size_t)W * H * 6), *u = malloc((size_t)W * H * 6); memcpy(t, raw, (size_t)W * H * 6);
      for (int y = 100; y < 160; y++) for (int x = 100; x < 190; x++) { uint16_t *q = t + ((size_t)y * W + x) * 3; q[0] = 70; q[1] = 8; q[2] = 1; }        /* holder (s39: 70 / 8 / 1) */
      memcpy(u, t, (size_t)W * H * 6);
      for (int y = 400; y < 460; y++) for (int x = 500; x < 590; x++) { uint16_t *q = t + ((size_t)y * W + x) * 3; q[0] = 52000; q[1] = 50000; q[2] = 51000;   /* sprocket hole */
                                                                             q = u + ((size_t)y * W + x) * 3; q[0] = 70; q[1] = 8; q[2] = 1; }
      NcLevels l2, l3; nc_levels(t, W, H, 3, 0, 0, W - 1, H - 1, &p, &l2); nc_levels(u, W, H, 3, 0, 0, W - 1, H - 1, &p, &l3); double d2 = 0, d3 = 0;
      for (int c = 0; c < 3; c++) { d2 = fmax(d2, fmax(fabs(l2.lo[c] - l3.lo[c]), fabs(l2.hi[c] - l3.hi[c]))); d3 = fmax(d3, fmax(fabs(l2.lo[c] - lv.lo[c]), fabs(l2.hi[c] - lv.hi[c]))); }
      CHECK(l2.ok && l3.ok && d2 < 1e-9 && d3 < 0.05 && l2.n == lv.n - 2 * 5400, "a patch of holder and one of clear film (1 %% of the picture each) are left out (%ld pixels fewer; levels %.4f D from the picture without them)", lv.n - l2.n, d3);
      free(t); free(u); }
    /* 3. the preview (12-bit PM, the driver's table undone, x 4): the holder is found, the levels agree with the RAW's */
    { uint16_t *p16 = malloc((size_t)PW * PH * 6);
      for (size_t i = 0; i < (size_t)PW * PH * 3; i++) { double v = pv[i], v14 = v < 255 ? v : 255 + (v - 255 + 0.5) / 0.2380952; p16[i] = (uint16_t)fmin(65535, v14 * 4 + 0.5); }
      int a0, b0, a1, b1; int ra = nc_area(p16, PW, PH, 3, &a0, &b0, &a1, &b1);
      CHECK(ra == 0 && a0 >= 10 && a0 <= 40 && a1 >= 490 && a1 <= 510 && b0 >= 15 && b0 <= 40 && b1 >= 320 && b1 <= 345, "preview: the holder around the frame is left out (picture %d - %d x %d - %d of %d x %d)", a0, a1, b0, b1, PW, PH);
      NcLevels lp; int rp = nc_levels(p16, PW, PH, 3, a0, b0, a1, b1, &p, &lp); double d3 = 0;
      for (int c = 0; c < 3; c++) d3 = fmax(d3, fmax(fabs(lp.lo[c] - lv.lo[c]), fabs(lp.hi[c] - lv.hi[c])));
      CHECK(rp == 0 && d3 < 0.06, "preview levels (whole frame incl. the frame gap) lo %.3f %.3f %.3f, hi %.3f %.3f %.3f - within %.3f D of the RAW crop's", lp.lo[0], lp.lo[1], lp.lo[2], lp.hi[0], lp.hi[1], lp.hi[2], d3);
      /* the preview's levels applied to the RAW (one set for a roll) give nearly the same positive */
      uint16_t *o2 = malloc((size_t)W * H * 6); nc_convert(raw, (size_t)W * H, 3, &lp, &p, o2); double md = 0;
      for (int c = 0; c < 3; c++) md = fmax(md, fabs(pct(o2, (size_t)W * H, 3, c, 50) - pct(out, (size_t)W * H, 3, c, 50)));
      CHECK(md < 0.04, "the RAW with the preview's levels: medians within %.3f of its own", md);
      free(o2); free(p16); }
    /* 4. contrast: a lower film gamma (harder) spreads the middle tones */
    { NcParams ph = p; ph.gamma = 0.5; NcLevels lh; nc_levels(raw, W, H, 3, 0, 0, W - 1, H - 1, &ph, &lh); uint16_t *o3 = malloc((size_t)W * H * 6); nc_convert(raw, (size_t)W * H, 3, &lh, &ph, o3);
      double s0 = pct(out, (size_t)W * H, 3, 1, 90) - pct(out, (size_t)W * H, 3, 1, 10), s1 = pct(o3, (size_t)W * H, 3, 1, 90) - pct(o3, (size_t)W * H, 3, 1, 10);
      double m0 = pct(out, (size_t)W * H, 3, 1, 50), m1 = pct(o3, (size_t)W * H, 3, 1, 50);
      CHECK(lh.range > lv.range && s1 >= s0 && m1 < m0 - 0.02, "film gamma 0.5 (harder): range %.3f (0.6: %.3f), middle tones darker (green median %.3f, 0.6: %.3f), 10 - 90 %% spread %.3f (0.6: %.3f)", lh.range, lv.range, m1, m0, s1, s0); free(o3); }
    /* 5. black and white (one channel) */
    { uint16_t *g = malloc((size_t)W * H * 2); for (size_t i = 0; i < (size_t)W * H; i++) g[i] = raw[i * 3 + 1];
      NcLevels lg; int rg = nc_levels(g, W, H, 1, 0, 0, W - 1, H - 1, &p, &lg); uint16_t *og = malloc((size_t)W * H * 2); nc_convert(g, (size_t)W * H, 1, &lg, &p, og);
      int mono = 1; for (size_t i = 1; i < (size_t)W * H; i++) if ((g[i] > g[0]) && og[i] > og[0]) mono = 0;
      CHECK(rg == 0 && fabs(lg.lo[0] - lv.lo[1]) < 0.003 && fabs(lg.hi[0] - lv.hi[1]) < 0.003 && mono, "one channel: the green levels (%.4f / %.4f); more light in the negative = darker in the positive", lg.lo[0], lg.hi[0]);
      free(g); free(og); }
    /* 6. rotation */
    { uint16_t a[6] = { 1, 2, 3, 4, 5, 6 }; int ow, oh; uint16_t *r1 = nc_rotate(a, 3, 2, 1, 1, &ow, &oh);   /* 1 2 3 / 4 5 6 -> 4 1 / 5 2 / 6 3 */
      int ok1 = ow == 2 && oh == 3 && r1[0] == 4 && r1[1] == 1 && r1[2] == 5 && r1[3] == 2 && r1[4] == 6 && r1[5] == 3;
      uint16_t *r3 = nc_rotate(a, 3, 2, 1, 3, &ow, &oh); int ok3 = ow == 2 && oh == 3 && r3[0] == 3 && r3[1] == 6 && r3[4] == 1 && r3[5] == 4;
      uint16_t *r2 = nc_rotate(a, 3, 2, 1, 2, &ow, &oh); int ok2 = ow == 3 && oh == 2 && r2[0] == 6 && r2[5] == 1;
      uint16_t *x = nc_rotate(raw, W, H, 3, 1, &ow, &oh); uint16_t *y = nc_rotate(x, ow, oh, 3, 3, &ow, &oh); int back = ow == W && oh == H && !memcmp(y, raw, (size_t)W * H * 6);
      CHECK(ok1 && ok2 && ok3 && back, "rotation: 90 clockwise, 180, 90 counter-clockwise; right then left gives the picture back");
      free(r1); free(r2); free(r3); free(x); free(y); }
    /* 7. refusals */
    { NcLevels lz; uint16_t flat[64 * 64 * 3]; for (int i = 0; i < 64 * 64 * 3; i++) flat[i] = 3000;
      CHECK(nc_levels(flat, 64, 64, 3, 0, 0, 63, 63, &p, &lz) == -1 && nc_convert(raw, 10, 3, &lz, &p, out) == -1 && nc_levels(raw, W, H, 2, 0, 0, W - 1, H - 1, &p, &lz) == -1,
            "no density range / no levels / 2 channels refused"); }
    free(out); free(raw); free(pv);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails != 0;
}
