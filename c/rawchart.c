/* rawchart.c - the scanner's sharpness from the Scitex focus chart (see rawchart.h). */
#include "rawchart.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static double px_at(const uint16_t *img, int w, int ch, int x, int y) { return img[((size_t)y * w + x) * ch + (ch == 3 ? 1 : 0)]; }

/* the strongest periodicity of a profile (cycles per sample in [1/40, 1/3]) and the share of its variance there */
static double period_score(const double *p, int n, double *freq) {
    double mean = 0; for (int i = 0; i < n; i++) mean += p[i]; mean /= n;
    double var = 0; for (int i = 0; i < n; i++) var += (p[i] - mean) * (p[i] - mean);
    if (var <= 0) { *freq = 0; return 0; }
    double best = 0, bf = 0;
    for (double f = 1.0 / 40; f <= 1.0 / 3; f += 0.0025) {
        double re = 0, im = 0;
        for (int i = 0; i < n; i++) { double h = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1)), v = (p[i] - mean) * h; re += v * cos(2 * M_PI * f * i); im -= v * sin(2 * M_PI * f * i); }
        double pw = re * re + im * im; if (pw > best) { best = pw; bf = f; }
    }
    /* a pure sine with a Hann window: |X|^2 = (A n / 4)^2, its variance n A^2 / 2 -> ratio n / 8 */
    *freq = bf; return best / (var * n / 8.0);
}

#define T 64
#define TS 32
int chart_find(const uint16_t *img, int w, int h, int ch, ChBox *bx, ChBox *by) {
    memset(bx, 0, sizeof *bx); memset(by, 0, sizeof *by);
    if (w < T || h < T) return 0;
    int nx = (w - T) / TS + 1, ny = (h - T) / TS + 1;
    signed char *cls = calloc((size_t)nx * ny, 1); double *sc = calloc((size_t)nx * ny, sizeof(double));
    if (!cls || !sc) { free(cls); free(sc); return 0; }
    double prx[T], pry[T];
    for (int ty = 0; ty < ny; ty++) for (int tx = 0; tx < nx; tx++) {
        int x0 = tx * TS, y0 = ty * TS; double mean = 0, var = 0;
        for (int i = 0; i < T; i++) prx[i] = pry[i] = 0;
        for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) { double v = px_at(img, w, ch, x0 + x, y0 + y); prx[x] += v; pry[y] += v; mean += v; }
        mean /= T * T; for (int i = 0; i < T; i++) { prx[i] /= T; pry[i] /= T; }
        for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) { double d = px_at(img, w, ch, x0 + x, y0 + y) - mean; var += d * d; }
        var /= T * T;
        double vx = 0, vy = 0; for (int i = 0; i < T; i++) { vx += (prx[i] - mean) * (prx[i] - mean); vy += (pry[i] - mean) * (pry[i] - mean); } vx /= T; vy /= T;
        if (var <= 0 || sqrt(var) < 0.05 * mean) continue;          /* too little contrast for bars */
        double fx, fy, rx = period_score(prx, T, &fx), ry = period_score(pry, T, &fy);
        /* bars varying across: the column means carry most of the variance and are periodic; and the other way */
        if (rx > 0.5 && vx > 0.6 * var && vy < 0.1 * var) { cls[ty * nx + tx] = 1; sc[ty * nx + tx] = rx; }
        else if (ry > 0.5 && vy > 0.6 * var && vx < 0.1 * var) { cls[ty * nx + tx] = 2; sc[ty * nx + tx] = ry; }
    }
    int found = 0;
    for (int c = 1; c <= 2; c++) {
        /* the largest 4-connected group of tiles of this class */
        int *lab = calloc((size_t)nx * ny, sizeof(int)), *q = malloc((size_t)nx * ny * sizeof(int)); int bestn = 0, bestl = 0, nl = 0;
        if (!lab || !q) { free(lab); free(q); break; }
        for (int i = 0; i < nx * ny; i++) if (cls[i] == c && !lab[i]) {
            nl++; int qh = 0, qt = 0, cnt = 0; q[qt++] = i; lab[i] = nl;
            while (qh < qt) { int k = q[qh++], kx = k % nx, ky = k / nx; cnt++;
                int nb[4][2] = { { kx - 1, ky }, { kx + 1, ky }, { kx, ky - 1 }, { kx, ky + 1 } };
                for (int j = 0; j < 4; j++) { int ax = nb[j][0], ay = nb[j][1]; if (ax < 0 || ay < 0 || ax >= nx || ay >= ny) continue;
                    int a = ay * nx + ax; if (cls[a] == c && !lab[a]) { lab[a] = nl; q[qt++] = a; } } }
            if (cnt > bestn) { bestn = cnt; bestl = nl; }
        }
        ChBox *b = c == 1 ? bx : by;
        if (bestn >= 4) {
            int X0 = w, Y0 = h, X1 = 0, Y1 = 0; double s = 0;
            for (int i = 0; i < nx * ny; i++) if (lab[i] == bestl) { int x0 = (i % nx) * TS, y0 = (i / nx) * TS;
                X0 = x0 < X0 ? x0 : X0; Y0 = y0 < Y0 ? y0 : Y0; X1 = x0 + T > X1 ? x0 + T : X1; Y1 = y0 + T > Y1 ? y0 + T : Y1; s += sc[i]; }
            /* the tiles reach a little past the block's edge: keep away from it */
            X0 += T / 4; Y0 += T / 4; X1 -= T / 4; Y1 -= T / 4;
            if (X1 - X0 >= 32 && Y1 - Y0 >= 32) { b->x0 = X0; b->y0 = Y0; b->x1 = X1; b->y1 = Y1; b->ok = 1; b->score = s / bestn; found++; }
        }
        free(lab); free(q);
    }
    free(cls); free(sc);
    return found;
}

int chart_bars(const uint16_t *img, int w, int h, int ch, const ChBox *b, int axis, double px_mm, ChBars *o) {
    memset(o, 0, sizeof *o);
    if (!b->ok || px_mm <= 0) return -1;
    int L = axis ? b->x1 - b->x0 : b->y1 - b->y0, N = axis ? b->y1 - b->y0 : b->x1 - b->x0;   /* lines, samples per line */
    if (L < 8 || N < 32) return -1;
    double *blk = malloc((size_t)L * N * sizeof(double)); if (!blk) return -1;
    for (int r = 0; r < L; r++) for (int i = 0; i < N; i++)
        blk[(size_t)r * N + i] = axis ? px_at(img, w, ch, b->x0 + r, b->y0 + i) : px_at(img, w, ch, b->x0 + i, b->y0 + r);
    double *han = malloc(N * sizeof(double)), *m = calloc(N, sizeof(double));
    for (int i = 0; i < N; i++) han[i] = 0.5 - 0.5 * cos(2 * M_PI * i / (N - 1));
    for (int r = 0; r < L; r++) for (int i = 0; i < N; i++) m[i] += blk[(size_t)r * N + i] / L;
    double mm = 0; for (int i = 0; i < N; i++) mm += m[i]; mm /= N;
    /* the fundamental: the largest peak of the 16 x zero-padded spectrum above 3 cycles per block */
    double best = -1, f0 = 0; int K = 16 * N;
    for (int k = 48; k <= K / 2; k++) { double f = (double)k / K, re = 0, im = 0;
        for (int i = 0; i < N; i++) { double v = (m[i] - mm) * han[i]; re += v * cos(2 * M_PI * f * i); im -= v * sin(2 * M_PI * f * i); }
        double a = re * re + im * im; if (a > best) { best = a; f0 = f; } }
    if (f0 <= 0) { free(blk); free(han); free(m); return -1; }
    /* each line's phase of the fundamental, unwrapped, and a straight line through them (the bars' tilt) */
    double *ph = malloc(L * sizeof(double));
    for (int r = 0; r < L; r++) { const double *v = blk + (size_t)r * N; double mv = 0; for (int i = 0; i < N; i++) mv += v[i]; mv /= N;
        double re = 0, im = 0; for (int i = 0; i < N; i++) { double t = (v[i] - mv) * han[i]; re += t * cos(2 * M_PI * f0 * i); im -= t * sin(2 * M_PI * f0 * i); }
        ph[r] = atan2(im, re);
        if (r) { while (ph[r] - ph[r - 1] > M_PI) ph[r] -= 2 * M_PI; while (ph[r] - ph[r - 1] < -M_PI) ph[r] += 2 * M_PI; } }
    double sr = 0, sp = 0, srr = 0, srp = 0; for (int r = 0; r < L; r++) { sr += r; sp += ph[r]; srr += (double)r * r; srp += r * ph[r]; }
    double sl = (L * srp - sr * sp) / (L * srr - sr * sr), ic = (sp - sl * sr) / L;
    /* every sample's place within the period, folded onto 128 bins */
    enum { NB = 128 }; double sum[NB] = { 0 }, cnt[NB] = { 0 }, prof[NB];
    for (int r = 0; r < L; r++) for (int i = 0; i < N; i++) {
        double u = f0 * i + (sl * r + ic) / (2 * M_PI); u -= floor(u); int k = (int)(u * NB); if (k >= NB) k = NB - 1;
        sum[k] += blk[(size_t)r * N + i]; cnt[k] += 1; }
    int empty = 0; for (int k = 0; k < NB; k++) { prof[k] = cnt[k] > 0 ? sum[k] / cnt[k] : 0; if (!cnt[k]) empty++; }
    double amp[CH_MAXH + 2];
    for (int n = 1; n <= CH_MAXH + 1 && n < NB / 2; n++) { double re = 0, im = 0;
        for (int k = 0; k < NB; k++) { re += prof[k] * cos(2 * M_PI * n * k / NB); im -= prof[k] * sin(2 * M_PI * n * k / NB); }
        amp[n] = 2 * sqrt(re * re + im * im) / NB; }
    double hi = prof[0], lo = prof[0]; for (int k = 1; k < NB; k++) { if (prof[k] > hi) hi = prof[k]; if (prof[k] < lo) lo = prof[k]; }
    double mid = (hi + lo) / 2, up = -1, dn = -1; int nup = 0, ndn = 0;
    for (int i = 0; i < NB; i++) { double a0 = prof[i] - mid, a1 = prof[(i + 1) % NB] - mid;
        if ((a0 < 0) != (a1 < 0)) { double c = (i + a0 / (a0 - a1)) / NB; if (a1 > a0) { up = c; nup++; } else { dn = c; ndn++; } } }
    double duty;
    if (nup == 1 && ndn == 1) { duty = dn - up; duty -= floor(duty); }
    else { int a = 0; for (int k = 0; k < NB; k++) a += prof[k] > mid; duty = (double)a / NB; }
    o->f0 = f0 / px_mm; o->period_px = 1 / f0; o->tilt = sl / (2 * M_PI) / f0; o->duty = duty; o->michelson = (hi - lo) / (hi + lo);
    double id1 = fabs(sin(M_PI * duty));
    for (int n = 2; n <= 11 && o->nh < CH_MAXH; n++) {   /* as barmtf.py */
        double idn = fabs(sin(M_PI * n * duty)) / n; if (idn < 0.08 * id1) continue;
        o->f[o->nh] = n * o->f0; o->v[o->nh] = (amp[n] / amp[1]) / (idn / id1); o->nh++; }
    o->ok = empty == 0 && amp[1] > 0;
    free(blk); free(han); free(m); free(ph);
    return o->ok ? 0 : -1;
}

/* the model of fit_chart.py (per-axis halo): film-side frequency fu (cycles/um) */
static double sincf(double x) { return fabs(x) < 1e-12 ? 1 : sin(M_PI * x) / (M_PI * x); }
typedef struct { double m, N, lam, ap, mot; } ChOpt;
static double model(const ChOpt *o, const double *p, double f, int axis) {
    double fu = f / 1000.0, v = fu * o->lam * o->N, d = v >= 1 ? 0 : (2 / M_PI) * (acos(v) - v * sqrt(1 - v * v));
    double s = (axis ? p[1] : p[0]) / o->m, a = axis ? p[3] : p[2], hs = p[4] / o->m;
    double r = d * sincf(fu * o->ap) * exp(-2 * M_PI * M_PI * s * s * fu * fu) * ((1 - a) + a * exp(-2 * M_PI * M_PI * hs * hs * fu * fu));
    if (axis) r *= sincf(fu * o->mot);
    return r;
}
static int resid(const ChOpt *o, const double *p, const ChBars *bx, const ChBars *by, double *r) {
    int k = 0;
    for (int ax = 0; ax < 2; ax++) { const ChBars *b = ax ? by : bx; if (!b || !b->ok) continue;
        double m0 = model(o, p, b->f0, ax); if (m0 <= 0) m0 = 1e-9;
        for (int i = 0; i < b->nh; i++) if (b->v[i] >= 0.02) r[k++] = model(o, p, b->f[i], ax) / m0 - b->v[i]; }
    return k;
}
int chart_fit(const ChBars *bx, const ChBars *by, double mag, double fnum, double motion_um, double lam_um, ChFit *fit) {
    memset(fit, 0, sizeof *fit);
    if (!bx || !by || !bx->ok || !by->ok || mag <= 0 || fnum <= 0) return -1;
    ChOpt o = { mag, fnum * (1 + mag) / mag, lam_um, 7.0 / mag, motion_um };
    const double lo[5] = { 0, 0, 0, 0, 2 }, hi[5] = { 10, 10, 0.9, 0.9, 60 };
    double p[5] = { 1.7, 2.4, 0.5, 0.5, 7 }, r[2 * CH_MAXH], r2[2 * CH_MAXH], J[2 * CH_MAXH][5];
    int n = resid(&o, p, bx, by, r); if (n < 8) return -1;
    double lam = 1e-3, cost = 0; for (int i = 0; i < n; i++) cost += r[i] * r[i];
    for (int it = 0; it < 200; it++) {
        for (int j = 0; j < 5; j++) { double q[5]; memcpy(q, p, sizeof q); double hstep = 1e-5 * (fabs(p[j]) + 1e-3); q[j] += hstep; resid(&o, q, bx, by, r2);
            for (int i = 0; i < n; i++) J[i][j] = (r2[i] - r[i]) / hstep; }
        double A[5][5] = { { 0 } }, g[5] = { 0 };
        for (int i = 0; i < n; i++) for (int a = 0; a < 5; a++) { g[a] += J[i][a] * r[i]; for (int c = 0; c < 5; c++) A[a][c] += J[i][a] * J[i][c]; }
        int improved = 0;
        for (int tries = 0; tries < 12 && !improved; tries++) {
            double M[5][6]; for (int a = 0; a < 5; a++) { for (int c = 0; c < 5; c++) M[a][c] = A[a][c] + (a == c ? lam * (A[a][a] + 1e-9) : 0); M[a][5] = -g[a]; }
            int bad = 0;
            for (int c = 0; c < 5 && !bad; c++) { int pv = c; for (int a = c + 1; a < 5; a++) if (fabs(M[a][c]) > fabs(M[pv][c])) pv = a;
                if (fabs(M[pv][c]) < 1e-15) { bad = 1; break; }
                if (pv != c) for (int k = 0; k < 6; k++) { double t = M[c][k]; M[c][k] = M[pv][k]; M[pv][k] = t; }
                for (int a = c + 1; a < 5; a++) { double f = M[a][c] / M[c][c]; for (int k = c; k < 6; k++) M[a][k] -= f * M[c][k]; } }
            if (bad) { lam *= 10; continue; }
            double dp[5]; for (int c = 4; c >= 0; c--) { double v = M[c][5]; for (int k = c + 1; k < 5; k++) v -= M[c][k] * dp[k]; dp[c] = v / M[c][c]; }
            double q[5]; for (int j = 0; j < 5; j++) { q[j] = p[j] + dp[j]; if (q[j] < lo[j]) q[j] = lo[j]; if (q[j] > hi[j]) q[j] = hi[j]; }
            resid(&o, q, bx, by, r2); double c2 = 0; for (int i = 0; i < n; i++) c2 += r2[i] * r2[i];
            if (c2 < cost) { memcpy(p, q, sizeof p); memcpy(r, r2, sizeof(double) * n); improved = (cost - c2) > 1e-14 ? 1 : 2; cost = c2; lam = lam > 1e-9 ? lam / 3 : lam; }
            else lam *= 10;
        }
        if (!improved || improved == 2) break;
    }
    fit->sx = p[0]; fit->sy = p[1]; fit->ax = p[2]; fit->ay = p[3]; fit->sh = p[4]; fit->npts = n;
    fit->rms = sqrt(cost / n); for (int i = 0; i < n; i++) if (fabs(r[i]) > fit->maxres) fit->maxres = fabs(r[i]);
    return 0;
}
