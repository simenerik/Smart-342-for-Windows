/* hle_core.c - Toolbox HLE: memory, strings, resources, time, Gestalt, processes, events,
 * math and SANE conversions. Port of scripts/mac_hle.py (sections outside the AppleEvent code). */
#include "mac.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define W (E->W)
#define REG(name) static int64_t h_##name(Emu *E)
#define resNotFound (-192)
#define procNotFound (-600)
#define gestaltUndefSelectorErr (-5551)

static void ost(uint32_t v, char *out) { char raw[5]; ostype_s(v, raw); macroman_to_utf8(raw, 4, out, 32); }
static void repr_mac(const char *s, char *out) { py_repr_str(s, -1, out, 2048); }

/* -------------------------------------------------- resource handle maps */
typedef struct { uint32_t h; int ref; uint32_t type; int id; } HR;
typedef struct { HR *v; int n, cap; } HRTab;
/* handle_res lives in a side table keyed by World */
typedef struct { World *w; HRTab t; } HRW;
static HRW *hrw; static int nhrw;
static HRTab *handle_res(World *w) {
    for (int i = 0; i < nhrw; i++) if (hrw[i].w == w) return &hrw[i].t;
    hrw = realloc(hrw, (nhrw + 1) * sizeof(HRW)); hrw[nhrw].w = w; memset(&hrw[nhrw].t, 0, sizeof(HRTab)); return &hrw[nhrw++].t;
}
void hle_note_res_handle(World *w, uint32_t h, int ref, uint32_t type, int id) {
    HRTab *t = handle_res(w);
    for (int i = 0; i < t->n; i++) if (t->v[i].h == h) { t->v[i].ref = ref; t->v[i].type = type; t->v[i].id = id; return; }
    if (t->n == t->cap) { t->cap = t->cap * 2 + 64; t->v = realloc(t->v, t->cap * sizeof(HR)); }
    t->v[t->n].h = h; t->v[t->n].ref = ref; t->v[t->n].type = type; t->v[t->n].id = id; t->n++;
}
static HR *hr_get(World *w, uint32_t h) {
    HRTab *t = handle_res(w); for (int i = 0; i < t->n; i++) if (t->v[i].h == h) return &t->v[i]; return NULL;
}
static int hr_pop(World *w, uint32_t h, HR *out) {
    HRTab *t = handle_res(w);
    for (int i = 0; i < t->n; i++) if (t->v[i].h == h) { *out = t->v[i]; memmove(&t->v[i], &t->v[i + 1], (t->n - i - 1) * sizeof(HR)); t->n--; return 1; }
    return 0;
}
int hle_handle_is_resource(World *w, uint32_t h) { return hr_get(w, h) != NULL; }
/* W.handle_res accessors for hle_ui.c (dict order = insertion order) */
int hle_res_handle_at(World *w, int i, uint32_t *h, int *ref, uint32_t *type, int *id) {
    HRTab *t = handle_res(w); if (i < 0 || i >= t->n) return 0;
    *h = t->v[i].h; *ref = t->v[i].ref; *type = t->v[i].type; *id = t->v[i].id; return 1;
}
int hle_res_handle_info(World *w, uint32_t h, int *ref, uint32_t *type, int *id) {
    HR *r = hr_get(w, h); if (!r) return 0; *ref = r->ref; *type = r->type; *id = r->id; return 1;
}
void world_rh_set(World *w, int ref, uint32_t type, int id, uint32_t h);
void world_rh_del(World *w, int ref, uint32_t type, int id);
void world_rh_del_ref(World *w, int ref, uint32_t **hs, int *nh);

/* ------------------------------------------------------------- memory */
REG(NewPtr) { uint32_t n = A(E, 0), a = new_ptr(E, n, 0); W->mem_err = 0; T(E, "NewPtr", "%u -> %08x", n, a); return a; }
REG(NewPtrClear) { uint32_t n = A(E, 0), a = new_ptr(E, n, 0); W->mem_err = 0; T(E, "NewPtrClear", "%u -> %08x", n, a); return a; }
REG(DisposePtr) {
    uint32_t a = A(E, 0);
    if (a && ptr_size(E, a) < 0) emu_fail(E, "DisposePtr of non-block %08x", a);
    dispose_ptr(E, a); W->mem_err = 0; return NORET;
}
REG(SetPtrSize) { uint32_t a = A(E, 0), n = A(E, 1); int64_t cap = ptr_size(E, a); W->mem_err = (cap >= 0 && n <= cap) ? 0 : -108; return NORET; }
REG(GetPtrSize) { int64_t s = ptr_size(E, A(E, 0)); return s < 0 ? 0 : s; }
REG(NewHandle) { uint32_t n = A(E, 0), h = new_handle(E, n, NULL, 0); W->mem_err = 0; T(E, "NewHandle", "%u -> %08x", n, h); return h; }
REG(NewHandleClear) { uint32_t n = A(E, 0), h = new_handle(E, n, NULL, 0); W->mem_err = 0; T(E, "NewHandleClear", "%u -> %08x", n, h); return h; }
REG(DisposeHandle) { dispose_handle(E, A(E, 0)); W->mem_err = 0; return NORET; }
REG(GetHandleSize) { return handle_size(E, A(E, 0)); }
REG(SetHandleSize) { set_handle_size(E, A(E, 0), A(E, 1)); W->mem_err = 0; return NORET; }
REG(noop) { (void)E; return NORET; }
REG(zero) { (void)E; return 0; }
REG(MemError) { return W->mem_err; }
REG(CompactMem) { (void)E; return 64 << 20; }
REG(PurgeSpace) { wr32(E, A(E, 0), 200u << 20); wr32(E, A(E, 1), 128u << 20); return NORET; }
REG(FreeMem) { (void)E; return 200 << 20; }
REG(MaxBlock) { (void)E; return 128 << 20; }
REG(ApplicationZone) { return W->zone; }
REG(HandToHand) {
    uint32_t hp = A(E, 0), h = rd32(E, hp), n; uint8_t *d = handle_copy(E, h, &n);
    uint32_t nh = new_handle(E, n, d, 0); free(d); wr32(E, hp, nh); return 0;
}
REG(PtrToHand) {
    uint32_t src = A(E, 0), hp = A(E, 1), n = A(E, 2); uint8_t *d = malloc(n + 1); emu_read(E, src, d, n);
    uint32_t nh = new_handle(E, n, d, 0); free(d); wr32(E, hp, nh); return 0;
}
REG(BlockMove) {
    uint32_t s = A(E, 0), d = A(E, 1); int32_t n = (int32_t)A(E, 2);
    if (n > 0) { uint8_t *t = malloc(n); emu_read(E, s, t, n); emu_write(E, d, t, n); free(t); }
    return NORET;
}

/* ------------------------------------------------------ routine descriptors */
REG(NewRoutineDescriptor) {
    uint32_t p = A(E, 0); T(E, "NewRoutineDescriptor", "%08x info %x isa %d", p, A(E, 1), A(E, 2) & 0xFF);
    if ((A(E, 2) & 0xFF) == 0) {
        for (int i = 0; i < W->nm68k; i++) if (W->m68k[i].p == p) { W->m68k[i].info = A(E, 1); return p; }
        if (W->nm68k == W->capm68k) { W->capm68k = W->capm68k * 2 + 8; W->m68k = realloc(W->m68k, W->capm68k * sizeof *W->m68k); }
        W->m68k[W->nm68k].p = p; W->m68k[W->nm68k].info = A(E, 1); W->nm68k++;
    }
    return p;
}
int64_t hle_call_universal_default(Emu *E) {
    uint32_t upp = A(E, 0), info = A(E, 1), args[6];
    for (int i = 0; i < 6; i++) args[i] = A(E, 2 + i);
    T(E, "CallUniversalProc", "%08x info %x", upp, info);
    return emu_call_nested(E, upp, args, 6);
}

/* -------------------------------------------------------------- strings */
REG(c2pstr) { uint32_t a = A(E, 0); char s[4096]; int n = rd_cstr(E, a, s, sizeof s); wr_pstr(E, a, s, n); return a; }
REG(p2cstr) { uint32_t a = A(E, 0); char s[300]; rd_pstr(E, a, s); wr_cstr(E, a, s); return a; }
REG(NumToString) { char s[32]; snprintf(s, sizeof s, "%d", (int32_t)A(E, 0)); wr_pstr(E, A(E, 1), s, -1); return NORET; }
REG(StringToNum) {
    char s[300]; rd_pstr(E, A(E, 0), s);
    /* python: int(s.strip()) else 0 */
    char *p = s; while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    char *e = p + strlen(p); while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    int ok = *p != 0; char *q = p; if (*q == '+' || *q == '-') q++; if (!*q) ok = 0;
    for (char *r = q; *r; r++) if ((*r < '0' || *r > '9') && *r != '_') ok = 0;
    long v = ok ? strtol(p, NULL, 10) : 0;
    wr32(E, A(E, 1), (uint32_t)v); return NORET;
}
/* python str.lower() on Mac Roman text decoded to unicode, re-encoded for comparison */
static void lower_mac(const char *s, uint32_t *out, int *n) {
    int k = 0;
    for (; *s; s++) {
        uint32_t cp = macroman_cp((uint8_t)*s);
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        else if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)) cp += 32;       /* Latin-1 uppercase */
        else if (cp == 0x0152) cp = 0x0153;                                 /* OE ligature */
        else if (cp == 0x0178) cp = 0x00FF;                                 /* Y diaeresis */
        out[k++] = cp;
    }
    *n = k;
}
REG(EqualString) {
    char a[300], b[300]; rd_pstr(E, A(E, 0), a); rd_pstr(E, A(E, 1), b);
    if (A(E, 2) & 0xFF) return strcmp(a, b) == 0;
    uint32_t x[300], y[300]; int nx, ny; lower_mac(a, x, &nx); lower_mac(b, y, &ny);
    return nx == ny && !memcmp(x, y, nx * 4);
}
REG(IUEqualString) { char a[300], b[300]; rd_pstr(E, A(E, 0), a); rd_pstr(E, A(E, 1), b); return strcmp(a, b) ? 1 : 0; }
static int cp_cmp(const char *a, const char *b) {
    for (; *a && *b; a++, b++) { uint32_t x = macroman_cp((uint8_t)*a), y = macroman_cp((uint8_t)*b); if (x != y) return x < y ? -1 : 1; }
    return *a ? 1 : *b ? -1 : 0;
}
REG(IUCompString) { char a[300], b[300]; rd_pstr(E, A(E, 0), a); rd_pstr(E, A(E, 1), b); int c = cp_cmp(a, b); return c == 0 ? 0 : c < 0 ? 0xFFFFFFFFu : 1; }

/* ------------------------------------------------------------ date math */
static void secs_to_date(uint32_t secs, int *y, int *mo, int *d, int *h, int *mi, int *s, int *dow) {
    int64_t days = secs / 86400, rem = secs % 86400;
    *h = (int)(rem / 3600); *mi = (int)(rem % 3600 / 60); *s = (int)(rem % 60);
    *dow = (int)((days + 5) % 7) + 1;   /* 1904-01-01 was a Friday; Mac: 1 = Sunday */
    int yy = 1904;
    for (;;) { int dy = (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365; if (days < dy) break; days -= dy; yy++; }
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int m = 0; for (;; m++) { int dm = md[m] + (m == 1 && (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0))); if (days < dm) break; days -= dm; }
    *y = yy; *mo = m + 1; *d = (int)days + 1;
}
REG(IUDateString) { int y, mo, d, h, mi, s, w; secs_to_date(A(E, 0), &y, &mo, &d, &h, &mi, &s, &w); char b[32]; snprintf(b, sizeof b, "%d/%d/%02d", mo, d, y % 100); wr_pstr(E, A(E, 2), b, -1); return NORET; }
REG(IUTimeString) { int y, mo, d, h, mi, s, w; secs_to_date(A(E, 0), &y, &mo, &d, &h, &mi, &s, &w); char b[32]; snprintf(b, sizeof b, "%02d:%02d", h, mi); wr_pstr(E, A(E, 2), b, -1); return NORET; }

/* ------------------------------------------------------------- resources */
REG(GetResource) { char t[32]; ost(A(E, 0), t); int i = SA(E, 1); uint32_t h = get_res(W, A(E, 0), i, 0, NULL); T(E, "GetResource", "'%s' %d -> %08x", t, i, h); return h; }
REG(Get1Resource) { char t[32]; ost(A(E, 0), t); int i = SA(E, 1); uint32_t h = get_res(W, A(E, 0), i, 1, NULL); T(E, "Get1Resource", "'%s' %d -> %08x", t, i, h); return h; }
REG(RGetResource) { char t[32]; ost(A(E, 0), t); int i = SA(E, 1); uint32_t h = get_res(W, A(E, 0), i, 0, NULL); T(E, "RGetResource", "'%s' %d -> %08x", t, i, h); return h; }
REG(GetNamedResource) {
    char t[32], n[300], r[2048]; ost(A(E, 0), t); rd_pstr(E, A(E, 1), n);
    uint32_t h = get_res(W, A(E, 0), 0, 0, n); repr_mac(n, r); T(E, "GetNamedResource", "'%s' %s -> %08x", t, r, h); return h;
}
REG(Count1Resources) {
    char t[32]; ost(A(E, 0), t); int n = 0; for (int i = 0; i < W->cur_res->n; i++) if (W->cur_res->r[i].type == A(E, 0)) n++;
    W->res_err = 0; T(E, "Count1Resources", "'%s' -> %d", t, n); return n;
}
REG(Get1IndResource) {
    char t[32]; ost(A(E, 0), t); int idx = SA(E, 1), k = 0; uint32_t h = 0;
    for (int i = 0; i < W->cur_res->n; i++) if (W->cur_res->r[i].type == A(E, 0) && ++k == idx) { h = get_res(W, A(E, 0), W->cur_res->r[i].id, 1, NULL); break; }
    if (!h) W->res_err = resNotFound;
    T(E, "Get1IndResource", "'%s' %d -> %08x", t, idx, h); return h;
}
REG(GetResInfo) {
    HR *k = hr_get(W, A(E, 0));
    if (!k) { W->res_err = -192; return NORET; }
    ResFile *rf = res_by_ref(W, k->ref); Res *r = rf ? resfile_get(rf, k->type, k->id) : NULL;
    if (A(E, 1)) wr16(E, A(E, 1), k->id);
    if (A(E, 2)) wr32(E, A(E, 2), k->type);
    if (A(E, 3)) wr_pstr(E, A(E, 3), (r && r->name) ? r->name : "", -1);
    W->res_err = 0; return NORET;
}
REG(SetResInfo) {
    uint32_t h = A(E, 0); HR *k = hr_get(W, h);
    if (k) {
        ResFile *rf = res_by_ref(W, k->ref); int ni = SA(E, 1); char nm[300]; int have = A(E, 2) != 0; if (have) rd_pstr(E, A(E, 2), nm);
        Res *r = resfile_get(rf, k->type, k->id);
        if (r) {
            Bytes d = bytes_dup(r->data.b, r->data.n); char *oldn = r->name ? strdup(r->name) : NULL; uint8_t at = r->attr;
            resfile_remove(rf, k->type, k->id);
            resfile_put(rf, k->type, ni, have ? nm : oldn, at, d.b, d.n); bytes_free(&d); free(oldn);
        }
        rf->dirty = 1;
        world_rh_del(W, k->ref, k->type, k->id); world_rh_set(W, k->ref, k->type, ni, h); k->id = ni;
    }
    W->res_err = 0; return NORET;
}
REG(GetResourceSizeOnDisk) {
    HR *k = hr_get(W, A(E, 0)); if (!k) { W->res_err = -192; return 0; }
    Res *r = resfile_get(res_by_ref(W, k->ref), k->type, k->id); return r ? r->data.n : 0;
}
REG(ReleaseResource) {
    uint32_t h = A(E, 0); HR k;
    if (hr_pop(W, h, &k)) { world_rh_del(W, k.ref, k.type, k.id); dispose_handle(E, h); }
    W->res_err = 0; return NORET;
}
REG(DetachResource) { HR k; if (hr_pop(W, A(E, 0), &k)) world_rh_del(W, k.ref, k.type, k.id); W->res_err = 0; return NORET; }
REG(ResError) { return W->res_err & 0xFFFF; }
REG(CurResFile) { return W->cur_res->refnum; }
REG(UseResFile) {
    int ref = SA(E, 0); ResFile *rf = res_by_ref(W, ref);
    if (ref == 0) rf = W->sys_res;
    if (rf) { W->cur_res = rf; W->res_err = 0; } else W->res_err = -193;
    T(E, "UseResFile", "%d", ref); return NORET;
}
REG(SetResLoad) { W->res_load = (A(E, 0) & 0xFF) != 0; return NORET; }
REG(AddResource) {
    uint32_t h = A(E, 0), t = A(E, 1); int i = SA(E, 2); char nm[300] = ""; int have = A(E, 3) != 0; if (have) rd_pstr(E, A(E, 3), nm);
    ResFile *rf = W->cur_res; uint32_t n; uint8_t *d = handle_copy(E, h, &n);
    resfile_put(rf, t, i, (have && nm[0]) ? nm : NULL, 0, d, n); free(d); rf->dirty = 1;
    world_rh_set(W, rf->refnum, t, i, h); hle_note_res_handle(W, h, rf->refnum, t, i); W->res_err = 0;
    char ts[32], rn[2048]; ost(t, ts); if (have && nm[0]) repr_mac(nm, rn); else strcpy(rn, "None");
    T(E, "AddResource", "'%s' %d %s (%u bytes) to %s", ts, i, rn, n, rf->path);
    return NORET;
}
REG(ChangedResource) {
    HR *k = hr_get(W, A(E, 0));
    if (k) {
        ResFile *rf = res_by_ref(W, k->ref); Res *r = resfile_get(rf, k->type, k->id);
        uint32_t n; uint8_t *d = handle_copy(E, A(E, 0), &n); if (r) bytes_set(&r->data, d, n); free(d); rf->dirty = 1;
        char ts[32]; ost(k->type, ts); T(E, "ChangedResource", "'%s' %d in %s", ts, k->id, rf->path);
    }
    W->res_err = 0; return NORET;
}
REG(WriteResource) {
    HR *k = hr_get(W, A(E, 0));
    if (k) {
        ResFile *rf = res_by_ref(W, k->ref); Res *r = resfile_get(rf, k->type, k->id);
        uint32_t n; uint8_t *d = handle_copy(E, A(E, 0), &n); if (r) bytes_set(&r->data, d, n); free(d); rf->dirty = 1;
        update_res_file(W, rf);
    }
    W->res_err = 0; return NORET;
}
REG(RemoveResource) {
    HR k;
    if (hr_pop(W, A(E, 0), &k)) {
        ResFile *rf = res_by_ref(W, k.ref); resfile_remove(rf, k.type, k.id); rf->dirty = 1;
        world_rh_del(W, k.ref, k.type, k.id);
        char ts[32]; ost(k.type, ts); T(E, "RemoveResource", "'%s' %d", ts, k.id);
    }
    W->res_err = 0; return NORET;
}
REG(UpdateResFile) { ResFile *rf = res_by_ref(W, SA(E, 0)); if (rf) update_res_file(W, rf); W->res_err = 0; return NORET; }
REG(CloseResFile) {
    ResFile *rf = res_by_ref(W, SA(E, 0));
    if (rf && rf != W->app_res && rf != W->sys_res) {
        update_res_file(W, rf);
        for (int i = 0; i < W->nrf; i++) if (W->rf[i] == rf) { memmove(&W->rf[i], &W->rf[i + 1], (W->nrf - i - 1) * sizeof(ResFile *)); W->nrf--; break; }
        uint32_t *hs; int nh; world_rh_del_ref(W, rf->refnum, &hs, &nh);
        for (int i = 0; i < nh; i++) { HR k; hr_pop(W, hs[i], &k); }
        free(hs);
        if (W->cur_res == rf) W->cur_res = W->rf[0];
    }
    T(E, "CloseResFile", "%d", SA(E, 0)); return NORET;
}
REG(LMGetCurMap) { return W->cur_res->refnum; }
REG(GetIndString) {
    uint32_t dst = A(E, 0); int rid = SA(E, 1), idx = SA(E, 2);
    uint32_t h = get_res(W, ostype_i("STR#"), rid, 0, NULL); char s[300] = ""; int sl = 0;
    if (h) {
        uint32_t n; uint8_t *d = handle_copy(E, h, &n); int cnt = (d[0] << 8) | d[1]; uint32_t p = 2;
        for (int k = 1; k <= cnt; k++) { int ln = d[p]; if (k == idx) { memcpy(s, d + p + 1, ln); s[ln] = 0; sl = ln; break; } p += 1 + ln; }
        free(d);
    }
    wr_pstr(E, dst, s, sl);
    char sh[64], r[2048]; int cut = 0, k = 0;
    /* python: s[:60] counts unicode characters; Mac Roman is one byte per character */
    for (; s[k] && cut < 60; k++, cut++) sh[k] = s[k]; sh[k] = 0;
    repr_mac(sh, r); T(E, "GetIndString", "%d,%d -> %s", rid, idx, r);
    return NORET;
}
REG(GetString) { int rid = SA(E, 0); uint32_t h = get_res(W, ostype_i("STR "), rid, 0, NULL); T(E, "GetString", "%d -> %08x", rid, h); return h; }

/* ------------------------------------------------------------------ time */
REG(TickCount) { tick(W, 1); return (uint32_t)ticks(W); }
REG(LMGetTime) { return now_secs(W); }
REG(Delay) { uint32_t n = A(E, 0); world_delay(W, n); if (A(E, 1)) wr32(E, A(E, 1), (uint32_t)ticks(W)); T(E, "Delay", "%u ticks", n); return NORET; }
REG(GetDateTime) { wr32(E, A(E, 0), now_secs(W)); return NORET; }
REG(ReadDateTime) { wr32(E, A(E, 0), now_secs(W)); return 0; }
static void put_date(Emu *E, uint32_t a, uint32_t secs) {
    int v[7]; secs_to_date(secs, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]);
    for (int k = 0; k < 7; k++) wr16(E, a + 2 * k, v[k]);
}
REG(GetTime) { put_date(E, A(E, 0), now_secs(W)); return NORET; }
REG(SecondsToDate) { put_date(E, A(E, 1), A(E, 0)); return NORET; }
REG(DateToSeconds) {
    uint32_t a = A(E, 0); int y = rd16(E, a), mo = rd16(E, a + 2), d = rd16(E, a + 4), h = rd16(E, a + 6), mi = rd16(E, a + 8), s = rd16(E, a + 10);
    static const int md[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t v = 0;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    if (y < 1 || mo < 1 || mo > 12 || d < 1 || d > md[mo - 1] + (mo == 2 && leap) || h > 23 || mi > 59 || s > 59) v = 0;
    else {
        int64_t days = 0;
        if (y >= 1904) for (int yy = 1904; yy < y; yy++) days += (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
        else for (int yy = y; yy < 1904; yy++) days -= (yy % 4 == 0 && (yy % 100 != 0 || yy % 400 == 0)) ? 366 : 365;
        for (int m = 0; m < mo - 1; m++) days += md[m] + (m == 1 && leap);
        days += d - 1; v = days * 86400 + h * 3600 + mi * 60 + s;
    }
    wr32(E, A(E, 1), (uint32_t)v); return NORET;
}
REG(Microseconds) { uint64_t us = (uint64_t)ticks(W) * 16667; wr32(E, A(E, 0), (uint32_t)(us >> 32)); wr32(E, A(E, 0) + 4, (uint32_t)us); return NORET; }
REG(PrimeTime) { T(E, "PrimeTime", "%08x %d", A(E, 0), (int32_t)A(E, 1)); return NORET; }

/* --------------------------------------------------------------- gestalt */
REG(Gestalt) {
    char s[32]; ost(A(E, 0), s);
    for (int i = 0; i < W->ngestalt; i++) if (W->gestalt_key[i] == A(E, 0)) {
        T(E, "Gestalt", "'%s' -> %u", s, W->gestalt_val[i]); wr32(E, A(E, 1), W->gestalt_val[i]); return 0;
    }
    T(E, "Gestalt", "'%s' -> None", s); return gestaltUndefSelectorErr & 0xFFFF;
}
REG(SysEnvirons) {
    uint32_t a = A(E, 1); emu_zero(E, a, 16); wr16(E, a, 2); wr16(E, a + 2, 406); wr16(E, a + 4, 0x0904); wr16(E, a + 6, 4);
    wr8(E, a + 10, 1); wr8(E, a + 11, 1); return 0;
}
REG(NGetTrapAddress) { return 0x00F00000u + (A(E, 0) & 0xFFFF); }
REG(DebugStr) { char s[300], r[2048]; rd_pstr(E, A(E, 0), s); repr_mac(s, r); T(E, "DebugStr", "%s", r); return NORET; }

/* ------------------------------------------------------------- processes */
REG(GetCurrentProcess) { wr32(E, A(E, 0), W->psn_hi); wr32(E, A(E, 0) + 4, W->psn_lo); return 0; }
REG(GetNextProcess) {
    uint32_t a = A(E, 0), hi = rd32(E, a), lo = rd32(E, a + 4); int n = *W->nprocs; ProcEnt *p = W->procs;
    int nxt = -1;
    if (hi == 0 && lo == 0) nxt = n ? 0 : -1;
    else { int i = n; for (int k = 0; k < n; k++) if (p[k].psn_hi == hi && p[k].psn_lo == lo) { i = k; break; } nxt = (i + 1 < n) ? i + 1 : -1; }
    if (nxt < 0) { wr32(E, a, 0); wr32(E, a + 4, 0); return procNotFound & 0xFFFF; }
    wr32(E, a, p[nxt].psn_hi); wr32(E, a + 4, p[nxt].psn_lo); return 0;
}
static uint32_t pmode(World *w, uint32_t hi, uint32_t lo) {
    for (int i = 0; i < w->npmode; i++) if (w->pmode[i].hi == hi && w->pmode[i].lo == lo) return w->pmode[i].mode;
    return 0;
}
REG(GetProcessInformation) {
    uint32_t hi = rd32(E, A(E, 0)), lo = rd32(E, A(E, 0) + 4), info = A(E, 1);
    if (hi == 0 && lo == 2) { hi = W->psn_hi; lo = W->psn_lo; }
    ProcEnt *p = NULL; for (int k = 0; k < *W->nprocs; k++) if (W->procs[k].psn_hi == hi && W->procs[k].psn_lo == lo) { p = &W->procs[k]; break; }
    char r[256]; if (p) py_repr_str(p->name, -1, r, sizeof r); else strcpy(r, "None");
    T(E, "GetProcessInformation", "(%u, %u) -> %s", hi, lo, r);
    if (!p) return procNotFound & 0xFFFF;
    uint32_t namep = rd32(E, info + 4); if (namep) wr_pstr(E, namep, p->name, -1);
    wr32(E, info + 8, p->psn_hi); wr32(E, info + 12, p->psn_lo); wr32(E, info + 16, ostype_i(p->type)); wr32(E, info + 20, ostype_i(p->sig));
    wr32(E, info + 24, pmode(W, p->psn_hi, p->psn_lo)); wr32(E, info + 28, 0); wr32(E, info + 32, 8u << 20); wr32(E, info + 36, 4u << 20);
    wr32(E, info + 40, p->lhi); wr32(E, info + 44, p->llo); wr32(E, info + 48, 0); wr32(E, info + 52, 0);
    uint32_t fsp = rd32(E, info + 56); if (fsp) write_fsspec(W, fsp, W->vref, W->app_dir_id, p->name);
    return 0;
}
REG(LaunchApplication) {
    uint32_t pb = A(E, 0);
    if (!W->launch_hook) return procNotFound & 0xFFFF;
    char rel[2048], r[2100]; spec_rel(W, rd32(E, pb + 16), rel, sizeof rel);
    char relm[2048]; utf8_to_macroman(rel, relm, sizeof relm); py_repr_str(relm, -1, r, sizeof r);
    T(E, "LaunchApplication", "%s flags %04x", r, rd16(E, pb + 14));
    uint32_t hi, lo; int64_t res = W->launch_hook(W, rel, &hi, &lo);
    if (res == 0) { wr32(E, pb + 20, hi); wr32(E, pb + 24, lo); return 0; }
    return res & 0xFFFF;
}
REG(ExitToShell) { T(E, "ExitToShell", "%s", ""); emu_fail(E, "ExitToShell called"); }

/* ---------------------------------------------------------------- events */
int ui_next_event(World *w, uint32_t mask, int remove, uint32_t *what, uint32_t *msg, uint32_t *mods);
static int get_event(Emu *E, uint32_t mask, uint32_t ev, int remove) {
    emu_zero(E, ev, 16); wr32(E, ev + 6, (uint32_t)ticks(W));
    uint32_t what, msg, mods;
    if (W->ui && ui_next_event(W, mask, remove, &what, &msg, &mods)) { wr16(E, ev, what); wr32(E, ev + 2, msg); wr16(E, ev + 14, mods); return 1; }
    if (W->nq && (mask & 0x0400)) { wr16(E, ev, 23); wr32(E, ev + 2, ostype_i(W->q[0].cls)); wr32(E, ev + 10, ostype_i(W->q[0].id)); return 1; }
    return 0;
}
REG(WaitNextEvent) {
    uint32_t mask = A(E, 0), ev = A(E, 1);
    if (W->yield_hook) W->yield_hook(W);
    uint32_t sl = A(E, 2); tick(W, sl < 60 ? (sl ? sl : 1) : 1);
    int r = get_event(E, mask, ev, 1);
    if (!r && !W->nq && W->on_idle) W->on_idle(W);
    return r;
}
REG(GetNextEvent) { if (W->yield_hook) W->yield_hook(W); tick(W, 1); return get_event(E, A(E, 0), A(E, 1), 1); }
REG(EventAvail) { return get_event(E, A(E, 0), A(E, 1), 0); }
REG(GetMouse) { wr32(E, A(E, 0), 0); return NORET; }
REG(GetKeys) { emu_zero(E, A(E, 0), 16); return NORET; }

/* ---------------------------------------------- driver-side UI stand-ins kept with mac_ui */
REG(GetPicture) { return get_res(W, ostype_i("PICT"), SA(E, 0), 0, NULL); }
REG(GetCTable) { return new_handle(E, 8 + 256 * 8, NULL, 0); }
REG(GetScrap) { (void)E; return 0xFFFFFF9Au; }     /* -102 noTypeErr */
REG(LMGetCurApName) { (void)E; return 0; }
REG(LMGetEventQueue) { return new_ptr(E, 16, 0); }
REG(BitSet) { uint32_t a = A(E, 0), n = A(E, 1); uint8_t b = rd8(E, a + n / 8); wr8(E, a + n / 8, b | (0x80 >> (n % 8))); return NORET; }
REG(Long2Fix) { return (A(E, 0) << 16) & 0xFFFFFFFFu; }
REG(FixDiv) {
    int64_t a = (int32_t)A(E, 0), b = (int32_t)A(E, 1);
    if (!b) return 0x7FFFFFFF;
    double q = (double)(a * 65536) / (double)b;   /* python: int(a * 65536 / b) truncates toward zero */
    return (uint32_t)(int64_t)q;
}
REG(GetPictInfo) { (void)E; return 0; }

/* ----------------------------------------------------------------- math */
#define M1(name, expr) REG(name) { double x = FA(E, 0); (void)x; SETF1(E, (expr)); return NORET; }
M1(sqrt, x >= 0 ? sqrt(x) : NAN)
M1(exp, exp(x))
M1(log10, x > 0 ? log10(x) : -INFINITY)
M1(ceil, ceil(x))
M1(floor, floor(x))
M1(log, x > 0 ? log(x) : (x == 0 ? -INFINITY : NAN))
M1(sin, isinf(x) ? NAN : sin(x))
M1(cos, isinf(x) ? NAN : cos(x))
M1(tan, isinf(x) ? NAN : tan(x))
M1(atan, atan(x))
M1(asin, (x < -1 || x > 1) ? NAN : asin(x))
M1(acos, (x < -1 || x > 1) ? NAN : acos(x))
M1(fabs, fabs(x))
REG(pow) {
    double x = FA(E, 0), y = FA(E, 1), r = pow(x, y);
    /* python math.pow raises (-> NaN in the reference) for domain errors and overflow */
    if ((x < 0 && y != floor(y)) || (x == 0 && y < 0) || (isinf(r) && !isinf(x) && !isinf(y))) r = NAN;
    SETF1(E, r); return NORET;
}
REG(atan2) { SETF1(E, atan2(FA(E, 0), FA(E, 1))); return NORET; }

/* ------------------------------------------------------- SANE decimal */
/* decimal = {char sgn; char unused; short exp; struct {uchar length; uchar text[36]; uchar unused;} sig;} 42 bytes
 * decform = {char style; char unused; short digits;}  style 0 FLOATDECIMAL, 1 FIXEDDECIMAL */
static void wr_dec(Emu *E, uint32_t a, int sgn, int exp, const char *sig) {
    int n = (int)strlen(sig); if (n > 36) n = 36;
    wr8(E, a, sgn); wr8(E, a + 1, 0); wr16(E, a + 2, exp); wr8(E, a + 4, n);
    uint8_t t[36] = {0}; memcpy(t, sig, n); emu_write(E, a + 5, t, 36);
}
static void rd_dec(Emu *E, uint32_t a, int *sgn, int *exp, char *sig) {
    *sgn = rd8(E, a); *exp = rds16(E, a + 2); int n = rd8(E, a + 4); emu_read(E, a + 5, sig, n); sig[n] = 0;
}
REG(str2dec) {
    uint32_t s_ptr = A(E, 0), ixp = A(E, 1), d = A(E, 2), vpp = A(E, 3);
    char s[513]; int n = rd_cstr(E, s_ptr, s, sizeof s); int i = rds16(E, ixp);
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    int sgn = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { sgn = s[i] == '-'; i++; }
    int j = i; char intd[600] = "", frac[600] = ""; int ni = 0, nf = 0;
    while (j < n && s[j] >= '0' && s[j] <= '9') intd[ni++] = s[j++];
    intd[ni] = 0;
    if (j < n && s[j] == '.') {
        int k = j + 1; while (k < n && s[k] >= '0' && s[k] <= '9') frac[nf++] = s[k++];
        frac[nf] = 0; if (ni || nf) j = k;
    }
    if (!(ni || nf)) {
        char up[4] = {0}; for (int k = 0; k < 3 && i + k < n; k++) up[k] = (char)((s[i + k] >= 'a' && s[i + k] <= 'z') ? s[i + k] - 32 : s[i + k]);
        if (!strcmp(up, "INF")) { wr_dec(E, d, sgn, 0, "I"); wr16(E, ixp, i + 3); wr16(E, vpp, 1); return NORET; }
        if (!strcmp(up, "NAN")) { wr_dec(E, d, sgn, 0, "N"); wr16(E, ixp, i + 3); wr16(E, vpp, 1); return NORET; }
        wr_dec(E, d, 0, 0, "N0011"); wr16(E, vpp, i >= n ? 1 : 0); return NORET;
    }
    int exp = 0;
    if (j < n && (s[j] == 'e' || s[j] == 'E')) {
        int k = j + 1, es = 1; if (k < n && (s[k] == '+' || s[k] == '-')) { es = s[k] == '-' ? -1 : 1; k++; }
        char ed[600]; int ne = 0; while (k < n && s[k] >= '0' && s[k] <= '9') ed[ne++] = s[k++];
        ed[ne] = 0; if (ne) { exp = es * atoi(ed); j = k; }
    }
    char digits[1200]; snprintf(digits, sizeof digits, "%s%s", intd, frac);
    char *dg = digits; while (*dg == '0') dg++;
    exp -= nf;
    char sig[1200];
    if (!*dg) { strcpy(sig, "0"); exp = 0; }
    else {
        int l = (int)strlen(dg), t = l; while (t > 0 && dg[t - 1] == '0') t--;
        exp += l - t; dg[t] = 0;
        if (t > 36) { exp += t - 36; dg[36] = 0; }
        strcpy(sig, dg);
    }
    if (exp < -32768) exp = -32768; if (exp > 32767) exp = 32767;
    wr_dec(E, d, sgn, exp, sig); wr16(E, ixp, j); wr16(E, vpp, j >= n ? 1 : 0);
    return NORET;
}
REG(dec2num) {
    int sgn, exp; char sig[64]; rd_dec(E, A(E, 0), &sgn, &exp, sig); double v;
    if (sig[0] == 'I') v = INFINITY; else if (sig[0] == 'N') v = NAN;
    else { char b[128]; snprintf(b, sizeof b, "%se%d", sig[0] ? sig : "0", exp); v = strtod(b, NULL); }   /* correctly rounded */
    SETF1(E, sgn ? -v : v); return NORET;
}
/* exact decimal expansion helpers (round-half-even on the exact binary value, like Python's Decimal) */
static int fmt_e(double v, int ndig, char *digits, int *e10) {
    /* ndig significant digits */
    char b[512]; snprintf(b, sizeof b, "%.*e", ndig - 1, v);
    char *p = b; int k = 0; for (; *p && *p != 'e'; p++) if (*p >= '0' && *p <= '9') digits[k++] = *p;
    digits[k] = 0; *e10 = atoi(p + 1); return k;
}
REG(num2dec) {
    uint32_t f = A(E, 0); double x = FA(E, 0); uint32_t d = A(E, 3);
    int style = rd8(E, f), digits = rds16(E, f + 2);
    int sgn = signbit(x) ? 1 : 0;
    if (isinf(x)) { wr_dec(E, d, sgn, 0, "I"); return NORET; }
    if (isnan(x)) { wr_dec(E, d, sgn, 0, "N4000"); return NORET; }
    double v = fabs(x);
    if (v == 0) { wr_dec(E, d, sgn, 0, "0"); return NORET; }
    if (style == 1) {
        char b[1200]; snprintf(b, sizeof b, "%.*f", digits < 0 ? 0 : digits, v);
        /* quantize to 10^-digits: sig = digits without leading zeros, exponent = -digits */
        char sig[1200]; int k = 0; for (char *p = b; *p; p++) if (*p >= '0' && *p <= '9') sig[k++] = *p;
        sig[k] = 0; char *sg = sig; while (*sg == '0' && sg[1]) sg++;
        int e = -(digits < 0 ? 0 : digits);
        if (digits < 0) { /* python quantize with positive exponent */ e = -digits; double q = v / pow(10, -digits); snprintf(b, sizeof b, "%.0f", q); sg = b; }
        int l = (int)strlen(sg); if (l > 36) { e += l - 36; sg[36] = 0; }
        wr_dec(E, d, sgn, e, sg); return NORET;
    }
    int n = digits < 1 ? 1 : digits > 36 ? 36 : digits;
    char dg[64]; int e10; fmt_e(v, n, dg, &e10);
    wr_dec(E, d, sgn, e10 - n + 1, dg); return NORET;
}
REG(dec2str) {
    uint32_t f = A(E, 0), dp = A(E, 1), out = A(E, 2);
    int style = rd8(E, f), digits = rds16(E, f + 2), sgn, exp; char sig[64];
    rd_dec(E, dp, &sgn, &exp, sig);
    const char *sc = sgn ? "-" : " ";
    char res[1300];
    if (sig[0] == 'I') { snprintf(res, sizeof res, "%sINF", sc); wr_cstr(E, out, res); return NORET; }
    if (sig[0] == 'N') { snprintf(res, sizeof res, "%sNAN", sc); wr_cstr(E, out, res); return NORET; }
    if (style == 1) {
        /* Decimal(sig E exp).quantize(Decimal(1).scaleb(-nd), ROUND_HALF_EVEN), formatted 'f' */
        int nd = digits < 0 ? 0 : digits, L = (int)strlen(sig), scale = exp + nd;
        char I[1300];
        if (scale >= 0) { snprintf(I, sizeof I, "%s", sig); for (int i = 0; i < scale && i < 1200; i++) strcat(I, "0"); }
        else {
            int cut = L + scale;
            if (cut <= 0) strcpy(I, "0");          /* remainder < 0.5 because of the leading zero(s) */
            else {
                memcpy(I, sig, cut); I[cut] = 0;
                int first = sig[cut] - '0', beyond = 0;
                for (int i = cut + 1; i < L; i++) if (sig[i] != '0') beyond = 1;
                int up = first > 5 || (first == 5 && (beyond || ((I[cut - 1] - '0') & 1)));
                if (up) {
                    int i = cut - 1; while (i >= 0 && I[i] == '9') { I[i] = '0'; i--; }
                    if (i >= 0) I[i]++; else { memmove(I + 1, I, strlen(I) + 1); I[0] = '1'; }
                }
            }
        }
        char *z = I; while (*z == '0' && z[1]) z++;
        char num[1300]; int zl = (int)strlen(z);
        if (nd == 0) snprintf(num, sizeof num, "%s", z);
        else {
            char t[1300]; int pad = nd + 1 - zl, k = 0; for (int i = 0; i < pad; i++) t[k++] = '0';
            strcpy(t + k, z); int tl = (int)strlen(t);
            snprintf(num, sizeof num, "%.*s.%s", tl - nd, t, t + tl - nd);
        }
        snprintf(res, sizeof res, "%s%s", sc, num); wr_cstr(E, out, res); return NORET;
    }
    int e10 = exp + (int)strlen(sig) - 1;
    char mant[80]; if (strlen(sig) > 1) snprintf(mant, sizeof mant, "%c.%s", sig[0], sig + 1); else snprintf(mant, sizeof mant, "%s", sig);
    snprintf(res, sizeof res, "%s%se%s%d", sc, mant, e10 >= 0 ? "+" : "-", e10 < 0 ? -e10 : e10);
    wr_cstr(E, out, res); return NORET;
}

/* ---------------------------------------------------------- PackBits */
static int packbits(const uint8_t *src, int n, uint8_t *out) {
    int o = 0, i = 0; uint8_t lit[128]; int nl = 0;
#define FLUSH() do { if (nl) { out[o++] = (uint8_t)(nl - 1); memcpy(out + o, lit, nl); o += nl; nl = 0; } } while (0)
    while (i < n) {
        int j = i; while (j < n && j - i < 128 && src[j] == src[i]) j++;
        int run = j - i;
        if (run >= 3) { FLUSH(); out[o++] = (uint8_t)(1 - run); out[o++] = src[i]; i = j; }
        else { lit[nl++] = src[i]; i++; if (nl == 128) FLUSH(); }
    }
    FLUSH();
#undef FLUSH
    return o;
}
REG(PackBits) {
    uint32_t ps = A(E, 0), pd = A(E, 1); int n = SA(E, 2);
    uint32_t s = rd32(E, ps), d = rd32(E, pd);
    uint8_t *src = malloc(n > 0 ? n : 1), *enc = malloc(n * 2 + 16);
    if (n > 0) emu_read(E, s, src, n);
    int m = packbits(src, n > 0 ? n : 0, enc); emu_write(E, d, enc, m);
    wr32(E, ps, s + n); wr32(E, pd, d + m); free(src); free(enc); return NORET;
}
REG(UnpackBits) {
    uint32_t ps = A(E, 0), pd = A(E, 1); int n = SA(E, 2); uint32_t s = rd32(E, ps), d = rd32(E, pd), p = s;
    uint8_t *out = malloc(n + 300); int k = 0;
    while (k < n) {
        int c = rd8(E, p++);
        if (c < 128) { emu_read(E, p, out + k, c + 1); k += c + 1; p += c + 1; }
        else if (c > 128) { int v = rd8(E, p++); memset(out + k, v, 257 - c); k += 257 - c; }
    }
    emu_write(E, d, out, n); wr32(E, ps, p); wr32(E, pd, d + n); free(out); return NORET;
}

/* ------------------------------------------------------------- registry */
void hle_ae_register(void);
void hle_core_register(void) {
#define R(n) emu_reg(#n, h_##n)
    R(NewPtr); R(NewPtrClear); emu_reg("NewPtrSys", h_NewPtr); emu_reg("NewPtrSysClear", h_NewPtrClear);
    R(DisposePtr); R(SetPtrSize); R(GetPtrSize); R(NewHandle); R(NewHandleClear); R(DisposeHandle); R(GetHandleSize); R(SetHandleSize);
    static const char *noops[] = { "HLock", "HUnlock", "HNoPurge", "HPurge", "MoveHHi", "HLockHi", "MoreMasters", "MaxApplZone", "HSetState",
        "PurgeMem", "SetZone", "DisposeRoutineDescriptor", "SysBeep", "FlushEvents", "SystemTask", "SystemClick", "CloseDeskAcc",
        "InitFonts", "InitWindows", "InitMenus", "InitDialogs", "DisposeCTable", "ZeroScrap", "LMSetSFSaveDisk", "LMSetCurDirStore",
        "LMSetTEScrpLength", "LMSetResErrProc", "PrOpen", "PrClose", "PrOpenDoc", "PrCloseDoc", "PrOpenPage", "PrClosePage",
        "PrPicFile", "PrintDefault", "InsXTime", "InsTime", "RmvTime", "feclearexcept", NULL };
    for (int i = 0; noops[i]; i++) emu_reg(noops[i], h_noop);
    static const char *zeros[] = { "HGetState", "LockMemory", "UnlockMemory", "PBAllocContigSync", "PBAllocateSync", "TruncString",
        "StillDown", "Button", "WaitMouseUp", "PrError", "PrJobDialog", "PrStlDialog", "OpenDeskAcc", "Dequeue", "PutScrap",
        "LMGetTEScrpLength", "fetestexcept", NULL };
    for (int i = 0; zeros[i]; i++) emu_reg(zeros[i], h_zero);
    R(MemError); R(CompactMem); R(PurgeSpace); R(FreeMem); R(MaxBlock); R(ApplicationZone); emu_reg("SystemZone", h_ApplicationZone);
    R(HandToHand); R(PtrToHand); R(BlockMove); emu_reg("BlockMoveData", h_BlockMove);
    R(NewRoutineDescriptor); emu_reg("CallUniversalProc", hle_call_universal_default);
    R(c2pstr); R(p2cstr); R(NumToString); R(StringToNum); R(EqualString); R(IUEqualString); R(IUCompString); R(IUDateString); R(IUTimeString);
    R(GetResource); R(Get1Resource); R(RGetResource); R(GetNamedResource); R(Count1Resources); R(Get1IndResource); R(GetResInfo);
    R(SetResInfo); R(GetResourceSizeOnDisk); R(ReleaseResource); R(DetachResource); R(ResError); R(CurResFile); R(UseResFile);
    emu_reg("LMSetCurMap", h_UseResFile); R(SetResLoad); R(AddResource); R(ChangedResource); R(WriteResource); R(RemoveResource);
    R(UpdateResFile); R(CloseResFile); R(LMGetCurMap); R(GetIndString); R(GetString);
    R(TickCount); R(LMGetTime); R(Delay); R(GetDateTime); R(ReadDateTime); R(GetTime); R(SecondsToDate); R(DateToSeconds); R(Microseconds); R(PrimeTime);
    R(Gestalt); R(SysEnvirons); R(NGetTrapAddress); emu_reg("GetToolTrapAddress", h_NGetTrapAddress); R(DebugStr);
    R(GetCurrentProcess); R(GetNextProcess); R(GetProcessInformation); R(LaunchApplication); R(ExitToShell);
    R(WaitNextEvent); R(GetNextEvent); R(EventAvail); R(GetMouse); R(GetKeys);
    R(GetPicture); R(GetCTable); R(GetScrap); R(LMGetCurApName); R(LMGetEventQueue); R(BitSet); R(Long2Fix); R(FixDiv); R(GetPictInfo);
    R(sqrt); R(pow); R(exp); R(log10); R(ceil); R(floor); R(log); R(sin); R(cos); R(tan); R(atan); R(asin); R(acos); R(fabs); R(atan2);
    R(str2dec); R(dec2num); R(num2dec); R(dec2str); R(PackBits); R(UnpackBits);
    hle_ae_register();
#undef R
}
