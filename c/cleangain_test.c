/* cleangain_test: the streak fix on the hardware's own white lines (s34, 11 Oct, 35 mm lens, 12 calibrations) and the
 * vendor archive's L110 clean vector (GAIN 605). The scanner software's correction is modelled as STR# 1202 says
 * ("GainClean ... AvrSiz 150"): after = clean x (150-sample average of before / clean).
 *   cleangain_test <s34_white12.bin> <vendor_gain605.bin> */
#include "cleangain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *d = malloc(l + 1);
    if (fread(d, 1, l, f) != (size_t)l) exit(2);
    fclose(f); *n = (size_t)l; return d;
}
static void correct(const uint16_t *before, const uint16_t *clean, uint16_t *after) {   /* the model of correct_gefen_gain_vec */
    static double q[CG_N];
    for (int i = 0; i < CG_N; i++) q[i] = clean[i] > 0 ? (double)before[i] / clean[i] : 0;
    for (int c = 0; c < 3; c++) for (int p = 0; p < CG_NPIX; p++) {
        double s = 0; int n = 0; for (int k = p - 75; k < p + 75; k++) if (k >= 0 && k < CG_LIGHT) { s += q[k * 3 + c]; n++; }
        double v = clean[p * 3 + c] * (n ? s / n : 1); after[p * 3 + c] = (uint16_t)(v > 65535 ? 65535 : v + 0.5);
    }
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    size_t nw, nv; uint8_t *w = slurp(argv[1], &nw), *gv = slurp(argv[2], &nv);
    int nl = (int)(nw / (CG_N * 2)); static uint16_t L[12][CG_N], vend[CG_N], ours[CG_N], after[CG_N];
    for (int k = 0; k < nl && k < 12; k++) for (int i = 0; i < CG_N; i++) L[k][i] = (uint16_t)(w[(size_t)k * CG_N * 2 + 2 * i] | w[(size_t)k * CG_N * 2 + 2 * i + 1] << 8);
    for (int i = 0; i < CG_N; i++) vend[i] = (uint16_t)(gv[200 + 2 * i] << 8 | gv[201 + 2 * i]);
    double pat = cg_pattern(L[nl - 1]);
    CHECK(pat > 0.006 && pat < 0.01, "this CCD's own pixel pattern (green, per parity): %.2f %% (s34 analysis: 0.73 %%)", pat * 100);
    correct(L[nl - 1], vend, after); double rv = cg_residual(L[nl - 1], after);
    CHECK(rv > 0.008 && rv < 0.013, "left in the scans with the vendor's clean vector (model): %.2f %% (the s34 RAWs show 1.05 %%; white x gain 1.00 %%)", rv * 100);
    /* our clean vector from the first lines, applied to the last one (not part of it) */
    CgAcc *a = calloc(1, sizeof *a); int added = 0; double dmax = 0, lmax = 0;
    for (int k = 0; k < nl - 1; k++) { double d[2]; added += cg_acc_add(a, L[k], d) == 1; if (d[0] > dmax) dmax = d[0]; if (d[1] > lmax) lmax = d[1]; }
    CHECK(added == nl - 1 && a->count == (uint32_t)(nl - 1), "all %d white lines of the scanner taken (largest difference from the average %.3f %% rms, %.3f %% local; limits %.2f / %.2f %%)", added, dmax * 100, lmax * 100, CG_OUTLIER * 100, CG_LOCAL * 100);
    double op; int vr = cg_acc_vector(a, ours, &op);
    CHECK(vr == 0 && cg_acc_valid(a), "the clean vector is a plausible white line");
    correct(L[nl - 1], ours, after); double ro = cg_residual(L[nl - 1], after);
    CHECK(ro < 0.0008, "left with this scanner's own clean vector (%d lines, not the tested one): %.3f %% - %.0f x less", a->count, ro * 100, rv / (ro > 0 ? ro : 1e-9));
    CHECK(fabs(op - pat) < 0.001, "the clean vector keeps the CCD's pattern (%.3f %% vs %.3f %%)", op * 100, pat * 100);
    /* a noisier line (as one averaged 16 times instead of 256: 4 x the noise) is still used */
    static uint16_t noisy[CG_N]; srand(7);
    for (int i = 0; i < CG_N; i++) { double g = 0; for (int k = 0; k < 12; k++) g += rand() / (double)RAND_MAX; g -= 6; noisy[i] = (uint16_t)(L[nl - 1][i] * (1 + 0.0006 * g) + 0.5); }
    CgAcc *b = malloc(sizeof *b); memcpy(b, a, sizeof *b); double dd[2];
    int r0 = cg_acc_add(b, noisy, dd);
    CHECK(r0 == 1, "a line with 4 x the noise is used (difference %.3f %% rms, %.3f %% local)", dd[0] * 100, dd[1] * 100);
    /* defects on the glass, small and wide: none is used */
    static const double DEF[6][2] = { { 30, .05 }, { 30, .03 }, { 100, .02 }, { 10, .05 }, { 60, .01 }, { 200, .01 } };
    static uint16_t dust[6][CG_N]; int nrej = 0; char lst[300]; int lk = 0;
    for (int t = 0; t < 6; t++) { memcpy(dust[t], L[nl - 1], sizeof dust[t]);
        for (int p = 1500 + 300 * t; p < 1500 + 300 * t + DEF[t][0]; p++) for (int c = 0; c < 3; c++) dust[t][p * 3 + c] = (uint16_t)(dust[t][p * 3 + c] * (1 - DEF[t][1]));
        memcpy(b, a, sizeof *b); int r = cg_acc_add(b, dust[t], dd); nrej += r == 0 && b->count == a->count && b->ccount == 1 && b->nref == 1;
        lk += snprintf(lst + lk, sizeof lst - lk, "%s%.0f cells %.0f %%: %.2f", t ? ", " : "", DEF[t][0], DEF[t][1] * 100, dd[1] * 100); }
    CHECK(nrej == 6, "white lines with dust on the glass are not used (local difference %s %%)", lst);
    /* dust: different, or the same 3 times, does not replace the average; a speck that stays for 8 lines does, and
       8 clean lines afterwards replace it again (nothing is locked out) */
    memcpy(b, a, sizeof *b); int q1 = cg_acc_add(b, dust[0], NULL), q2 = cg_acc_add(b, dust[1], NULL), q3 = cg_acc_add(b, dust[2], NULL);
    int q4 = cg_acc_add(b, dust[0], NULL), q5 = cg_acc_add(b, dust[0], NULL), q6 = cg_acc_add(b, dust[0], NULL);
    CHECK(q1 == 0 && q2 == 0 && q3 == 0 && q4 == 0 && q5 == 0 && q6 == 0 && b->count == a->count && b->ccount == 3 && b->nref == 6 && b->total == a->total,
          "3 lines with different dust, then 3 with the same dust: the average stays (6 refused in a row)");
    int qn = 0; for (int k = 0; k < CG_REPLACE_LOCAL - 4; k++) qn += cg_acc_add(b, dust[0], NULL);
    int q8 = cg_acc_add(b, dust[0], NULL);
    CHECK(qn == 0 && q8 == 2 && b->count == CG_REPLACE_LOCAL && b->nref == 0, "the same speck in %d lines in a row replaces it (it stays in the light path)", CG_REPLACE_LOCAL);
    int cn = 0, c8; for (int k = 0; k < CG_REPLACE_LOCAL - 1; k++) cn += cg_acc_add(b, L[k], NULL); c8 = cg_acc_add(b, L[CG_REPLACE_LOCAL - 1], NULL);
    CHECK(cn == 0 && c8 == 2 && b->count == CG_REPLACE_LOCAL, "... and %d clean lines afterwards replace that again", CG_REPLACE_LOCAL);
    /* another CCD pattern (lamp / CCD changed: every pixel 0.7 %% different, the same in each line): 3 replace it */
    static uint16_t other[3][CG_N]; srand(11); static double fct[CG_NPIX];
    for (int p = 0; p < CG_NPIX; p++) { double g = 0; for (int k = 0; k < 12; k++) g += rand() / (double)RAND_MAX; fct[p] = 1 + 0.007 * (g - 6); }
    for (int t = 0; t < 3; t++) for (int i = 0; i < CG_N; i++) other[t][i] = (uint16_t)(L[t][i] * fct[i / 3] + 0.5);
    memcpy(b, a, sizeof *b); double od[2]; int s1 = cg_acc_add(b, other[0], od), s2 = cg_acc_add(b, other[1], NULL), s3 = cg_acc_add(b, other[2], NULL);
    CHECK(s1 == 0 && s2 == 0 && s3 == 2 && b->count == 3 && b->total == 3 && b->ccount == 0 && cg_acc_valid(b),
          "3 lines of a changed light path (%.2f %% rms, %.2f %% local) replace the average", od[0] * 100, od[1] * 100);
    memcpy(b, a, sizeof *b); cg_acc_add(b, other[0], NULL); int g1 = cg_acc_add(b, L[0], NULL);
    CHECK(g1 == 1 && b->ccount == 0 && b->nref == 0 && b->count == a->count + 1, "a good line after a candidate is used and clears the candidate");
    /* a dusty first line: never storable (it stays 1 line), the good ones replace it after CG_REPLACE_LOCAL */
    memset(b, 0, sizeof *b); cg_acc_add(b, dust[1], NULL); int f = 0, fl = 0; for (int k = 0; k < CG_REPLACE_LOCAL; k++) { fl = cg_acc_add(b, L[k], NULL); f += fl == 0; }
    CHECK(f == CG_REPLACE_LOCAL - 1 && fl == 2 && b->count == CG_REPLACE_LOCAL && b->total == CG_REPLACE_LOCAL, "a dusty first line stays alone (not storable) until %d good ones replace it", CG_REPLACE_LOCAL);
    /* a clean first line followed by 3 dusty ones: the clean one stays; the next clean line is added */
    memset(b, 0, sizeof *b); cg_acc_add(b, L[0], NULL); int h3 = 0; for (int k = 0; k < 3; k++) h3 += cg_acc_add(b, dust[0], NULL);
    int h = cg_acc_add(b, L[1], NULL);
    CHECK(h3 == 0 && h == 1 && b->count == 2 && b->ccount == 0, "a clean first line followed by 3 dusty ones stays; the next clean line is added");
    /* the average follows the newest lines: at CG_KEEP lines the weights halve */
    memset(b, 0, sizeof *b); for (int k = 0; k < CG_KEEP + 3; k++) cg_acc_add(b, L[k % (nl - 1)], NULL);
    CHECK(b->count == CG_KEEP / 2 + 3 && b->total == CG_KEEP + 3 && cg_acc_valid(b), "after %d lines the average weighs the newest more (count %u)", CG_KEEP + 3, b->count);
    /* damaged accumulators */
    memcpy(b, a, sizeof *b); b->sum[100] = NAN; int v1 = cg_acc_valid(b), v1r = cg_acc_vector(b, after, NULL);
    memcpy(b, a, sizeof *b); b->level[1] = 0; int v2 = cg_acc_valid(b);
    memcpy(b, a, sizeof *b); b->magic = 0x43474133; int v3 = cg_acc_valid(b);
    memcpy(b, a, sizeof *b); b->ccount = 2; b->csum[5] = -1; int v4 = cg_acc_valid(b);
    CHECK(!v1 && v1r == -1 && !v2 && !v3 && !v4, "a damaged or old-format accumulator is not used");
    memcpy(b, a, sizeof *b); b->sum[100] = NAN; cg_acc_add(b, L[0], NULL);
    CHECK(b->count == 1 && cg_acc_valid(b), "... and adding to it starts again");
    static uint16_t dim[CG_N]; for (int i = 0; i < CG_N; i++) dim[i] = (uint16_t)(L[0][i] / 40);
    memset(b, 0, sizeof *b); cg_acc_add(b, dim, NULL); cg_acc_add(b, dim, NULL);
    CHECK(cg_acc_vector(b, after, NULL) == -2, "a dark 'white line' (level %d) gives no vector", after[3001]);
    free(b);
    /* a lens that lights only part of the CCD: the measures use the lit cells only */
    static uint16_t part[CG_N]; memcpy(part, L[nl - 1], sizeof part);
    for (int p = 0; p < CG_NPIX; p++) if (p < 900 || p > 4100) for (int c = 0; c < 3; c++) part[p * 3 + c] = (uint16_t)(5 + (p * 7 + c) % 4);
    for (int p = 900; p < 960; p++) for (int c = 0; c < 3; c++) part[p * 3 + c] = (uint16_t)(part[p * 3 + c] * (p - 900) / 60.0);
    int lo, hi, nlit = cg_lit(part, &lo, &hi), flo, fhi; cg_lit(L[nl - 1], &flo, &fhi);
    double pp = cg_pattern(part);
    static uint16_t narrow[CG_N]; memcpy(narrow, L[nl - 1], sizeof narrow);   /* a lens lighting only 30 % of the photosites */
    for (int p = 0; p < CG_NPIX; p++) if (p < 1800 || p > 3300) for (int c = 0; c < 3; c++) narrow[p * 3 + c] = 6;
    int nlo, nhi; cg_lit(narrow, &nlo, &nhi); double np2 = cg_pattern(narrow);
    CHECK(flo == 0 && fhi == CG_LIGHT - 1 && lo > 940 && lo < 1000 && hi > 4060 && hi < 4100 && nlit > 3000, "lit photosites: full line %d - %d; partly lit line %d - %d", flo, fhi, lo, hi);
    CHECK(nlo > 1800 && nlo < 1830 && nhi > 3270 && nhi < 3300 && fabs(np2 - pat) < 0.001, "a lens lighting 30 %% of the CCD: lit %d - %d, pattern %.2f %%", nlo, nhi, np2 * 100);
    CHECK(fabs(pp - pat) < 0.0008, "pixel pattern on the partly lit line %.2f %% (whole line %.2f %%)", pp * 100, pat * 100);
    /* the resource */
    static uint8_t res[200 + CG_N * 2]; int id; char name[64]; int when[6] = { 2026, 10, 12, 14, 5, 9 };
    int n = cg_resource(ours, 2, 0, when, "scanner 95171130/01, 11 calibrations", res, sizeof res, &id, name, sizeof name);
    char mk[100], mk2[100]; int own = cg_resource_ours(res, n, mk, sizeof mk); static uint8_t vres[31022]; memcpy(vres, gv, nv < sizeof vres ? nv : sizeof vres);
    CHECK(own && !strcmp(mk, "scanner 95171130/01, 11 calibrations") && !cg_resource_ours(vres, nv, mk2, sizeof mk2), "this program's vectors are recognised (\"%s\"), the vendor's GAIN 605 is not", mk);
    CHECK(n == 31022 && id == 605 && !strcmp(name, "L110 Transparent Clean Gain Vector") && res[3] == 12 && res[5] == 10 && (res[6] << 8 | res[7]) == 2026 && (res[200] << 8 | res[201]) == ours[0],
          "GAIN resource: %d bytes, id %d, \"%s\", header date 12.10.2026, data big-endian from byte 200", n, id, name);
    n = cg_resource(ours, 0, 1, when, NULL, res, sizeof res, &id, name, sizeof name);
    CHECK(id == 602 && !strcmp(name, "L67 Reflective Clean Gain Vector"), "ids and names: %d \"%s\"", id, name);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n"); return fails != 0;
}
