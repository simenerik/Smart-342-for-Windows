/* streakcal.c - 0.4.0: the guided streak calibration's arithmetic (see streakcal.h) */
#include "streakcal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

static void chan_means(const uint16_t *v, double m[3]) {
    double s[3] = { 0, 0, 0 };
    for (int p = 0; p < CG_LIGHT; p++) for (int c = 0; c < 3; c++) s[c] += v[p * 3 + c];
    for (int c = 0; c < 3; c++) { m[c] = s[c] / CG_LIGHT; if (m[c] < 1) m[c] = 1; }
}
int sc_median_vector(const uint16_t *const *lines, int k, uint16_t *out) {
    if (k < 1 || k > SC_KMAX) return -1;
    double m[SC_KMAX][3], lev[3] = { 0, 0, 0 };
    for (int j = 0; j < k; j++) { chan_means(lines[j], m[j]); for (int c = 0; c < 3; c++) lev[c] += m[j][c] / k; }
    for (int i = 0; i < CG_N; i++) {
        double a[SC_KMAX]; int c = i % 3;
        for (int j = 0; j < k; j++) {                          /* insertion sort of k <= 9 values */
            double v = lines[j][i] / m[j][c]; int q = j;
            while (q > 0 && a[q - 1] > v) { a[q] = a[q - 1]; q--; }
            a[q] = v;
        }
        double med = (k & 1) ? a[k / 2] : 0.5 * (a[k / 2 - 1] + a[k / 2]);
        double v = med * lev[c];
        out[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5);
    }
    return 0;
}

void sc_profile_free(ScProf *pr) {
    for (int c = 0; c < 3; c++) { free(pr->p[c]); free(pr->h1[c]); free(pr->h2[c]); pr->p[c] = pr->h1[c] = pr->h2[c] = NULL; }
    pr->n = 0;
}
int sc_profile(const uint16_t *img, int w, int h, int ch, int ccd_x0, int dir, ScProf *pr) {
    memset(pr, 0, sizeof *pr);
    if (!img || w < 64 || h < 8 || (ch != 1 && ch != 3) || (dir != 1 && dir != -1)) return -1;
    int r0 = h / 20, r1 = h - h / 20, half = (r1 - r0) / 2;
    if (half < 2) return -1;
    pr->n = w; pr->c0 = dir > 0 ? ccd_x0 : ccd_x0 - (w - 1);
    for (int c = 0; c < 3; c++) {
        pr->p[c] = calloc((size_t)w, sizeof(double)); pr->h1[c] = calloc((size_t)w, sizeof(double)); pr->h2[c] = calloc((size_t)w, sizeof(double));
        if (!pr->p[c] || !pr->h1[c] || !pr->h2[c]) { sc_profile_free(pr); return -1; }
    }
    for (int x = 0; x < w; x++) {
        int i = ccd_x0 + dir * x - pr->c0;
        for (int c = 0; c < 3; c++) {
            int cc = ch == 1 ? 0 : c; double s1 = 0, s2 = 0;
            for (int y = r0; y < r0 + half; y++) s1 += img[((size_t)y * w + x) * ch + cc];
            for (int y = r0 + half; y < r0 + 2 * half; y++) s2 += img[((size_t)y * w + x) * ch + cc];
            pr->h1[c][i] = s1 / half; pr->h2[c][i] = s2 / half; pr->p[c][i] = (s1 + s2) / (2.0 * half);
        }
    }
    return 0;
}

/* each readout parity (parity of the CCD photosite) on its own: value / its 31-sample moving average - 1 */
static void hp(const double *v, int n, int c0, double *out) {
    double *g = malloc(sizeof(double) * (n / 2 + 2)), *cs = malloc(sizeof(double) * (n / 2 + 3));
    if (!g || !cs) { free(g); free(cs); for (int i = 0; i < n; i++) out[i] = 0; return; }
    for (int par = 0; par < 2; par++) {
        int i0 = ((c0 & 1) == par) ? 0 : 1, m = 0;
        for (int i = i0; i < n; i += 2) g[m++] = v[i];
        cs[0] = 0; for (int q = 0; q < m; q++) cs[q + 1] = cs[q] + g[q];
        for (int q = 0; q < m; q++) {
            int a = q - 15 < 0 ? 0 : q - 15, b = q + 15 >= m ? m - 1 : q + 15;
            double mean = (cs[b + 1] - cs[a]) / (b - a + 1);
            out[i0 + 2 * q] = mean > 0 ? g[q] / mean - 1 : 0;
        }
    }
    free(g); free(cs);
}
static double sdev(const double *v, int a, int b) {
    double s = 0, ss = 0; int n = b - a; if (n < 2) return 0;
    for (int i = a; i < b; i++) { s += v[i]; ss += v[i] * v[i]; }
    double mu = s / n, var = ss / n - mu * mu; return var > 0 ? sqrt(var) : 0;
}
static double corr(const double *a, const double *b, int i0, int i1) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0; int n = i1 - i0; if (n < 3) return 0;
    for (int i = i0; i < i1; i++) { sa += a[i]; sb += b[i]; saa += a[i] * a[i]; sbb += b[i] * b[i]; sab += a[i] * b[i]; }
    double va = saa - sa * sa / n, vb = sbb - sb * sb / n, cv = sab - sa * sb / n;
    return va > 0 && vb > 0 ? cv / sqrt(va * vb) : 0;
}
static double vat(const uint16_t *v, int c, int ch) { return (c >= 0 && c < CG_LIGHT) ? v[c * 3 + ch] : 0; }

int sc_eval(const ScProf *prof, const uint16_t *const *line, int k, const uint16_t *vold, const uint16_t *vnew, ScEval *e) {
    memset(e, 0, sizeof *e);
    if (k < 1 || k > SC_KMAX || !vold || !vnew) return -1;
    int n = prof[0].n, c0 = prof[0].c0;
    for (int j = 1; j < k; j++) if (prof[j].n != n || prof[j].c0 != c0) return -1;
    if (n < 96 || c0 < 0 || c0 + n > CG_LIGHT) return -1;
    int i0 = 16, i1 = n - 16;
    double *t = malloc(sizeof(double) * n), *u = malloc(sizeof(double) * n), *ao[3], *an[3], *pb = malloc(sizeof(double) * n);
    for (int c = 0; c < 3; c++) { ao[c] = calloc((size_t)n, sizeof(double)); an[c] = calloc((size_t)n, sizeof(double)); }
    int ok = t && u && pb && ao[0] && ao[1] && ao[2] && an[0] && an[1] && an[2];
    if (!ok) goto out;
    /* the mapping check (green): the old pictures' pattern against (white line / vold) */
    for (int j = 0; j < k; j++) { hp(prof[j].p[1], n, c0, t); for (int i = 0; i < n; i++) ao[1][i] += t[i] / k; }
    e->shift = 0;
    if (line) {
        double best = -2; int bd = 0;
        for (int d = -6; d <= 6; d++) {
            for (int i = 0; i < n; i++) pb[i] = 0;
            int have = 0;
            for (int j = 0; j < k; j++) { if (!line[j]) continue; have++;
                for (int i = 0; i < n; i++) { double vo = vat(vold, c0 + i + d, 1); u[i] = vo > 0 ? vat(line[j], c0 + i + d, 1) / vo : 1; }
                hp(u, n, c0 + d, t); for (int i = 0; i < n; i++) pb[i] += t[i]; }
            if (!have) break;
            double r = corr(ao[1], pb, i0, i1);
            if (d == 0) e->corr0 = r;
            if (r > best) { best = r; bd = d; }
        }
        e->corr = best;
        if (best > 0.3 && best > e->corr0 + 0.05) e->shift = bd; else e->corr = e->corr0;
    }
    for (int i = 0; i < n; i++) ao[1][i] = 0;
    int d = e->shift;
    for (int c = 0; c < 3; c++) {
        for (int j = 0; j < k; j++) {
            hp(prof[j].p[c], n, c0, t);
            e->rms_old[c] += sdev(t, i0, i1) / k;
            for (int i = 0; i < n; i++) ao[c][i] += t[i] / k;
            for (int i = 0; i < n; i++) { double vn = vat(vnew, c0 + i + d, c); u[i] = vn > 0 ? prof[j].p[c][i] * vat(vold, c0 + i + d, c) / vn : prof[j].p[c][i]; }
            hp(u, n, c0, t);
            e->rms_new[c] += sdev(t, i0, i1) / k;
            for (int i = 0; i < n; i++) an[c][i] += t[i] / k;
            hp(prof[j].h1[c], n, c0, t); hp(prof[j].h2[c], n, c0, u);
            for (int i = 0; i < n; i++) t[i] -= u[i];
            e->noise[c] += sdev(t, i0, i1) / 2 / k;
        }
        double nf = e->noise[c];
        e->ex_old[c] = e->rms_old[c] > nf ? sqrt(e->rms_old[c] * e->rms_old[c] - nf * nf) : 0;
        e->ex_new[c] = e->rms_new[c] > nf ? sqrt(e->rms_new[c] * e->rms_new[c] - nf * nf) : 0;
    }
    /* bright places in the corrected picture (mean over the pictures, GREEN only - see streakcal.h; 5-photosite mean) */
    {
        double *m = t; for (int i = 0; i < n; i++) m[i] = an[1][i];
        for (int i = 0; i < n; i++) { double s = 0; int c = 0; for (int q = i - 2; q <= i + 2; q++) if (q >= 0 && q < n) { s += m[q]; c++; } u[i] = s / c; }
        int i = i0;
        while (i < i1) {
            if (u[i] <= SC_BUMP) { i++; continue; }
            int a = i; double pk = u[i];
            while (i < i1 && u[i] > SC_BUMP / 2) { if (u[i] > pk) pk = u[i]; i++; }
            int lo = c0 + d + a - SC_MARGIN, hi = c0 + d + i - 1 + SC_MARGIN;
            if (e->nfr > 0 && lo <= e->flag_c1[e->nfr - 1] + 1) { e->flag_c1[e->nfr - 1] = hi; if (pk > e->flag_d[e->nfr - 1]) e->flag_d[e->nfr - 1] = pk; }
            else { e->nflag++; if (e->nfr < SC_MAXR) { e->flag_c0[e->nfr] = lo; e->flag_c1[e->nfr] = hi; e->flag_d[e->nfr] = pk; e->nfr++; } }
        }
    }
out:
    free(t); free(u); free(pb); for (int c = 0; c < 3; c++) { free(ao[c]); free(an[c]); }
    return ok ? 0 : -1;
}

int sc_repair(const ScProf *prof, int k, const uint16_t *vold, const uint16_t *vnew, const ScEval *e, uint16_t *vfix) {
    memcpy(vfix, vnew, sizeof(uint16_t) * CG_N);
    if (k < 1 || k > SC_KMAX) return -1;
    int n = prof[0].n, c0 = prof[0].c0, d = e->shift, changed = 0;
    double *t = malloc(sizeof(double) * n), *u = calloc((size_t)n, sizeof(double)), *an = calloc((size_t)n, sizeof(double));
    if (!t || !u || !an) { free(t); free(u); free(an); return -1; }
    /* the green corrected picture's pattern (see streakcal.h: red and blue cannot be predicted per photosite) */
    for (int j = 0; j < k; j++) {
        for (int i = 0; i < n; i++) { double vn = vat(vnew, c0 + i + d, 1); u[i] = vn > 0 ? prof[j].p[1][i] * vat(vold, c0 + i + d, 1) / vn : prof[j].p[1][i]; }
        hp(u, n, c0, t); for (int i = 0; i < n; i++) an[i] += t[i] / k;
    }
    for (int r = 0; r < e->nfr; r++) for (int cc = e->flag_c0[r]; cc <= e->flag_c1[r]; cc++) {
        int i = cc - c0 - d; if (i < 0 || i >= n || cc < 0 || cc >= CG_LIGHT) continue;
        double f = an[i] > SC_MAXFIX ? SC_MAXFIX : an[i] < -SC_MAXFIX ? -SC_MAXFIX : an[i];
        for (int c = 0; c < 3; c++) { double v = vnew[cc * 3 + c] * (1 + f); vfix[cc * 3 + c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5); }
        changed++;
    }
    free(t); free(u); free(an); return changed;
}
int sc_decide(const ScEval *e) {
    double o = e->ex_old[1], n = e->ex_new[1];   /* green (streakcal.h) */
    if (n <= o + 0.0002 || n < 0.0005) return 0;
    return 2;
}
