/* rawchart_test: the Scitex focus chart on the hardware (s23 / s24 pass 1: 35 mm lens, 2662 dpi, continuous motion):
 * the two bar blocks are found, their harmonics match scripts/sharpness/barmtf.py on the same boxes, and the fit lands
 * near the model the program uses since 0.1.24 (fitted to s21 / s23 / s24 together).
 *   rawchart_test <s23p1.bin> <s24p1.bin> [print]   (.bin: <III w h ch> + uint16 LE samples) */
#include "rawchart.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static uint16_t *load(const char *p, int *w, int *h, int *ch) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    uint32_t hd[3]; if (fread(hd, 4, 3, f) != 3) exit(2); *w = hd[0]; *h = hd[1]; *ch = hd[2];
    size_t n = (size_t)*w * *h * *ch; uint16_t *d = malloc(n * 2); if (fread(d, 2, n, f) != n) exit(2); fclose(f); return d;
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    const double px = 25.4 / 2662.1, mag = 0.007 * 2662.1 / 25.4, mot = px * 1000;
    for (int k = 1; k <= 2; k++) {
        int w, h, ch; uint16_t *img = load(argv[k], &w, &h, &ch);
        ChBox bx, by; int nf = chart_find(img, w, h, ch, &bx, &by);
        CHECK(nf == 2 && bx.x1 <= 345 && by.x0 >= 335, "%s: blocks found: across x %d-%d y %d-%d, along x %d-%d y %d-%d (the picture: across bars left of x 340, along bars right)", argv[k], bx.x0, bx.x1, bx.y0, bx.y1, by.x0, by.x1, by.y0, by.y1);
        ChBars a, b; chart_bars(img, w, h, ch, &bx, 0, px, &a); chart_bars(img, w, h, ch, &by, 1, px, &b);
        printf("   across: f0 %.3f lp/mm, duty %.3f, tilt %.4f:", a.f0, a.duty, a.tilt); for (int i = 0; i < a.nh; i++) printf(" %.1f:%.3f", a.f[i], a.v[i]); printf("\n");
        printf("   along:  f0 %.3f lp/mm, duty %.3f, tilt %.4f:", b.f0, b.duty, b.tilt); for (int i = 0; i < b.nh; i++) printf(" %.1f:%.3f", b.f[i], b.v[i]); printf("\n");
        if (argc > 3) printf("BOXES %d %d %d %d %d %d %d %d\n", bx.x0, bx.x1, bx.y0, bx.y1, by.x0, by.x1, by.y0, by.y1);
        CHECK(a.ok && b.ok && fabs(a.f0 - 8.33) < 0.05 && fabs(b.f0 - 8.33) < 0.05, "fundamentals %.3f / %.3f lp/mm (the chart's 8.33)", a.f0, b.f0);
        ChFit ft; int r = chart_fit(&a, &b, mag, 5.0, mot, 0.54, &ft);
        printf("   fit: sigma across %.3f, along %.3f um at the CCD, halo %.3f / %.3f of %.2f um; rms %.4f, largest %.4f over %d points\n", ft.sx, ft.sy, ft.ax, ft.ay, ft.sh, ft.rms, ft.maxres, ft.npts);
        CHECK(r == 0 && ft.rms < 0.02 && fabs(ft.sx - 1.693) < 0.6 && fabs(ft.sy - 3.573) < 0.8, "fit close to the program's model (1.693 / 3.573 um, fitted to s21 + s23 + s24): %.3f / %.3f, rms %.4f", ft.sx, ft.sy, ft.rms);
        free(img);
    }
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n"); return fails != 0;
}
