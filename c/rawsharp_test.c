/* rawsharp_test.c - checks rawsharp.c against the Python reference (scripts/sharpness/wiener_proto.py: kernels and a
 * convolved test image in ref/sharp/) and its own properties. Run from c/: ./rawsharp_test */
#include "rawsharp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static void *rd(const char *p, size_t n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; void *b = malloc(n); size_t g = fread(b, 1, n, f); fclose(f); if (g != n) { free(b); return NULL; } return b; }
/* developer mode: rawsharp_test IN.bin W H XDPI YDPI K OUT.bin - the app's 0.1.24 model for the 35 mm lens (CCD samples
   across), applied to a 16-bit RGB picture; prints the predicted response (G) relative to 8.33 lp/mm before / after */
static int apply_mode(char **argv) {
    int w = atoi(argv[2]), h = atoi(argv[3]); double xd = atof(argv[4]), yd = atof(argv[5]); size_t n = (size_t)w * h * 3;
    RshModel md; memset(&md, 0, sizeof md);
    md.fnum = 5.0; md.cell_um = 7.0; md.mag = 0.73366; md.pitch_x_um = 25400.0 / xd; md.pitch_y_um = md.motion_um = 25400.0 / yd;
    md.sigma_ccd_um = 1.83; md.K = atof(argv[6]); md.sigma_x_ccd_um = 1.693; md.sigma_y_ccd_um = 3.573; md.halo_x = 0.458; md.halo_y = 0.198; md.halo_sigma_ccd_um = 7.66;
    RshInfo in; if (rsh_design(&md, &in)) return 2;
    uint16_t *a = rd(argv[1], n * 2), *o = malloc(n * 2); if (!a) { printf("read failed\n"); return 2; }
    rsh_apply(&in, a, w, h, 3, o); FILE *f = fopen(argv[7], "wb"); fwrite(o, 2, n, f); fclose(f);
    /* predicted: model x kernel response, relative to 8.33 lp/mm */
    for (int ax = 0; ax < 2; ax++) {
        printf("%s predicted after/before:", ax ? "along " : "across");
        double r0 = 0, m0 = rsh_model(&md, 1, ax ? 0 : 8.33e-3, ax ? 8.33e-3 : 0);
        for (int pass = 0; pass < 2; pass++) for (int q = 0; q <= 8; q++) {
            double fl = q == 0 ? 8.33 : 8.33 * (q + 1); if (q > 0 && fl > 85) break; double f = fl / 1000;
            double fx = ax ? 0 : f, fy = ax ? f : 0, s = 0; int hh = RSH_TAPS / 2;
            for (int j = 0; j < RSH_TAPS; j++) for (int i = 0; i < RSH_TAPS; i++) s += in.kern[1][j * RSH_TAPS + i] * cos(2 * M_PI * (fx * (i - hh) * md.pitch_x_um + fy * (j - hh) * md.pitch_y_um));
            double mm = rsh_model(&md, 1, fx, fy), v = mm * s;
            if (q == 0) { r0 = v; continue; }
            if (pass == 0) printf(" %.1f:%.3f/%.3f", fl, v / r0, mm / m0);
        }
        printf("\n");
    }
    return 0;
}
int main(int argc, char **argv) {
    if (argc == 8) return apply_mode(argv);
    RshModel m = { 5.0, 0.73366, 7.0, 25400 / 2662.138, 25400 / 5324.275, 25400 / 5324.275, 1.83 * 1.0, 0, 0.015 };
    m.sigma_ccd_um = 2.5 * 0.73366;   /* the prototype's 2.5 um on the film */
    RshInfo in; CHECK(rsh_design(&m, &in) == 0, "design");
    double *ref = rd("../ref/sharp/kern_L110_px9541_py4770_K0015.bin", sizeof(double) * 3 * RSH_TAPS * RSH_TAPS);
    CHECK(ref != NULL, "reference kernels read");
    if (ref) {
        double md = 0; for (int c = 0; c < 3; c++) for (int i = 0; i < RSH_TAPS * RSH_TAPS; i++) { double d = fabs(in.kern[c][i] - ref[c * RSH_TAPS * RSH_TAPS + i]); if (d > md) md = d; }
        CHECK(md < 1e-6, "kernels equal the Python reference (largest difference %.2e)", md);
    }
    for (int c = 0; c < 3; c++) { double s = 0; for (int i = 0; i < RSH_TAPS * RSH_TAPS; i++) s += in.kern[c][i]; CHECK(fabs(s - 1) < 1e-9, "channel %d kernel sums to 1 (%.12f): flat areas keep their level", c, s); }
    CHECK(fabs(in.centre[1] - 1.99631) < 1e-4 && fabs(in.noise_gain[1] - 2.2161) < 1e-3, "G centre %.5f noise gain %.4f (Python 1.99631 / 2.2161)", in.centre[1], in.noise_gain[1]);
    /* symmetric kernel */
    { double md = 0; for (int j = 0; j < RSH_TAPS; j++) for (int i = 0; i < RSH_TAPS; i++) { double d = fabs(in.kern[1][j * RSH_TAPS + i] - in.kern[1][(RSH_TAPS - 1 - j) * RSH_TAPS + (RSH_TAPS - 1 - i)]); if (d > md) md = d; } CHECK(md < 1e-12, "kernel point-symmetric (no shift of the picture)"); }
    /* the model along matches the s19 measurement it was fitted to (model at 40/50/60 lp/mm = 0.407/0.267/0.159 in the prototype) */
    CHECK(fabs(in.model_y[1][3] - 0.407) < 0.003 && fabs(in.model_y[1][4] - 0.267) < 0.003 && fabs(in.model_y[1][5] - 0.159) < 0.003,
          "model along G at 40/50/60 lp/mm %.3f %.3f %.3f (prototype 0.407 0.267 0.159)", in.model_y[1][3], in.model_y[1][4], in.model_y[1][5]);
    CHECK(in.resp_y[1][4] > 0.7 && in.resp_y[1][4] < 0.8, "after restoration along G at 50 lp/mm %.2f (prototype 0.75)", in.resp_y[1][4]);
    /* apply equals the reference convolution */
    uint16_t *img = rd("../ref/sharp/in_64x48x3.bin", 64 * 48 * 3 * 2), *exp_ = rd("../ref/sharp/out_64x48x3.bin", 64 * 48 * 3 * 2);
    CHECK(img && exp_, "reference image read");
    if (img && exp_) {
        uint16_t *out = malloc(64 * 48 * 3 * 2); CHECK(rsh_apply(&in, img, 64, 48, 3, out) == 0, "apply");
        int md = 0, nd = 0; for (int i = 0; i < 64 * 48 * 3; i++) { int d = abs((int)out[i] - (int)exp_[i]); if (d > md) md = d; nd += d > 0; }
        CHECK(md <= 1, "applied image equals scipy.ndimage.convolve(mode='nearest') within 1 (largest %d, %d samples differ by 1)", md, nd);
        /* flat image stays flat */
        for (int i = 0; i < 64 * 48 * 3; i++) img[i] = 12345;
        rsh_apply(&in, img, 64, 48, 3, out); int flat = 1; for (int i = 0; i < 64 * 48 * 3; i++) if (abs(out[i] - 12345) > 1) flat = 0;
        CHECK(flat, "a flat picture stays flat (edges included)");
        free(out);
    }
    /* interpolated across: a 2x interpolation (step 0.5) adds the cubic's loss -> more restoration across, not along */
    { RshModel m2 = m; m2.pitch_x_um = m.pitch_x_um / 2; m2.interp_x = 0.5; RshInfo i2; rsh_design(&m2, &i2);
      CHECK(i2.model_x[1][3] < in.model_x[1][3] && fabs(i2.model_y[1][3] - in.model_y[1][3]) < 1e-9, "interpolated across: model across at 40 lp/mm %.3f < %.3f, along unchanged", i2.model_x[1][3], in.model_x[1][3]); }
    /* other lenses: L89 (m 0.37) is softer on the film */
    { RshModel m3 = m; m3.mag = 0.37; m3.sigma_ccd_um = 1.83; m3.pitch_x_um = 25400 / 1349.0; m3.pitch_y_um = m3.motion_um = 25400 / 2698.0; RshInfo i3; CHECK(rsh_design(&m3, &i3) == 0, "L89 design");
      CHECK(i3.model_y[1][1] < in.model_y[1][1], "L89 model along at 20 lp/mm %.3f < L110 %.3f", i3.model_y[1][1], in.model_y[1][1]); }
    CHECK(fabs(in.f_rep[3] - 52.40) < 0.05 && fabs(in.f_rep[0] - 20.96) < 0.05, "report frequencies for the 35 mm lens: %.2f .. %.2f lp/mm (0.4 .. 1.0 x the CCD's Nyquist 52.40)", in.f_rep[0], in.f_rep[3]);
    CHECK(rsh_lens_fnum(2) == 5.0 && rsh_lens_fnum(3) == 5.6 && rsh_lens_fnum(4) == 0, "lens f-numbers (L110 5.0, L60 5.6, L80 unknown)");
    /* 0.1.24: the model fitted to the Scitex focus chart (scripts/sharpness/fit_chart.py; s21 / s23 / s24 bars, green,
       relative to 8.33 lp/mm): kernels equal the Python reference, and the model reproduces the measured response */
    {
        RshModel c = m; c.sigma_x_ccd_um = 1.693; c.sigma_y_ccd_um = 3.573; c.halo_x = 0.458; c.halo_y = 0.198; c.halo_sigma_ccd_um = 7.66; c.K = 0.010;
        RshInfo ic; CHECK(rsh_design(&c, &ic) == 0, "chart model: design");
        double *rc = rd("../ref/sharp/kern_L110_chart_px9541_py4770_K0010.bin", sizeof(double) * 3 * RSH_TAPS * RSH_TAPS);
        CHECK(rc != NULL, "chart model: reference kernels read");
        if (rc) { double md = 0; for (int ch = 0; ch < 3; ch++) for (int i = 0; i < RSH_TAPS * RSH_TAPS; i++) { double d = fabs(ic.kern[ch][i] - rc[ch * RSH_TAPS * RSH_TAPS + i]); if (d > md) md = d; }
                  CHECK(md < 1e-6, "chart model: kernels equal the Python reference (largest difference %.2e)", md); free(rc); }
        CHECK(fabs(ic.centre[1] - 1.74514) < 1e-4 && fabs(ic.noise_gain[1] - 2.1944) < 1e-3, "chart model: G centre %.5f noise gain %.4f (Python 1.74514 / 2.1944)", ic.centre[1], ic.noise_gain[1]);
        /* measured (s21, along at 5324 dpi = this model's motion; across CCD samples) against the model, relative to 8.33 lp/mm */
        static const double FX[6] = { 25, 33.3, 41.6, 50, 58.3, 66.6 }, MX[6] = { .498, .377, .276, .203, .135, .083 }, MY[6] = { .500, .336, .202, .114, .055, .021 };
        double ex = 0, ey = 0; c.motion_um = 25400 / 5324.275;
        for (int i = 0; i < 6; i++) {
            double rx = rsh_model(&c, 1, FX[i] / 1000, 0) / rsh_model(&c, 1, 8.33 / 1000, 0), ry = rsh_model(&c, 1, 0, FX[i] / 1000) / rsh_model(&c, 1, 0, 8.33 / 1000);
            if (fabs(rx - MX[i]) > ex) ex = fabs(rx - MX[i]); if (fabs(ry - MY[i]) > ey) ey = fabs(ry - MY[i]); }
        CHECK(ex < 0.05 && ey < 0.05, "chart model reproduces the s21 bars at 25 - 67 lp/mm: largest difference across %.3f, along %.3f", ex, ey);
        /* the old model was far off there (review of 0.1.23's sharpening: it assumed too little loss) */
        { double r = rsh_model(&m, 1, 0, 0.0416) / rsh_model(&m, 1, 0, 0.00833); CHECK(r > 0.35, "0.1.22 model along at 41.6 lp/mm %.3f (measured 0.20) - kept for reference", r); }
    }
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
