/* rawsr.c - see rawsr.h. */
#include "rawsr.h"
#include "rawstack.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define SR_REG_SIGMA 1.5    /* pixels; tested 0 (biased) and 1.5 - see rawsr_test.c */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void lanczos6(double f, double w[6]) {   /* as rawstack.c / rawtile.c, with the first-moment correction */
    double s = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f, v;
        if (fabs(t) < 1e-9) v = 1; else if (fabs(t) >= 3) v = 0; else v = 3 * sin(M_PI * t) * sin(M_PI * t / 3) / (M_PI * M_PI * t * t);
        w[i] = v; s += v; }
    for (int i = 0; i < 6; i++) w[i] /= s;
    double m1 = 0, mt = 0, st = 0;
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; m1 += w[i] * t; mt += t; }
    mt /= 6; for (int i = 0; i < 6; i++) { double t = (i - 2) - f; st += (t - mt) * t; }
    for (int i = 0; i < 6; i++) { double t = (i - 2) - f; w[i] -= m1 / st * (t - mt); }
}

/* one channel, Gaussian-blurred (separable, sigma in pixels, edges clamped), for the registration only. Why: passes
   at different sub-cell phases carry detail above the single-pass limit folded back (aliased) differently, which pulls
   the sub-pixel registration towards whole pixels - found in testing: 0.2 cell errors at phases 0.27 and 0.76, none
   at 0.5. Blurring both pictures first leaves mainly the coarse detail, which every pass carries alike. */
static uint16_t *blur_chan(const uint16_t *p, int w, int h, int ch, int c, double sigma) {
    int r = (int)ceil(3 * sigma); double k[64]; double ks = 0; if (r > 31) r = 31;
    for (int i = -r; i <= r; i++) { k[i + r] = exp(-0.5 * i * i / (sigma * sigma)); ks += k[i + r]; }
    for (int i = 0; i <= 2 * r; i++) k[i] /= ks;
    float *t = malloc((size_t)w * h * sizeof(float)); uint16_t *o = malloc((size_t)w * h * sizeof(uint16_t));
    if (!t || !o) { free(t); free(o); return NULL; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double a = 0;
        for (int i = -r; i <= r; i++) { int xx = x + i < 0 ? 0 : x + i >= w ? w - 1 : x + i; a += k[i + r] * p[((size_t)y * w + xx) * ch + c]; }
        t[(size_t)y * w + x] = (float)a; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { double a = 0;
        for (int i = -r; i <= r; i++) { int yy = y + i < 0 ? 0 : y + i >= h ? h - 1 : y + i; a += k[i + r] * t[(size_t)yy * w + x]; }
        o[(size_t)y * w + x] = (uint16_t)(a < 0 ? 0 : a > 65535 ? 65535 : a + 0.5); }
    free(t); return o;
}

double sr_griddist(const double *phase, int n, int F) {
    double gd = 0;
    for (int r = 0; r < F; r++) { double best = 1e9;
        for (int k = 0; k < n; k++) { double pos = F * phase[k], d = fmod(fabs(pos - r), (double)F); if (F - d < d) d = F - d; if (d < best) best = d; }
        if (best > gd) gd = best; }
    return gd;
}
static double circ(double pos, double r, int F) { double d = fmod(fabs(pos - r), (double)F); return F - d < d ? F - d : d; }
/* the offset a step will give: measured (mean of its passes) if taken before, else interpolated between the measured
   steps around it, else extrapolated from the two nearest measured steps */
static double pred_offset(const double *dx, const int *steps, int n, int k) {
    double sum = 0; int cnt = 0; for (int i = 0; i < n; i++) if (steps[i] == k) { sum += dx[i]; cnt++; }
    if (cnt) return sum / cnt;
    int lo = -1, hi = -1; double vlo = 0, vhi = 0;   /* nearest measured steps below / above (means) */
    for (int i = 0; i < n; i++) { int s = steps[i];
        if (s < k && (lo < 0 || s > lo)) lo = s; if (s > k && (hi < 0 || s < hi)) hi = s; }
    if (lo >= 0) { double a = 0; int c = 0; for (int i = 0; i < n; i++) if (steps[i] == lo) { a += dx[i]; c++; } vlo = a / c; }
    if (hi >= 0) { double a = 0; int c = 0; for (int i = 0; i < n; i++) if (steps[i] == hi) { a += dx[i]; c++; } vhi = a / c; }
    if (lo >= 0 && hi >= 0) return vlo + (vhi - vlo) * (k - lo) / (hi - lo);
    /* outside: the two nearest measured steps on that side */
    int a1 = lo >= 0 ? lo : hi, a2 = -1;
    for (int i = 0; i < n; i++) { int s = steps[i]; if (s == a1) continue; if (lo >= 0 ? (s < a1 && (a2 < 0 || s > a2)) : (s > a1 && (a2 < 0 || s < a2))) a2 = s; }
    double v1 = lo >= 0 ? vlo : vhi; if (a2 < 0) return v1;
    double v2 = 0; int c = 0; for (int i = 0; i < n; i++) if (steps[i] == a2) { v2 += dx[i]; c++; } v2 /= c;
    return v1 + (v1 - v2) * (k - a1) / (double)(a1 - a2);
}
/* 0.3.0: the model predictor (rawsr.h sr_pred; chosen on the replay of every hardware pixel-shift run and lens test,
   s11 - s36: in the from-above slow mode the passes predicted more than 0.25 px off fell from 70 % to 42 %, the median
   error from 0.41 to 0.21 px). Ridge least squares dx = a + s k (+ d i once 5 passes exist), with the slope pulled
   toward the prior s0 (weight (0.25 / 0.1)^2: a slope known to about 0.1 px/step against 0.25 px of landing scatter)
   and the drift toward 0 (0.1 px per pass). Returns 0 and the coefficients, -1 when it cannot be solved. */
SrPred sr_pred;
static int solve3(double A[3][3], double b[3], double x[3]);
static int model_fit(const double *dx, const int *steps, int n, double *a, double *s, double *d) {
    if (n < 1) return -1;
    int use_d = n >= 5; double lam = sr_pred.prior != 0 ? 6.25 : 0, lamd = 6.25;
    double A[3][3] = { { 0 } }, b[3] = { 0 }, x[3] = { 0 };
    for (int i = 0; i < n; i++) { double f[3] = { 1, (double)steps[i], (double)i };
        for (int r = 0; r < 3; r++) { b[r] += f[r] * dx[i]; for (int q = 0; q < 3; q++) A[r][q] += f[r] * f[q]; } }
    A[1][1] += lam; b[1] += lam * sr_pred.prior;
    if (use_d) { A[2][2] += lamd;
        double A2[3][3]; memcpy(A2, A, sizeof A2); double b2[3]; memcpy(b2, b, sizeof b2);
        if (!solve3(A2, b2, x)) { *a = x[0]; *s = x[1]; *d = x[2]; return 0; } }
    double det = A[0][0] * A[1][1] - A[0][1] * A[1][0];
    if (fabs(det) < 1e-12) return -1;
    *a = (b[0] * A[1][1] - A[0][1] * b[1]) / det; *s = (A[0][0] * b[1] - A[1][0] * b[0]) / det; *d = 0; return 0;
}
double sr_model_pred(const double *dx, const int *steps, int n, int k, double *slope) {
    double a, s, d; if (model_fit(dx, steps, n, &a, &s, &d)) return NAN;
    if (slope) *slope = s; return a + s * k + d * n;
}
static double predict(const double *dx, const int *steps, int n, int k) {
    if (sr_pred.on) { double v = sr_model_pred(dx, steps, n, k, NULL); if (!isnan(v)) return v; }
    return pred_offset(dx, steps, n, k);
}
double sr_grid_cost2(const double *pos, int n, int F, double *maxbias) {   /* pos: -dx of each pass (sample position in cells) */
    double sk[17]; int rk[17], nk[4] = { -1, -1, -1, -1 };
    if (n > 17 || F < 1 || F > 4) return 1e9;
    for (int k = 0; k < n; k++) { double u = F * pos[k]; int jj = (int)floor(u + 0.5); sk[k] = u - jj; rk[k] = ((jj % F) + F) % F;
        int r = rk[k]; if (nk[r] < 0 || fabs(sk[k]) < fabs(sk[nk[r]])) nk[r] = k; }
    double cost = 0; if (maxbias) *maxbias = 0;
    for (int r = 0; r < F; r++) {
        if (nk[r] < 0) { cost += 1; if (maxbias) *maxbias = 0.5; continue; }   /* a class without a sample: filled in between */
        double dmin = fabs(sk[nk[r]]), sw = 0, sw2 = 0, sws = 0;
        for (int k = 0; k < n; k++) { if (rk[k] != r) continue;
            if (!(fabs(sk[k]) <= dmin + SR_NEAR || (dmin > SR_NEAR && sk[k] * sk[nk[r]] < 0 && fabs(sk[k]) < 0.5))) continue;
            double w = (1 - 2 * fabs(sk[k])) * (1 - 2 * fabs(sk[k])); sw += w; sw2 += w * w; sws += w * sk[k]; }
        if (sw <= 1e-9) { cost += 1; if (maxbias) *maxbias = 0.5; continue; }
        double b = sws / sw, neff = sw * sw / sw2;
        cost += b * b + SR_NOISE_W / neff;
        if (maxbias && fabs(b) > *maxbias) *maxbias = fabs(b);
    }
    return cost;
}
double sr_grid_cost(const double *pos, int n, int F) { return sr_grid_cost2(pos, n, F, NULL); }
/* the uncertainty (px) of the predicted offset of step k: measured before -> repeat error; else SR_STEP_SIGMA per step
   of distance to the nearest measured step */
static double pred_sigma(const int *steps, int n, int k) {
    int d = 1 << 20; for (int i = 0; i < n; i++) { int e = abs(steps[i] - k); if (e < d) d = e; }
    if (d == 0) return SR_REPEAT_SIGMA;
    if (sr_pred.on) { double s0 = sr_pred.sigma0 > 0 ? sr_pred.sigma0 : 0.3, s1 = sr_pred.sigma_step > 0 ? sr_pred.sigma_step : 0.1;
        return sqrt(s0 * s0 + (s1 * d) * (s1 * d)); }   /* 0.3.0 replay: with the model the error hardly grows with d */
    return SR_STEP_SIGMA * d;
}
int sr_next_step3(const double *dx, const int *steps, int n, int F, int balance, double max_off, double *pred_gd, double *pred_phase) {
    if (n < 2 || n >= 16 || F < 1 || F > 4) return -1;
    /* the local slope for exploring (the grid not yet covered): from the two largest steps taken */
    int s1 = -1, s2 = -1; for (int i = 0; i < n; i++) { if (s1 < 0 || steps[i] > steps[s1]) s1 = i; }
    for (int i = 0; i < n; i++) { if (steps[i] < steps[s1] && (s2 < 0 || steps[i] > steps[s2])) s2 = i; }
    int last = s1 >= 0 ? steps[s1] : 0;
    double slope = (s1 >= 0 && s2 >= 0) ? (pred_offset(dx, steps, n, steps[s1]) - pred_offset(dx, steps, n, steps[s2])) / (steps[s1] - steps[s2]) : 0;
    if (sr_pred.on) { double sl; if (!isnan(sr_model_pred(dx, steps, n, 0, &sl))) slope = sl; }
    double ph[17]; for (int i = 0; i < n; i++) { double p = -dx[i]; p -= floor(p); if (p >= 1 - 1e-9) p = 0; ph[i] = p; }
    int best = -1; double bgd = 1e9, bph = 0;
    double gd_now = sr_griddist(ph, n, F);
    if (gd_now <= SR_STEP_TARGET) {   /* covered: the lowest expected error (see rawsr.h) */
        int kmax = last + SR_STEP_RANGE; if (kmax > SR_STEP_MAX) kmax = SR_STEP_MAX;
        double sv[17]; for (int i = 0; i < n; i++) sv[i] = -dx[i];
        double bc = 1e9, bp = -1, bu = 1e9;
        for (int k = 0; k <= kmax; k++) {
            double c0 = -predict(dx, steps, n, k), u = pred_sigma(steps, n, k), c = 0, pok = 0, wsum = 0;
            if (max_off > 0 && fabs(c0) > max_off) continue;
            if (u == SR_REPEAT_SIGMA) { if (!balance) continue; u = 0; }   /* a repeat: normal setting - not taken (it does not move a
                   sample nearer a column); 8-pass setting - its position is known, it adds samples there (less noise) */
            /* expected cost and the chance that every class ends within SR_BIAS_OK, over the position's uncertainty:
               the cost repeats every cell, so from u >= 0.5 cell the phase is as good as unknown - averaged over a cell */
            int nq = u <= 0 ? 1 : 41;
            for (int q = 0; q < nq; q++) {
                double t, w;
                if (u <= 0) { t = 0; w = 1; }
                else if (u >= 0.5) { t = q / (double)nq; w = 1; }
                else { double z = -3 + 6.0 * q / (nq - 1); t = z * u; w = exp(-z * z / 2); }
                double mb = 1; sv[n] = c0 + t; c += w * sr_grid_cost2(sv, n + 1, F, &mb); if (mb <= SR_BIAS_OK) pok += w; wsum += w; }
            c /= wsum; pok /= wsum;
            /* normal setting: the step most likely to bring every class within SR_BIAS_OK, then the lower expected
               error; 8-pass setting: the lower expected error. Near-equal (chance within 0.05 - the uncertainty model is rough -, error within 2 %):
               the better-known position, then the smaller step (review 0.1.23: no far guess when a near step is as good) */
            int pe = fabs(pok - bp) <= 0.05, ce = fabs(c - bc) <= 0.02 * bc, better;
            if (balance) better = !ce ? c < bc : u < bu - 1e-9;
            else better = !pe ? pok > bp : !ce ? c < bc : u < bu - 1e-9;
            if (better) { bc = c; bp = pok; bu = u; best = k; double p = c0; p -= floor(p); if (p >= 1 - 1e-9) p = 0; bph = p; }
        }
        if (best >= 0) { ph[n] = bph; bgd = sr_griddist(ph, n + 1, F); }
    } else {
        if (fabs(slope) < 0.05) return -1;   /* the carriage does not move the picture */
        double dl = pred_offset(dx, steps, n, last);
        for (int k = last + 1; k <= last + SR_STEP_RANGE && k <= SR_STEP_MAX; k++) {
            double d = sr_pred.on ? predict(dx, steps, n, k) : dl + slope * (k - last), p = -d; p -= floor(p); if (p >= 1 - 1e-9) p = 0;
            if (max_off > 0 && fabs(d) > max_off) break;
            ph[n] = p; double gd = sr_griddist(ph, n + 1, F);
            if (gd < bgd - 1e-9) { bgd = gd; best = k; bph = p; }
            if (gd <= SR_STEP_TARGET) break;   /* the smallest step that is good enough: small steps change the light least and are predicted best */
        }
    }
    if (pred_gd) *pred_gd = bgd; if (pred_phase) *pred_phase = bph;
    return best;
}
int sr_next_step2(const double *dx, const int *steps, int n, int F, int balance, double *pred_gd, double *pred_phase) { return sr_next_step3(dx, steps, n, F, balance, 0, pred_gd, pred_phase); }
int sr_next_step(const double *dx, const int *steps, int n, int F, double *pred_gd, double *pred_phase) { return sr_next_step3(dx, steps, n, F, 0, 0, pred_gd, pred_phase); }
double sr_register1(const uint16_t *p0, const uint16_t *pk, int w, int h, int ch, int start, double *dx, double *dy) {
    int g = ch == 3 ? 1 : 0;
    uint16_t *b0 = blur_chan(p0, w, h, ch, g, SR_REG_SIGMA), *bk = blur_chan(pk, w, h, ch, g, SR_REG_SIGMA);
    if (!b0 || !bk) { free(b0); free(bk); return -1; }
    RsImg a = { b0, w, h, 1, 0, NULL }, b = { bk, w, h, 1, 0, NULL };
    if (!start) { *dx = 0; *dy = 0; }
    double sc = rs_register(a, b, NULL, start, dx, dy);
    free(b0); free(bk); return sc;
}
int sr_check_offset(const double *dx, const double *dy, const int *steps, int n, int step, int sign, double mx, double my,
                    double along_tol, double ex[2], int *nex, double *ey) {
    *nex = 0; *ey = 0; if (n < 1 || n > 16) return 0;
    double v[16]; for (int i = 0; i < n; i++) v[i] = dy[i];
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    *ey = n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    int oky = !(along_tol > 0) || fabs(my - *ey) <= along_tol;
    /* across: the accepted steps below / at / above this one */
    int lo = -1 << 20, hi = 1 << 20, at = 0; double sa = 0;
    for (int i = 0; i < n; i++) { if (steps[i] == step) { sa += dx[i]; at++; } else if (steps[i] < step && steps[i] > lo) lo = steps[i]; else if (steps[i] > step && steps[i] < hi) hi = steps[i]; }
    #define MEAN_AT(s_, out_) { double a_ = 0; int c_ = 0; for (int i = 0; i < n; i++) if (steps[i] == (s_)) { a_ += dx[i]; c_++; } out_ = a_ / c_; }
    int okx;
    if (at) { ex[0] = sa / at; *nex = 1; okx = fabs(mx - ex[0]) <= SR_CHECK_SLACK; }
    else if (lo > (-1 << 20) && hi < (1 << 20)) {
        double vl, vh; MEAN_AT(lo, vl); MEAN_AT(hi, vh);
        ex[0] = vl + (vh - vl) * (step - lo) / (double)(hi - lo); *nex = 1;
        double a = vl < vh ? vl : vh, b = vl < vh ? vh : vl;
        okx = mx >= a - SR_CHECK_SLACK && mx <= b + SR_CHECK_SLACK;
    } else {
        int s0 = lo > (-1 << 20) ? lo : hi; double v0; MEAN_AT(s0, v0);
        int D = step - s0, aD = D < 0 ? -D : D; double u = mx - v0;
        double pmax = SR_PXSTEP_MAX; if (sr_pred.on && fabs(sr_pred.prior) * 1.5 > pmax) pmax = fabs(sr_pred.prior) * 1.5;   /* 0.3.0: another scanner's lens may step further */
        double lo_m = SR_PXSTEP_MIN * aD - SR_CHECK_SLACK, hi_m = pmax * aD + SR_CHECK_SLACK;
        /* the local slope (px per step) when two steps are known, as the start for re-measuring */
        double typ = (sr_pred.on && sr_pred.prior != 0) ? fabs(sr_pred.prior) : SR_PXSTEP_TYP; int typ_known = 0;
        { int s1 = -1 << 20; for (int i = 0; i < n; i++) if (steps[i] != s0 && (s1 == (-1 << 20) || abs(steps[i] - s0) < abs(s1 - s0))) s1 = steps[i];
          if (s1 != (-1 << 20)) { double v1; MEAN_AT(s1, v1); double sl = fabs((v0 - v1) / (s0 - s1)); if (sl >= SR_PXSTEP_MIN && sl <= pmax) { typ = sl; typ_known = 1; } } }
        if (sign) { double sd = sign * (D > 0 ? 1 : -1); okx = sd * u >= lo_m && sd * u <= hi_m; ex[0] = v0 + sd * typ * aD; *nex = 1;
            /* review 0.1.24: with the local step known, also within SR_CHECK_SLACK + SR_CHECK_PERSTEP x |D| of its
               extrapolation - the 0.3 .. 2.2 px band alone is wider than two bar periods for long jumps (s22's 6.3 px
               bars, D = 6). 0.75 px per step: the hardware's worst real case, the short first step of the straight
               approach (s26: +1 -> +5 lands 4.33 px beyond the extrapolation, allowed 4.5; s20: +1 -> +7, 4.45 of
               6.0), while a 6.3 px alias at D = 6 stays outside (6.0) */
            if (typ_known) okx = okx && fabs(mx - ex[0]) <= SR_CHECK_SLACK + SR_CHECK_PERSTEP * aD; }
        else { okx = fabs(u) >= lo_m && fabs(u) <= hi_m; ex[0] = v0 - typ * aD; ex[1] = v0 + typ * aD; *nex = 2; }
    }
    #undef MEAN_AT
    return okx && oky;
}
int sr_accept_pass(const uint16_t *p0, const uint16_t *pk, int w, int h, int ch, const double *adx, const double *ady,
                   const int *asteps, int n, int step, int sign, double along_tol, double *dx, double *dy, double *score,
                   int *how, double *fdx, double *fdy, double *fsc) {
    double mx = 0, my = 0, ex[2], ey; int nex = 0; *how = 0;
    double sc = sr_register1(p0, pk, w, h, ch, 0, &mx, &my); if (sc == -1) return -1;
    if (fdx) *fdx = mx; if (fdy) *fdy = my; if (fsc) *fsc = sc;
    *dx = mx; *dy = my; *score = sc;
    int ok = sr_check_offset(adx, ady, asteps, n, step, sign, mx, my, along_tol, ex, &nex, &ey);
    if (ok && sc >= 0.5) return 1;
    double bsc = -9, bx = 0, by = 0; int found = 0;
    for (int i = 0; i < nex; i++) {
        double rx = ex[i], ry = ey; double r = sr_register1(p0, pk, w, h, ch, 1, &rx, &ry); if (r == -1) return -1;
        double ex2[2], ey2; int n2;
        if (r >= 0.5 && sr_check_offset(adx, ady, asteps, n, step, sign, rx, ry, along_tol, ex2, &n2, &ey2) && r > bsc) { bsc = r; bx = rx; by = ry; found = 1; }
    }
    if (!found) return 0;
    *dx = bx; *dy = by; *score = bsc; *how = 1; return 1;
}
/* least squares on columns (k, [J], 1) by the normal equations (2 or 3 unknowns, Cramer) */
static int lsq3(const double *x, const int *k, const int *J, int n, double *c) {
    int m = J ? 3 : 2; double A[3][3] = { { 0 } }, b[3] = { 0 };
    for (int i = 0; i < n; i++) { double r[3] = { (double)k[i], J ? (double)J[i] : 1.0, 1.0 };
        for (int p = 0; p < m; p++) { b[p] += r[p] * x[i]; for (int q = 0; q < m; q++) A[p][q] += r[p] * r[q]; } }
    if (m == 2) { double d = A[0][0] * A[1][1] - A[0][1] * A[1][0]; if (fabs(d) < 1e-12) return -1;
        c[0] = (b[0] * A[1][1] - A[0][1] * b[1]) / d; c[1] = (A[0][0] * b[1] - b[0] * A[1][0]) / d; c[2] = 0; return 0; }
    double d = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
    if (fabs(d) < 1e-12) return -1;
    for (int col = 0; col < 3; col++) { double M[3][3]; memcpy(M, A, sizeof M); for (int r = 0; r < 3; r++) M[r][col] = b[r];
        c[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / d; }
    return 0;
}
int sr_fit_series(const double *x, const int *k, int n, double *slope, double *rms, double *dev,
                  int *jpos, double *jump, double *slope_j, double *rms_j) {
    *jpos = 0; *jump = 0; *slope_j = 0; *rms_j = 0;
    if (n < 3 || n > 16) return -1;
    double c[3]; if (lsq3(x, k, NULL, n, c)) return -1;   /* c[0] slope, c[1] offset */
    double ss = 0; *dev = 0; for (int i = 0; i < n; i++) { double r = x[i] - (c[0] * k[i] + c[1]); ss += r * r; if (fabs(r) > *dev) *dev = fabs(r); }
    *slope = c[0]; *rms = sqrt(ss / (n - 2));
    if (n < 4) return 0;
    double best = 1e300;
    for (int j = 1; j < n; j++) {
        int J[16]; for (int i = 0; i < n; i++) J[i] = i >= j;
        double cj[3]; if (lsq3(x, k, J, n, cj)) continue;   /* cj[0] slope, cj[1] jump, cj[2] offset */
        double s2 = 0; for (int i = 0; i < n; i++) { double r = x[i] - (cj[0] * k[i] + cj[1] * J[i] + cj[2]); s2 += r * r; }
        if (s2 < best) { best = s2; *jpos = j; *jump = cj[1]; *slope_j = cj[0]; }
    }
    if (*jpos) *rms_j = sqrt(best / (n - 3));
    return 0;
}
int sr_measure(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, SrInfo *info) {
    return sr_measure2(passes, n, w, h, ch, F, dx_hint, NULL, info);
}
int sr_measure2(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, const double *dy_hint, SrInfo *info) {
    memset(info, 0, sizeof *info); if (n < 1 || n > 16 || w < 16 || h < 16 || F < 1 || F > 4) return -1;
    info->n = n;
    int g = ch == 3 ? 1 : 0;
    /* offsets of each pass against pass 1 (sub-pixel, green, on blurred copies - see blur_chan) */
    uint16_t *b0 = blur_chan(passes[0], w, h, ch, g, SR_REG_SIGMA); if (!b0) return -1;
    for (int k = 1; k < n; k++) {
        uint16_t *bk = blur_chan(passes[k], w, h, ch, g, SR_REG_SIGMA); if (!bk) { free(b0); return -1; }
        RsImg a = { b0, w, h, 1, 0, NULL }, b = { bk, w, h, 1, 0, NULL }; double dx = 0, dy = 0;
        if (dx_hint) {   /* the commanded steps x the measured image movement per step: sub-pixel step from there only
                            (a whole-pixel search can lock onto repeating detail elsewhere - found in testing) */
            dx = dx_hint[k]; dy = dy_hint ? dy_hint[k] : 0; info->score[k] = rs_register(a, b, NULL, 1, &dx, &dy);
            if (fabs(dx - dx_hint[k]) > 1.5 || fabs(dy - (dy_hint ? dy_hint[k] : 0)) > 1.5) info->score[k] = -4;
        } else info->score[k] = rs_register(a, b, NULL, 0, &dx, &dy);
        free(bk); info->dx[k] = dx; info->dy[k] = dy;
        if (info->score[k] < 0.5) { free(b0); return -2; }
    }
    free(b0);
    info->score[0] = 1;
    double ph[16]; for (int k = 0; k < n; k++) { double p = -info->dx[k]; p -= floor(p); if (p >= 1 - 1e-9) p = 0; info->phase[k] = p; ph[k] = p; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && ph[j] < ph[j - 1]; j--) { double t = ph[j]; ph[j] = ph[j - 1]; ph[j - 1] = t; }
    double gap = 1 - ph[n - 1] + ph[0]; for (int k = 1; k < n; k++) if (ph[k] - ph[k - 1] > gap) gap = ph[k] - ph[k - 1];
    info->maxgap = gap;
    info->griddist = sr_griddist(info->phase, n, F);
    for (int k = 0; k < n; k++) for (int c = 0; c < 3; c++) info->gain[k][c] = 1;
    return 0;
}

int sr_combine(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, uint16_t *out, SrInfo *info) {
    return sr_combine2(passes, n, w, h, ch, F, dx_hint, NULL, out, info);
}
int sr_combine2(const uint16_t *const *passes, int n, int w, int h, int ch, int F, const double *dx_hint, const double *dy_hint, uint16_t *out, SrInfo *info) {
    if (F < 1 || F > 4 || ch < 1 || ch > 3) { memset(info, 0, sizeof *info); return -1; }
    int r = sr_measure2(passes, n, w, h, ch, F, dx_hint, dy_hint, info); if (r) return r;
    /* 1b. brightness of each pass against pass 1, per channel: means over the area all passes cover (pass 1's interior
       minus the largest shift; pass k's area moved by its whole-pixel shift) */
    int mx = 3, my = 3; for (int k = 0; k < n; k++) { int ax = (int)ceil(fabs(info->dx[k])) + 3, ay = (int)ceil(fabs(info->dy[k])) + 3; if (ax > mx) mx = ax; if (ay > my) my = ay; }
    if (2 * mx < w && 2 * my < h)
        for (int k = 1; k < n; k++) {
            int ox = (int)floor(info->dx[k] + 0.5), oy = (int)floor(info->dy[k] + 0.5);
            for (int c = 0; c < ch; c++) { double s1 = 0, sk = 0;
                for (int y = my; y < h - my; y++) for (int x = mx; x < w - mx; x++) {
                    s1 += passes[0][((size_t)y * w + x) * ch + c]; sk += passes[k][((size_t)(y + oy) * w + x + ox) * ch + c]; }
                info->gain[k][c] = sk > 0 ? s1 / sk : 1; }
        }
    /* 2. each sample placed on the fine grid across: pass k's column i saw the picture at x = i - dx_k (pass-1
       coordinates); fine column j sits at x = j / F; each sample goes to its nearest fine column (weight below). Along the scan
       each pass is first aligned to pass 1 by its dy (Lanczos-3; rows beyond the picture repeat its first / last row). */
    int W = w * F; size_t nf = (size_t)W * h * ch;
    float *acc = calloc(nf, sizeof(float)), *wt = calloc((size_t)W * h, sizeof(float)), *row = malloc((size_t)w * ch * sizeof(float));
    if (!acc || !wt || !row) { free(acc); free(wt); free(row); return -1; }
    /* 0.1.23: with many passes (the 8-pass setting) a fine column can get samples from several passes at different
       distances; a sample 0.1 column off, put on the column, adds a position error that outweighs the noise it averages
       (simulator, 8 passes at distances 0 .. 0.12: rms 110 against 80 for 3 exact passes). Each pass's distance d to
       its fine columns is the same everywhere (it depends only on F x dx), and with F = 2 each pass feeds one class of
       columns: per class only the passes within SR_NEAR of that class's nearest pass are used. */
    /* Review (0.1.23): a pass on the OTHER side of the column from the nearest one is kept too when the nearest is
       itself more than SR_NEAR off - opposite position errors cancel (s11's offsets 0, 0.444, 1.593: pass 2 0.112 below,
       pass 3 0.186 above its column - dropping pass 3 raised the error from 46 to 165). */
    int usek[16]; { double sk[16]; int rk[16], nk[4] = { -1, -1, -1, -1 };
      for (int k = 0; k < n; k++) { double u = F * (0 - info->dx[k]); int jj = (int)floor(u + 0.5); sk[k] = u - jj; rk[k] = ((jj % F) + F) % F;
          int r = rk[k]; if (nk[r] < 0 || fabs(sk[k]) < fabs(sk[nk[r]])) nk[r] = k; }
      for (int k = 0; k < n; k++) { int r = rk[k]; double dmin = fabs(sk[nk[r]]);
          usek[k] = fabs(sk[k]) <= dmin + SR_NEAR || (dmin > SR_NEAR && sk[k] * sk[nk[r]] < 0 && fabs(sk[k]) < 0.5);
          info->used[k] = usek[k]; } }
    for (int k = 0; k < n; k++) {
        if (!usek[k]) continue;
        double dy = info->dy[k]; int iy = (int)floor(dy); double wy[6]; lanczos6(dy - iy, wy);
        for (int y = 0; y < h; y++) {
            /* along: row y of pass 1 = pass k at y + dy */
            int s0 = y + iy - 2;
            for (int x = 0; x < w; x++) for (int c = 0; c < ch; c++) {
                double a = 0; for (int t = 0; t < 6; t++) { int sy = s0 + t; sy = sy < 0 ? 0 : sy >= h ? h - 1 : sy; a += wy[t] * passes[k][((size_t)sy * w + x) * ch + c]; }
                row[(size_t)x * ch + c] = (float)(a * info->gain[k][c]); }
            for (int x = 0; x < w; x++) {
                /* to the nearest fine column only, weight (1 - 2d)^2 (d = distance in fine columns, < 0.5): a sample far
                   from a column hardly counts there (linear weights to both neighbours blurred: with 4 passes at
                   phases 0 .27 .51 .76 and F = 2 the error was 233 against 61 for the two passes at 0 and .51) */
                double u = F * (x - info->dx[k]); int jj = (int)floor(u + 0.5); double d = fabs(u - jj), ww = (1 - 2 * d) * (1 - 2 * d);
                if (jj < 0 || jj >= W || ww <= 1e-6) continue;
                size_t gi = (size_t)y * W + jj; wt[gi] += (float)ww;
                for (int c = 0; c < ch; c++) acc[gi * ch + c] += (float)(ww * row[(size_t)x * ch + c]); }
        }
    }
    /* 3. normalise; fine columns no sample reached (only with large phase gaps, or at the borders): linear between
       the nearest filled ones in the row */
    for (int y = 0; y < h; y++) {
        int last = -1;
        for (int j = 0; j < W; j++) {
            size_t gi = (size_t)y * W + j;
            if (wt[gi] > 1e-6) {
                for (int c = 0; c < ch; c++) { double v = acc[gi * ch + c] / wt[gi]; out[gi * ch + c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5); }
                if (last >= 0 && j - last > 1) for (int m = last + 1; m < j; m++) for (int c = 0; c < ch; c++) {
                    double t = (double)(m - last) / (j - last), v = out[((size_t)y * W + last) * ch + c] * (1 - t) + out[gi * ch + c] * t;
                    out[((size_t)y * W + m) * ch + c] = (uint16_t)(v + 0.5); }
                else if (last < 0) for (int m = 0; m < j; m++) for (int c = 0; c < ch; c++) out[((size_t)y * W + m) * ch + c] = out[gi * ch + c];
                last = j;
            }
        }
        if (last < 0) { for (int j = 0; j < W; j++) for (int c = 0; c < ch; c++) out[((size_t)y * W + j) * ch + c] = 0; }
        else for (int m = last + 1; m < W; m++) for (int c = 0; c < ch; c++) out[((size_t)y * W + m) * ch + c] = out[((size_t)y * W + last) * ch + c];
    }
    free(acc); free(wt); free(row); return 0;
}

/* 0.3.0: the lens-step series of Set up this scanner (rawsr.h) */
static int solve3(double A[3][3], double b[3], double x[3]) {
    for (int c = 0; c < 3; c++) {
        int p = c; for (int r = c + 1; r < 3; r++) if (fabs(A[r][c]) > fabs(A[p][c])) p = r;
        if (fabs(A[p][c]) < 1e-12) return -1;
        if (p != c) { for (int j = 0; j < 3; j++) { double t = A[c][j]; A[c][j] = A[p][j]; A[p][j] = t; } double t = b[c]; b[c] = b[p]; b[p] = t; }
        for (int r = c + 1; r < 3; r++) { double f = A[r][c] / A[c][c]; for (int j = c; j < 3; j++) A[r][j] -= f * A[c][j]; b[r] -= f * b[c]; }
    }
    for (int c = 2; c >= 0; c--) { double v = b[c]; for (int j = c + 1; j < 3; j++) v -= A[c][j] * x[j]; x[c] = v / A[c][c]; }
    return 0;
}
int sr_calib_fit(const double *dx, const double *dy, const int *k, int n, SrCalib *c) {
    memset(c, 0, sizeof *c); c->n = n; c->first = -1;
    if (n < 4 || n > 64) return -1;
    double A[3][3] = { { 0 } }, b[3] = { 0 }, x[3];
    for (int i = 0; i < n; i++) { double f[3] = { 1, (double)k[i], (double)i };
        for (int r = 0; r < 3; r++) { b[r] += f[r] * dx[i]; for (int q = 0; q < 3; q++) A[r][q] += f[r] * f[q]; } }
    if (solve3(A, b, x)) return -1;
    c->a = x[0]; c->slope = x[1]; c->drift = x[2];
    double ss = 0, res[64];
    for (int i = 0; i < n; i++) { res[i] = dx[i] - (x[0] + x[1] * k[i] + x[2] * i); ss += res[i] * res[i]; if (fabs(res[i]) > c->maxdev) c->maxdev = fabs(res[i]); }
    c->rms = n > 3 ? sqrt(ss / (n - 3)) : 0;
    /* without the drift term (for comparison) */
    { double sk = 0, sx = 0, skk = 0, skx = 0; for (int i = 0; i < n; i++) { sk += k[i]; sx += dx[i]; skk += (double)k[i] * k[i]; skx += k[i] * dx[i]; }
      double den = n * skk - sk * sk; if (fabs(den) > 1e-12) { c->slope_nd = (n * skx - sk * sx) / den; double a = (sx - c->slope_nd * sk) / n, s2 = 0;
          for (int i = 0; i < n; i++) { double e = dx[i] - a - c->slope_nd * k[i]; s2 += e * e; } c->rms_nd = n > 2 ? sqrt(s2 / (n - 2)) : 0; } }
    for (int i = 0; i + 1 < n; i++) if (k[i] == 0 && k[i + 1] == 1 && fabs(c->slope) > 1e-9) { c->first = (dx[i + 1] - dx[i] - c->drift) / c->slope; break; }
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (k[i] == k[j] && fabs(res[i] - res[j]) > c->repeat) c->repeat = fabs(res[i] - res[j]);
    if (dy) { double lo = dy[0], hi = dy[0]; for (int i = 1; i < n; i++) { if (dy[i] < lo) lo = dy[i]; if (dy[i] > hi) hi = dy[i]; } c->along = hi - lo; }
    c->ok = n >= 6 && fabs(c->slope) >= 0.3 && fabs(c->slope) <= 2.5 && c->rms <= 0.35 * fabs(c->slope);
    return 0;
}
