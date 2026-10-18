/* rawhdr.c - merging a normal RAW pass with a longer-exposed one (see rawhdr.h).
 *
 * The longer pass has the same white calibration as a normal one (the driver calibrates at the normal integration
 * time; only the picture data is taken with the longer time), so it is about g = t_long / t_normal times brighter
 * and clips earlier. Nothing about g is assumed: per channel, brighter = g * normal + o is fitted by least squares over
 * 8 x 8 block means where both passes are well inside their range (two rounds, the second without residuals above
 * 4 x rms);
 * o takes the dark level difference (dark current grows with the time). Then per pixel, with m = the largest channel
 * of the brighter pass:   m <= 65 % of the clip level: the brighter pass, (b - o) / g (less noise in the shadows);
 * m >= 85 %: the normal pass;   between: a linear blend (no visible seam). In addition the brighter pass is only used
 * where the normal pass is dark (largest channel within 4 pixels below 3 % of white, blended out by 8 %): the aligned pass is
 * interpolated, which softens fine detail by about 1 % - less than the noise gain in the shadows, more above. Where the brighter pass has no data (edges
 * lost to the alignment) the normal pass is used. */
#include "rawhdr.h"
#include "rawstack.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CLIP 65532.0          /* 16383 (14 bits) x 4: the highest value a RAW pass can hold */

/* largest channel of each pixel, then the largest value within r pixels (square); no-data (-1) counts as CLIP */
static float *dilated_max(const uint16_t *n16, const float *bf, int w, int h, int ch, int r) {
    size_t np = (size_t)w * h; float *a = malloc(np * sizeof(float)), *t = malloc(np * sizeof(float));
    if (!a || !t) { free(a); free(t); return NULL; }
    for (size_t i = 0; i < np; i++) { double m = 0; for (int c = 0; c < ch; c++) { double v = n16 ? n16[i * ch + c] : bf[i * ch + c] < 0 ? CLIP : bf[i * ch + c]; if (v > m) m = v; } a[i] = (float)m; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float m = 0; for (int d = -r; d <= r; d++) { int xx = x + d; if (xx >= 0 && xx < w && a[(size_t)y * w + xx] > m) m = a[(size_t)y * w + xx]; }
        t[(size_t)y * w + x] = m; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float m = 0; for (int d = -r; d <= r; d++) { int yy = y + d; if (yy >= 0 && yy < h && t[(size_t)yy * w + x] > m) m = t[(size_t)yy * w + x]; }
        a[(size_t)y * w + x] = m; }
    free(t); return a;
}
/* ---- 0.4.1: spill-over check ----
   Found on the hardware (13 Oct, HDR +2): in the 4 x pass the CCD's green row spills charge from overexposed areas
   (bright sky through leaves) into its neighbours along the CCD line - usually 2 px, up to ~65 px - while red and blue
   do not (1197 green pixels at full scale where 4 x the normal pass is far below it). Where the alignment interpolates
   across such a clipped edge the values come out in range (about 35000), so the clip rule did not catch them and the
   merge wrote up to 16 x too much green (4 small spots, 34 pixels). Rule: the brighter pass, scaled back, must agree
   with the normal pass within what the noise allows. On 3 x 3 means of both, per channel, the spread of the difference
   is measured in the picture itself (robust: 1.4826 x the median absolute deviation, in level bins of the normal
   pass, over the pixels where the brighter pass would be used); a pixel whose brighter pass is more than 8 x that
   spread + 10 % above the normal pass (in any channel) takes the normal pass, with 2 pixels around it and a soft
   edge. On the 13 Oct pair: 0.07 % of the pixels that would use the brighter pass rejected, every spill spot gone,
   the shadow noise gain unchanged (prototype on the hardware files). */
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void box3(const float *src, float *dst, int w, int h) {    /* 3 x 3 mean; NaN in -> NaN out */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if (y < 1 || y >= h - 1 || x < 1 || x >= w - 1) { dst[(size_t)y * w + x] = NAN; continue; }
        double s = 0; for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) s += src[(size_t)(y + j) * w + x + i];
        dst[(size_t)y * w + x] = (float)(s / 9);
    }
}
static void spill_check(const uint16_t *n, const float *b, int w, int h, int ch, HdrInfo *hi, float *wg) {
    size_t np = (size_t)w * h; long long use = 0;
    for (size_t i = 0; i < np; i++) use += wg[i] > 0;
    hi->spill_px = -1; hi->spill_frac = 0;            /* -1: the check did not run */
    if (!use) { hi->spill_px = 0; return; }           /* nothing would use the longer pass: nothing to check */
    if (w < 8 || h < 8) return;
    static const double E[] = { 0, 50, 100, 200, 400, 800, 1600, 3200, 6000, 1e30 }; enum { NB = 9 };
    float *a = malloc(np * sizeof(float)), *nm = malloc(np * sizeof(float)), *bm = malloc(np * sizeof(float)), *smp = malloc(200000 * sizeof(float));
    unsigned char *bad = calloc(np, 1), *bad2 = calloc(np, 1);
    if (!a || !nm || !bm || !smp || !bad || !bad2) goto done;
    hi->spill_px = 0;
    for (int c = 0; c < ch; c++) {
        for (size_t i = 0; i < np; i++) a[i] = n[i * ch + c];
        box3(a, nm, w, h);
        for (size_t i = 0; i < np; i++) { float v = b[i * ch + c]; a[i] = v < 0 ? NAN : (float)((v - hi->o[c]) / hi->g[c]); }
        box3(a, bm, w, h);
        double sig[NB], med_k[NB]; int have[NB];
        for (int k = 0; k < NB; k++) {
            /* up to 200000 samples of this level bin, evenly spread over the picture */
            long long cnt = 0; for (size_t i = 0; i < np; i++) if (wg[i] > 0 && nm[i] == nm[i] && bm[i] == bm[i] && nm[i] >= E[k] && nm[i] < E[k + 1]) cnt++;
            have[k] = 0; sig[k] = 0; med_k[k] = 0;
            if (cnt < 500) continue;
            long long step = cnt / 200000 + 1, j = 0; int m = 0;
            for (size_t i = 0; i < np && m < 200000; i++) if (wg[i] > 0 && nm[i] == nm[i] && bm[i] == bm[i] && nm[i] >= E[k] && nm[i] < E[k + 1]) { if (j++ % step == 0) smp[m++] = bm[i] - nm[i]; }
            qsort(smp, m, sizeof(float), cmp_f); double med = smp[m / 2]; med_k[k] = med > 0 ? med : 0;
            for (int q = 0; q < m; q++) smp[q] = fabsf(smp[q] - (float)med);
            qsort(smp, m, sizeof(float), cmp_f); sig[k] = 1.4826 * smp[m / 2]; have[k] = 1;
        }
        double fallback = 0; for (int k = 0; k < NB; k++) if (have[k] && sig[k] > fallback) fallback = sig[k];
        if (fallback <= 0) continue;
        for (int k = 0; k < NB; k++) if (!have[k]) {     /* an empty bin: the nearest measured one */
            int d; for (d = 1; d < NB; d++) { if (k - d >= 0 && have[k - d]) { sig[k] = sig[k - d]; med_k[k] = med_k[k - d]; break; } if (k + d < NB && have[k + d]) { sig[k] = sig[k + d]; med_k[k] = med_k[k + d]; break; } }
            if (d >= NB) sig[k] = fallback; }
        for (size_t i = 0; i < np; i++) {
            if (wg[i] <= 0 || nm[i] != nm[i] || bm[i] != bm[i]) continue;
            int k = 0; while (k < NB - 1 && nm[i] >= E[k + 1]) k++;
            /* measured from the bin's typical difference (review: a small steady offset between the passes - the fit's
               intercept, the black level at the longer time - must not push noisy pixels over the limit; with the
               test against 0, an offset of 0.03 % of white rejected 23 % of the shadow pixels in a test) */
            if (bm[i] - nm[i] - med_k[k] > 8 * sig[k] + 0.10 * nm[i]) bad[i] = 1;
        }
    }
    /* 2 pixels around each rejected one, then a soft edge (the weight never above the 5 x 5 mean of the weights) */
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (bad[(size_t)y * w + x])
        for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { int yy = y + j, xx = x + i; if (yy >= 0 && yy < h && xx >= 0 && xx < w) bad2[(size_t)yy * w + xx] = 1; }
    long long nrej = 0;
    for (size_t i = 0; i < np; i++) if (bad2[i]) { if (wg[i] > 0) nrej++; wg[i] = 0; }
    if (nrej) {
        /* the soft edge only next to rejected pixels (within 4): elsewhere the weights stay as the rules above made them */
        for (size_t i = 0; i < np; i++) { a[i] = wg[i]; bad[i] = 0; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (bad2[(size_t)y * w + x])
            for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { int yy = y + j, xx = x + i; if (yy >= 0 && yy < h && xx >= 0 && xx < w) bad[(size_t)yy * w + xx] = 1; }
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            if (!bad[(size_t)y * w + x]) continue;
            double s = 0; int m = 0;
            for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) { int yy = y + j, xx = x + i; if (yy >= 0 && yy < h && xx >= 0 && xx < w) { s += a[(size_t)yy * w + xx]; m++; } }
            float mean = (float)(s / m); if (wg[(size_t)y * w + x] > mean) wg[(size_t)y * w + x] = mean;
        }
    }
    hi->spill_px = nrej; hi->spill_frac = (double)nrej / use;
done:
    free(a); free(nm); free(bm); free(smp); free(bad); free(bad2);
}

int hdr_merge(const uint16_t *n, const float *b, int w, int h, int ch, uint16_t *out, HdrInfo *hi) {
    memset(hi, 0, sizeof *hi); hi->clip = CLIP;
    size_t np = (size_t)w * h;
    int brighter = 0;
    /* the fit uses 8 x 8 block means of both passes: pixel noise (in both) and the slight softening of fine detail by
       the alignment interpolation (in the brighter pass only) would otherwise bias g low and o high (found in
       testing: o 254 instead of 30 on a finely textured picture) */
    enum { BS = 8 };
    int bw = w / BS, bh = h / BS; size_t nbk = (size_t)bw * bh;
    /* a block is used only if the brighter pass has no near-clipped value within 4 pixels of it: the aligned pass is
       interpolated from that neighbourhood, and clipped neighbours ring into it (striped test: g 1.86 instead of 2) */
    float *bgd = dilated_max(NULL, b, w, h, ch, 4);
    double *mx = malloc(nbk * sizeof(double)), *my = malloc(nbk * sizeof(double)); unsigned char *ok = malloc(nbk);
    if (!mx || !my || !ok || !bgd) { free(mx); free(my); free(ok); free(bgd); memcpy(out, n, np * ch * sizeof(uint16_t)); return -1; }
    for (int c = 0; c < ch; c++) {
        for (int by = 0; by < bh; by++) for (int bx = 0; bx < bw; bx++) {
            double sx = 0, sy = 0; int good = 1;
            for (int y = by * BS; y < by * BS + BS && good; y++) for (int x = bx * BS; x < bx * BS + BS; x++) {
                size_t i = ((size_t)y * w + x) * ch + c; double xv = n[i], yv = b[i];
                if (yv < 0 || yv > 0.6 * CLIP || xv < 200 || xv > 0.6 * CLIP || bgd[(size_t)y * w + x] > 0.6 * CLIP) { good = 0; break; }   /* both well inside their range */
                sx += xv; sy += yv; }
            size_t k = (size_t)by * bw + bx; ok[k] = (unsigned char)good; mx[k] = sx / (BS * BS); my[k] = sy / (BS * BS);
        }
        double g = 1, o = 0, rms = 1e30; long long cnt = 0;
        for (int round = 0; round < 2; round++) {
            double sx = 0, sy = 0, sxx = 0, sxy = 0, see = 0; long long m = 0;
            for (size_t k = 0; k < nbk; k++) {
                if (!ok[k]) continue;
                if (round == 1 && fabs(my[k] - (g * mx[k] + o)) > 4 * rms) continue;
                sx += mx[k]; sy += my[k]; sxx += mx[k] * mx[k]; sxy += mx[k] * my[k]; m++;
            }
            if (m < 50) { cnt = m; break; }
            double d = m * sxx - sx * sx; if (d <= 0) { cnt = 0; break; }
            double ng = (m * sxy - sx * sy) / d, no = (sy - ng * sx) / m;
            for (size_t k = 0; k < nbk; k++) {
                if (!ok[k]) continue;
                if (round == 1 && fabs(my[k] - (g * mx[k] + o)) > 4 * rms) continue;
                double e = my[k] - (ng * mx[k] + no); see += e * e;
            }
            g = ng; o = no; cnt = m; rms = sqrt(see / m); if (rms < 0.5) rms = 0.5;
        }
        hi->g[c] = g; hi->o[c] = o; hi->nfit[c] = cnt; hi->fit_rms[c] = rms;
        if (cnt < 50) { free(mx); free(my); free(ok); free(bgd); memcpy(out, n, np * ch * sizeof(uint16_t)); return -1; }
        if (g >= 1.2) brighter = 1;
    }
    free(mx); free(my); free(ok); free(bgd);
    if (!brighter) { memcpy(out, n, np * ch * sizeof(uint16_t)); return -2; }
    /* neighbourhood guard: the aligned brighter pass is interpolated from a 6 x 6 neighbourhood, and next to a bright
       edge (clipped in the brighter pass, or just steep) the interpolation rings into the dark side (found in an
       independent review: shadows within 6 px of a clipped edge 365 rms against 8 for the normal pass). So the
       darkness test uses the brightest normal-pass value within 4 pixels (largest channel), not the pixel itself. */
    float *tmp = dilated_max(n, NULL, w, h, ch, 4);
    if (!tmp) { memcpy(out, n, np * ch * sizeof(uint16_t)); return -1; }
    long long nb = 0, nm = 0, nn = 0;
    double lo = 0.65 * CLIP, hiw = 0.85 * CLIP;
    float *wg = malloc(np * sizeof(float));
    if (!wg) { free(tmp); memcpy(out, n, np * ch * sizeof(uint16_t)); return -1; }
    for (size_t i = 0; i < np; i++) {
        const float *bp = b + i * ch;
        double m = 0, mn = tmp[i]; int have = 1;
        for (int c = 0; c < ch; c++) { if (bp[c] < 0) have = 0; if (bp[c] > m) m = bp[c]; }
        double wgt = !have ? 0 : m <= lo ? 1 : m >= hiw ? 0 : (hiw - m) / (hiw - lo);
        /* only the dark part needs the longer pass: above 3 % of white (normal pass, brightest within 4 pixels) the
           normal pass takes over by 8 % (chosen by measurement: radius 2 lets ringing through, 3 - 4 do not; 3 / 8 % gave
           the lowest shadow error in both HDR tests),
           so the rest keeps the normal pass' detail (the aligned pass is interpolated: about 1 % of the fine detail's
           contrast is lost, which outweighs the noise gain above a few % of white - measured in rawhdr_test) */
        double wl = mn <= 0.03 * CLIP ? 1 : mn >= 0.08 * CLIP ? 0 : (0.08 * CLIP - mn) / (0.05 * CLIP);
        if (wl < wgt) wgt = wl;
        wg[i] = (float)wgt;
    }
    free(tmp);
    spill_check(n, b, w, h, ch, hi, wg);
    for (size_t i = 0; i < np; i++) {
        const float *bp = b + i * ch; const uint16_t *pp = n + i * ch; uint16_t *op = out + i * ch; double wgt = wg[i];
        if (wgt >= 1) nb++; else if (wgt <= 0) nn++; else nm++;
        for (int c = 0; c < ch; c++) {
            double v = pp[c];
            if (wgt > 0) v = wgt * (bp[c] - hi->o[c]) / hi->g[c] + (1 - wgt) * pp[c];
            op[c] = v <= 0 ? 0 : v >= 65535 ? 65535 : (uint16_t)(v + 0.5);
        }
    }
    free(wg);
    hi->frac_bright = (double)nb / np; hi->frac_blend = (double)nm / np; hi->frac_normal = (double)nn / np;
    return 0;
}

double hdr_rough_gain(const uint16_t *n, const uint16_t *b, size_t ns) {
    double sn = 0, sb = 0; size_t m = 0;
    for (size_t i = 0; i < ns; i++) if (n[i] >= 1000 && n[i] <= 0.15 * CLIP && b[i] <= 0.6 * CLIP) { sn += n[i]; sb += b[i]; m++; }
    return m >= 1000 && sn > 0 ? sb / sn : 0;
}

/* ---- 0.4.1: the shift measured in bands along the scan, per channel ----
   Found on the hardware (13 Oct, continuous motion; s36, step-and-hold): the second pass is not shifted by one constant
   amount - along the scan it drifts by 0.1 - 0.2 px over a 35 mm frame (with a jump of 0.12 px in one place), and red
   lies up to 0.08 px differently from green. Identical motor commands do not give identical positions in either motion
   mode. So the shift is measured again in bands of rows (the scan direction), for each channel, starting from the
   whole-picture result and with the same dark-part mask; bands that cannot be measured (too few dark points, a weak
   direction, a poor match, or more than 0.6 px from the whole-picture shift) take their neighbours' values; a 3-band
   median removes single outliers; between band centres the shift is interpolated linearly, so every row and channel
   gets its own shift, and the brighter pass is resampled with it (Lanczos-3, as for the whole-picture shift). */
static int lerp_fill(double *v, const unsigned char *ok, int n) {   /* invalid entries from the nearest valid ones; returns valid count */
    int nv = 0; for (int i = 0; i < n; i++) nv += ok[i] != 0;
    if (!nv) return 0;
    for (int i = 0; i < n; i++) if (!ok[i]) {
        int a = i - 1, b = i + 1; while (a >= 0 && !ok[a]) a--; while (b < n && !ok[b]) b++;
        if (a < 0) v[i] = v[b]; else if (b >= n) v[i] = v[a]; else v[i] = v[a] + (v[b] - v[a]) * (i - a) / (double)(b - a);
    }
    return nv;
}
static void median3(double *v, int n) {
    if (n < 3) return;
    double *t = malloc(n * sizeof(double)); if (!t) return;
    for (int i = 0; i < n; i++) {
        if (i == 0 || i == n - 1) { t[i] = v[i]; continue; }
        double a = v[i - 1], b = v[i], c = v[i + 1];
        t[i] = a > b ? (b > c ? b : a > c ? c : a) : (a > c ? a : b > c ? c : b);
    }
    memcpy(v, t, n * sizeof(double)); free(t);
}
static void local_align(const uint16_t *rn, const uint16_t *rb, const uint8_t *mask, const uint16_t *b, int w, int h, int ch,
                        double gdx, double gdy, RsRect gcover, float *dst, HdrAlignInfo *la) {
    la->gdx = gdx; la->gdy = gdy;
    if (h < 512 || w < 128) { la->why = 1; return; }                  /* too small to split: the whole-picture shift */
    int nb = h / 256; if (nb > HDR_MAXBANDS) nb = HDR_MAXBANDS; if (nb < 2) nb = 2;
    la->nbands = nb;
    double bdx[3][HDR_MAXBANDS], bdy[3][HDR_MAXBANDS], yc[HDR_MAXBANDS]; unsigned char ok[3][HDR_MAXBANDS];
    for (int k = 0; k < nb; k++) {
        int y0 = h * k / nb, y1 = h * (k + 1) / nb; yc[k] = 0.5 * (y0 + y1 - 1); la->band_y[k] = yc[k];
        RsRect win = { 16, y0 + 8, w - 16, y1 - 8 };
        long cnt = 0; for (int y = win.y0; y < win.y1; y++) for (int x = win.x0; x < win.x1; x++) cnt += mask[(size_t)y * w + x] != 0;
        for (int c = 0; c < ch; c++) {
            bdx[c][k] = gdx; bdy[c][k] = gdy; ok[c][k] = 0;
            if (cnt < 2048) continue;
            RsImg R = { rn, w, h, ch, c, mask }, M = { rb, w, h, ch, c, NULL };
            double dx = gdx, dy = gdy; int weak = 0;
            double sc = rs_register2(R, M, &win, 1, &dx, &dy, &weak);
            la->bscore[c][k] = sc;
            if (sc < 0.8 || weak || fabs(dx - gdx) > 0.6 || fabs(dy - gdy) > 0.6) continue;   /* drift measured: 0.1 - 0.2 px */
            bdx[c][k] = dx; bdy[c][k] = dy; ok[c][k] = 1;
        }
    }
    /* a channel with too few measured bands takes the profile of the best-measured channel, moved to its own mean
       offset where it has any band, else the whole-picture shift */
    int best = 0; for (int c = 0; c < ch; c++) { int v = 0; for (int k = 0; k < nb; k++) v += ok[c][k]; la->nvalid[c] = v; if (v > la->nvalid[best]) best = c; }
    int need = nb / 2 > 2 ? nb / 2 : 2;
    if (la->nvalid[best] < need) { la->why = 2; return; }               /* not measurable: the whole-picture shift stays */
    double pdx[HDR_MAXBANDS], pdy[HDR_MAXBANDS];
    memcpy(pdx, bdx[best], sizeof pdx); memcpy(pdy, bdy[best], sizeof pdy);
    lerp_fill(pdx, ok[best], nb); lerp_fill(pdy, ok[best], nb);
    for (int c = 0; c < ch; c++) {
        if (la->nvalid[c] >= need) { lerp_fill(bdx[c], ok[c], nb); lerp_fill(bdy[c], ok[c], nb); }
        else {
            /* its own offset from the best channel's profile when at least 2 of its bands were measured (clamped to
               +-0.25 px: the colours differ by under 0.15 px on the hardware); otherwise the profile moved so that its
               mean is the whole-picture shift (review: one band could move the whole channel) */
            double ox = 0, oy = 0; int m = 0;
            for (int k = 0; k < nb; k++) if (ok[c][k]) { ox += bdx[c][k] - pdx[k]; oy += bdy[c][k] - pdy[k]; m++; }
            if (m >= 2) { ox /= m; oy /= m; ox = ox > 0.25 ? 0.25 : ox < -0.25 ? -0.25 : ox; oy = oy > 0.25 ? 0.25 : oy < -0.25 ? -0.25 : oy; }
            else { double mx = 0, my = 0; for (int k = 0; k < nb; k++) { mx += pdx[k]; my += pdy[k]; } ox = gdx - mx / nb; oy = gdy - my / nb; }
            for (int k = 0; k < nb; k++) { bdx[c][k] = pdx[k] + ox; bdy[c][k] = pdy[k] + oy; }
            la->borrowed[c] = 1;
        }
        median3(bdx[c], nb); median3(bdy[c], nb);
        la->dxmin[c] = la->dymin[c] = 1e9; la->dxmax[c] = la->dymax[c] = -1e9;
        for (int k = 0; k < nb; k++) {
            la->bdx[c][k] = bdx[c][k]; la->bdy[c][k] = bdy[c][k];
            if (bdx[c][k] < la->dxmin[c]) { la->dxmin[c] = bdx[c][k]; }
            if (bdx[c][k] > la->dxmax[c]) { la->dxmax[c] = bdx[c][k]; }
            if (bdy[c][k] < la->dymin[c]) { la->dymin[c] = bdy[c][k]; }
            if (bdy[c][k] > la->dymax[c]) { la->dymax[c] = bdy[c][k]; }
        }
    }
    /* resample: per channel and row, the shift interpolated between band centres (constant beyond the outer ones) */
    size_t ns = (size_t)w * h * ch;
    float *out = dst;                       /* nothing can fail from here on: written in place of the whole-picture result */
    for (size_t i = 0; i < ns; i++) out[i] = -1;
    for (int y = 0; y < h; y++) {
        int k1, k2; double t = 0;
        if (y <= yc[0]) k1 = k2 = 0;
        else if (y >= yc[nb - 1]) k1 = k2 = nb - 1;
        else { k1 = 0; while (yc[k1 + 1] < y) k1++; k2 = k1 + 1; t = (y - yc[k1]) / (yc[k2] - yc[k1]); }   /* yc[k1] < y <= yc[k2] */
        for (int c = 0; c < ch; c++) {
            double dx = bdx[c][k1] + (bdx[c][k2] - bdx[c][k1]) * t, dy = bdy[c][k1] + (bdy[c][k2] - bdy[c][k1]) * t;
            double ys = y + dy; int oy = (int)floor(ys), ox = (int)floor(dx);
            if (oy - 2 < 0 || oy + 3 > h - 1) continue;
            double wx[6], wy[6]; rs_lanczos6(dx - ox, wx); rs_lanczos6(ys - oy, wy);
            int x0 = 2 - ox > 0 ? 2 - ox : 0, x1 = w - 3 - ox < w ? w - 3 - ox : w;
            for (int x = x0; x < x1; x++) {
                double v = 0;
                for (int j = 0; j < 6; j++) {
                    const uint16_t *q = b + ((size_t)(oy - 2 + j) * w + (x + ox - 2)) * ch + c;
                    v += wy[j] * (wx[0] * q[0] + wx[1] * q[ch] + wx[2] * q[2 * ch] + wx[3] * q[3 * ch] + wx[4] * q[4 * ch] + wx[5] * q[5 * ch]);
                }
                out[((size_t)y * w + x) * ch + c] = (float)v;
            }
        }
    }
    /* a pixel is usable only if every channel has data (as with the whole-picture shift) */
    for (size_t i = 0; i < (size_t)w * h; i++) { int miss = 0; for (int c = 0; c < ch; c++) if (out[i * ch + c] < 0) miss = 1; if (miss) for (int c = 0; c < ch; c++) out[i * ch + c] = -1; }
    (void)gcover;
    la->local = 1;
}

int hdr_align2(const uint16_t *n, const uint16_t *b, int w, int h, int ch, float *dst, void *rsinfo, double *gain0, HdrAlignInfo *la) {
    if (la) memset(la, 0, sizeof *la);   /* la NULL: the whole-picture shift only (as 0.4.0) */
    RsInfo *info = (RsInfo *)rsinfo; size_t ns = (size_t)w * h * ch;
    double g0 = hdr_rough_gain(n, b, ns); *gain0 = g0;
    if (g0 <= 0) return -3;
    /* registration copies: the brighter pass scaled by the rough gain, both cut at the same level (enough for the
       whole-pixel search); the sub-pixel step uses only points whose neighbourhood (5 pixels) in the normal pass is
       below half the level where the brighter pass clips - the part the merge uses - so neither the clip nor the cut
       nor a steep bright edge is in it. Found in testing and in an independent review: next to clipped or bright
       edges the sub-pixel shift was biased by 0.1 - 0.3 pixel when every point was used (also with a smooth
       compression instead of the cut). */
    double cut = 0.9 * CLIP / (g0 > 1 ? g0 : 1), lim = 0.5 * CLIP / (g0 > 1 ? g0 : 1);
    size_t np = (size_t)w * h;
    uint16_t *rn = malloc(ns * sizeof(uint16_t)), *rb = malloc(ns * sizeof(uint16_t));
    float *t1 = malloc(np * sizeof(float)), *t2 = malloc(np * sizeof(float)); uint8_t *mask = malloc(np);
    if (!rn || !rb || !t1 || !t2 || !mask) { free(rn); free(rb); free(t1); free(t2); free(mask); return -1; }
    for (size_t i = 0; i < ns; i++) {
        double a = n[i] < cut ? n[i] : cut, v = b[i] / g0; if (v > cut) v = cut;
        rn[i] = (uint16_t)(a + 0.5); rb[i] = (uint16_t)(v + 0.5);
    }
    for (size_t i = 0; i < np; i++) { double m = 0; for (int c = 0; c < ch; c++) if (n[i * ch + c] > m) m = n[i * ch + c]; t1[i] = (float)m; }
    enum { MR = 5 };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float m = 0; for (int d = -MR; d <= MR; d++) { int xx = x + d; if (xx >= 0 && xx < w && t1[(size_t)y * w + xx] > m) m = t1[(size_t)y * w + xx]; }
        t2[(size_t)y * w + x] = m; }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float m = 0; for (int d = -MR; d <= MR; d++) { int yy = y + d; if (yy >= 0 && yy < h && t2[(size_t)yy * w + x] > m) m = t2[(size_t)yy * w + x]; }
        mask[(size_t)y * w + x] = m < lim; }
    free(t1); free(t2);
    RawStack s; if (rs_init(&s, w, h, ch)) { free(rn); free(rb); free(mask); return -1; }
    s.regmask = mask;
    RsInfo first; int r = rs_add(&s, rn, &first);
    if (!r) r = rs_align(&s, rb, b, info, dst);
    rs_free(&s);
    if (!r && info->used && la) local_align(rn, rb, mask, b, w, h, ch, info->dx, info->dy, info->cover, dst, la);
    free(rn); free(rb); free(mask);
    if (r) return -1;
    return info->used ? 0 : -4;
}
int hdr_align(const uint16_t *n, const uint16_t *b, int w, int h, int ch, float *dst, void *rsinfo, double *gain0) {
    HdrAlignInfo la; return hdr_align2(n, b, w, h, ch, dst, rsinfo, gain0, &la);
}
