/* cleangain.c - 0.3.1: this scanner's own clean gain vectors.
 * Found 2026-10-12 (docs/re-progress.md "Highlight streaks explained"): at every gain calibration the scanner software
 * replaces the pixel-to-pixel shape of the white line it measured by the stored "Clean Gain Vector" of the lens
 * (Smart342.Prefs GAIN 601 - 609; leagal_gain_vector 0x05ca80 -> correct_gefen_gain_vec 0x05d9c0, STR# 1202
 * "GainClean ... AvrSiz 150": the measured line is used only as a 150-sample average). The package's Prefs come from
 * the vendor archive - another scanner's CCD - so the scans carried this CCD's own pixel pattern plus the other one's
 * inverse (1.0 - 1.2 % rms, single columns up to 5 %, s19 / s34 / s36): streaks along the scan, largest where the
 * film is bright. The fix: clean vectors made from this scanner's own white lines, which the program captures at
 * every calibration (app.c gain_bp_before). A missing clean vector is not an option: the software then stops with
 * "Cln gain file for lens ... is missing" (0x05b47c). */
#include "cleangain.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

static int cmp_u16(const void *a, const void *b) { return (int)*(const uint16_t *)a - (int)*(const uint16_t *)b; }
int cg_lit(const uint16_t *v, int *lo, int *hi) {
    uint16_t s[CG_LIGHT]; for (int p = 0; p < CG_LIGHT; p++) s[p] = v[p * 3 + 1];
    qsort(s, CG_LIGHT, sizeof s[0], cmp_u16);
    double thr = 0.5 * s[CG_LIGHT * 95 / 100]; *lo = 0; *hi = -1;   /* review: not the median - a lens may light less than half */
    if (thr <= 0) return 0;
    int best = 0, bl = 0, start = -1;
    for (int p = 0; p <= CG_LIGHT; p++) {
        int lit = 0;
        if (p < CG_LIGHT) {   /* 31-sample average centred on p (shorter at the ends) */
            int a = p - 15 < 0 ? 0 : p - 15, b = p + 15 >= CG_LIGHT ? CG_LIGHT - 1 : p + 15; double m = 0;
            for (int k = a; k <= b; k++) m += v[k * 3 + 1];
            lit = m / (b - a + 1) > thr;
        }
        if (lit && start < 0) start = p;
        if (!lit && start >= 0) { if (p - start > best) { best = p - start; bl = start; } start = -1; }
    }
    if (best <= 0) return 0;
    int l = bl, h = bl + best - 1;
    if (l > 0) l += 20;
    if (h < CG_LIGHT - 1) h -= 20;
    if (h - l < 100) return 0;
    *lo = l; *hi = h; return h - l + 1;
}
static double pattern_of(const double *g, int npar) {   /* g: one parity's samples; rms of v / mean15(v) - 1 */
    double s = 0, ss = 0; int n = 0;
    for (int i = 7; i < npar - 7; i++) {
        double m = 0; for (int k = -7; k <= 7; k++) m += g[i + k]; m /= 15;
        if (m <= 0) continue;
        double d = g[i] / m - 1; s += d; ss += d * d; n++;
    }
    if (n < 2) return 0;
    double mu = s / n, var = ss / n - mu * mu; return var > 0 ? sqrt(var) : 0;
}
/* ratio of num / den (den NULL: num alone) over num's lit photosites, per parity */
static double measure(const uint16_t *num, const uint16_t *den) {
    int lo, hi; if (!cg_lit(num, &lo, &hi)) return 0;
    double *g = malloc(sizeof(double) * (CG_LIGHT / 2 + 2)); if (!g) return 0;
    double r = 0;
    for (int par = 0; par < 2; par++) {
        int n = 0;
        for (int p = lo + ((lo & 1) != par); p <= hi; p += 2) {
            double a = den ? den[p * 3 + 1] : 1; g[n++] = a > 0 ? num[p * 3 + 1] / a : 0; }
        r += pattern_of(g, n);
    }
    free(g); return r / 2;
}
double cg_pattern(const uint16_t *v) { return measure(v, NULL); }
double cg_residual(const uint16_t *before, const uint16_t *after) { return measure(before, after); }

/* the largest local difference of v from ref: green, per parity, v / ref relative to its own 151-sample average (the
   lamp profile and level drop out), averaged over 15 samples (30 photosites) - a dust spot or a dark patch */
static double local_of(const uint16_t *v, const uint16_t *ref) {
    int lo, hi; if (!cg_lit(v, &lo, &hi)) return 1;
    double *g = malloc(sizeof(double) * (CG_LIGHT / 2 + 2)), *c = malloc(sizeof(double) * (CG_LIGHT / 2 + 3)); double mx = 0;
    if (!g || !c) { free(g); free(c); return 1; }
    for (int par = 0; par < 2; par++) {
        int n = 0;
        for (int p = lo + ((lo & 1) != par); p <= hi; p += 2) { double a = ref ? ref[p * 3 + 1] : 1; g[n++] = a > 0 ? v[p * 3 + 1] / a : 0; }
        c[0] = 0; for (int k = 0; k < n; k++) c[k + 1] = c[k] + g[k];
        for (int k = 7; k < n - 7; k++) {
            int a = k - 75 < 0 ? 0 : k - 75, b = k + 75 >= n ? n - 1 : k + 75;
            double sm = (c[b + 1] - c[a]) / (b - a + 1), w = (c[k + 8] - c[k - 7]) / 15;
            if (sm > 0) { double d = fabs(w / sm - 1); if (d > mx) mx = d; }
        }
    }
    free(g); free(c); return mx;
}
double cg_local(const uint16_t *v) { return local_of(v, NULL); }
void cg_diff(const uint16_t *v, const uint16_t *ref, double *rms, double *local) {
    if (rms) *rms = cg_residual(v, ref);
    if (local) *local = local_of(v, ref);
}

static int part_valid(const double *level, const double *sum) {
    for (int c = 0; c < 3; c++) if (!isfinite(level[c]) || level[c] <= 0) return 0;
    for (int i = 0; i < CG_N; i++) if (!isfinite(sum[i]) || sum[i] < 0) return 0;
    return 1;
}
int cg_acc_valid(const CgAcc *a) {
    if (!a || a->magic != CG_MAGIC || a->count < 1 || a->count > CG_KEEP || a->ccount >= CG_REPLACE_LOCAL || a->clocal > 1 || a->total < a->count) return 0;
    if (!part_valid(a->level, a->sum)) return 0;
    if (a->ccount && !part_valid(a->clevel, a->csum)) return 0;
    return 1;
}
static void part_put(double *level, double *sum, uint32_t *count, const uint16_t *v) {
    double m[3] = { 0, 0, 0 };
    for (int p = 0; p < CG_LIGHT; p++) for (int c = 0; c < 3; c++) m[c] += v[p * 3 + c];
    for (int c = 0; c < 3; c++) { m[c] /= CG_LIGHT; if (m[c] < 1) m[c] = 1; level[c] += m[c]; }
    for (int i = 0; i < CG_N; i++) sum[i] += v[i] / m[i % 3];
    (*count)++;
}
static void part_vector(const double *level, const double *sum, uint32_t count, uint16_t *out) {
    for (int i = 0; i < CG_N; i++) {
        double lv = level[i % 3] / count, v = sum[i] / count * lv;
        out[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5);
    }
}
/* 0 = like ref; 1 = differs broadly (the whole pattern - another light path); 3 = differs locally (dust, a patch) */
static int is_outlier(const uint16_t *v, const uint16_t *ref, double *d) {
    double r, l; cg_diff(v, ref, &r, &l); if (d) { d[0] = r; d[1] = l; }
    if (r <= CG_OUTLIER && l <= CG_LOCAL) return 0;
    return r > CG_OUTLIER && l < 3 * r ? 1 : 3;
}
int cg_acc_add(CgAcc *a, const uint16_t *v, double *d) {
    if (d) d[0] = d[1] = 0;
    if (a->magic != CG_MAGIC || (a->count && !cg_acc_valid(a))) { memset(a, 0, sizeof *a); a->magic = CG_MAGIC; }
    uint16_t *ref = malloc(sizeof(uint16_t) * CG_N); if (!ref) return -1;
    int r = 1;
    if (a->count) {
        part_vector(a->level, a->sum, a->count, ref);
        int o = is_outlier(v, ref, d);
        if (o) {   /* not used; kept as a candidate - candidates that agree with each other replace the average after
                      3 lines (a broad change: another lamp / CCD) or after
                      CG_REPLACE_LOCAL lines (a local one: a speck in the light path that stays - dust on the glass is
                      usually gone before) */
            r = 0; a->nref++;
            if (a->ccount) { part_vector(a->clevel, a->csum, a->ccount, ref); if (is_outlier(v, ref, NULL)) a->ccount = 0; }
            if (!a->ccount) { memset(a->clevel, 0, sizeof a->clevel); memset(a->csum, 0, sizeof a->csum); a->clocal = 0; }
            if (o == 3) a->clocal = 1;   /* review: also against a single line - a dusty line never wins after 3 */
            part_put(a->clevel, a->csum, &a->ccount, v);
            if (a->ccount >= (a->clocal ? CG_REPLACE_LOCAL : 3)) {
                memcpy(a->level, a->clevel, sizeof a->level); memcpy(a->sum, a->csum, sizeof a->sum); a->count = a->total = a->ccount;
                a->ccount = 0; a->clocal = 0; a->nref = 0; memset(a->clevel, 0, sizeof a->clevel); memset(a->csum, 0, sizeof a->csum); r = 2;
            }
            free(ref); return r;
        }
    }
    free(ref);
    a->ccount = 0; a->clocal = 0; a->nref = 0; part_put(a->level, a->sum, &a->count, v); a->total++;
    if (a->count >= CG_KEEP) {   /* the older lines count half from now on: the average follows a slowly changing lamp / CCD */
        for (int c = 0; c < 3; c++) a->level[c] *= 0.5;
        for (int i = 0; i < CG_N; i++) a->sum[i] *= 0.5;
        a->count /= 2;
    }
    return r;
}
int cg_acc_vector(const CgAcc *a, uint16_t *out, double *pattern) {
    if (!cg_acc_valid(a)) return -1;
    part_vector(a->level, a->sum, a->count, out);
    double g = 0; for (int p = 0; p < CG_LIGHT; p++) g += out[p * 3 + 1]; g /= CG_LIGHT;
    double pat = cg_pattern(out); if (pattern) *pattern = pat;
    if (g < 1000 || pat > 0.05) return -2;   /* a white line is bright and its pixels differ by a few tenths of a percent */
    return 0;
}
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
int cg_resource(const uint16_t *v, int lens, int reflective, const int when[6], const char *mark, uint8_t *out, size_t outmax, int *id, char *name, int nname) {
    static const char *LN[5] = { "L67", "L89", "L110", "L60", "L80" };
    if (lens < 0 || lens > 4 || outmax < 200 + CG_N * 2) return -1;
    memset(out, 0, 200);
    /* header as the original's (vendor archive: 0, 26, 2, 2022, 0, 16, 27, 44 = 26 Feb 2022 16:27:44); the scanner
       software reads only from byte 200 on (read_clean_gain_vec 0x05dbf4) */
    put16(out + 2, (unsigned)when[2]); put16(out + 4, (unsigned)when[1]); put16(out + 6, (unsigned)when[0]);
    put16(out + 10, (unsigned)when[3]); put16(out + 12, (unsigned)when[4]); put16(out + 14, (unsigned)when[5]);
    snprintf((char *)out + 100, 99, "%s %s", CG_MARK, mark ? mark : "");   /* unused by the software; says whose it is */
    for (int i = 0; i < CG_N; i++) put16(out + 200 + 2 * i, v[i]);
    *id = 601 + 2 * lens + (reflective ? 1 : 0);
    snprintf(name, nname, "%s %s Clean Gain Vector", LN[lens], reflective ? "Reflective" : "Transparent");
    return 200 + CG_N * 2;
}
int cg_resource_ours(const uint8_t *res, size_t n, char *mark, int markmax) {
    size_t L = strlen(CG_MARK);
    if (mark && markmax > 0) mark[0] = 0;
    if (!res || n < 200 || memcmp(res + 100, CG_MARK, L)) return 0;
    if (mark && markmax > 0) { int k = 0; for (size_t i = 100 + L + 1; i < 199 && res[i] && k < markmax - 1; i++) mark[k++] = (char)res[i]; mark[k] = 0; }
    return 1;
}
