/* hle_files.c - File Manager / Folder Manager HLE on top of the World (host directory = boot volume).
 * Port of scripts/mac_files.py. Param-block offsets per Inside Macintosh: Files (1992), ch. 2. */
#include "mac.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#define mkdir1(p) u_mkdir(p)

#define W (E->W)
#define REG(name) static int64_t h_##name(Emu *E)

enum { IO_COMPLETION = 12, IO_RESULT = 16, IO_NAMEPTR = 18, IO_VREFNUM = 22,
       IO_REFNUM = 24, IO_PERMSSN = 27, IO_MISC = 28, IO_BUFFER = 32, IO_REQCOUNT = 36, IO_ACTCOUNT = 40,
       IO_POSMODE = 44, IO_POSOFFSET = 46,
       IO_FDIRINDEX = 28, IO_FLATTRIB = 30, IO_FLFNDRINFO = 32, IO_DIRID = 48, IO_FLLGLEN = 54, IO_FLPYLEN = 58,
       IO_FLRLGLEN = 64, IO_FLRPYLEN = 68, IO_FLCRDAT = 72, IO_FLMDDAT = 76, IO_FLPARID = 100 };

/* ------------------------------------------------------------- helpers */
void repr_rel(const char *rel, char *out, int outmax) {
    char m[2048]; utf8_to_macroman(rel, m, sizeof m); py_repr_str(m, -1, out, outmax);
}
static void ost(uint32_t v, char *raw, char *u) { ostype_s(v, raw); macroman_to_utf8(raw, 4, u, 32); }
static int exists(World *w, const char *rel) { return file_exists(w, rel); }
static int isdir(World *w, const char *rel) { return is_dir(w, rel); }
static void hp_of(World *w, const char *rel, char *out) { host_path(w, rel, out, 2048); }
static void touch(const char *p) { FILE *f = u_fopen(p, "wb"); if (f) fclose(f); }
static long fsize(const char *p) { int64_t n; return u_stat(p, &n, NULL) == 0 ? (long)n : -1; }
/* a Mac file is deleted with both forks (the resource fork lives in '<name>.rsrc' on the host) */
static void del_forks(const char *hp) { char rp[2100]; u_remove(hp); snprintf(rp, sizeof rp, "%s.rsrc", hp); if (fsize(rp) >= 0) u_remove(rp); }
static int set_result(Emu *E, uint32_t pb, int err) { wr16(E, pb + IO_RESULT, (uint32_t)err); return err & 0xFFFF; }
static void makedirs(const char *p) {
    char t[2048]; snprintf(t, sizeof t, "%s", p);
    for (char *s = t + 1; *s; s++) if (*s == '/') { *s = 0; mkdir1(t); *s = '/'; }
    mkdir1(t);
}
static uint32_t crc32_s(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        c ^= *p; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
/* python: sorted(x for x in os.listdir(host(base)) if not x.endswith('.rsrc') and not x.startswith('.')) */
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char **list_dir(World *w, const char *rel, int *n) {
    char hp[2048]; hp_of(w, rel, hp); *n = 0;
    UDir *d = u_opendir(hp); if (!d) return NULL;
    char **v = NULL; int cap = 0; const char *hn;
    while ((hn = u_readdir(d))) {
        size_t hl = strlen(hn);
        if (hl >= 5 && !strcmp(hn + hl - 5, ".rsrc")) continue;
        char x[1024]; host_name_to_rel(hn, x, sizeof x);
        if (x[0] == '.') continue;
        if (*n == cap) { cap = cap * 2 + 16; v = realloc(v, cap * sizeof(char *)); }
        v[(*n)++] = strdup(x);
    }
    u_closedir(d);
    qsort(v, *n, sizeof(char *), cmpstr);    /* UTF-8 byte order == code point order */
    return v;
}
static void free_list(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }
static void pname(Emu *E, uint32_t p, char *out) { if (p) rd_pstr(E, p, out); else out[0] = 0; }
static void wr_finfo(Emu *E, uint32_t a, const char *t, const char *c, int fl) {
    emu_write(E, a, t, 4); emu_write(E, a + 4, c, 4);
    wr16(E, a + 8, (uint32_t)fl); wr16(E, a + 10, 0); wr16(E, a + 12, 0); wr16(E, a + 14, 0);
}
static void rd_finfo_set(Emu *E, const char *rel, uint32_t a) {
    char t[5], c[5]; emu_read(E, a, t, 4); t[4] = 0; emu_read(E, a + 4, c, 4); c[4] = 0;
    set_finfo(W, rel, t, c, rd16(E, a + 8));
}

/* ------------------------------------------------------------- FSSpec */
REG(FSMakeFSSpec) {
    int v = SA(E, 0); uint32_t d = A(E, 1), namep = A(E, 2), spec = A(E, 3);
    char name[300]; pname(E, namep, name);
    char rel[2048], par[2048], leaf[300]; world_resolve(W, v, d, name, rel, sizeof rel); parent_of(rel, par);
    uint32_t pid = rel[0] ? (uint32_t)dir_id_for(W, par) : 1;
    leaf_of(W, rel, leaf); write_fsspec(W, spec, W->vref, pid, leaf);
    int err = exists(W, rel) ? noErr : ((exists(W, par) || !par[0]) ? fnfErr : dirNFErr);
    char rn[1024], rr[4096]; py_repr_str(name, -1, rn, sizeof rn); repr_rel(rel, rr, sizeof rr);
    T(E, "FSMakeFSSpec", "%d %u %s -> %s err %d", v, d, rn, rr, err);
    return err & 0xFFFF;
}
static int64_t open_spec(Emu *E, int rsrc, const char *nm) {
    char rel[2048], rr[4096]; spec_rel(W, A(E, 0), rel, sizeof rel); int ref; int err = open_fork(W, rel, rsrc, &ref);
    wr16(E, A(E, 2), (uint32_t)ref); repr_rel(rel, rr, sizeof rr); T(E, nm, "%s -> %d err %d", rr, ref, err); return err & 0xFFFF;
}
REG(FSpOpenDF) { return open_spec(E, 0, "FSpOpenDF"); }
REG(FSpOpenRF) { return open_spec(E, 1, "FSpOpenRF"); }
REG(FSpCreate) {
    char rel[2048], rr[4096], par[2048], hp[2048]; spec_rel(W, A(E, 0), rel, sizeof rel); repr_rel(rel, rr, sizeof rr);
    char cre[5], typ[5], cu[32], tu[32]; ost(A(E, 1), cre, cu); ost(A(E, 2), typ, tu);
    if (exists(W, rel)) { T(E, "FSpCreate", "%s exists", rr); return dupFNErr & 0xFFFF; }
    parent_of(rel, par);
    if (!isdir(W, par) && par[0]) return dirNFErr & 0xFFFF;
    hp_of(W, rel, hp); touch(hp); set_finfo(W, rel, typ, cre, 0);
    T(E, "FSpCreate", "%s '%s' '%s'", rr, tu, cu); return 0;
}
REG(FSpDelete) {
    char rel[2048], rr[4096], hp[2048]; spec_rel(W, A(E, 0), rel, sizeof rel);
    if (!exists(W, rel)) return fnfErr & 0xFFFF;
    hp_of(W, rel, hp);
    if (isdir(W, rel)) u_rmdir(hp);
    else del_forks(hp);
    repr_rel(rel, rr, sizeof rr); T(E, "FSpDelete", "%s", rr); return 0;
}
REG(FSpGetFInfo) {
    char rel[2048]; spec_rel(W, A(E, 0), rel, sizeof rel);
    if (!exists(W, rel)) return fnfErr & 0xFFFF;
    char t[5], c[5]; int fl; get_finfo(W, rel, t, c, &fl); wr_finfo(E, A(E, 1), t, c, fl); return 0;
}
REG(FSpSetFInfo) {
    char rel[2048]; spec_rel(W, A(E, 0), rel, sizeof rel);
    if (!exists(W, rel)) return fnfErr & 0xFFFF;
    rd_finfo_set(E, rel, A(E, 1)); return 0;
}
REG(FSpOpenResFile) {
    char rel[2048], rr[4096]; spec_rel(W, A(E, 0), rel, sizeof rel); int ref = open_res_file(W, rel);
    repr_rel(rel, rr, sizeof rr); T(E, "FSpOpenResFile", "%s -> %d", rr, ref); return ref & 0xFFFF;
}
REG(FSpCreateResFile) {
    char rel[2048], rr[4096]; spec_rel(W, A(E, 0), rel, sizeof rel);
    if (!exists(W, rel)) { char t[5], c[5], u[32]; ost(A(E, 2), t, u); ost(A(E, 1), c, u); set_finfo(W, rel, t, c, 0); }
    create_res_file(W, rel); repr_rel(rel, rr, sizeof rr); T(E, "FSpCreateResFile", "%s", rr); return NORET;
}

/* ---------------------------------------------------------- H-calls */
static void hrel(Emu *E, int vi, int di, int ni, char *rel) {
    char n[300]; rd_pstr(E, A(E, ni), n);
    world_resolve(W, vi < 0 ? 0 : SA(E, vi), di < 0 ? 0 : A(E, di), n, rel, 2048);
}
REG(HOpenResFile) {
    char rel[2048], rr[4096]; hrel(E, 0, 1, 2, rel); int ref = open_res_file(W, rel);
    repr_rel(rel, rr, sizeof rr); T(E, "HOpenResFile", "%s -> %d", rr, ref); return ref & 0xFFFF;
}
REG(HCreateResFile) {
    char rel[2048], rr[4096]; hrel(E, 0, 1, 2, rel); create_res_file(W, rel);
    repr_rel(rel, rr, sizeof rr); T(E, "HCreateResFile", "%s", rr); return NORET;
}
REG(OpenResFile) {
    char rel[2048], rr[4096], n[300]; rd_pstr(E, A(E, 0), n); world_resolve(W, 0, 0, n, rel, sizeof rel);
    int ref = open_res_file(W, rel); repr_rel(rel, rr, sizeof rr); T(E, "OpenResFile", "%s -> %d", rr, ref); return ref & 0xFFFF;
}
static int64_t hopen(Emu *E, int rsrc, const char *nm) {
    char rel[2048], rr[4096]; hrel(E, 0, 1, 2, rel); int ref; int err = open_fork(W, rel, rsrc, &ref);
    wr16(E, A(E, 4), (uint32_t)ref); repr_rel(rel, rr, sizeof rr); T(E, nm, "%s -> %d err %d", rr, ref, err); return err & 0xFFFF;
}
REG(HOpen) { return hopen(E, 0, "HOpen"); }
REG(HOpenRF) { return hopen(E, 1, "HOpenRF"); }
REG(HCreate) {
    char rel[2048], rr[4096], hp[2048]; hrel(E, 0, 1, 2, rel);
    if (exists(W, rel)) return dupFNErr & 0xFFFF;
    hp_of(W, rel, hp); touch(hp); char t[5], c[5], u[32]; ost(A(E, 4), t, u); ost(A(E, 3), c, u); set_finfo(W, rel, t, c, 0);
    repr_rel(rel, rr, sizeof rr); T(E, "HCreate", "%s", rr); return 0;
}
REG(HDelete) {
    char rel[2048], rr[4096], hp[2048]; hrel(E, 0, 1, 2, rel);
    if (!exists(W, rel)) return fnfErr & 0xFFFF;
    hp_of(W, rel, hp); del_forks(hp); repr_rel(rel, rr, sizeof rr); T(E, "HDelete", "%s", rr); return 0;
}
REG(OpenDF) {
    char rel[2048], rr[4096], n[300]; rd_pstr(E, A(E, 0), n); world_resolve(W, SA(E, 1), 0, n, rel, sizeof rel);
    int ref; int err = open_fork(W, rel, 0, &ref);
    wr16(E, A(E, 2), (uint32_t)ref); repr_rel(rel, rr, sizeof rr); T(E, "OpenDF", "%s -> %d err %d", rr, ref, err); return err & 0xFFFF;
}
REG(OpenRF) {
    char rel[2048], n[300]; rd_pstr(E, A(E, 0), n); world_resolve(W, SA(E, 1), 0, n, rel, sizeof rel);
    int ref; int err = open_fork(W, rel, 1, &ref); wr16(E, A(E, 2), (uint32_t)ref); return err & 0xFFFF;
}
REG(Create) {
    char rel[2048], hp[2048], n[300]; rd_pstr(E, A(E, 0), n); world_resolve(W, SA(E, 1), 0, n, rel, sizeof rel);
    if (exists(W, rel)) return dupFNErr & 0xFFFF;
    hp_of(W, rel, hp); touch(hp); char t[5], c[5], u[32]; ost(A(E, 3), t, u); ost(A(E, 2), c, u); set_finfo(W, rel, t, c, 0); return 0;
}
REG(FSDelete) {
    char rel[2048], hp[2048], n[300]; rd_pstr(E, A(E, 0), n); world_resolve(W, SA(E, 1), 0, n, rel, sizeof rel);
    if (!exists(W, rel)) return fnfErr & 0xFFFF;
    hp_of(W, rel, hp); del_forks(hp); return 0;
}
REG(DirCreate) {
    char rel[2048], rr[4096], hp[2048]; hrel(E, 0, 1, 2, rel);
    if (exists(W, rel)) return dupFNErr & 0xFFFF;
    hp_of(W, rel, hp); makedirs(hp); wr32(E, A(E, 3), (uint32_t)dir_id_for(W, rel));
    repr_rel(rel, rr, sizeof rr); T(E, "DirCreate", "%s", rr); return 0;
}

/* ------------------------------------------------------------ fork I/O */
static FCB *fcb(Emu *E, int ref) { FCB *f = fcb_get(W, ref); if (!f) emu_fail(E, "bad refnum %d", ref); return f; }
static int do_read(Emu *E, int ref, uint32_t buf, uint32_t n, uint32_t *m) {
    FCB *f = fcb(E, ref); int64_t avail = (int64_t)f->data.n - f->pos; if (avail < 0) avail = 0;
    *m = (int64_t)n < avail ? n : (uint32_t)avail;
    if (*m) emu_write(E, buf, f->data.b + f->pos, *m);
    f->pos += *m; return *m == n ? noErr : eofErr;
}
static void data_resize(Bytes *d, size_t n) {
    if (n > d->cap) { d->cap = n * 2 + 64; d->b = realloc(d->b, d->cap); }
    if (n > d->n) memset(d->b + d->n, 0, n - d->n);
    d->n = n;
}
static int do_write(Emu *E, int ref, uint32_t buf, uint32_t n, uint32_t *m) {
    FCB *f = fcb(E, ref); uint32_t p = f->pos;
    if (p > f->data.n) data_resize(&f->data, p);
    if (p + (size_t)n > f->data.n) data_resize(&f->data, p + (size_t)n);
    emu_read(E, buf, f->data.b + p, n); f->pos = p + n; f->dirty = 1; *m = n; return noErr;
}
static int do_setpos(Emu *E, int ref, int mode, uint32_t offu) {
    FCB *f = fcb(E, ref); int64_t off = (int32_t)offu, np;
    mode &= 3;
    if (mode == 1) np = off; else if (mode == 2) np = (int64_t)f->data.n + off; else if (mode == 3) np = (int64_t)f->pos + off; else np = f->pos;
    if (np < 0) return posErr;
    if (np > (int64_t)f->data.n) { f->pos = (uint32_t)f->data.n; return eofErr; }
    f->pos = (uint32_t)np; return noErr;
}
REG(FSRead) { uint32_t cp = A(E, 1), m; int err = do_read(E, SA(E, 0), A(E, 2), rd32(E, cp), &m); wr32(E, cp, m); return err & 0xFFFF; }
REG(FSWrite) { uint32_t cp = A(E, 1), m; int err = do_write(E, SA(E, 0), A(E, 2), rd32(E, cp), &m); wr32(E, cp, m); return err & 0xFFFF; }
REG(FSClose) { int ref = SA(E, 0); T(E, "FSClose", "%d", ref); return close_fork(W, ref) & 0xFFFF; }
REG(GetEOF) { wr32(E, A(E, 1), (uint32_t)fcb(E, SA(E, 0))->data.n); return 0; }
REG(SetEOF) {
    FCB *f = fcb(E, SA(E, 0)); uint32_t n = A(E, 1); data_resize(&f->data, n);
    f->dirty = 1; if (f->pos > n) f->pos = n; return 0;
}
REG(GetFPos) { wr32(E, A(E, 1), fcb(E, SA(E, 0))->pos); return 0; }
REG(SetFPos) { return do_setpos(E, SA(E, 0), SA(E, 1), A(E, 2)) & 0xFFFF; }

/* ------------------------------------------------------- PB (sync) calls */
static void pb_name(Emu *E, uint32_t pb, char *out) { uint32_t p = rd32(E, pb + IO_NAMEPTR); pname(E, p, out); }
static void pb_rel(Emu *E, uint32_t pb, char *rel) {
    char n[300]; pb_name(E, pb, n); world_resolve(W, rds16(E, pb + IO_VREFNUM), rd32(E, pb + IO_DIRID), n, rel, 2048);
}
REG(PBReadSync) {
    uint32_t pb = A(E, 0); int ref = rds16(E, pb + IO_REFNUM), m_err;
    int err = do_setpos(E, ref, rds16(E, pb + IO_POSMODE), rd32(E, pb + IO_POSOFFSET)); uint32_t m;
    m_err = do_read(E, ref, rd32(E, pb + IO_BUFFER), rd32(E, pb + IO_REQCOUNT), &m);
    wr32(E, pb + IO_ACTCOUNT, m); wr32(E, pb + IO_POSOFFSET, fcb(E, ref)->pos);
    return set_result(E, pb, err ? err : m_err);
}
REG(PBWriteSync) {
    uint32_t pb = A(E, 0); int ref = rds16(E, pb + IO_REFNUM);
    int err = do_setpos(E, ref, rds16(E, pb + IO_POSMODE), rd32(E, pb + IO_POSOFFSET));
    if (err == eofErr) {
        FCB *f = fcb(E, ref); uint32_t off = rd32(E, pb + IO_POSOFFSET);
        if (off > f->data.n) data_resize(&f->data, off);
        f->pos = off; err = 0;
    }
    uint32_t m; int e2 = do_write(E, ref, rd32(E, pb + IO_BUFFER), rd32(E, pb + IO_REQCOUNT), &m);
    wr32(E, pb + IO_ACTCOUNT, m); wr32(E, pb + IO_POSOFFSET, fcb(E, ref)->pos);
    return set_result(E, pb, err ? err : e2);
}
REG(PBWriteAsync) {
    int64_t r = h_PBWriteSync(E); uint32_t pb = A(E, 0);
    if (rd32(E, pb + IO_COMPLETION)) emu_fail(E, "PBWriteAsync with completion routine not modelled");
    return r;
}
REG(PBCloseSync) { uint32_t pb = A(E, 0); return set_result(E, pb, close_fork(W, rds16(E, pb + IO_REFNUM))); }
REG(PBGetEOFSync) { uint32_t pb = A(E, 0); wr32(E, pb + IO_MISC, (uint32_t)fcb(E, rds16(E, pb + IO_REFNUM))->data.n); return set_result(E, pb, 0); }
REG(PBSetEOFSync) {
    uint32_t pb = A(E, 0); FCB *f = fcb(E, rds16(E, pb + IO_REFNUM)); data_resize(&f->data, rd32(E, pb + IO_MISC));
    f->dirty = 1; return set_result(E, pb, 0);
}
REG(PBGetFPosSync) {
    uint32_t pb = A(E, 0); FCB *f = fcb(E, rds16(E, pb + IO_REFNUM));
    wr32(E, pb + IO_POSOFFSET, f->pos); wr32(E, pb + IO_REQCOUNT, 0); wr32(E, pb + IO_ACTCOUNT, 0); wr16(E, pb + IO_POSMODE, 0);
    return set_result(E, pb, 0);
}
REG(PBSetFPosSync) {
    uint32_t pb = A(E, 0);
    return set_result(E, pb, do_setpos(E, rds16(E, pb + IO_REFNUM), rds16(E, pb + IO_POSMODE), rd32(E, pb + IO_POSOFFSET)));
}
REG(PBFlushVolSync) { return set_result(E, A(E, 0), 0); }
static int64_t pbhopen(Emu *E, int rsrc, const char *nm) {
    uint32_t pb = A(E, 0); char rel[2048], rr[4096]; pb_rel(E, pb, rel);
    int ref; int err = open_fork(W, rel, rsrc, &ref); wr16(E, pb + IO_REFNUM, (uint32_t)ref);
    repr_rel(rel, rr, sizeof rr); T(E, nm, "%s -> %d err %d", rr, ref, err); return set_result(E, pb, err);
}
REG(PBHOpenDFSync) { return pbhopen(E, 0, "PBHOpenDF"); }
REG(PBHOpenRFSync) { return pbhopen(E, 1, "PBHOpenRF"); }
REG(PBHCreateSync) {
    uint32_t pb = A(E, 0); char rel[2048], rr[4096], hp[2048]; pb_rel(E, pb, rel);
    if (exists(W, rel)) return set_result(E, pb, dupFNErr);
    hp_of(W, rel, hp); touch(hp); set_finfo(W, rel, "????", "????", 0);
    repr_rel(rel, rr, sizeof rr); T(E, "PBHCreate", "%s", rr); return set_result(E, pb, 0);
}
REG(PBHDeleteSync) {
    uint32_t pb = A(E, 0); char rel[2048], rr[4096], hp[2048]; pb_rel(E, pb, rel);
    if (!exists(W, rel)) return set_result(E, pb, fnfErr);
    hp_of(W, rel, hp); if (isdir(W, rel)) u_rmdir(hp); else del_forks(hp);
    repr_rel(rel, rr, sizeof rr); T(E, "PBHDelete", "%s", rr); return set_result(E, pb, 0);
}
REG(PBRenameSync) {
    uint32_t pb = A(E, 0); char n[300], nn[300], rel[2048], dst[2048], hs[2048], hd[2048];
    pb_name(E, pb, n); world_resolve(W, rds16(E, pb + IO_VREFNUM), 0, n, rel, sizeof rel);
    rd_pstr(E, rd32(E, pb + IO_MISC), nn); world_resolve(W, rds16(E, pb + IO_VREFNUM), 0, nn, dst, sizeof dst);
    hp_of(W, rel, hs); hp_of(W, dst, hd);
    if (u_rename(hs, hd)) emu_fail(E, "PBRename %s -> %s: %s", hs, hd, strerror(errno));
    return set_result(E, pb, 0);
}
static void fill_finfo(Emu *E, uint32_t pb, const char *rel, int cat) {
    char t[5], c[5], hp[2048], rp[2100]; int fl; get_finfo(W, rel, t, c, &fl); hp_of(W, rel, hp);
    wr8(E, pb + IO_FLATTRIB, 0); wr_finfo(E, pb + IO_FLFNDRINFO, t, c, fl);
    long dl = fsize(hp); snprintf(rp, sizeof rp, "%s.rsrc", hp); long rl = fsize(rp); if (rl < 0) rl = 0;
    if (dl < 0) emu_fail(E, "fill_finfo: no such file %s", hp);
    if (!cat) wr32(E, pb + IO_DIRID, 0);
    wr32(E, pb + IO_FLLGLEN, (uint32_t)dl); wr32(E, pb + IO_FLPYLEN, (uint32_t)((dl + 511) & ~511L));
    wr32(E, pb + IO_FLRLGLEN, (uint32_t)rl); wr32(E, pb + IO_FLRPYLEN, (uint32_t)((rl + 511) & ~511L));
    wr32(E, pb + IO_FLCRDAT, now_secs(W)); wr32(E, pb + IO_FLMDDAT, now_secs(W));
}
REG(PBHGetFInfoSync) {
    uint32_t pb = A(E, 0); int idx = rds16(E, pb + IO_FDIRINDEX); char rel[2048]; pb_rel(E, pb, rel);
    if (idx > 0) emu_fail(E, "indexed PBHGetFInfo not modelled");
    if (!exists(W, rel) || isdir(W, rel)) return set_result(E, pb, fnfErr);
    fill_finfo(E, pb, rel, 0); return set_result(E, pb, 0);
}
REG(PBHSetFInfoSync) {
    uint32_t pb = A(E, 0); char rel[2048]; pb_rel(E, pb, rel);
    if (!exists(W, rel)) return set_result(E, pb, fnfErr);
    rd_finfo_set(E, rel, pb + IO_FLFNDRINFO); return set_result(E, pb, 0);
}
REG(PBGetCatInfoSync) {
    uint32_t pb = A(E, 0); int idx = rds16(E, pb + IO_FDIRINDEX), v = rds16(E, pb + IO_VREFNUM);
    uint32_t d = rd32(E, pb + IO_DIRID), namep = rd32(E, pb + IO_NAMEPTR);
    char rel[2048], par[2048], rr[4096];
    if (idx > 0) {
        const char *base = dir_rel(W, (int)d); if (!base) base = "";
        int n; char **ents = list_dir(W, base, &n);
        if (idx > n) { free_list(ents, n); return set_result(E, pb, fnfErr); }
        const char *ent = ents[idx - 1];
        char nm[1024]; int k = 0;
        for (const char *q = ent; *q && k < 1000;) { if (!strncmp(q, "%2F", 3)) { nm[k++] = '/'; q += 3; } else nm[k++] = *q++; }
        nm[k] = 0;
        if (base[0]) snprintf(rel, sizeof rel, "%s/%s", base, ent); else snprintf(rel, sizeof rel, "%s", ent);
        if (namep) { char m[1024]; utf8_to_macroman(nm, m, sizeof m); wr_pstr(E, namep, m, -1); }
        free_list(ents, n);
    } else if (idx == 0) {
        char n[300]; pname(E, namep, n); world_resolve(W, v, d, n, rel, sizeof rel);
    } else {
        const char *b = dir_rel(W, (int)d); snprintf(rel, sizeof rel, "%s", b ? b : "");
        if (namep) { char lf[300]; leaf_of(W, rel, lf); wr_pstr(E, namep, lf, -1); }
    }
    if (!exists(W, rel) && rel[0]) return set_result(E, pb, fnfErr);
    parent_of(rel, par);
    if (isdir(W, rel) || !rel[0]) {
        wr8(E, pb + IO_FLATTRIB, 0x10); wr32(E, pb + IO_DIRID, rel[0] ? (uint32_t)dir_id_for(W, rel) : 2);
        int n; char **ents = list_dir(W, rel, &n); free_list(ents, n);
        wr16(E, pb + 52, (uint32_t)n);
        wr32(E, pb + IO_FLPARID, rel[0] ? (uint32_t)dir_id_for(W, par) : 1);
    } else {
        fill_finfo(E, pb, rel, 1); wr32(E, pb + IO_DIRID, 0x1000 + (crc32_s(rel) & 0xFFFF));
        wr32(E, pb + IO_FLPARID, (uint32_t)dir_id_for(W, par));
    }
    repr_rel(rel, rr, sizeof rr); T(E, "PBGetCatInfo", "idx %d %s", idx, rr);
    return set_result(E, pb, 0);
}
REG(PBDirCreateSync) {
    uint32_t pb = A(E, 0); char rel[2048], rr[4096], hp[2048]; pb_rel(E, pb, rel);
    if (exists(W, rel)) return set_result(E, pb, dupFNErr);
    hp_of(W, rel, hp); makedirs(hp); wr32(E, pb + IO_DIRID, (uint32_t)dir_id_for(W, rel));
    repr_rel(rel, rr, sizeof rr); T(E, "PBDirCreate", "%s", rr); return set_result(E, pb, 0);
}
static void fill_vinfo(Emu *E, uint32_t pb) {
    if (rd32(E, pb + IO_NAMEPTR)) wr_pstr(E, rd32(E, pb + IO_NAMEPTR), W->volname, -1);
    wr16(E, pb + IO_VREFNUM, (uint32_t)W->vref);
    wr32(E, pb + 30, now_secs(W)); wr32(E, pb + 34, now_secs(W)); wr16(E, pb + 38, 0);
    wr16(E, pb + 40, 10); wr16(E, pb + 46, 0xFFFF); wr32(E, pb + 48, 32768); wr32(E, pb + 52, 32768);
    wr16(E, pb + 56, 0); wr32(E, pb + 58, 10000); wr16(E, pb + 62, 0x8000);
}
REG(PBHGetVInfoSync) { uint32_t pb = A(E, 0); fill_vinfo(E, pb); return set_result(E, pb, 0); }
REG(GetVInfo) {
    if (A(E, 1)) wr_pstr(E, A(E, 1), W->volname, -1);
    wr16(E, A(E, 2), (uint32_t)W->vref); wr32(E, A(E, 3), 1u << 30); return 0;
}
REG(PBHGetVolSync) {
    uint32_t pb = A(E, 0);
    wr16(E, pb + IO_VREFNUM, (uint32_t)W->vref); wr32(E, pb + 28, (uint32_t)W->app_dir_id); wr16(E, pb + 32, (uint32_t)W->vref);
    wr32(E, pb + IO_DIRID, (uint32_t)W->app_dir_id);
    if (rd32(E, pb + IO_NAMEPTR)) wr_pstr(E, rd32(E, pb + IO_NAMEPTR), W->volname, -1);
    return set_result(E, pb, 0);
}
REG(PBHSetVolSync) { uint32_t pb = A(E, 0); W->wd_dir = (int)rd32(E, pb + IO_DIRID); return set_result(E, pb, 0); }
REG(HGetVol) {
    if (A(E, 0)) wr_pstr(E, A(E, 0), W->volname, -1);
    wr16(E, A(E, 1), (uint32_t)W->vref); wr32(E, A(E, 2), (uint32_t)(W->wd_dir ? W->wd_dir : W->app_dir_id)); return 0;
}
REG(HSetVol) { if (A(E, 2)) W->wd_dir = (int)A(E, 2); return 0; }
REG(GetVol) { if (A(E, 0)) wr_pstr(E, A(E, 0), W->volname, -1); wr16(E, A(E, 1), (uint32_t)W->vref); return 0; }
REG(zero) { (void)E; return 0; }
REG(GetWDInfo) { wr16(E, A(E, 1), (uint32_t)W->vref); wr32(E, A(E, 2), (uint32_t)W->app_dir_id); wr32(E, A(E, 3), 0); return 0; }
REG(OpenWD) { wr16(E, A(E, 3), (uint32_t)W->vref); return 0; }
REG(PBGetFCBInfoSync) {
    uint32_t pb = A(E, 0); FCB *f = fcb_get(W, rds16(E, pb + IO_REFNUM));
    if (!f) return set_result(E, pb, -51);
    char par[2048]; parent_of(f->rel, par);
    if (rd32(E, pb + IO_NAMEPTR)) { char lf[300]; leaf_of(W, f->rel, lf); wr_pstr(E, rd32(E, pb + IO_NAMEPTR), lf, -1); }
    wr32(E, pb + 32, (uint32_t)dir_id_for(W, par));
    wr32(E, pb + 58, (uint32_t)dir_id_for(W, par));
    wr16(E, pb + 52, (uint32_t)W->vref);
    wr32(E, pb + 36, (uint32_t)f->data.n); wr32(E, pb + 44, f->pos);
    return set_result(E, pb, 0);
}
REG(PBControlSync) { return set_result(E, A(E, 0), -18); }
REG(ResolveAliasFile) {
    wr8(E, A(E, 2), 0); wr8(E, A(E, 3), 0);
    char rel[2048]; spec_rel(W, A(E, 0), rel, sizeof rel); return exists(W, rel) ? 0 : (fnfErr & 0xFFFF);
}

/* ---------------------------------------------------------- Folder Mgr */
static const struct { const char *sel, *rel; } folders[] = {
    { "pref", "System Folder/Preferences" }, { "temp", "Temporary Items" }, { "macs", "System Folder" },
    { "desk", "Desktop Folder" }, { "trsh", "Trash" }, { "extn", "System Folder/Extensions" }, { "ctrl", "System Folder/Control Panels" } };
REG(FindFolder) {
    char raw[5], sel[32]; ost(A(E, 1), raw, sel); const char *rel = NULL;
    for (size_t i = 0; i < sizeof folders / sizeof *folders; i++) if (!strcmp(folders[i].sel, sel)) rel = folders[i].rel;
    if (!rel) { T(E, "FindFolder", "'%s' unknown", sel); return fnfErr & 0xFFFF; }
    char hp[2048], rr[4096]; hp_of(W, rel, hp); makedirs(hp);
    wr16(E, A(E, 3), (uint32_t)W->vref); wr32(E, A(E, 4), (uint32_t)dir_id_for(W, rel));
    repr_rel(rel, rr, sizeof rr); T(E, "FindFolder", "'%s' -> %s", sel, rr); return 0;
}
REG(LMGetSFSaveDisk) { return (-W->vref) & 0xFFFF; }
REG(LMGetCurDirStore) { return (uint32_t)W->app_dir_id; }
REG(SFPutFile) { wr8(E, A(E, 4), 0); return NORET; }
REG(StandardPutFile) { wr8(E, A(E, 2), 0); return NORET; }
REG(StandardGetFile) { wr8(E, A(E, 4), 0); return NORET; }

void hle_files_register(void) {
#define R(n) emu_reg(#n, h_##n)
    R(FSMakeFSSpec); R(FSpOpenDF); R(FSpOpenRF); R(FSpCreate); R(FSpDelete); R(FSpGetFInfo); R(FSpSetFInfo);
    R(FSpOpenResFile); R(FSpCreateResFile);
    R(HOpenResFile); R(HCreateResFile); R(OpenResFile); R(HOpen); emu_reg("HOpenDF", h_HOpen); R(HOpenRF); R(HCreate); R(HDelete);
    R(OpenDF); R(OpenRF); R(Create); R(FSDelete); R(DirCreate);
    R(FSRead); R(FSWrite); R(FSClose); R(GetEOF); R(SetEOF); R(GetFPos); R(SetFPos); emu_reg("Allocate", h_zero);
    R(PBReadSync); emu_reg("PBReadAsync", h_PBReadSync); R(PBWriteSync); R(PBWriteAsync); R(PBCloseSync); R(PBGetEOFSync);
    R(PBSetEOFSync); R(PBGetFPosSync); R(PBSetFPosSync); R(PBFlushVolSync);
    R(PBHOpenDFSync); emu_reg("PBHOpenSync", h_PBHOpenDFSync); R(PBHOpenRFSync); R(PBHCreateSync);
    R(PBHDeleteSync); emu_reg("PBDeleteSync", h_PBHDeleteSync); R(PBRenameSync);
    R(PBHGetFInfoSync); R(PBHSetFInfoSync); R(PBGetCatInfoSync); R(PBDirCreateSync);
    R(PBHGetVInfoSync); emu_reg("PBGetVInfoSync", h_PBHGetVInfoSync); R(GetVInfo); R(PBHGetVolSync); R(PBHSetVolSync);
    R(HGetVol); R(HSetVol); R(GetVol); emu_reg("SetVol", h_zero); emu_reg("FlushVol", h_zero); R(GetWDInfo); R(OpenWD);
    R(PBGetFCBInfoSync); R(PBControlSync); emu_reg("PBStatusSync", h_PBControlSync);
    emu_reg("Eject", h_zero); emu_reg("UnmountVol", h_zero); emu_reg("DIBadMount", h_zero); R(ResolveAliasFile);
    R(FindFolder); R(LMGetSFSaveDisk); R(LMGetCurDirStore); R(SFPutFile); R(StandardPutFile); R(StandardGetFile);
#undef R
}
