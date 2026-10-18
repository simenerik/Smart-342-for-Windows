/* rawtile_test.c - checks of rawtile.c (developer use; run by regress.sh): tiling plans, and stitching of tiles cut
 * from a known picture with known sub-pixel offsets, gains and noise.
 * Criteria: no uncovered samples (with a margin of 8 scanned around the picture); every tile but the first
 * registered; positions within 0.1 px of the truth - each tile is matched to what is already placed, so small
 * residuals add up along a chain of tiles (seen: 0.02 px per step, 0.056 px after 3 steps); 0.1 px is the
 * scanner's own carriage repeatability (measured, session s8); and no seam: the error in the overlap bands is not
 * larger than elsewhere (blending two noisy tiles makes it smaller). */
#include "rawtile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static double hsh(int64_t x, int64_t y, int c) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ULL ^ (uint64_t)y * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(c + 1) * 0x165667B19E3779F9ULL;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32; return (double)(h >> 11) * (2.0 / 9007199254740992.0) - 1;
}
static double vn(double x, double y, double cell, int c) {
    x /= cell; y /= cell; double fx = floor(x), fy = floor(y), tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty); int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double a = hsh(ix, iy, c), b = hsh(ix + 1, iy, c), d = hsh(ix, iy + 1, c), e = hsh(ix + 1, iy + 1, c);
    return (a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty;
}
static double amp = 1;
static double f(double x, double y, int c) { return 4 * (2000 + amp * (500 * vn(x, y, 37, c) + 250 * vn(x, y, 9, c + 7) + 120 * vn(x, y, 4, c + 13))); }
static uint64_t rs = 88172645463325252ULL;
static double urand(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (rs >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) { double u = urand() + 1e-12, v = urand(); return sqrt(-2 * log(u)) * cos(6.283185307 * v); }

static void plan_checks(void) {
    int sizes[][4] = { { 2618, 3783, 512, 698 }, { 1890, 2835, 512, 698 }, { 500, 600, 512, 698 }, { 5787, 4488, 1024, 2048 }, { 1025, 699, 512, 698 }, { 9400, 11800, 2618, 6000 } };
    for (int s = 0; s < 6; s++) {
        int W = sizes[s][0], H = sizes[s][1], mw = sizes[s][2], mh = sizes[s][3], ovl = 64;
        static int r[4096][4]; int n = rt_plan(W, H, mw, mh, ovl, r, 4096);
        int ok = n > 0 && n <= 4096; uint8_t *cov = calloc((size_t)W * H, 1); int minov = 1 << 30;
        for (int i = 0; ok && i < n; i++) {
            if (r[i][2] > mw || r[i][3] > mh || r[i][0] < 0 || r[i][1] < 0 || r[i][0] + r[i][2] > W || r[i][1] + r[i][3] > H) ok = 0;
            for (int y = r[i][1]; ok && y < r[i][1] + r[i][3]; y++) memset(cov + (size_t)y * W + r[i][0], 1, r[i][2]);
            if (i > 0 && r[i][1] == r[i - 1][1]) { int o = r[i - 1][0] + r[i - 1][2] - r[i][0]; if (o < minov) minov = o; }
        }
        for (size_t g = 0; ok && g < (size_t)W * H; g++) if (!cov[g]) ok = 0;
        free(cov);
        CHECK(ok && (minov == (1 << 30) || minov >= ovl), "plan %d x %d in tiles of max %d x %d: %d tiles, all covered, overlap across %s", W, H, mw, mh, n, minov == (1 << 30) ? "-" : "ok");
    }
}

static void stitch_check(int W, int H, int mw, int mh, double maxsh, double gspread, double sigma, int M, const char *what) {
    int ovl = 64, r[256][4]; int n = rt_plan(W + 2 * M, H + 2 * M, mw, mh, ovl, r, 256);
    for (int i = 0; i < n; i++) { r[i][0] -= M; r[i][1] -= M; }   /* the plan covers the picture plus a margin of M */
    RtMosaic m; if (rtm_init(&m, W, H, 3)) { CHECK(0, "memory"); return; }
    double maxerr = 0; int nreg = 0;
    for (int i = 0; i < n; i++) {
        int x0 = r[i][0], y0 = r[i][1], tw = r[i][2], th = r[i][3];
        double sx = i ? (urand() * 2 - 1) * maxsh : 0, sy = i ? (urand() * 2 - 1) * maxsh : 0;
        double g[3]; for (int c = 0; c < 3; c++) g[c] = i ? 1 + (urand() * 2 - 1) * gspread : 1;
        uint16_t *t = malloc((size_t)tw * th * 6);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < 3; c++) {
            double v = g[c] * f(x0 + x - sx, y0 + y - sy, c) + sigma * gauss();
            t[((size_t)y * tw + x) * 3 + c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v + 0.5); }
        RtTileInfo inf; rtm_add(&m, t, tw, th, x0, y0, 32, 4, &inf); free(t);
        if (inf.registered) { nreg++; double e = fmax(fabs(inf.dx - sx), fabs(inf.dy - sy)); if (e > maxerr) maxerr = e; if (getenv("RT_VERBOSE")) printf("   tile %d at %d,%d: true %+.3f %+.3f found %+.3f %+.3f (overlap %d px, score %.3f)\n", i, x0, y0, sx, sy, inf.dx, inf.dy, inf.novl, inf.score); }
    }
    uint16_t *out = malloc((size_t)W * H * 6); long long miss = rtm_result(&m, out);
    /* error against the clean picture: overall, and in the overlap bands vs elsewhere (a seam would show there) */
    uint8_t *band = calloc((size_t)W * H, 1);
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++) if (i != j) {
        int ax0 = r[i][0] > r[j][0] ? r[i][0] : r[j][0], ay0 = r[i][1] > r[j][1] ? r[i][1] : r[j][1];
        int ax1 = r[i][0] + r[i][2] < r[j][0] + r[j][2] ? r[i][0] + r[i][2] : r[j][0] + r[j][2], ay1 = r[i][1] + r[i][3] < r[j][1] + r[j][3] ? r[i][1] + r[i][3] : r[j][1] + r[j][3];
        for (int y = ay0 < 0 ? 0 : ay0; y < ay1 && y < H; y++) for (int x = ax0 < 0 ? 0 : ax0; x < ax1 && x < W; x++) band[(size_t)y * W + x] = 1; }
    double e[2] = { 0 }; long ne[2] = { 0 };
    int bm = M >= 8 ? 0 : 8;   /* without a margin the filled border samples are approximate: not judged */
    for (int y = bm; y < H - bm; y++) for (int x = bm; x < W - bm; x++) for (int c = 0; c < 3; c++) {
        double d = out[((size_t)y * W + x) * 3 + c] - f(x, y, c); int b = band[(size_t)y * W + x]; e[b] += d * d; ne[b]++; }
    double rin = ne[1] ? sqrt(e[1] / ne[1]) : 0, rout = sqrt(e[0] / ne[0]);
    CHECK((M >= 8 ? miss == 0 : 1) && nreg == n - 1 && maxerr < 0.1 && rout < 1.15 * sigma + 3 && (!ne[1] || rin < 1.10 * rout + 3),
          "%s: %d x %d from %d tiles (margin %d), shifts found within %.3f px, rms error %.1f outside / %.1f in the overlaps (noise %.0f), border samples filled %lld",
          what, W, H, n, M, maxerr, rout, rin, sigma, miss);
    free(band); free(out); rtm_free(&m);
}

int main(void) {
    plan_checks();
    stitch_check(1400, 1000, 512, 698, 1.5, 0.0, 20, 8, "sub-pixel shifts +-1.5 px");
    stitch_check(1400, 1000, 512, 698, 1.5, 0.03, 20, 8, "shifts and gains +-3 %");
    stitch_check(1200, 900, 400, 400, 3.5, 0.02, 40, 8, "shifts +-3.5 px, noise 40");
    stitch_check(1000, 700, 512, 698, 0.0, 0.0, 0, 8, "exact (no shift, no noise)");
    stitch_check(1400, 1000, 512, 698, 1.5, 0.03, 20, 0, "no margin (border filled)");
    /* a plain area (no detail, only noise; e.g. film base): no shift can be measured - the tiles must stay at their
       nominal places (true offsets 0 here) instead of following the noise, and show no seam */
    { amp = 0; int W = 1400, H = 1000, r[64][4], n = rt_plan(W + 16, H + 16, 512, 698, 64, r, 64); RtMosaic m; rtm_init(&m, W, H, 3); double mx = 0;
      for (int i = 0; i < n; i++) { r[i][0] -= 8; r[i][1] -= 8; int tw = r[i][2], th = r[i][3]; uint16_t *t = malloc((size_t)tw * th * 6);
        for (size_t k = 0; k < (size_t)tw * th * 3; k++) t[k] = (uint16_t)(8000 + 20 * gauss() + 0.5);
        RtTileInfo inf; rtm_add(&m, t, tw, th, r[i][0], r[i][1], 32, 4, &inf); free(t); double e = fmax(fabs(inf.dx), fabs(inf.dy)); if (e > mx) mx = e; }
      uint16_t *out = malloc((size_t)W * H * 6); rtm_result(&m, out); double sd = 0; for (size_t k = 0; k < (size_t)W * H * 3; k++) sd += (out[k] - 8000.0) * (out[k] - 8000.0);
      sd = sqrt(sd / ((double)W * H * 3)); CHECK(mx < 0.5 && sd < 25, "plain area (noise only): tiles kept within %.2f px of their places, rms %.1f", mx, sd); free(out); rtm_free(&m); amp = 1; }
    /* review cases (2026-10-08): weak detail with L-shaped overlaps (the masked sub-pixel step), black areas
       (Lanczos undershoot must not become holes), and a shift beyond the search range (must be flagged) */
    amp = 0.1; stitch_check(1400, 1000, 512, 698, 1.5, 0.0, 20, 8, "weak detail (texture x 0.1), L-shaped overlaps"); amp = 1;
    { int W = 1000, H = 700, r[64][4], n = rt_plan(W + 16, H + 16, 512, 698, 64, r, 64); RtMosaic m; rtm_init(&m, W, H, 3); long long holes;
      for (int i = 0; i < n; i++) { r[i][0] -= 8; r[i][1] -= 8; int tw = r[i][2], th = r[i][3]; uint16_t *t = malloc((size_t)tw * th * 6);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < 3; c++) {
            int gx = r[i][0] + x; double v = gx < 500 ? 6 + 6 * gauss() : 40000 + 20 * gauss();      /* black next to bright */
            t[((size_t)y * tw + x) * 3 + c] = (uint16_t)(v < 0 ? 0 : v + 0.5); }
        RtTileInfo inf; rtm_add(&m, t, tw, th, r[i][0], r[i][1], 32, 4, &inf); free(t); }
      uint16_t *out = malloc((size_t)W * H * 6); holes = rtm_result(&m, out); free(out); rtm_free(&m);
      CHECK(holes == 0, "black next to bright: no holes (uncovered samples %lld)", holes); }
    { int W = 1000, H = 600, r[64][4], n = rt_plan(W + 16, H + 16, 512, 698, 64, r, 64); RtMosaic m; rtm_init(&m, W, H, 3); int flagged = 0;
      for (int i = 0; i < n; i++) { r[i][0] -= 8; r[i][1] -= 8; int tw = r[i][2], th = r[i][3]; double sx = i ? 6.5 : 0; uint16_t *t = malloc((size_t)tw * th * 6);
        for (int y = 0; y < th; y++) for (int x = 0; x < tw; x++) for (int c = 0; c < 3; c++) { double v = f(r[i][0] + x - sx, r[i][1] + y, c); t[((size_t)y * tw + x) * 3 + c] = (uint16_t)(v + 0.5); }
        RtTileInfo inf; rtm_add(&m, t, tw, th, r[i][0], r[i][1], 32, 4, &inf); free(t); if (i && inf.at_edge) flagged = 1; }
      rtm_free(&m); CHECK(flagged, "a shift of 6.5 px with a search range of 4: flagged as at the edge of the search"); }
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails ? 1 : 0;
}
