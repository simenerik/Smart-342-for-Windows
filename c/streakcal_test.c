/* streakcal_test: the guided streak calibration's arithmetic on the hardware data of 12 Oct (s37b: empty 4 T35mm H frame,
 * RAW 1887 x 314 at 2662 dpi, CCD = 4382 - x; the three 256-line white lines of that day's scans/RAW; the vector in use)
 * and on synthetic cases.   streakcal_test <ref/streak directory> */
#include "streakcal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static void *slurp(const char *dir, const char *name, size_t want) {
    char p[600]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    void *d = malloc(want); if (fread(d, 1, want, f) != want) { fprintf(stderr, "short %s\n", p); exit(2); }
    fclose(f); return d;
}
static uint32_t rs = 12345; static double urand(void) { rs = rs * 1664525u + 1013904223u; return (rs >> 8) / 16777216.0; }
static double gauss(void) { double a = urand() + 1e-12, b = urand(); return sqrt(-2 * log(a)) * cos(6.283185307 * b); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "../ref/streak";
    /* 1. median vector */
    static uint16_t L[5][CG_N], out[CG_N];
    for (int i = 0; i < CG_N; i++) { double v = 20000 * (1 + 0.007 * gauss()); for (int j = 0; j < 5; j++) L[j][i] = (uint16_t)(v * (j == 2 ? 1.1 : 1.0)); }
    for (int p = 2000; p < 2010; p++) for (int c = 0; c < 3; c++) L[4][p * 3 + c] = (uint16_t)(L[4][p * 3 + c] * 0.9);   /* a speck in one line */
    const uint16_t *lp[5] = { L[0], L[1], L[2], L[3], L[4] };
    CHECK(sc_median_vector(lp, 5, out) == 0, "median vector built");
    double worst = 0; for (int i = 0; i < CG_LIGHT * 3; i++) { double r = fabs((double)out[i] / L[0][i] * (20000.0 * (4 + 1.1) / 5 / 20000.0) - 1); (void)r; }
    { double m0 = 0, mo = 0; for (int p = 0; p < CG_LIGHT; p++) { m0 += L[0][p * 3 + 1]; mo += out[p * 3 + 1]; }
      for (int p = 0; p < CG_LIGHT; p++) { double r = fabs(((double)out[p * 3 + 1] / mo) / ((double)L[0][p * 3 + 1] / m0) - 1); if (r > worst) worst = r; } }
    CHECK(worst < 2e-4, "median = the lines' shape (level-normalised), the speck in one line ignored (largest difference %.5f %%)", worst * 100);
    CHECK(sc_median_vector(lp, 0, out) == -1 && sc_median_vector(lp, SC_KMAX + 1, out) == -1, "k out of range refused");

    /* 2. hardware s37b */
    const int W = 1887, H = 314;
    uint16_t *img = slurp(dir, "s37b_empty_raw_1887x314.bin", (size_t)W * H * 3 * 2);
    uint16_t *sl = slurp(dir, "s37_scanlines3.bin", (size_t)3 * CG_N * 2), *pl = slurp(dir, "s37_previewlines4.bin", (size_t)4 * CG_N * 2);
    uint16_t *vold = slurp(dir, "s37_vector605_in_use.bin", CG_N * 2);
    const uint16_t *s3[3] = { sl, sl + CG_N, sl + 2 * CG_N }, *p4[4] = { pl, pl + CG_N, pl + 2 * CG_N, pl + 3 * CG_N };
    static uint16_t vnew[CG_N], vprev[CG_N];
    sc_median_vector(s3, 3, vnew); sc_median_vector(p4, 4, vprev);
    ScProf pr; CHECK(sc_profile(img, W, H, 3, 4382, -1, &pr) == 0 && pr.c0 == 2496 && pr.n == W, "profile of the empty frame: CCD %d .. %d", pr.c0, pr.c0 + pr.n - 1);
    const uint16_t *own[1] = { s3[2] };   /* the RAW's own white line (18:43) */
    ScEval e; CHECK(sc_eval(&pr, own, 1, vold, vnew, &e) == 0, "evaluated");
    printf("     old %.3f / %.3f / %.3f %%, new %.3f / %.3f / %.3f %%, noise %.3f / %.3f / %.3f %%; above noise G %.3f -> %.3f; mapping corr %.2f (shift %d); %d specks\n",
           e.rms_old[0] * 100, e.rms_old[1] * 100, e.rms_old[2] * 100, e.rms_new[0] * 100, e.rms_new[1] * 100, e.rms_new[2] * 100, e.noise[0] * 100, e.noise[1] * 100, e.noise[2] * 100, e.ex_old[1] * 100, e.ex_new[1] * 100, e.corr0, e.shift, e.nflag);
    for (int r = 0; r < e.nfr; r++) printf("     speck CCD %d - %d, picture +%.3f %% there\n", e.flag_c0[r], e.flag_c1[r], e.flag_d[r] * 100);
    CHECK(e.shift == 0 && e.corr0 > 0.6, "mapping from the driver's DROP/CUT confirmed by the picture (corr %.2f)", e.corr0);
    CHECK(e.ex_old[1] > 0.0016 && e.ex_old[1] < 0.0020 && e.ex_new[1] > 0.0007 && e.ex_new[1] < 0.0011, "streaks above noise 0.18 %% -> about 0.09 %% (as measured by hand, 12 Oct)");
    int hit = 0, hit2 = 0; for (int r = 0; r < e.nfr; r++) { if (e.flag_c0[r] <= 2856 && e.flag_c1[r] >= 2860) hit = 1; if (e.flag_c0[r] <= 4078 && e.flag_c1[r] >= 4080) hit2 = 1; }
    CHECK(hit && hit2 && e.nflag == 2, "the two specks on the keep-clean strip (CCD 2856 - 2860 and 4075 - 4083, seen by the white lines, not by the picture) are found, nothing else (%d)", e.nflag);
    static uint16_t vfix[CG_N]; int nch = sc_repair(&pr, 1, vold, vnew, &e, vfix);
    ScEval ef; sc_eval(&pr, own, 1, vold, vfix, &ef);
    int outside = 0; for (int i = 0; i < CG_N; i++) { int c = i / 3, in = 0; for (int r = 0; r < e.nfr; r++) if (c >= e.flag_c0[r] && c <= e.flag_c1[r]) in = 1; if (!in && vfix[i] != vnew[i]) outside++; }
    CHECK(nch > 0 && nch < 60 && outside == 0 && ef.nflag == 0 && ef.ex_new[1] < e.ex_new[1] && sc_decide(&ef) == 0,
          "repaired from the picture (%d photosites, nothing else changed): above noise G %.3f -> %.3f %%, no speck left; store", nch, e.ex_new[1] * 100, ef.ex_new[1] * 100);
    /* a geometry 3 photosites off is found and corrected */
    ScProf pr3; sc_profile(img, W, H, 3, 4385, -1, &pr3); ScEval e3; sc_eval(&pr3, own, 1, vold, vnew, &e3);
    CHECK(e3.shift == -3 && fabs(e3.ex_new[1] - e.ex_new[1]) < 0.00005, "mapping 3 photosites off: shift %d found, same result (%.3f %%)", e3.shift, e3.ex_new[1] * 100);
    sc_profile_free(&pr3);
    /* the preview lines' median is not what the picture follows */
    ScEval ep; sc_eval(&pr, own, 1, vold, vprev, &ep);
    CHECK(ep.ex_new[1] > 0.0015, "a vector from the 16-line preview calibrations would leave %.3f %% (not better)", ep.ex_new[1] * 100);
    /* the vector in use against itself: nothing changes, nothing flagged */
    ScEval es; sc_eval(&pr, own, 1, vold, vold, &es);
    { int h4 = 0; for (int r = 0; r < es.nfr; r++) if (es.flag_c0[r] <= 4078 && es.flag_c1[r] >= 4080) h4 = 1;
      int h26 = 0; for (int r = 0; r < es.nfr; r++) if (es.flag_c0[r] <= 2698 && es.flag_c1[r] >= 2698) h26 = 1;
      CHECK(fabs(es.ex_new[1] - es.ex_old[1]) < 1e-9 && sc_decide(&es) == 0 && h4 && h26,
            "the vector in use (from 3 preview lines) against itself: no change; %d bright places of its own are found, among them CCD 2698 and 4078", es.nflag); }
    sc_profile_free(&pr);

    /* 3. synthetic: the light path's pattern (CCD + dust in the light path) is in the white lines AND the pictures -> found */
    {
        int w = 1200, h = 200, cx0 = 1500; static double path[CG_LIGHT]; static uint16_t vo[CG_N], ln[3][CG_N], vn[CG_N];
        for (int p = 0; p < CG_LIGHT; p++) path[p] = 1 + 0.008 * gauss() + (p >= 2000 && p < 2012 ? -0.02 : 0);   /* a speck in the path */
        for (int i = 0; i < CG_N; i++) vo[i] = (uint16_t)(20000 * (1 + 0.009 * gauss()));                            /* another scanner's */
        for (int j = 0; j < 3; j++) for (int i = 0; i < CG_N; i++) ln[j][i] = (uint16_t)(20000 * path[i / 3 < CG_LIGHT ? i / 3 : 0] * (1 + 0.0003 * gauss()));
        const uint16_t *lq[3] = { ln[0], ln[1], ln[2] }; sc_median_vector(lq, 3, vn);
        uint16_t *pic = malloc((size_t)w * h * 3 * 2); ScProf ps[2]; const uint16_t *lo[2] = { ln[0], ln[1] };
        for (int k = 0; k < 2; k++) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { int c = cx0 + x; for (int ch = 0; ch < 3; ch++) {
                double v = 30000 * path[c] / (vo[c * 3 + ch] / 20000.0) * (1 + 0.004 * gauss()); pic[((size_t)y * w + x) * 3 + ch] = (uint16_t)v; } }
            sc_profile(pic, w, h, 3, cx0, 1, &ps[k]);
        }
        ScEval ey; sc_eval(ps, lo, 2, vo, vn, &ey);
        CHECK(ey.ex_old[1] > 0.008 && ey.ex_new[1] < 0.0004 && ey.nflag == 0 && sc_decide(&ey) == 0,
              "synthetic: the other scanner's pattern %.3f %% -> %.3f %%, the speck that is in the path is NOT flagged, stored", ey.ex_old[1] * 100, ey.ex_new[1] * 100);
        /* the same, but the white lines see a speck the pictures do not (dust on the strip only) */
        for (int j = 0; j < 3; j++) for (int p = 2400; p < 2410; p++) for (int ch = 0; ch < 3; ch++) ln[j][p * 3 + ch] = (uint16_t)(ln[j][p * 3 + ch] * 0.985);
        sc_median_vector(lq, 3, vn); sc_eval(ps, lo, 2, vo, vn, &ey);
        int f = 0; for (int r = 0; r < ey.nfr; r++) if (ey.flag_c0[r] <= 2400 && ey.flag_c1[r] >= 2409) f = 1;
        static uint16_t vf[CG_N]; sc_repair(ps, 2, vo, vn, &ey, vf); ScEval ez; sc_eval(ps, lo, 2, vo, vf, &ez);
        CHECK(f && ey.nflag == 1 && ez.nflag == 0 && ez.ex_new[1] < 0.0004, "synthetic: a 1.5 %% speck on the strip only is found at CCD 2400 - 2409 and repaired (%.3f %% left above noise)", ez.ex_new[1] * 100);
        sc_profile_free(&ps[0]); sc_profile_free(&ps[1]); free(pic);
    }

    /* 5. 0.5.0 (hardware 18 Oct): the scanner moves red and blue sideways by a fraction of a photosite (its colour alignment
       interpolates them onto green). The red / blue prediction then fails; the result, the specks and the repair must come
       from green, and a repair must not put large red / blue errors into the vector (18 Oct: up to 11 %). 30 specks on the
       strip: all found and repaired (0.4.0 kept 16 and left the rest). */
    {
        int w = 3000, h = 160, cx0 = 900; static double path[CG_LIGHT]; static uint16_t vo[CG_N], ln[3][CG_N], vn[CG_N], vf[CG_N];
        for (int p = 0; p < CG_LIGHT; p++) path[p] = 1 + 0.0073 * gauss();
        for (int i = 0; i < CG_N; i++) vo[i] = (uint16_t)(20000 * (1 + 0.009 * gauss()));
        for (int j = 0; j < 3; j++) for (int i = 0; i < CG_N; i++) ln[j][i] = (uint16_t)(20000 * path[i / 3 < CG_LIGHT ? i / 3 : 0] * (1 + 0.0003 * gauss()));
        int nsp = 30;
        for (int s2 = 0; s2 < nsp; s2++) { int p0 = cx0 + 60 + s2 * 95; for (int j = 0; j < 3; j++) for (int p = p0; p < p0 + 9; p++) for (int ch = 0; ch < 3; ch++) ln[j][p * 3 + ch] = (uint16_t)(ln[j][p * 3 + ch] * 0.99); }
        const uint16_t *lq[3] = { ln[0], ln[1], ln[2] }; sc_median_vector(lq, 3, vn);
        uint16_t *pic = malloc((size_t)w * h * 3 * 2); ScProf ps[2]; const uint16_t *lo[2] = { ln[0], ln[1] };
        static const double sh[3] = { 0.37, 0, -0.42 };      /* red / blue interpolated this far from the photosite */
        for (int k = 0; k < 2; k++) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int ch = 0; ch < 3; ch++) {
                double pos = cx0 + x + sh[ch]; int a = (int)floor(pos); double f = pos - a;
                double va = path[a] / (vo[a * 3 + ch] / 20000.0), vb = path[a + 1] / (vo[(a + 1) * 3 + ch] / 20000.0);
                pic[((size_t)y * w + x) * 3 + ch] = (uint16_t)(30000 * ((1 - f) * va + f * vb) * (1 + 0.004 * gauss())); }
            sc_profile(pic, w, h, 3, cx0, 1, &ps[k]);
        }
        ScEval ey; sc_eval(ps, lo, 2, vo, vn, &ey);
        sc_repair(ps, 2, vo, vn, &ey, vf);
        double worst = 0; for (int i = 0; i < CG_LIGHT * 3; i++) { double r = fabs((double)vf[i] / vn[i] - 1); if (r > worst) worst = r; }
        int found = 0; for (int s2 = 0; s2 < nsp; s2++) { int p0 = cx0 + 60 + s2 * 95; for (int r = 0; r < ey.nfr; r++) if (ey.flag_c0[r] <= p0 && ey.flag_c1[r] >= p0 + 8) { found++; break; } }
        /* the repaired vector against the true light path, green and red alike */
        double eg = 0, er = 0; int m = 0;
        for (int s2 = 0; s2 < nsp; s2++) { int p0 = cx0 + 60 + s2 * 95; for (int p = p0; p < p0 + 9; p++) {
            double tg = (double)vf[p * 3 + 1] / vn[p * 3 + 1] - 1 / 0.99, tr = (double)vf[p * 3] / vn[p * 3] - 1 / 0.99; eg += tg * tg; er += tr * tr; m++; } }
        eg = sqrt(eg / m); er = sqrt(er / m);
        CHECK(sc_decide(&ey) == 0 && ey.ex_new[1] < 0.003 && ey.nflag == nsp && found == nsp && worst <= SC_MAXFIX + 1e-4 && er < 0.004,
              "red / blue interpolated (+0.37 / -0.42 photosite): decided on green (%.3f %% -> %.3f %%; red would show %.3f %%), %d of %d specks found (none extra: %d), largest repair %.2f %%, repaired vector vs the true path: green %.3f %%, red %.3f %%",
              ey.ex_old[1] * 100, ey.ex_new[1] * 100, ey.ex_new[0] * 100, found, nsp, ey.nflag, worst * 100, eg * 100, er * 100);
        sc_profile_free(&ps[0]); sc_profile_free(&ps[1]); free(pic);
    }
    /* 4. bad input */
    { ScProf q; uint16_t tiny[64 * 3]; CHECK(sc_profile(tiny, 10, 2, 3, 0, 1, &q) == -1 && sc_profile(img, W, H, 2, 0, 1, &q) == -1, "bad sizes / channels refused"); }
    free(img); free(sl); free(pl); free(vold);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails != 0;
}
