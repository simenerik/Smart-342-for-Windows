/* rawsharp.c - see rawsharp.h. */
#include "rawsharp.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

const double RSH_LAMBDA[3] = { 0.61, 0.54, 0.46 };
#define GRID 128

double rsh_lens_fnum(int lens) {
    static const double F[4] = { 4.9, 5.0, 5.0, 5.6 };   /* S-3 4.9/67, S-3 5.0/89, S-3 5.0/110, Rogonar-S 5.6/60 */
    return lens >= 0 && lens < 4 ? F[lens] : 0;
}

static double sinc(double x) { if (fabs(x) < 1e-12) return 1; return sin(M_PI * x) / (M_PI * x); }
static double diff_mtf(double f, double lam, double N) {
    double fc = 1.0 / (lam * N), v = fabs(f) / fc; if (v >= 1) return 0;
    return (2 / M_PI) * (acos(v) - v * sqrt(1 - v * v));
}
/* Keys cubic kernel (driver piecewise_cubic: a = -1 for steps >= 1, -0.5 below) and its Fourier transform */
static double keys(double t, double a) {
    t = fabs(t);
    if (t < 1) return (a + 2) * t * t * t - (a + 3) * t * t + 1;
    if (t < 2) return a * t * t * t - 5 * a * t * t + 8 * a * t - 4 * a;
    return 0;
}
static double keys_ft(double f, double a) {   /* f in cycles per input pixel */
    double s = 0, dt = 1.0 / 512;
    for (double t = -2 + dt / 2; t < 2; t += dt) s += keys(t, a) * cos(2 * M_PI * f * t) * dt;
    return s;
}

static double model_core(const RshModel *m, int ch, double fx, double fy) {   /* without the interpolation factor */
    double Nobj = m->fnum * (1 + m->mag) / m->mag, ap = m->cell_um / m->mag;
    /* per axis, each falling back to sigma_ccd_um when 0 (as the Python prototype's p.get('sigx', p['sig'])) */
    double sx = (m->sigma_x_ccd_um > 0 ? m->sigma_x_ccd_um : m->sigma_ccd_um) / m->mag, sy = (m->sigma_y_ccd_um > 0 ? m->sigma_y_ccd_um : m->sigma_ccd_um) / m->mag;
    double r = diff_mtf(hypot(fx, fy), RSH_LAMBDA[ch], Nobj);
    r *= sinc(fx * ap) * sinc(fy * ap) * sinc(fy * m->motion_um);
    r *= exp(-2 * M_PI * M_PI * (sx * sx * fx * fx + sy * sy * fy * fy));
    if (m->halo_x > 0 || m->halo_y > 0) {   /* 0.1.24: halo (separable) */
        double h = m->halo_sigma_ccd_um / m->mag, gx = exp(-2 * M_PI * M_PI * h * h * fx * fx), gy = exp(-2 * M_PI * M_PI * h * h * fy * fy);
        r *= ((1 - m->halo_x) + m->halo_x * gx) * ((1 - m->halo_y) + m->halo_y * gy);
    }
    return r;
}
static double interp_factor(const RshModel *m, double fx) { return m->interp_x > 0 ? keys_ft(fx * m->cell_um / m->mag, m->interp_x < 1 ? -0.5 : -1.0) : 1.0; }
double rsh_model(const RshModel *m, int ch, double fx, double fy) { return model_core(m, ch, fx, fy) * interp_factor(m, fx); }

static double resp(const double *k, double px, double py, double fx, double fy) {
    int h = RSH_TAPS / 2; double s = 0;
    for (int j = 0; j < RSH_TAPS; j++) for (int i = 0; i < RSH_TAPS; i++)
        s += k[j * RSH_TAPS + i] * cos(2 * M_PI * (fx * (i - h) * px + fy * (j - h) * py));
    return s;
}

int rsh_design(const RshModel *m, RshInfo *info) {
    memset(info, 0, sizeof *info);
    if (m->fnum <= 0 || m->mag <= 0 || m->pitch_x_um <= 0 || m->pitch_y_um <= 0 || m->K <= 0) return -1;
    double *W = malloc(sizeof(double) * GRID * GRID), *T = malloc(sizeof(double) * GRID * RSH_TAPS);
    if (!W || !T) { free(W); free(T); return -2; }
    int h = RSH_TAPS / 2;
    double win[RSH_TAPS]; for (int i = 0; i < RSH_TAPS; i++) win[i] = 0.5 - 0.5 * cos(2 * M_PI * (i + 1) / (RSH_TAPS + 1));
    for (int c = 0; c < 3; c++) {
        /* W on the DFT grid: frequency index u -> (u < GRID/2 ? u : u - GRID) / (GRID x pitch) */
        double w00 = 0, ik[GRID];   /* the interpolation factor depends on fx only: once per column (review: speed) */
        for (int u = 0; u < GRID; u++) ik[u] = interp_factor(m, (u < GRID / 2 ? u : u - GRID) / (GRID * m->pitch_x_um));
        for (int v = 0; v < GRID; v++) for (int u = 0; u < GRID; u++) {
            double fx = (u < GRID / 2 ? u : u - GRID) / (GRID * m->pitch_x_um), fy = (v < GRID / 2 ? v : v - GRID) / (GRID * m->pitch_y_um);
            double M = model_core(m, c, fx, fy) * ik[u], w = M / (M * M + m->K);
            W[v * GRID + u] = w; if (!u && !v) w00 = w;
        }
        for (int i = 0; i < GRID * GRID; i++) W[i] /= w00;
        /* inverse DFT at the centre taps (W is real and even in fx and fy): separable cosine sums */
        for (int v = 0; v < GRID; v++) for (int i = 0; i < RSH_TAPS; i++) {
            double s = 0; for (int u = 0; u < GRID; u++) s += W[v * GRID + u] * cos(2 * M_PI * u * (i - h) / GRID);
            T[v * RSH_TAPS + i] = s; }
        double sum = 0;
        for (int j = 0; j < RSH_TAPS; j++) for (int i = 0; i < RSH_TAPS; i++) {
            double s = 0; for (int v = 0; v < GRID; v++) s += T[v * RSH_TAPS + i] * cos(2 * M_PI * v * (j - h) / GRID);
            s = s / ((double)GRID * GRID) * win[i] * win[j];
            info->kern[c][j * RSH_TAPS + i] = s; sum += s; }
        double ng = 0;
        for (int i = 0; i < RSH_TAPS * RSH_TAPS; i++) { info->kern[c][i] /= sum; ng += info->kern[c][i] * info->kern[c][i]; }
        info->centre[c] = info->kern[c][h * RSH_TAPS + h]; info->noise_gain[c] = sqrt(ng);
        if (c == 1) for (int q = 0; q < 4; q++) {
            double f = (0.4 + 0.2 * q) * m->mag / (2 * m->cell_um);   /* cycles per um */
            info->f_rep[q] = f * 1000;
            info->rep_model_y[q] = rsh_model(m, c, 0, f); info->rep_model_x[q] = rsh_model(m, c, f, 0);
            info->rep_resp_y[q] = info->rep_model_y[q] * resp(info->kern[c], m->pitch_x_um, m->pitch_y_um, 0, f);
            info->rep_resp_x[q] = info->rep_model_x[q] * resp(info->kern[c], m->pitch_x_um, m->pitch_y_um, f, 0);
        }
        for (int q = 0; q < 8; q++) {
            double f = (q + 1) * 10 / 1000.0;   /* 10..80 lp/mm */
            info->model_y[c][q] = rsh_model(m, c, 0, f); info->model_x[c][q] = rsh_model(m, c, f, 0);
            info->resp_y[c][q] = info->model_y[c][q] * resp(info->kern[c], m->pitch_x_um, m->pitch_y_um, 0, f);
            info->resp_x[c][q] = info->model_x[c][q] * resp(info->kern[c], m->pitch_x_um, m->pitch_y_um, f, 0);
        }
    }
    free(W); free(T);
    return 0;
}

int rsh_apply(const RshInfo *info, const uint16_t *img, int w, int h, int ch, uint16_t *out) {
    if (!img || !out || w < 1 || h < 1 || (ch != 1 && ch != 3)) return -1;
    int r = RSH_TAPS / 2;
    int *xi = malloc(sizeof(int) * (w + 2 * r));
    if (!xi) return -2;
    for (int i = -r; i < w + r; i++) xi[i + r] = i < 0 ? 0 : i >= w ? w - 1 : i;
    for (int c = 0; c < ch; c++) {
        const double *k = info->kern[ch == 1 ? 1 : c];
        for (int y = 0; y < h; y++) {
            const uint16_t *rows[RSH_TAPS];
            for (int j = 0; j < RSH_TAPS; j++) { int yy = y + j - r; yy = yy < 0 ? 0 : yy >= h ? h - 1 : yy; rows[j] = img + (size_t)yy * w * ch; }
            for (int x = 0; x < w; x++) {
                double s = 0;
                for (int j = 0; j < RSH_TAPS; j++) {
                    const uint16_t *rw = rows[j]; const double *kj = k + j * RSH_TAPS; const int *xx = xi + x;
                    for (int i = 0; i < RSH_TAPS; i++) s += kj[i] * rw[(size_t)xx[i] * ch + c];
                }
                out[((size_t)y * w + x) * ch + c] = (uint16_t)(s < 0 ? 0 : s > 65535 ? 65535 : s + 0.5);
            }
        }
    }
    free(xi);
    return 0;
}
