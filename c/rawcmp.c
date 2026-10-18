/* rawcmp.c - see rawcmp.h. */
#include "rawcmp.h"
#include "rawstack.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

int rc_compare(const uint16_t *ref, int rw, int rh, const uint16_t *img, int w, int h, int x0, int y0, double max_off, RcCmp *r) {
    memset(r, 0, sizeof *r); r->jump_row = -1;
    if (!ref || !img || x0 < 0 || y0 < 0 || x0 + rw > w || y0 + rh > h) return -1;
    uint16_t *sub = malloc((size_t)rw * rh * 3 * 2); if (!sub) return -2;
    for (int y = 0; y < rh; y++) memcpy(sub + (size_t)y * rw * 3, img + ((size_t)(y0 + y) * w + x0) * 3, (size_t)rw * 3 * 2);
    RsImg a = { ref, rw, rh, 3, 1, NULL }, b = { sub, rw, rh, 3, 1, NULL };
    double dx = 0, dy = 0; int weak = 0;
    r->ncc = rs_register2(a, b, NULL, 0, &dx, &dy, &weak); r->dx = dx; r->dy = dy;
    double s1 = 0, s2 = 0; for (size_t i = 1; i < (size_t)rw * rh * 3; i += 3) { s1 += ref[i]; s2 += sub[i]; }
    r->bright = s2 / (s1 > 0 ? s1 : 1);
    /* bands of rows, each registered from the whole-reference shift (the window is in ref coordinates; img is sampled
       around it, so the band's own edges lose nothing) */
    r->bdx_min = r->bdy_min = 1e9; r->bdx_max = r->bdy_max = -1e9; r->bncc_min = 1;
    double prev_dy = 0; int have_prev = 0;
    for (int yb = 0; yb < rh; yb += RC_BAND) {
        int ye = yb + RC_BAND > rh ? rh : yb + RC_BAND;
        if (ye - yb < RC_BAND / 2) break;          /* a short last band is part of the one before */
        if (rh - ye < RC_BAND / 2) ye = rh;
        r->bands++;
        RsRect win = { 0, yb, rw, ye };
        double bx = dx, by = dy; int bw = 0;
        double sc = rs_register2(a, b, &win, 1, &bx, &by, &bw);
        if (sc == -2 || bw) { if (ye == rh) break; continue; }   /* no measurable detail in both directions */
        r->measured++;
        r->bdx_min = fmin(r->bdx_min, bx); r->bdx_max = fmax(r->bdx_max, bx);
        r->bdy_min = fmin(r->bdy_min, by); r->bdy_max = fmax(r->bdy_max, by);
        if (sc < r->bncc_min) r->bncc_min = sc;
        if (have_prev && fabs(by - prev_dy) > fabs(r->jump)) { r->jump = by - prev_dy; r->jump_row = yb; }
        prev_dy = by; have_prev = 1;
        if (ye == rh) break;
    }
    free(sub);
    int enough = r->measured >= 2 && r->measured * 2 >= r->bands;
    r->ok = r->ncc >= 0.995 && fabs(dx) <= max_off && fabs(dy) <= max_off && enough &&
            r->bdy_max - r->bdy_min <= RC_SPREAD_MAX && r->bdx_max - r->bdx_min <= RC_SPREAD_MAX && r->bncc_min >= RC_BAND_NCC_MIN;
    return 0;
}
