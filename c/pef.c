/* pef.c - PEF loader: sections, pattern-initialised data, relocation VM, exports, traceback names.
 * Faithful port of scripts/pef.py (the verified Python reference). */
#include "pef.h"
#include "fsu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

typedef struct { uint8_t *b; size_t n, cap; } Buf;
static void bput(Buf *b, const uint8_t *s, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->b = realloc(b->b, b->cap); }
    if (s) memcpy(b->b + b->n, s, n); else memset(b->b + b->n, 0, n);
    b->n += n;
}

static int unpack_pidata(const uint8_t *p, size_t plen, uint32_t total, uint8_t **out) {
    Buf o = {0}; size_t i = 0;
#define ARG(v) do { v = 0; for (;;) { if (i >= plen) goto bad; uint8_t _b = p[i++]; v = (v << 7) | (_b & 0x7F); if (!(_b & 0x80)) break; } } while (0)
    while (i < plen) {
        uint8_t b = p[i++]; int op = b >> 5; uint32_t cnt = b & 0x1F;
        if (cnt == 0) ARG(cnt);
        if (op == 0) bput(&o, NULL, cnt);
        else if (op == 1) { if (i + cnt > plen) goto bad; bput(&o, p + i, cnt); i += cnt; }
        else if (op == 2) { uint32_t rc; ARG(rc); if (i + cnt > plen) goto bad; for (uint32_t k = 0; k <= rc; k++) bput(&o, p + i, cnt); i += cnt; }
        else if (op == 3) {
            uint32_t cs = cnt, cu, rc; ARG(cu); ARG(rc); const uint8_t *common = p + i; i += cs;
            for (uint32_t k = 0; k < rc; k++) { bput(&o, common, cs); bput(&o, p + i, cu); i += cu; }
            bput(&o, common, cs);
        } else if (op == 4) {
            uint32_t cs = cnt, cu, rc; ARG(cu); ARG(rc);
            for (uint32_t k = 0; k < rc; k++) { bput(&o, NULL, cs); bput(&o, p + i, cu); i += cu; }
            bput(&o, NULL, cs);
        } else goto bad;
    }
    if (o.n > total) goto bad;
    if (o.n < total) bput(&o, NULL, total - o.n);
    *out = o.b; return 0;
bad:
    free(o.b); return -1;
#undef ARG
}

/* relocation bookkeeping: one slot per data word */
typedef struct { PefReloc *v; uint32_t n, cap, seq; uint32_t *order; } RelTab;   /* append-only, deduped later */
static void rel_add(RelTab *t, uint32_t off, char kind, uint32_t val) {
    if (t->n == t->cap) { t->cap = t->cap * 2 + 256; t->v = realloc(t->v, t->cap * sizeof(PefReloc)); t->order = realloc(t->order, t->cap * 4); }
    t->v[t->n].off = off; t->v[t->n].kind = kind; t->v[t->n].val = val; t->order[t->n] = t->n; t->n++;
}

typedef struct {
    Pef *P; const uint16_t *ins; uint32_t nins; RelTab *rt;
    uint32_t r, imp; int C, D; int err;
} RelVM;

static void add_sec(RelVM *vm, int sec) {
    uint32_t off = vm->r;
    if (off + 4 > vm->P->data_len) { vm->err = 1; return; }
    rel_add(vm->rt, off, (sec == vm->P->code_idx) ? 'C' : 'D', be32(vm->P->data + off));
    vm->r += 4;
}
static void add_imp(RelVM *vm, uint32_t i) {
    uint32_t off = vm->r;
    if (off + 4 > vm->P->data_len) { vm->err = 1; return; }
    rel_add(vm->rt, off, 'I', i); vm->r += 4;
}
static int execute(RelVM *vm, uint32_t pc) {
    uint16_t op = vm->ins[pc];
    if ((op >> 14) == 0) {                          /* RelocBySectDWithSkip */
        uint32_t skip = (op >> 6) & 0xFF, rc = op & 0x3F;
        vm->r += 4 * skip; for (uint32_t k = 0; k < rc; k++) add_sec(vm, vm->D);
        return 1;
    }
    if ((op >> 13) == 2) {
        int sub = (op >> 9) & 0xF; uint32_t n = (op & 0x1FF) + 1;
        for (uint32_t k = 0; k < n; k++) {
            switch (sub) {
            case 0: add_sec(vm, vm->C); break;
            case 1: add_sec(vm, vm->D); break;
            case 2: add_sec(vm, vm->C); add_sec(vm, vm->D); vm->r += 4; break;
            case 3: add_sec(vm, vm->C); add_sec(vm, vm->D); break;
            case 4: add_sec(vm, vm->D); vm->r += 4; break;
            case 5: add_imp(vm, vm->imp); vm->imp++; break;
            default: vm->err = 1; return 1;
            }
        }
        return 1;
    }
    if ((op >> 13) == 3) {
        int sub = (op >> 9) & 0xF; uint32_t idx = op & 0x1FF;
        if (sub == 0) { add_imp(vm, idx); vm->imp = idx + 1; }
        else if (sub == 1) vm->C = idx;
        else if (sub == 2) vm->D = idx;
        else if (sub == 3) add_sec(vm, idx);
        else vm->err = 1;
        return 1;
    }
    if ((op >> 12) == 8) { vm->r += (op & 0xFFF) + 1; return 1; }
    if ((op >> 12) == 9) {
        uint32_t blk = ((op >> 8) & 0xF) + 1, rep = (op & 0xFF) + 1;
        if (blk > pc) { vm->err = 1; return 1; }
        for (uint32_t k = 0; k < rep; k++) for (uint32_t q = pc - blk; q < pc; q++) execute(vm, q);
        return 1;
    }
    if (pc + 1 >= vm->nins) { vm->err = 1; return 1; }
    if ((op >> 10) == 0x28) { vm->r = ((uint32_t)(op & 0x3FF) << 16) | vm->ins[pc + 1]; return 2; }
    if ((op >> 10) == 0x29) { uint32_t i = ((uint32_t)(op & 0x3FF) << 16) | vm->ins[pc + 1]; add_imp(vm, i); vm->imp = i + 1; return 2; }
    if ((op >> 10) == 0x2D) {
        int sub = (op >> 6) & 0xF; uint32_t idx = ((uint32_t)(op & 0x3F) << 16) | vm->ins[pc + 1];
        if (sub == 0) add_sec(vm, idx); else if (sub == 1) vm->C = idx; else if (sub == 2) vm->D = idx; else vm->err = 1;
        return 2;
    }
    vm->err = 1; return 1;   /* RelocLgRepeat (0x2C) is not used by these binaries */
}

static int cmp_reloc(const void *a, const void *b) {
    const PefReloc *x = a, *y = b; return x->off < y->off ? -1 : x->off > y->off;
}

Pef *pef_load(const char *path, char *err, size_t errlen) {
    FILE *f = u_fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "cannot open %s", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != (size_t)n) { fclose(f); free(d); snprintf(err, errlen, "read error"); return NULL; }
    fclose(f);
    if (n < 40 || memcmp(d, "Joy!peffpwpc", 12)) { free(d); snprintf(err, errlen, "not a PowerPC PEF"); return NULL; }
    Pef *P = calloc(1, sizeof *P); P->code_idx = P->data_idx = -1;
    int nsec = be16(d + 0x20); const uint8_t *loader = NULL; uint32_t loader_len = 0;
    for (int i = 0; i < nsec; i++) {
        const uint8_t *s = d + 40 + 28 * i;
        uint32_t total = be32(s + 8), packed = be32(s + 16), off = be32(s + 20); int kind = s[24];
        const uint8_t *raw = d + off;
        if (kind == 0) {
            P->code = calloc(1, total + 64); memcpy(P->code, raw, packed < total ? packed : total); P->code_len = total; P->code_idx = i;
        } else if (kind == 1) {
            P->data = calloc(1, total + 64); memcpy(P->data, raw, packed < total ? packed : total); P->data_len = total; P->data_idx = i;
        } else if (kind == 2) {
            uint8_t *u; if (unpack_pidata(raw, packed, total, &u)) { snprintf(err, errlen, "bad pidata"); free(d); pef_free(P); return NULL; }
            P->data = realloc(u, total + 64); memset(P->data + total, 0, 64); P->data_len = total; P->data_idx = i;
        } else if (kind == 4) { loader = raw; loader_len = packed; }
    }
    if (!loader || P->code_idx < 0 || P->data_idx < 0) { snprintf(err, errlen, "missing sections"); free(d); pef_free(P); return NULL; }
    (void)loader_len;
    /* ---- loader header */
    const uint8_t *L = loader;
    P->main_sec = (int32_t)be32(L); P->main_off = be32(L + 4);
    uint32_t nlib = be32(L + 24), nimp = be32(L + 28), nrelsec = be32(L + 32), relInstrOff = be32(L + 36),
             strOff = be32(L + 40), hashOff = be32(L + 44), hashPow = be32(L + 48), nexp = be32(L + 52);
    uint32_t o = 56;
    uint32_t *libfirst = calloc(nlib + 1, 4), *libcnt = calloc(nlib + 1, 4);
    P->libs = calloc(nlib + 1, sizeof(char *)); P->nlibs = nlib;
    for (uint32_t i = 0; i < nlib; i++) {
        P->libs[i] = strdup((const char *)L + strOff + be32(L + o));
        libcnt[i] = be32(L + o + 12); libfirst[i] = be32(L + o + 16); o += 24;
    }
    P->imports = calloc(nimp + 1, sizeof(PefImport)); P->nimports = nimp;
    for (uint32_t i = 0; i < nimp; i++) {
        uint32_t w = be32(L + o); o += 4;
        P->imports[i].name = strdup((const char *)L + strOff + (w & 0xFFFFFF));
        P->imports[i].cls = (w >> 24) & 0xF; P->imports[i].lib = -1;
        for (uint32_t k = 0; k < nlib; k++) if (libfirst[k] <= i && i < libfirst[k] + libcnt[k]) { P->imports[i].lib = k; break; }
    }
    free(libfirst); free(libcnt);
    /* ---- relocations */
    RelTab rt = {0};
    for (uint32_t s = 0; s < nrelsec; s++) {
        const uint8_t *h = L + o; o += 12;
        int sidx = be16(h); uint32_t cnt = be32(h + 4), first = be32(h + 8);
        if (sidx != P->data_idx) { snprintf(err, errlen, "unexpected relocation section"); free(d); pef_free(P); return NULL; }
        uint16_t *ins = malloc(cnt * 2 + 2);
        for (uint32_t k = 0; k < cnt; k++) ins[k] = be16(L + relInstrOff + first + 2 * k);
        RelVM vm = { P, ins, cnt, &rt, 0, 0, P->code_idx, P->data_idx, 0 };
        uint32_t pc = 0;
        while (pc < cnt && !vm.err) pc += execute(&vm, pc);
        free(ins);
        if (vm.err) { snprintf(err, errlen, "relocation error"); free(d); pef_free(P); return NULL; }
    }
    /* python dict semantics: a later relocation of the same offset replaces the earlier one */
    P->relocs = calloc(rt.n + 1, sizeof(PefReloc)); P->nrelocs = 0;
    {   /* stable sort by offset (insertion order kept for equal offsets), then keep the last of each run */
        uint32_t *idx = rt.order; PefReloc *v = rt.v;
        /* simple merge sort on indices */
        uint32_t *tmp = malloc((rt.n + 1) * 4);
        for (uint32_t w = 1; w < rt.n; w *= 2)
            for (uint32_t lo = 0; lo < rt.n; lo += 2 * w) {
                uint32_t mid = lo + w < rt.n ? lo + w : rt.n, hi = lo + 2 * w < rt.n ? lo + 2 * w : rt.n, a = lo, b = mid, k = lo;
                while (a < mid && b < hi) tmp[k++] = (v[idx[b]].off < v[idx[a]].off) ? idx[b++] : idx[a++];
                while (a < mid) tmp[k++] = idx[a++];
                while (b < hi) tmp[k++] = idx[b++];
                memcpy(idx + lo, tmp + lo, (hi - lo) * 4);
            }
        free(tmp);
        for (uint32_t k = 0; k < rt.n; k++) {
            PefReloc *r = &v[idx[k]];
            if (P->nrelocs && P->relocs[P->nrelocs - 1].off == r->off) P->relocs[P->nrelocs - 1] = *r;
            else P->relocs[P->nrelocs++] = *r;
        }
    }
    free(rt.v); free(rt.order);
    qsort(P->relocs, P->nrelocs, sizeof(PefReloc), cmp_reloc);
    /* ---- exports and function names (same precedence as pef.py) */
    const char **fn = calloc(P->code_len / 4 + 2, sizeof(char *));
    uint32_t keyOff = hashOff + (1u << hashPow) * 4, symOff = keyOff + nexp * 4;
    P->exports = calloc(nexp + 1, sizeof(PefExport)); P->nexports = nexp;
    int have_toc = 0;
    for (uint32_t i = 0; i < nexp; i++) {
        uint32_t key = be32(L + keyOff + 4 * i), nlen = key >> 16;
        const uint8_t *sy = L + symOff + 10 * i;
        uint32_t cn = be32(sy), val = be32(sy + 4); int sec = (int16_t)be16(sy + 8);
        char *name = malloc(nlen + 1); memcpy(name, L + strOff + (cn & 0xFFFFFF), nlen); name[nlen] = 0;
        PefExport *e = &P->exports[i]; e->name = name; e->cls = cn >> 24; e->val = val; e->sec = sec;
        if (sec == P->data_idx) {
            if ((e->cls & 0xF) == 2) {
                const PefReloc *r = pef_reloc_at(P, val);
                if (r && r->kind == 'C') {
                    if (r->val / 4 <= P->code_len / 4 && (r->val & 3) == 0) fn[r->val / 4] = name;
                    const PefReloc *t = pef_reloc_at(P, val + 4);
                    if (t && t->kind == 'D' && !have_toc) { P->toc = t->val; have_toc = 1; }
                }
            }
        } else if (sec == P->code_idx) {
            if ((val & 3) == 0 && val / 4 <= P->code_len / 4 && !fn[val / 4]) fn[val / 4] = name;
        }
    }
    if (!have_toc && P->main_sec == P->data_idx) {
        const PefReloc *t = pef_reloc_at(P, P->main_off + 4);
        if (t) { P->toc = t->val; have_toc = 1; }
    }
    /* ---- AIX traceback tables */
    uint32_t nw = P->code_len / 4; const uint8_t *c = P->code;
    const char **tb = calloc(nw + 2, sizeof(char *));
    for (uint32_t i = 1; i + 6 < nw; i++) {
        if (be32(c + 4 * i) != 0) continue;
        uint32_t prev = be32(c + 4 * i - 4);
        if (prev != 0x4E800020 && prev != 0x4E800420) continue;
        const uint8_t *b = c + 4 * i + 4;
        if (b[0] != 0) continue;
        int has_tboff = b[2] & 0x20, int_proc = b[2] & 0x10, has_ctl = b[2] & 0x08, name_present = b[3] & 0x40;
        uint32_t p = 4 * i + 12;
        if (b[6] + (b[7] >> 1) > 0) p += 4;
        uint32_t tboff = 0; int have_tboff = 0;
        if (has_tboff) { if (p + 4 > P->code_len) continue; tboff = be32(c + p); p += 4; have_tboff = 1; }
        if (int_proc) p += 4;
        if (has_ctl) { if (p + 4 > P->code_len) continue; uint32_t ncl = be32(c + p); p += 4 + 4 * ncl; }
        if (name_present && have_tboff && p + 2 <= P->code_len) {
            uint32_t ln = be16(c + p);
            if (ln > 0 && ln < 300 && p + 2 + ln <= P->code_len) {
                int ok = 1; for (uint32_t k = 0; k < ln; k++) if (c[p + 2 + k] < 32 || c[p + 2 + k] >= 127) { ok = 0; break; }
                uint32_t start = 4 * i - tboff;
                if (ok && start < P->code_len && (start & 3) == 0) {
                    char *nm = malloc(ln + 1); memcpy(nm, c + p + 2, ln); nm[ln] = 0; tb[start / 4] = nm;
                }
            }
        }
    }
    for (uint32_t i = 0; i <= nw; i++) if (tb[i] && !fn[i]) fn[i] = tb[i];
    int nf = 0; for (uint32_t i = 0; i <= nw; i++) if (fn[i]) nf++;
    P->funcs = calloc(nf + 1, sizeof(PefFunc)); P->nfuncs = 0;
    for (uint32_t i = 0; i <= nw; i++) if (fn[i]) { P->funcs[P->nfuncs].addr = i * 4; P->funcs[P->nfuncs].name = fn[i]; P->nfuncs++; }
    free(fn); free(tb);   /* names themselves stay allocated for the program's lifetime */
    free(d);
    return P;
}

const PefReloc *pef_reloc_at(const Pef *p, uint32_t off) {
    int lo = 0, hi = p->nrelocs - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (p->relocs[m].off == off) return &p->relocs[m];
        if (p->relocs[m].off < off) lo = m + 1; else hi = m - 1;
    }
    return NULL;
}

const char *pef_owner(const Pef *p, uint32_t a, uint32_t *start) {
    int lo = 0, hi = p->nfuncs - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (p->funcs[m].addr <= a) { best = m; lo = m + 1; } else hi = m - 1; }
    if (best < 0) { if (start) *start = 0; return "?"; }
    if (start) *start = p->funcs[best].addr;
    return p->funcs[best].name;
}

int pef_func_addr(const Pef *p, const char *name, uint32_t *addr) {
    for (int i = 0; i < p->nfuncs; i++) if (!strcmp(p->funcs[i].name, name)) { *addr = p->funcs[i].addr; return 0; }
    return -1;
}

void pef_free(Pef *p) {
    if (!p) return;
    free(p->code); free(p->data); free(p->relocs); free(p->funcs);
    free(p);   /* strings intentionally kept (shared with funcs) */
}
