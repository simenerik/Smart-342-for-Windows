/* world.c - per-process Mac world: volume (host directory), directory IDs, FCBs, resource chain,
 * Finder info, virtual clock. Port of MacWorld in scripts/mac_hle.py. */
#include "mac.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef _WIN32
#include <direct.h>
#endif


/* ------------------------------------------------------------ helpers */
static char *xstrdup(const char *s) { return strdup(s ? s : ""); }
static uint8_t *read_file(const char *p, size_t *n) {
    FILE *f = u_fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l + 1); size_t r = fread(d, 1, l, f); fclose(f); *n = r; return d;
}
static void write_file(const char *p, const void *d, size_t n) {
    FILE *f = u_fopen(p, "wb"); if (!f) { fprintf(stderr, "cannot write %s\n", p); return; }
    if (n) fwrite(d, 1, n, f); fclose(f);
}
static int path_exists(const char *p) { return u_stat(p, NULL, NULL) == 0; }
static long file_size(const char *p) { int64_t n; return u_stat(p, &n, NULL) == 0 ? (long)n : -1; }

/* Host file names. The volume-relative path ('rel', UTF-8, '/' in Mac names as %2F) is the canonical form
 * used everywhere (and in traces). On Windows, characters a Windows file name cannot hold (control
 * characters, < > : " \ | ? *, and a trailing space or dot) are stored as %XX in the host name. */
#ifdef _WIN32
int world_escape_names = 1;
#else
int world_escape_names = 0;
#endif
static int bad_win_char(unsigned char c) { return c < 0x20 || strchr("<>:\"\\|?*", c) != NULL; }
static void esc_component(const char *c, size_t n, char *o, size_t *k, size_t max) {
    for (size_t i = 0; i < n && *k + 4 < max; i++) {
        unsigned char ch = (unsigned char)c[i];
        if (bad_win_char(ch) || ((ch == ' ' || ch == '.') && i == n - 1)) *k += (size_t)snprintf(o + *k, max - *k, "%%%02X", ch);
        else o[(*k)++] = (char)ch;
    }
}
void host_path(World *W, const char *rel, char *out, int outmax) {
    if (!rel[0]) { snprintf(out, outmax, "%s", W->root); return; }
    if (!world_escape_names) { snprintf(out, outmax, "%s/%s", W->root, rel); return; }
    size_t k = (size_t)snprintf(out, outmax, "%s/", W->root);
    for (const char *p = rel; *p && k + 4 < (size_t)outmax;) {
        const char *e = strchr(p, '/'); size_t n = e ? (size_t)(e - p) : strlen(p);
        esc_component(p, n, out, &k, (size_t)outmax);
        if (!e) break;
        out[k++] = '/'; p = e + 1;
    }
    out[k] = 0;
}
/* host directory entry name -> rel component (inverse of esc_component) */
void host_name_to_rel(const char *h, char *out, int outmax) {
    if (!world_escape_names) { snprintf(out, outmax, "%s", h); return; }
    size_t n = strlen(h), k = 0;
    for (size_t i = 0; i < n && k + 2 < (size_t)outmax;) {
        if (h[i] == '%' && i + 2 < n && isxdigit((unsigned char)h[i + 1]) && isxdigit((unsigned char)h[i + 2])) {
            char hx[3] = { h[i + 1], h[i + 2], 0 }; unsigned char ch = (unsigned char)strtoul(hx, NULL, 16);
            if (bad_win_char(ch) || ((ch == ' ' || ch == '.') && i + 3 == n)) { out[k++] = (char)ch; i += 3; continue; }
        }
        out[k++] = h[i++];
    }
    out[k] = 0;
}
int file_exists(World *W, const char *rel) { char p[2048]; host_path(W, rel, p, sizeof p); return path_exists(p); }
int is_dir(World *W, const char *rel) {
    char p[2048]; host_path(W, rel, p, sizeof p); int d = 0; return u_stat(p, NULL, &d) == 0 && d;
}

/* ---------------------------------------------------------- directories */
static void add_dir(DirTab *D, const char *rel, int id);
int dir_id_for(World *W, const char *rel_in) {
    char rel[1024]; const char *s = rel_in; while (*s == '/') s++;
    snprintf(rel, sizeof rel, "%s", s); size_t l = strlen(rel); while (l && rel[l - 1] == '/') rel[--l] = 0;
    DirTab *D = W->dirs;
    for (int i = 0; i < D->nbyp; i++) if (!strcmp(D->byp[i].rel, rel)) return D->byp[i].id;
    int id = D->ctr++;
    if (D->nbyp == D->capbyp) { D->capbyp = D->capbyp * 2 + 64; D->byp = realloc(D->byp, D->capbyp * sizeof *D->byp); }
    D->byp[D->nbyp].rel = xstrdup(rel); D->byp[D->nbyp].id = id; D->nbyp++;
    if (D->persist[0]) { FILE *f = u_fopen(D->persist, "ab"); if (f) { fprintf(f, "%d\t%s\n", id, rel); fclose(f); } }
    return id;
}
/* Directory IDs are kept from session to session (.dirids.txt in the volume root: "id<TAB>path" per line), as on a
   real HFS disk. The original stores directory IDs in its preferences (e.g. the tables folder, TSetUp::LoadPreferences
   0x0836a0, used without re-checking once the preferences exist); IDs given out in lookup order differed between
   sessions, so a stored ID could later name another folder. */
static void dirids_load(DirTab *D) {
    size_t n; uint8_t *d = read_file(D->persist, &n); if (!d) return;
    char *s = (char *)d; char *end = s + n;
    while (s < end) {
        char *nl = memchr(s, '\n', (size_t)(end - s)); if (!nl) nl = end;
        char *tab = memchr(s, '\t', (size_t)(nl - s));
        if (tab) {
            int id = atoi(s); char rel[1024]; size_t l = (size_t)(nl - tab - 1); if (l >= sizeof rel) l = sizeof rel - 1;
            memcpy(rel, tab + 1, l); rel[l] = 0;
            int known = 0; for (int i = 0; i < D->nbyp; i++) if (!strcmp(D->byp[i].rel, rel) || D->byp[i].id == id) known = 1;
            if (id > 2 && !known) { add_dir(D, rel, id); if (id >= D->ctr) D->ctr = id + 1; }
        }
        s = nl + 1;
    }
    free(d);
}
const char *dir_rel(World *W, int id) {
    DirTab *D = W->dirs;
    for (int i = 0; i < D->nbyp; i++) if (D->byp[i].id == id) return D->byp[i].rel;
    return NULL;
}
static void add_dir(DirTab *D, const char *rel, int id) {
    if (D->nbyp == D->capbyp) { D->capbyp = D->capbyp * 2 + 64; D->byp = realloc(D->byp, D->capbyp * sizeof *D->byp); }
    D->byp[D->nbyp].rel = xstrdup(rel); D->byp[D->nbyp].id = id; D->nbyp++;
}

/* name_mac: Mac Roman; result: UTF-8 host-relative path ('/' separated, '/' in names as %2F) */
void world_resolve(World *W, int vref, uint32_t dirid, const char *name_mac, char *out, int outmax) {
    (void)vref;
    const char *base = dir_rel(W, (int)dirid);
    if (dirid == 0 || !base) { base = W->wd_dir ? dir_rel(W, W->wd_dir) : NULL; if (!base) base = W->app_dir_rel; }
    char name[1024]; macroman_to_utf8(name_mac, -1, name, sizeof name);
    char *nm = name; if (nm[0] == ':') nm++;
    /* split on ':' dropping empty parts */
    char *parts[64]; int np = 0; char buf[1024]; snprintf(buf, sizeof buf, "%s", nm);
    for (char *p = buf; *p && np < 64;) { char *c = strchr(p, ':'); if (c) *c = 0; if (*p) parts[np++] = p; if (!c) break; p = c + 1; }
    char vn[64]; macroman_to_utf8(W->volname, -1, vn, sizeof vn);
    if (nm[0] && nm[0] != ':' && strchr(nm, ':') && np && !strcmp(parts[0], vn)) {
        memmove(parts, parts + 1, (np - 1) * sizeof(char *)); np--; base = "";
    }
    char rel[2048]; snprintf(rel, sizeof rel, "%s", base);
    for (int i = 0; i < np; i++) {
        char esc[1024]; int k = 0;
        for (const char *q = parts[i]; *q && k < 1000; q++) { if (*q == '/') { memcpy(esc + k, "%2F", 3); k += 3; } else esc[k++] = *q; }
        esc[k] = 0;
        size_t l = strlen(rel);
        if (l) snprintf(rel + l, sizeof rel - l, "/%s", esc); else snprintf(rel, sizeof rel, "%s", esc);
    }
    snprintf(out, outmax, "%s", rel);
}
void parent_of(const char *rel, char *out) {
    const char *s = strrchr(rel, '/');
    if (!s) { out[0] = 0; return; }
    memcpy(out, rel, s - rel); out[s - rel] = 0;
}
void leaf_of(World *W, const char *rel, char *out_mac) {
    if (!rel[0]) { strcpy(out_mac, W->volname); return; }
    const char *s = strrchr(rel, '/'); s = s ? s + 1 : rel;
    char t[1024]; int k = 0;
    for (const char *q = s; *q && k < 1000;) { if (!strncmp(q, "%2F", 3)) { t[k++] = '/'; q += 3; } else t[k++] = *q++; }
    t[k] = 0;
    utf8_to_macroman(t, out_mac, 256);
}
void read_fsspec(World *W, uint32_t a, int *v, uint32_t *d, char *name) {
    Emu *E = W->E; *v = rds16(E, a); *d = rd32(E, a + 2); rd_pstr(E, a + 6, name);
}
void write_fsspec(World *W, uint32_t a, int vref, uint32_t parid, const char *name_mac) {
    Emu *E = W->E; wr16(E, a, vref); wr32(E, a + 2, parid); wr_pstr(E, a + 6, name_mac, -1);
}
void spec_rel(World *W, uint32_t a, char *out, int outmax) {
    int v; uint32_t d; char n[256]; read_fsspec(W, a, &v, &d, n); world_resolve(W, v, d, n, out, outmax);
}

/* ------------------------------------------------------------ finfo json */
/* JSON string -> code points (python json.load) */
static int json_str(const char **pp, uint32_t *cps, int maxn, int *ncp) {
    const char *p = *pp; while (*p && *p != '"') p++;
    if (*p != '"') return -1;
    p++; int k = 0;
    while (*p && *p != '"') {
        uint32_t cp; const uint8_t *u = (const uint8_t *)p;
        if (*p == '\\') {
            p++;
            if (*p == 'u') { cp = (uint32_t)strtoul((char[5]){p[1], p[2], p[3], p[4], 0}, NULL, 16); p += 5; }
            else { cp = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : (uint8_t)*p; p++; }
        } else if (u[0] < 0x80) { cp = u[0]; p++; }
        else if ((u[0] & 0xE0) == 0xC0) { cp = ((u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu); p += 2; }
        else if ((u[0] & 0xF0) == 0xE0) { cp = ((u[0] & 0x0Fu) << 12) | ((u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu); p += 3; }
        else { cp = ((u[0] & 0x07u) << 18) | ((u[1] & 0x3Fu) << 12) | ((u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu); p += 4; }
        if (k >= maxn) return -1;
        cps[k++] = cp;
    }
    *ncp = k; *pp = *p ? p + 1 : p; return 0;
}
static int put_utf8c(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
static uint8_t cp_to_macroman(uint32_t cp) {
    if (cp < 128) return (uint8_t)cp;
    for (int i = 128; i < 256; i++) if (macroman_cp((uint8_t)i) == cp) return (uint8_t)i;
    return '?';
}
static void finfo_load(World *W) {
    size_t n; uint8_t *d = read_file(W->finfo_path, &n); if (!d) return;
    d[n] = 0; const char *p = (const char *)d;
    for (;;) {
        uint32_t kc[1024], tc[8], cc[8]; int nk, nt, nc;
        if (json_str(&p, kc, 1024, &nk)) break;
        while (*p && *p != '[') p++;
        if (json_str(&p, tc, 8, &nt) || json_str(&p, cc, 8, &nc)) break;
        while (*p && (*p < '0' || *p > '9') && *p != '-') p++;
        int fl = (int)strtol(p, (char **)&p, 10);
        while (*p && *p != ']') p++;
        char key[4200]; int k = 0; for (int i = 0; i < nk; i++) k += put_utf8c(kc[i], key + k); key[k] = 0;
        char tm[5] = { 0 }, cm[5] = { 0 };
        for (int i = 0; i < 4; i++) { tm[i] = (char)(i < nt ? cp_to_macroman(tc[i]) : 0); cm[i] = (char)(i < nc ? cp_to_macroman(cc[i]) : 0); }
        set_finfo(W, key, tm, cm, fl);
    }
    free(d);
}
/* python json.dump(ensure_ascii=True) string escaping, for a sequence of code points */
static void json_put_cp(FILE *f, uint32_t cp) {
    if (cp == '"' || cp == '\\') fprintf(f, "\\%c", (char)cp);
    else if (cp == '\n') fputs("\\n", f); else if (cp == '\r') fputs("\\r", f); else if (cp == '\t') fputs("\\t", f);
    else if (cp == '\b') fputs("\\b", f); else if (cp == '\f') fputs("\\f", f);
    else if (cp < 0x20 || cp >= 0x7F) {
        if (cp >= 0x10000) { uint32_t v = cp - 0x10000; fprintf(f, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF)); }
        else fprintf(f, "\\u%04x", cp);
    } else fputc((int)cp, f);
}
static void json_put_str(FILE *f, const char *s) {
    fputc('"', f);
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint32_t cp; int len = 1;
        if (*p < 0x80) cp = *p;
        else if ((*p & 0xE0) == 0xC0) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); len = 3; }
        else { cp = ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu); len = 4; }
        json_put_cp(f, cp); p += len;
    }
    fputc('"', f);
}
static void json_put_ostype(FILE *f, const char *t) {
    fputc('"', f); for (int i = 0; i < 4; i++) json_put_cp(f, macroman_cp((uint8_t)t[i])); fputc('"', f);
}
static void finfo_save(World *W) {
    FILE *f = u_fopen(W->finfo_path, "wb"); if (!f) return;
    fputs("{\n", f);
    for (int i = 0; i < W->nfi; i++) {
        json_put_str(f, W->fi[i].rel); fputs(": [\n", f); json_put_ostype(f, W->fi[i].type); fputs(",\n", f); json_put_ostype(f, W->fi[i].cre);
        fprintf(f, ",\n%d\n]%s\n", W->fi[i].flags, i + 1 < W->nfi ? "," : "");
    }
    fputs("}", f); fclose(f);
}
void get_finfo(World *W, const char *rel, char *type, char *cre, int *flags) {
    for (int i = 0; i < W->nfi; i++) if (!strcmp(W->fi[i].rel, rel)) {
        memcpy(type, W->fi[i].type, 5); memcpy(cre, W->fi[i].cre, 5); *flags = W->fi[i].flags; return;
    }
    strcpy(type, "????"); strcpy(cre, "????"); *flags = 0;
}
static int finfo_loading;
void set_finfo(World *W, const char *rel, const char *type, const char *cre, int flags) {
    int i = 0; for (; i < W->nfi; i++) if (!strcmp(W->fi[i].rel, rel)) break;
    if (i == W->nfi) {
        if (W->nfi == W->capfi) { W->capfi = W->capfi * 2 + 64; W->fi = realloc(W->fi, W->capfi * sizeof *W->fi); }
        W->fi[i].rel = xstrdup(rel); W->nfi++;
    }
    memcpy(W->fi[i].type, type, 4); W->fi[i].type[4] = 0; memcpy(W->fi[i].cre, cre, 4); W->fi[i].cre[4] = 0; W->fi[i].flags = flags;
    if (!finfo_loading) finfo_save(W);
}

/* ----------------------------------------------------------------- FCBs */
FCB *fcb_get(World *W, int ref) {
    for (int i = 0; i < W->nfcb; i++) if (W->fcb[i].ref == ref) return &W->fcb[i];
    return NULL;
}
int open_fork(World *W, const char *rel, int rsrc, int *ref) {
    char hp[2048], base[2048]; host_path(W, rel, base, sizeof base);
    snprintf(hp, sizeof hp, "%s%s", base, rsrc ? ".rsrc" : "");
    *ref = 0;
    if (!path_exists(base)) return fnfErr;
    if (!path_exists(hp)) write_file(hp, "", 0);
    size_t n = 0; uint8_t *d = read_file(hp, &n);
    if (W->nfcb == W->capfcb) { W->capfcb = W->capfcb * 2 + 16; W->fcb = realloc(W->fcb, W->capfcb * sizeof(FCB)); }
    FCB *f = &W->fcb[W->nfcb++]; memset(f, 0, sizeof *f);
    f->ref = W->next_ref; W->next_ref += 2;
    f->rel = xstrdup(rel); f->rsrc = rsrc; f->data = bytes_dup(d, n); f->pos = 0; f->path = xstrdup(hp);
    free(d); *ref = f->ref; return noErr;
}
int close_fork(World *W, int ref) {
    for (int i = 0; i < W->nfcb; i++) if (W->fcb[i].ref == ref) {
        FCB *f = &W->fcb[i];
        if (f->dirty) write_file(f->path, f->data.b, f->data.n);
        free(f->rel); free(f->path); bytes_free(&f->data);
        memmove(&W->fcb[i], &W->fcb[i + 1], (W->nfcb - i - 1) * sizeof(FCB)); W->nfcb--;
        return noErr;
    }
    return -51;
}

/* ------------------------------------------------------------ res chain */
static void rf_insert(World *W, int pos, ResFile *rf) {
    if (W->nrf == W->caprf) { W->caprf = W->caprf * 2 + 8; W->rf = realloc(W->rf, W->caprf * sizeof(ResFile *)); }
    memmove(&W->rf[pos + 1], &W->rf[pos], (W->nrf - pos) * sizeof(ResFile *)); W->rf[pos] = rf; W->nrf++;
}
int open_res_file(World *W, const char *rel) {
    char hp[2048], rp[2100]; host_path(W, rel, hp, sizeof hp); snprintf(rp, sizeof rp, "%s.rsrc", hp);
    for (int i = 0; i < W->nrf; i++) if (!strcmp(W->rf[i]->path, rp)) { W->cur_res = W->rf[i]; return W->rf[i]->refnum; }
    if (!path_exists(hp)) { W->res_err = fnfErr; return -1; }
    size_t n = 0; uint8_t *d = path_exists(rp) ? read_file(rp, &n) : NULL;
    if (!d || !n) { free(d); W->res_err = -39; return -1; }
    int ref = W->next_ref; W->next_ref += 2;
    ResFile *rf = resfile_new(ref, rp, d, n, 1); free(d);
    rf_insert(W, 0, rf); W->cur_res = rf; W->res_err = 0;
    return ref;
}
void create_res_file(World *W, const char *rel) {
    char hp[2048], rp[2100]; host_path(W, rel, hp, sizeof hp); snprintf(rp, sizeof rp, "%s.rsrc", hp);
    if (!path_exists(hp)) write_file(hp, "", 0);
    if (file_size(rp) > 0) { W->res_err = dupFNErr; return; }
    ResFile *rf = resfile_new(0, rp, NULL, 0, 1);
    Bytes b = resfile_serialize(rf); write_file(rp, b.b, b.n); bytes_free(&b);
    W->res_err = 0;
}
ResFile *res_by_ref(World *W, int ref) {
    for (int i = 0; i < W->nrf; i++) if (W->rf[i]->refnum == ref) return W->rf[i];
    return NULL;
}
void update_res_file(World *W, ResFile *rf) {
    (void)W;
    if (rf->writable && rf->dirty) { Bytes b = resfile_serialize(rf); write_file(rf->path, b.b, b.n); bytes_free(&b); rf->dirty = 0; }
}

/* ------------------------------------------------------------------ time */
/* Clock. Simulation (default): a virtual 60 Hz clock advanced by the event loop, Delay and SCSI transfer
 * time, so runs are deterministic. Hardware mode (world_realtime): the clock follows the host's monotonic
 * time and Delay really waits, because the driver uses Delay for physical settling (lamp, motors). */
int world_realtime = 0;
static int64_t host_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER f; LARGE_INTEGER t; if (!f.QuadPart) QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    return (int64_t)(t.QuadPart * 1000 / f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
static int64_t host_t0 = -1;
static int64_t host_ticks(void) { int64_t m = host_ms(); if (host_t0 < 0) host_t0 = m; return (m - host_t0) * 60 / 1000; }
void host_sleep_ms(int ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL);
#endif
}
int64_t ticks(World *W) {
    if (world_realtime) { int64_t r = host_ticks(); if (r > *W->clock) *W->clock = r; }
    return *W->clock;
}
/* replay of a recorded hardware session: the clock follows the recording (set_ext_clock from the SCSI layer);
   TickCount/Delay/event calls only creep forward slowly so that pure wait loops still end */
int world_extclock;
static double ext_frac;
static void ext_creep(World *W, double n) { ext_frac += n; if (ext_frac >= 1.0) { int64_t k = (int64_t)ext_frac; *W->clock += k; ext_frac -= (double)k; } }
void world_set_ext_clock(World *W, int64_t t) { if (t > *W->clock) *W->clock = t; }
void tick(World *W, int64_t n) { if (world_extclock) { ext_creep(W, 0.01 * (double)n); return; } if (!world_realtime) *W->clock += n; }
void world_delay(World *W, int64_t n) {
    if (world_extclock) { ext_creep(W, 0.01 * (double)n); return; }
    if (!world_realtime) { *W->clock += n; return; }
    int64_t until = ticks(W) + n;
    while (host_ticks() < until) { int64_t left = (until - host_ticks()) * 1000 / 60; host_sleep_ms(left > 50 ? 50 : (left < 1 ? 1 : (int)left)); }
    ticks(W);
}
uint32_t now_secs(World *W) {
    if (world_realtime) {
        /* local time, seconds since 1904-01-01 (the Mac keeps local time) */
        time_t t = time(NULL); struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        int64_t days = 0;
        for (int y = 1904; y < lt.tm_year + 1900; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
        static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
        int y = lt.tm_year + 1900; for (int m = 0; m < lt.tm_mon; m++) days += mdays[m] + (m == 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)));
        days += lt.tm_mday - 1;
        return (uint32_t)(days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec);
    }
    /* 2026-10-06 12:00:00 since 1904-01-01: python reference uses a fixed virtual date */
    int64_t days = 0;
    for (int y = 1904; y < 2026; y++) days += (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 366 : 365;
    static const int md[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30 };   /* Jan..Sep 2026 */
    for (int m = 0; m < 9; m++) days += md[m];
    days += 5;   /* Oct 6 */
    int64_t secs = days * 86400 + 12 * 3600;
    return (uint32_t)(secs + *W->clock / 60);
}

/* ----------------------------------------------------- resource lookup */
static void rh_set(World *W, int ref, uint32_t type, int id, uint32_t h) {
    for (int i = 0; i < W->nrh; i++) if (W->rh[i].ref == ref && W->rh[i].type == type && W->rh[i].id == id) { W->rh[i].h = h; return; }
    if (W->nrh == W->caprh) { W->caprh = W->caprh * 2 + 64; W->rh = realloc(W->rh, W->caprh * sizeof *W->rh); }
    W->rh[W->nrh].ref = ref; W->rh[W->nrh].type = type; W->rh[W->nrh].id = id; W->rh[W->nrh].h = h; W->nrh++;
}
static uint32_t rh_get(World *W, int ref, uint32_t type, int id) {
    for (int i = 0; i < W->nrh; i++) if (W->rh[i].ref == ref && W->rh[i].type == type && W->rh[i].id == id) return W->rh[i].h;
    return 0;
}
static void orig_set(World *W, uint32_t h, int ref, uint32_t type, int id) {
    for (int i = 0; i < W->norig; i++) if (W->orig[i].h == h) { W->orig[i].ref = ref; W->orig[i].type = type; W->orig[i].id = id; return; }
    if (W->norig == W->caporig) { W->caporig = W->caporig * 2 + 64; W->orig = realloc(W->orig, W->caporig * sizeof *W->orig); }
    W->orig[W->norig].h = h; W->orig[W->norig].ref = ref; W->orig[W->norig].type = type; W->orig[W->norig].id = id; W->norig++;
}
/* handle_res / res_handles are kept in the hle module (hle_core.c) through these hooks */
void hle_note_res_handle(World *W, uint32_t h, int ref, uint32_t type, int id);
uint32_t get_res(World *W, uint32_t type, int id, int one, const char *name) {
    Emu *E = W->E;
    int start = 0;
    for (int i = 0; i < W->nrf; i++) if (W->rf[i] == W->cur_res) { start = i; break; }
    int end = one ? start + 1 : W->nrf;
    for (int k = start; k < end; k++) {
        ResFile *rf = W->rf[k]; Res *r = NULL;
        if (one && rf != W->cur_res) continue;
        if (!name) r = resfile_get(rf, type, id);
        else for (int i = 0; i < rf->n; i++) if (rf->r[i].type == type && rf->r[i].name && !strcmp(rf->r[i].name, name)) { r = &rf->r[i]; break; }
        if (r) {
            int rid = r->id;
            uint32_t h = rh_get(W, rf->refnum, type, rid);
            if (!h || !handle_rec(E, h)) {
                h = new_handle(E, (uint32_t)r->data.n, r->data.b, 0);
                rh_set(W, rf->refnum, type, rid, h); hle_note_res_handle(W, h, rf->refnum, type, rid); orig_set(W, h, rf->refnum, type, rid);
            }
            W->res_err = 0;
            return h;
        }
    }
    int present = 0;
    for (int k = start; k < end && !present; k++) { ResFile *rf = W->rf[k]; for (int i = 0; i < rf->n; i++) if (rf->r[i].type == type) { present = 1; break; } }
    W->res_err = present ? -192 : 0;
    return 0;
}
/* used by hle_core.c to keep res_handles in sync */
void world_rh_set(World *W, int ref, uint32_t type, int id, uint32_t h) { rh_set(W, ref, type, id, h); }
void world_rh_del(World *W, int ref, uint32_t type, int id) {
    for (int i = 0; i < W->nrh; i++) if (W->rh[i].ref == ref && W->rh[i].type == type && W->rh[i].id == id) {
        memmove(&W->rh[i], &W->rh[i + 1], (W->nrh - i - 1) * sizeof *W->rh); W->nrh--; return;
    }
}
void world_rh_del_ref(World *W, int ref, uint32_t **hs, int *nh) {
    *hs = NULL; *nh = 0;
    for (int i = 0; i < W->nrh;) if (W->rh[i].ref == ref) {
        *hs = realloc(*hs, (*nh + 1) * 4); (*hs)[(*nh)++] = W->rh[i].h;
        memmove(&W->rh[i], &W->rh[i + 1], (W->nrh - i - 1) * sizeof *W->rh); W->nrh--;
    } else i++;
}
int world_orig3(World *W, uint32_t h, int *ref, uint32_t *type, int *id) {
    for (int i = 0; i < W->norig; i++) if (W->orig[i].h == h) { *ref = W->orig[i].ref; *type = W->orig[i].type; *id = W->orig[i].id; return 1; }
    return 0;
}
int world_orig(World *W, uint32_t h, uint32_t *type, int *id) {
    for (int i = 0; i < W->norig; i++) if (W->orig[i].h == h) { *type = W->orig[i].type; *id = W->orig[i].id; return 1; }
    return 0;
}

/* ------------------------------------------------------------- creation */
static uint8_t *std_clut8(size_t *n) {
    static const uint16_t lv[6] = { 0xFFFF, 0xCCCC, 0x9999, 0x6666, 0x3333, 0 };
    static const uint16_t ramp[10] = { 0xEEEE, 0xDDDD, 0xBBBB, 0xAAAA, 0x8888, 0x7777, 0x5555, 0x4444, 0x2222, 0x1111 };
    uint16_t ent[256][3]; int k = 0;
    for (int i = 0; i < 215; i++) { ent[k][0] = lv[i / 36]; ent[k][1] = lv[(i / 6) % 6]; ent[k][2] = lv[i % 6]; k++; }
    for (int c = 0; c < 4; c++) for (int i = 0; i < 10; i++) {
        uint16_t v = ramp[i];
        ent[k][0] = (c == 0 || c == 3) ? v : 0; ent[k][1] = (c == 1 || c == 3) ? v : 0; ent[k][2] = (c == 2 || c == 3) ? v : 0; k++;
    }
    ent[k][0] = ent[k][1] = ent[k][2] = 0; k++;
    uint8_t *o = malloc(8 + 8 * 256), *p = o;
    *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 8; *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 255;
    for (int i = 0; i < k; i++) { *p++ = (uint8_t)(i >> 8); *p++ = (uint8_t)i; for (int c = 0; c < 3; c++) { *p++ = ent[i][c] >> 8; *p++ = (uint8_t)ent[i][c]; } }
    *n = (size_t)(p - o); return o;
}

int world_persist_dirids;
World *world_new(Emu *E, const char *root, const char *app_rsrc_path, const char *app_dir_rel, FILE *log, DirTab *shared) {
    World *W = calloc(1, sizeof *W); W->E = E; E->W = W; W->log = log;
    if (u_fullpath(root, W->root, sizeof W->root)) snprintf(W->root, sizeof W->root, "%s", root);
    W->vref = -1; strcpy(W->volname, "Macintosh HD");
    if (shared) W->dirs = shared;
    else {
        W->dirs = calloc(1, sizeof(DirTab)); W->dirs->ctr = 100; add_dir(W->dirs, "", 2);
        if (world_persist_dirids) { snprintf(W->dirs->persist, sizeof W->dirs->persist, "%s/.dirids.txt", W->root); dirids_load(W->dirs); }
    }
    W->next_ref = 30;
    W->res_load = 1;
    W->own_clock = 0; W->clock = &W->own_clock;
    W->app_dir_rel = xstrdup(app_dir_rel);
    W->app_dir_id = dir_id_for(W, app_dir_rel);
    snprintf(W->finfo_path, sizeof W->finfo_path, "%s/.finfo.json", W->root);
    finfo_loading = 1; finfo_load(W); finfo_loading = 0;
    size_t n = 0; uint8_t *d = read_file(app_rsrc_path, &n);
    if (!d) { fprintf(stderr, "cannot read %s\n", app_rsrc_path); exit(2); }
    ResFile *app = resfile_new(1, app_rsrc_path, d, n, 0); free(d);
    rf_insert(W, W->nrf, app); W->cur_res = app; W->app_res = app;
    ResFile *sysf = resfile_new(2, "<System>", NULL, 0, 0);
    size_t cn; uint8_t *cl = std_clut8(&cn); resfile_put(sysf, ostype_i("clut"), 8, NULL, 0, cl, cn); free(cl);
    rf_insert(W, W->nrf, sysf); W->sys_res = sysf;
    static const struct { const char *k; uint32_t v; } G[] = {
        { "sysv", 0x0904 }, { "vm  ", 0 }, { "qd  ", 0x0230 }, { "sysa", 2 }, { "cput", 0x0108 }, { "proc", 0 }, { "mmu ", 0 },
        { "fpu ", 0 }, { "scsi", 1 }, { "mach", 406 }, { "ram ", 256u << 20 }, { "lram", 256u << 20 }, { "os  ", 0x0B },
        { "cpuf", 0 }, { "ppcf", 0 }, { "evnt", 7 }, { NULL, 0 } };
    for (int i = 0; G[i].k; i++) { W->gestalt_key[i] = ostype_i(G[i].k); W->gestalt_val[i] = G[i].v; W->ngestalt++; }
    W->zone = new_ptr(E, 64, 0);
    /* mac_hle.install defaults: driver process (single-process runs); macsys replaces them */
    W->psn_hi = 0; W->psn_lo = 0x2001;
    static const ProcEnt P0[3] = {
        { 0, 0x2001, "ScannerDriver Smart342(L)", "BGJB", "SC3D", 0, 0x2002 },
        { 0, 0x2002, "Smart342(L)", "APPL", "SC3D", 0, 0x2000 },
        { 0, 0x2000, "Finder", "FNDR", "MACS", 0, 0 } };
    memcpy(W->own_procs, P0, sizeof P0); W->own_nprocs = 3; W->procs = W->own_procs; W->nprocs = &W->own_nprocs;
    W->pmode[0].hi = 0; W->pmode[0].lo = 0x2001; W->pmode[0].mode = 0x54c0;
    W->pmode[1].hi = 0; W->pmode[1].lo = 0x2002; W->pmode[1].mode = 0x53e0; W->npmode = 2;
    return W;
}
