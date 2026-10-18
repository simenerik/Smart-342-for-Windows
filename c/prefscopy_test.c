/* prefscopy_test: the C copy must give exactly what scripts/flash_to_prefs.py gives (the Smart342.Prefs shipped
 * since 0.1.2, sha256 6b7ffb60...), and refuse damaged flash files.
 *   prefscopy_test <vendor prefs .rsrc> <flash14.bin> <expected prefs .rsrc> */
#include "mac.h"
#include "prefscopy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* the byte-buffer helpers of emu.c (the test does not link the emulator) */
void bytes_put(Bytes *x, const void *s, size_t n) {
    if (x->n + n + 1 > x->cap) { x->cap = (x->n + n + 1) * 2 + 16; x->b = realloc(x->b, x->cap); }
    if (n) { if (s) memcpy(x->b + x->n, s, n); else memset(x->b + x->n, 0, n); }
    x->n += n;
}
void bytes_set(Bytes *x, const void *s, size_t n) { x->n = 0; bytes_put(x, s, n); }
void bytes_free(Bytes *x) { free(x->b); x->b = NULL; x->n = x->cap = 0; }
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); uint8_t *d = malloc(l + 1);
    if (fread(d, 1, l, f) != (size_t)l) exit(2); fclose(f); *n = (size_t)l; return d;
}
static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS "); else { printf("FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage\n"); return 2; }
    size_t np, nf, ne; uint8_t *pr = slurp(argv[1], &np), *fl = slurp(argv[2], &nf), *ex = slurp(argv[3], &ne);
    char fs[32], ps[32], msg[300]; Bytes out;
    int n = prefs_copy_from_flash(fl, nf, pr, np, 1, &out, fs, ps, 32, msg, sizeof msg);
    CHECK(n == 11, "copy with line-work butting: %d resources (%s)", n, msg);
    CHECK(!strcmp(fs, "95171130/01") && !strcmp(ps, "96250969/01"), "numbers: scanner %s, prefs %s", fs, ps);
    CHECK(out.n == ne && !memcmp(out.b, ex, ne), "result = the Python copy (%zu / %zu bytes)", out.n, ne);
    { char s2[32]; prefs_serial(out.b, out.n, s2, 32); CHECK(!strcmp(s2, "95171130/01"), "new prefs say %s", s2); }
    bytes_free(&out);
    n = prefs_copy_from_flash(fl, nf, pr, np, 0, &out, fs, ps, 32, msg, sizeof msg);
    CHECK(n == 9, "copy without line-work butting: %d resources (%s)", n, msg); bytes_free(&out);
    /* damaged flash files are refused */
    uint8_t *bad = malloc(nf); memcpy(bad, fl, nf); bad[nf / 2] ^= 1;
    n = prefs_copy_from_flash(bad, nf, pr, np, 1, &out, fs, ps, 32, msg, sizeof msg); CHECK(n == 0, "one changed byte refused: %s", msg);
    n = prefs_copy_from_flash(fl, nf - 100, pr, np, 1, &out, fs, ps, 32, msg, sizeof msg); CHECK(n == 0, "short file refused: %s", msg);
    memcpy(bad, fl, nf); bad[7] = 1;
    n = prefs_copy_from_flash(bad, nf, pr, np, 1, &out, fs, ps, 32, msg, sizeof msg); CHECK(n == 0, "header padding refused: %s", msg);
    /* a consistent header but a broken map: offsets beyond the end */
    memcpy(bad, fl, nf); bad[16 + 4] = 0x7F; { int32_t s = 0; for (size_t i = 16; i < nf; i++) s += (int8_t)bad[i]; bad[4] = (uint8_t)((uint16_t)s >> 8); bad[5] = (uint8_t)s; }
    n = prefs_copy_from_flash(bad, nf, pr, np, 1, &out, fs, ps, 32, msg, sizeof msg); CHECK(n == 0, "broken resource map refused: %s", msg);
    printf(fails ? "SOME FAILED\n" : "ALL PASS\n");
    return fails != 0;
}
