/* text.c - Mac Roman text, Python-compatible repr() for trace output, resource fork codec. */
#include "mac.h"
#include <stdlib.h>
#include <string.h>
#include "macroman_tab.h"

uint32_t macroman_cp(uint8_t c) { return c < 128 ? c : MR_HI[c - 128]; }
static int cp_printable(uint32_t cp, uint8_t raw) {
    if (raw < 128) return raw >= 0x20 && raw < 0x7F;
    (void)cp; return MR_PRINT[raw - 128];
}
static int put_utf8(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
}
int macroman_to_utf8(const char *s, int n, char *out, int outmax) {
    if (n < 0) n = (int)strlen(s);
    int k = 0;
    for (int i = 0; i < n && k + 4 < outmax; i++) k += put_utf8(macroman_cp((uint8_t)s[i]), out + k);
    out[k] = 0; return k;
}
int utf8_to_macroman(const char *s, char *out, int outmax) {
    int k = 0; const uint8_t *p = (const uint8_t *)s;
    while (*p && k + 1 < outmax) {
        uint32_t cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); len = 3; }
        else { cp = '?'; len = 1; while (p[len] && (p[len] & 0xC0) == 0x80) len++; }
        uint8_t c = '?';
        if (cp < 128) c = (uint8_t)cp;
        else for (int i = 0; i < 128; i++) if (MR_HI[i] == cp) { c = (uint8_t)(128 + i); break; }
        out[k++] = (char)c; p += len;
    }
    out[k] = 0; return k;
}

void py_repr_str(const char *s, int n, char *out, int outmax) {
    if (n < 0) n = (int)strlen(s);
    int has_sq = 0, has_dq = 0;
    for (int i = 0; i < n; i++) { if (s[i] == '\'') has_sq = 1; if (s[i] == '"') has_dq = 1; }
    char q = (has_sq && !has_dq) ? '"' : '\'';
    int k = 0; out[k++] = q;
    for (int i = 0; i < n && k + 12 < outmax; i++) {
        uint8_t c = (uint8_t)s[i]; uint32_t cp = macroman_cp(c);
        if (c == '\\') { out[k++] = '\\'; out[k++] = '\\'; }
        else if (c == (uint8_t)q) { out[k++] = '\\'; out[k++] = q; }
        else if (c == '\t') { out[k++] = '\\'; out[k++] = 't'; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c == '\r') { out[k++] = '\\'; out[k++] = 'r'; }
        else if (!cp_printable(cp, c)) {
            if (cp < 0x100) k += sprintf(out + k, "\\x%02x", cp); else k += sprintf(out + k, "\\u%04x", cp);
        } else k += put_utf8(cp, out + k);
    }
    out[k++] = q; out[k] = 0;
}
void py_repr_bytes(const uint8_t *s, int n, char *out, int outmax) {
    int has_sq = 0, has_dq = 0;
    for (int i = 0; i < n; i++) { if (s[i] == '\'') has_sq = 1; if (s[i] == '"') has_dq = 1; }
    char q = (has_sq && !has_dq) ? '"' : '\'';
    int k = 0; out[k++] = 'b'; out[k++] = q;
    for (int i = 0; i < n && k + 8 < outmax; i++) {
        uint8_t c = s[i];
        if (c == '\\') { out[k++] = '\\'; out[k++] = '\\'; }
        else if (c == (uint8_t)q) { out[k++] = '\\'; out[k++] = q; }
        else if (c == '\t') { out[k++] = '\\'; out[k++] = 't'; }
        else if (c == '\n') { out[k++] = '\\'; out[k++] = 'n'; }
        else if (c == '\r') { out[k++] = '\\'; out[k++] = 'r'; }
        else if (c < 0x20 || c >= 0x7F) k += sprintf(out + k, "\\x%02x", c);
        else out[k++] = (char)c;
    }
    out[k++] = q; out[k] = 0;
}
void hexs(const uint8_t *s, int n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = hx[s[i] >> 4]; out[2 * i + 1] = hx[s[i] & 15]; }
    out[2 * n] = 0;
}
uint32_t ostype_i(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint8_t)s[1] << 16) | ((uint8_t)s[2] << 8) | (uint8_t)s[3];
}
void ostype_s(uint32_t v, char *out) { out[0] = (char)(v >> 24); out[1] = (char)(v >> 16); out[2] = (char)(v >> 8); out[3] = (char)v; out[4] = 0; }

/* ----------------------------------------------------- resource forks */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

ResFile *resfile_new(int refnum, const char *path, const uint8_t *data, size_t len, int writable) {
    ResFile *rf = calloc(1, sizeof *rf); rf->refnum = refnum; rf->path = strdup(path); rf->writable = writable;
    if (data && len) rsrc_parse(data, len, rf);
    return rf;
}
Res *resfile_get(ResFile *rf, uint32_t type, int id) {
    for (int i = 0; i < rf->n; i++) if (rf->r[i].type == type && rf->r[i].id == id) return &rf->r[i];
    return NULL;
}
Res *resfile_put(ResFile *rf, uint32_t type, int id, const char *name, uint8_t attr, const void *d, size_t n) {
    Res *x = resfile_get(rf, type, id);
    if (!x) {
        if (rf->n == rf->cap) { rf->cap = rf->cap * 2 + 32; rf->r = realloc(rf->r, rf->cap * sizeof(Res)); }
        x = &rf->r[rf->n++]; memset(x, 0, sizeof *x); x->type = type; x->id = (int16_t)id;
    } else { free(x->name); x->name = NULL; }
    x->name = name ? strdup(name) : NULL; x->attr = attr; bytes_set(&x->data, d, n);
    return x;
}
void resfile_remove(ResFile *rf, uint32_t type, int id) {
    for (int i = 0; i < rf->n; i++) if (rf->r[i].type == type && rf->r[i].id == id) {
        free(rf->r[i].name); bytes_free(&rf->r[i].data);
        memmove(&rf->r[i], &rf->r[i + 1], (rf->n - i - 1) * sizeof(Res)); rf->n--; return;
    }
}
int rsrc_parse(const uint8_t *buf, size_t len, ResFile *rf) {
    if (len < 16) return -1;
    uint32_t doff = be32(buf), moff = be32(buf + 4);
    const uint8_t *m = buf + moff;
    uint16_t tlo = be16(m + 24), nlo = be16(m + 26);
    const uint8_t *tl = m + tlo;
    int ntypes = (int16_t)be16(tl) + 1;
    for (int t = 0; t < ntypes; t++) {
        const uint8_t *te = tl + 2 + 8 * t;
        uint32_t typ = be32(te); int cnt = (int16_t)be16(te + 4); uint16_t ro = be16(te + 6);
        for (int k = 0; k <= cnt; k++) {
            const uint8_t *re = tl + ro + 12 * k;
            int rid = (int16_t)be16(re); uint16_t no = be16(re + 2); uint32_t ao = be32(re + 4);
            uint8_t attr = ao >> 24; uint32_t off = ao & 0xFFFFFF;
            uint32_t ln = be32(buf + doff + off);
            char name[256], *np = NULL;
            if (no != 0xFFFF) { int nl = m[nlo + no]; memcpy(name, m + nlo + no + 1, nl); name[nl] = 0; np = name; }
            resfile_put(rf, typ, rid, np, attr, buf + doff + off + 4, ln);
        }
    }
    return 0;
}
static int type_cmp(uint32_t a, uint32_t b) {
    for (int s = 24; s >= 0; s -= 8) {
        uint32_t x = macroman_cp((a >> s) & 0xFF), y = macroman_cp((b >> s) & 0xFF);
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
static int res_cmp(const void *pa, const void *pb) {
    const Res *a = *(Res *const *)pa, *b = *(Res *const *)pb;
    int c = type_cmp(a->type, b->type); if (c) return c;
    return a->id < b->id ? -1 : a->id > b->id;
}
static void p16(Bytes *b, uint32_t v) { uint8_t x[2] = { (uint8_t)(v >> 8), (uint8_t)v }; bytes_put(b, x, 2); }
static void p32(Bytes *b, uint32_t v) { uint8_t x[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bytes_put(b, x, 4); }
Bytes resfile_serialize(ResFile *rf) {
    Res **s = malloc((rf->n + 1) * sizeof(Res *));
    for (int i = 0; i < rf->n; i++) s[i] = &rf->r[i];
    qsort(s, rf->n, sizeof(Res *), res_cmp);
    Bytes data = {0}, names = {0}, refbytes = {0}, tl = {0};
    int ntypes = 0; for (int i = 0; i < rf->n; i++) if (i == 0 || s[i]->type != s[i - 1]->type) ntypes++;
    p16(&tl, (uint16_t)(ntypes - 1));
    int ro_base = 2 + 8 * ntypes;
    for (int i = 0; i < rf->n;) {
        int j = i; while (j < rf->n && s[j]->type == s[i]->type) j++;
        p32(&tl, s[i]->type); p16(&tl, (uint16_t)(j - i - 1)); p16(&tl, (uint16_t)(ro_base + refbytes.n));
        for (int k = i; k < j; k++) {
            Res *r = s[k]; uint32_t off = (uint32_t)data.n;
            p32(&data, (uint32_t)r->data.n); bytes_put(&data, r->data.b, r->data.n);
            while (data.n % 4) bytes_put(&data, NULL, 1);
            uint32_t no = 0xFFFF;
            if (r->name) { no = (uint32_t)names.n; uint8_t l = (uint8_t)strlen(r->name); bytes_put(&names, &l, 1); bytes_put(&names, r->name, l); }
            p16(&refbytes, (uint16_t)r->id); p16(&refbytes, no); p32(&refbytes, ((uint32_t)r->attr << 24) | off); p32(&refbytes, 0);
        }
        i = j;
    }
    Bytes typelist = {0}; bytes_put(&typelist, tl.b, tl.n); bytes_put(&typelist, refbytes.b, refbytes.n);
    Bytes m = {0}; bytes_put(&m, NULL, 24); p16(&m, 28); p16(&m, (uint32_t)(28 + typelist.n));
    bytes_put(&m, typelist.b, typelist.n); bytes_put(&m, names.b, names.n);
    uint32_t doff = 256, moff = doff + (uint32_t)data.n;
    Bytes out = {0}; p32(&out, doff); p32(&out, moff); p32(&out, (uint32_t)data.n); p32(&out, (uint32_t)m.n);
    bytes_put(&out, NULL, 240); bytes_put(&out, data.b, data.n); bytes_put(&out, m.b, m.n);
    memcpy(out.b + moff, out.b, 16);
    bytes_free(&data); bytes_free(&names); bytes_free(&refbytes); bytes_free(&tl); bytes_free(&typelist); bytes_free(&m); free(s);
    return out;
}
