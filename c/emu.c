/* emu.c - emulator core: memory map, Mac heap/handle model, PPC run loop, HLE dispatch,
 * nested calls into PPC code, breakpoints, tracing. Port of scripts/emu.py. */
#include "mac.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---------------------------------------------------------------- bytes */
void bytes_put(Bytes *x, const void *s, size_t n) {
    if (x->n + n + 1 > x->cap) { x->cap = (x->n + n + 1) * 2 + 16; x->b = realloc(x->b, x->cap); }
    if (n) { if (s) memcpy(x->b + x->n, s, n); else memset(x->b + x->n, 0, n); }
    x->n += n;
}
void bytes_set(Bytes *x, const void *s, size_t n) { x->n = 0; bytes_put(x, s, n); }
void bytes_free(Bytes *x) { free(x->b); x->b = NULL; x->n = x->cap = 0; }
Bytes bytes_dup(const void *s, size_t n) { Bytes b = {0}; bytes_put(&b, s, n); return b; }

/* ----------------------------------------------------------------- heap */
static void heap_init(Heap *h, uint32_t base, uint32_t end) {
    memset(h, 0, sizeof *h); h->base = base; h->end = end;
    h->capfr = 16; h->fr = malloc(16 * sizeof(Span)); h->fr[0].a = base; h->fr[0].n = end - base; h->nfr = 1;
    h->ucap = 1024; h->ukey = calloc(h->ucap, 4); h->uval = calloc(h->ucap, 4);
}
static uint32_t hslot(Heap *h, uint32_t a) { return (a >> 4) * 2654435761u & (h->ucap - 1); }
static void used_put(Heap *h, uint32_t a, uint32_t n);
static void used_grow(Heap *h) {
    uint32_t oc = h->ucap, *ok = h->ukey, *ov = h->uval;
    h->ucap *= 2; h->ukey = calloc(h->ucap, 4); h->uval = calloc(h->ucap, 4); h->ucount = 0;
    for (uint32_t i = 0; i < oc; i++) if (ok[i] && ok[i] != 1) used_put(h, ok[i], ov[i]);
    free(ok); free(ov);
}
/* key 0 = empty, 1 = deleted (addresses are >= base > 1) */
static void used_put(Heap *h, uint32_t a, uint32_t n) {
    if ((h->ucount + 1) * 2 > h->ucap) used_grow(h);
    uint32_t i = hslot(h, a);
    while (h->ukey[i] && h->ukey[i] != 1 && h->ukey[i] != a) i = (i + 1) & (h->ucap - 1);
    if (h->ukey[i] != a) h->ucount++;
    h->ukey[i] = a; h->uval[i] = n;
}
static int64_t used_get(Heap *h, uint32_t a) {
    uint32_t i = hslot(h, a);
    while (h->ukey[i]) { if (h->ukey[i] == a) return h->uval[i]; i = (i + 1) & (h->ucap - 1); }
    return -1;
}
static void used_del(Heap *h, uint32_t a) {
    uint32_t i = hslot(h, a);
    while (h->ukey[i]) { if (h->ukey[i] == a) { h->ukey[i] = 1; return; } i = (i + 1) & (h->ucap - 1); }
}
static int heap_alloc(Heap *h, uint32_t n, uint32_t *out) {
    n = n < 16 ? 16 : (n + 15) & ~15u;
    for (int i = 0; i < h->nfr; i++) {
        if (h->fr[i].n >= n) {
            uint32_t a = h->fr[i].a;
            if (h->fr[i].n == n) { memmove(&h->fr[i], &h->fr[i + 1], (h->nfr - i - 1) * sizeof(Span)); h->nfr--; }
            else { h->fr[i].a += n; h->fr[i].n -= n; }
            used_put(h, a, n); *out = a; return 0;
        }
    }
    return -1;
}
static int heap_release(Heap *h, uint32_t a) {
    int64_t n = used_get(h, a); if (n < 0) return -1;
    used_del(h, a);
    /* insert keeping address order, then coalesce (same result as append+sort+merge) */
    if (h->nfr == h->capfr) { h->capfr *= 2; h->fr = realloc(h->fr, h->capfr * sizeof(Span)); }
    int i = 0; while (i < h->nfr && h->fr[i].a < a) i++;
    memmove(&h->fr[i + 1], &h->fr[i], (h->nfr - i) * sizeof(Span)); h->fr[i].a = a; h->fr[i].n = (uint32_t)n; h->nfr++;
    if (i + 1 < h->nfr && h->fr[i].a + h->fr[i].n == h->fr[i + 1].a) {
        h->fr[i].n += h->fr[i + 1].n; memmove(&h->fr[i + 1], &h->fr[i + 2], (h->nfr - i - 2) * sizeof(Span)); h->nfr--;
    }
    if (i > 0 && h->fr[i - 1].a + h->fr[i - 1].n == h->fr[i].a) {
        h->fr[i - 1].n += h->fr[i].n; memmove(&h->fr[i], &h->fr[i + 1], (h->nfr - i - 1) * sizeof(Span)); h->nfr--;
    }
    return 0;
}

/* ------------------------------------------------------------- registry */
typedef struct { const char *name; HleFn fn; } RegEnt;
static RegEnt *reg_tab; static int nreg, capreg;
void emu_reg(const char *name, HleFn fn) {
    for (int i = 0; i < nreg; i++) if (!strcmp(reg_tab[i].name, name)) { reg_tab[i].fn = fn; return; }
    if (nreg == capreg) { capreg = capreg * 2 + 256; reg_tab = realloc(reg_tab, capreg * sizeof(RegEnt)); }
    reg_tab[nreg].name = name; reg_tab[nreg].fn = fn; nreg++;
}
void emu_bind_imports(Emu *E) {
    for (int i = 0; i < E->P->nimports; i++) {
        E->imp_fn[i] = NULL;
        for (int k = 0; k < nreg; k++) if (!strcmp(reg_tab[k].name, E->P->imports[i].name)) { E->imp_fn[i] = reg_tab[k].fn; break; }
    }
}

/* ---------------------------------------------------------------- memory */
static void map(Emu *E, uint32_t a, uint32_t n) { if (ppc_map(E->cpu, a, n)) { fprintf(stderr, "map failed %08x\n", a); exit(2); } }
void emu_read(Emu *E, uint32_t a, void *dst, uint32_t n) {
    if (n && ppc_read(E->cpu, a, dst, n)) emu_fail(E, "unmapped read %08x", a);
}
void emu_write(Emu *E, uint32_t a, const void *src, uint32_t n) {
    if (n && ppc_write(E->cpu, a, src, n)) emu_fail(E, "unmapped write %08x", a);
}
void emu_zero(Emu *E, uint32_t a, uint32_t n) {
    uint8_t z[4096] = {0};
    while (n) { uint32_t k = n > sizeof z ? sizeof z : n; emu_write(E, a, z, k); a += k; n -= k; }
}
uint8_t rd8(Emu *E, uint32_t a) { uint8_t b; emu_read(E, a, &b, 1); return b; }
uint16_t rd16(Emu *E, uint32_t a) { uint8_t b[2]; emu_read(E, a, b, 2); return (uint16_t)((b[0] << 8) | b[1]); }
int16_t rds16(Emu *E, uint32_t a) { return (int16_t)rd16(E, a); }
uint32_t rd32(Emu *E, uint32_t a) { uint8_t b[4]; emu_read(E, a, b, 4); return ((uint32_t)b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]; }
void wr8(Emu *E, uint32_t a, uint32_t v) { uint8_t b = (uint8_t)v; emu_write(E, a, &b, 1); }
void wr16(Emu *E, uint32_t a, uint32_t v) { uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v }; emu_write(E, a, b, 2); }
void wr32(Emu *E, uint32_t a, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; emu_write(E, a, b, 4); }
int rd_pstr(Emu *E, uint32_t a, char *out) { int n = rd8(E, a); emu_read(E, a + 1, out, n); out[n] = 0; return n; }
int rd_cstr(Emu *E, uint32_t a, char *out, int maxn) {
    int n = 0; while (n < maxn - 1) { uint8_t c = rd8(E, a + n); if (!c) break; out[n++] = (char)c; } out[n] = 0; return n;
}
void wr_pstr(Emu *E, uint32_t a, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (n > 255) n = 255;
    wr8(E, a, n); emu_write(E, a + 1, s, n);
}
void wr_cstr(Emu *E, uint32_t a, const char *s) { emu_write(E, a, s, (uint32_t)strlen(s) + 1); }

int heap_owns(Emu *E, uint32_t a) { (void)E; return a >= AUX_HEAP && a < AUX_HEAP_END; }
static Heap *heap_of(Emu *E, uint32_t a) { return (a >= AUX_HEAP && a < AUX_HEAP_END) ? &E->aux : &E->heap; }
uint32_t new_ptr(Emu *E, uint32_t n, int aux) {
    uint32_t a;
    if (heap_alloc(aux ? &E->aux : &E->heap, n, &a)) emu_fail(E, "heap exhausted (%u)", n);
    uint32_t sz = n < 16 ? 16 : (n + 15) & ~15u;
    map(E, a, sz); emu_zero(E, a, sz);
    return a;
}
void dispose_ptr(Emu *E, uint32_t a) {
    if (!a) return;
    if (heap_release(heap_of(E, a), a)) emu_fail(E, "release of non-block %08x", a);
}
int64_t ptr_size(Emu *E, uint32_t a) { return used_get(heap_of(E, a), a); }

static HandleRec *hrec_slot(Emu *E, uint32_t h, int create) {
    HandleRec **tab; uint32_t *cap, idx;
    if (h >= AUX_MP && h < AUX_MP + 0x100000) { tab = &E->haux; cap = &E->haux_cap; idx = (h - AUX_MP) / 4; }
    else if (h >= MPBLK && h < MPBLK + 0x100000) { tab = &E->hmain; cap = &E->hmain_cap; idx = (h - MPBLK) / 4; }
    else return NULL;
    if ((h & 3) != 0) return NULL;
    if (idx >= *cap) {
        if (!create) return NULL;
        uint32_t nc = *cap ? *cap : 1024; while (nc <= idx) nc *= 2;
        *tab = realloc(*tab, nc * sizeof(HandleRec)); memset(*tab + *cap, 0, (nc - *cap) * sizeof(HandleRec)); *cap = nc;
    }
    return &(*tab)[idx];
}
HandleRec *handle_rec(Emu *E, uint32_t h) { HandleRec *r = hrec_slot(E, h, 0); return (r && r->used) ? r : NULL; }
static void push_u32(uint32_t **v, int *n, int *cap, uint32_t x) {
    if (*n == *cap) { *cap = *cap * 2 + 64; *v = realloc(*v, *cap * 4); } (*v)[(*n)++] = x;
}
static uint32_t alloc_mp(Emu *E, int aux) {
    if (aux) {
        if (E->naux_free) return E->aux_mp_free[--E->naux_free];
        uint32_t m = E->aux_mp_next; E->aux_mp_next += 4; return m;
    }
    if (E->nmp_free) return E->mp_free[--E->nmp_free];
    uint32_t m = E->mp_next; E->mp_next += 4; return m;
}
uint32_t new_handle(Emu *E, uint32_t n, const void *data, int aux) {
    uint32_t mp = alloc_mp(E, aux);
    uint32_t blk = new_ptr(E, n ? n : 1, aux);
    if (data && n) emu_write(E, blk, data, n);
    wr32(E, mp, blk);
    HandleRec *r = hrec_slot(E, mp, 1); r->blk = blk; r->n = n; r->used = 1;
    return mp;
}
uint32_t new_empty_handle(Emu *E) {
    uint32_t mp = alloc_mp(E, 0);
    wr32(E, mp, 0); HandleRec *r = hrec_slot(E, mp, 1); r->blk = 0; r->n = 0; r->used = 1;
    return mp;
}
void handle_set_block(Emu *E, uint32_t h, uint32_t blk, uint32_t n) {
    HandleRec *r = hrec_slot(E, h, 1); r->blk = blk; r->n = n; r->used = 1; wr32(E, h, blk);
}
uint32_t handle_size(Emu *E, uint32_t h) { HandleRec *r = handle_rec(E, h); return r ? r->n : 0; }
uint8_t *handle_copy(Emu *E, uint32_t h, uint32_t *n) {
    HandleRec *r = handle_rec(E, h); if (!r) emu_fail(E, "not a handle %08x", h);
    uint8_t *d = malloc(r->n + 1); if (r->n) emu_read(E, r->blk, d, r->n); d[r->n] = 0;
    if (n) *n = r->n; return d;
}
void set_handle_size(Emu *E, uint32_t h, uint32_t n) {
    HandleRec *r = handle_rec(E, h); if (!r) emu_fail(E, "SetHandleSize of non-handle %08x", h);
    uint32_t blk = r->blk, old = r->n;
    uint32_t nb = new_ptr(E, n ? n : 1, h >= AUX_MP && h < AUX_MP + 0x100000);
    r = handle_rec(E, h);
    if (blk) {
        uint32_t k = old < n ? old : n;
        if (k) { uint8_t *t = malloc(k); emu_read(E, blk, t, k); emu_write(E, nb, t, k); free(t); }
    }
    dispose_ptr(E, blk);
    r->blk = nb; r->n = n; wr32(E, h, nb);
}
void dispose_handle(Emu *E, uint32_t h) {
    HandleRec *r = handle_rec(E, h); if (!r) return;
    uint32_t blk = r->blk; r->used = 0; r->blk = 0; r->n = 0;
    dispose_ptr(E, blk); wr32(E, h, 0);
    if (h >= AUX_MP && h < AUX_MP + 0x100000) push_u32(&E->aux_mp_free, &E->naux_free, &E->capaux_free, h);
    else push_u32(&E->mp_free, &E->nmp_free, &E->capmp_free, h);
}

/* ------------------------------------------------------------ registers */
uint32_t emu_r(Emu *E, int n) { return E->cpu->gpr[n]; }
void emu_setr(Emu *E, int n, uint32_t v) { E->cpu->gpr[n] = v; }
uint32_t A(Emu *E, int i) { return i < 8 ? E->cpu->gpr[3 + i] : rd32(E, E->cpu->gpr[1] + 24 + 4 * i); }
int32_t SA(Emu *E, int i) { return (int16_t)(A(E, i) & 0xFFFF); }
int32_t SA32(Emu *E, int i) { return (int32_t)A(E, i); }
double FA(Emu *E, int i) { double d; uint64_t b = E->cpu->fpr[1 + i]; memcpy(&d, &b, 8); return d; }
void SETF1(Emu *E, double x) { uint64_t b; memcpy(&b, &x, 8); E->cpu->fpr[1] = b; }

/* ------------------------------------------------------------- creation */
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
Emu *emu_new(const char *pef_path, FILE *trace, const char *tag) {
    char err[256];
    Pef *P = pef_load(pef_path, err, sizeof err);
    if (!P) { fprintf(stderr, "%s: %s\n", pef_path, err); return NULL; }
    Emu *E = calloc(1, sizeof *E);
    E->P = P; E->trace = trace; E->tag = tag;
    E->cpu = ppc_new(); E->cpu->msr = 0x2000;
    map(E, LOWMEM, 0x10000);
    map(E, CODE, P->code_len); map(E, DATA, P->data_len);
    map(E, IMPTV, 0x100000); map(E, IMPSTUB, 0x100000); map(E, TRAMP, 0x100000); map(E, SENTINEL, 0x10000);
    map(E, MPBLK, 0x100000); map(E, STACK_TOP - STACK_SIZE, STACK_SIZE);
    ppc_write(E->cpu, CODE, P->code, P->code_len);
    uint8_t *data = malloc(P->data_len + 8); memcpy(data, P->data, P->data_len);
    for (int i = 0; i < P->nrelocs; i++) {
        const PefReloc *r = &P->relocs[i]; uint32_t nv;
        if (r->kind == 'C') nv = CODE + r->val; else if (r->kind == 'D') nv = DATA + r->val; else nv = IMPTV + 8 * r->val;
        data[r->off] = nv >> 24; data[r->off + 1] = nv >> 16; data[r->off + 2] = nv >> 8; data[r->off + 3] = nv;
    }
    ppc_write(E->cpu, DATA, data, P->data_len); free(data);
    for (int i = 0; i < P->nimports; i++) {
        uint8_t tv[8];
        if (P->imports[i].cls == 1) {
            if (strcmp(P->imports[i].name, "pi")) { fprintf(stderr, "unknown data import %s\n", P->imports[i].name); exit(2); }
            double pi = 3.141592653589793; uint64_t b; memcpy(&b, &pi, 8);
            for (int k = 0; k < 8; k++) tv[k] = (uint8_t)(b >> (56 - 8 * k));
        } else {
            uint32_t s = IMPSTUB + 4 * i;
            tv[0] = s >> 24; tv[1] = s >> 16; tv[2] = s >> 8; tv[3] = s; tv[4] = tv[5] = tv[6] = tv[7] = 0;
        }
        ppc_write(E->cpu, IMPTV + 8 * i, tv, 8);
        uint8_t blr[4] = { 0x4E, 0x80, 0x00, 0x20 }; ppc_write(E->cpu, IMPSTUB + 4 * i, blr, 4);
    }
    uint8_t blr[4] = { 0x4E, 0x80, 0x00, 0x20 }; ppc_write(E->cpu, SENTINEL, blr, 4);
    E->cpu->hook_lo = BPZONE; E->cpu->hook_hi = SENTINEL + 4;
    E->toc = DATA + P->toc;
    heap_init(&E->heap, HEAPBASE, HEAPEND); heap_init(&E->aux, AUX_HEAP, AUX_HEAP_END);
    map(E, AUX_MP, 0x100000); E->aux_mp_next = AUX_MP; E->mp_next = MPBLK;
    E->imp_fn = calloc(P->nimports + 1, sizeof(HleFn)); E->imp_count = calloc(P->nimports + 1, 8);
    E->nest_ret = TRAMP + 0x80000;          /* return addresses for nested calls: TRAMP+0x80000+4*depth */
    E->trace_all = getenv("HLE_TRACE_ALL") != NULL;
    (void)be32;
    return E;
}

uint32_t emu_entry(Emu *E) {
    const PefReloc *r = pef_reloc_at(E->P, E->P->main_off);
    return r ? CODE + r->val : 0;
}
uint32_t emu_addr_of(Emu *E, const char *name) {
    uint32_t a; return pef_func_addr(E->P, name, &a) ? 0 : CODE + a;
}
uint32_t emu_tvec_of(Emu *E, const char *name) {
    for (int i = 0; i < E->P->nexports; i++)
        if (!strcmp(E->P->exports[i].name, name) && E->P->exports[i].sec == E->P->data_idx) return DATA + E->P->exports[i].val;
    return 0;
}

/* ------------------------------------------------------------- tracing */
static void owner_str(Emu *E, int64_t o, char *buf) {
    /* python: '%06x %s' % (o, P.owner(o - 4)[1]) */
    const char *nm = (o - 4 < 0) ? "?" : pef_owner(E->P, (uint32_t)(o - 4), NULL);
    if (o < 0) sprintf(buf, "-%05llx %s", (unsigned long long)(-o), nm);
    else sprintf(buf, "%06llx %s", (unsigned long long)o, nm);
}
const char *emu_where_caller(Emu *E, char *buf) { owner_str(E, (int64_t)E->saved_lr - CODE, buf); return buf; }
const char *emu_where(Emu *E, char *buf) {
    uint32_t pc = E->cpu->pc;
    if (pc >= CODE && pc < CODE + E->P->code_len) sprintf(buf, "%06x %s", pc - CODE, pef_owner(E->P, pc - CODE, NULL));
    else sprintf(buf, "%08x", pc);
    return buf;
}
int emu_backtrace(Emu *E, char out[][96], int depth) {
    int k = 0; emu_where(E, out[k++]);
    uint32_t sp = E->cpu->gpr[1];
    for (int i = 0; i < depth; i++) {
        uint32_t lr; uint8_t b[4];
        if (ppc_read(E->cpu, sp, b, 4)) break; sp = be32(b);
        if (!sp) break;
        if (ppc_read(E->cpu, sp + 8, b, 4)) break; lr = be32(b);
        if (lr >= CODE && lr < CODE + E->P->code_len) { uint32_t o = lr - CODE; sprintf(out[k++], "%06x %s", o, pef_owner(E->P, o - 4, NULL)); }
    }
    return k;
}
static void tagged_write(Emu *E, const char *line) {
    if (!E->trace) return;
    if (E->tag) fprintf(E->trace, "%s %s", E->tag, line); else fputs(line, E->trace);
}
void TLOG(Emu *E, const char *fmt, ...) {
    if (!E->trace) return;
    char buf[8192]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    /* split multi-line output so every line carries the tag */
    char *p = buf;
    while (*p) { char *nl = strchr(p, '\n'); char save = 0; if (nl) { save = nl[1]; nl[1] = 0; } tagged_write(E, p); if (!nl) break; nl[1] = save; p = nl + 1; }
}
static const char *QUIET[] = { "TickCount", "BlockMove", "BlockMoveData", "HLock", "HUnlock", "GetHandleSize", "MemError",
    "SystemTask", "WaitNextEvent", "GetNextEvent", "p2cstr", "c2pstr", "SetCursor", "GetPort", "SetPort", NULL };
void T(Emu *E, const char *name, const char *fmt, ...) {
    if (!E->trace) return;
    for (int i = 0; QUIET[i]; i++) if (!strcmp(QUIET[i], name)) return;
    char msg[6000], wc[200]; va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    TLOG(E, "[%s] %s %s\n", emu_where_caller(E, wc), name, msg);
}

void emu_fail(Emu *E, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(E->stopped_reason, sizeof E->stopped_reason, fmt, ap); va_end(ap);
    if (E->cur_hle) {   /* python _on_hook: 'HLE %s raised %r (called from %s)' */
        char msg[512], r[1100], wc[200]; snprintf(msg, sizeof msg, "%s", E->stopped_reason); py_repr_str(msg, -1, r, sizeof r);
        snprintf(E->stopped_reason, sizeof E->stopped_reason, "HLE %s raised EmuError(%s) (called from %s)", E->cur_hle, r, emu_where_caller(E, wc));
        E->cur_hle = NULL;
    }
    if (E->fail_jmp) longjmp(*E->fail_jmp, 1);
    fprintf(stderr, "emu_fail without handler: %s\n", E->stopped_reason); exit(3);
}

/* -------------------------------------------------------------- hooks */
static void on_hook(Emu *E, uint32_t addr) {
    for (int i = 0; i < E->nbps; i++) if (E->bps[i].slot == addr) { E->bps[i].fn(E, E->bps[i].ctx); return; }
    if (!(addr >= IMPSTUB && addr < IMPSTUB + 4u * E->P->nimports)) return;
    int idx = (addr - IMPSTUB) / 4;
    const char *name = E->P->imports[idx].name;
    E->imp_count[idx]++;
    HleFn fn = E->imp_fn[idx];
    E->saved_lr = E->cpu->lr; E->saved_r2 = E->cpu->gpr[2];
    if (!fn) {
        uint32_t caller = E->saved_lr - CODE;
        emu_fail(E, "unimplemented import %s called from %06x (%s)", name, caller, pef_owner(E->P, caller - 4, NULL));
    }
    if (E->trace_all) {
        char wc[200]; TLOG(E, "  .%s(%08x %08x %08x %08x) from %s\n", name, emu_r(E, 3), emu_r(E, 4), emu_r(E, 5), emu_r(E, 6), emu_where_caller(E, wc));
    }
    const char *prev = E->cur_hle; E->cur_hle = name;
    int64_t ret = fn(E);
    E->cur_hle = prev;
    if (ret != NORET) {
        E->cpu->gpr[3] = (uint32_t)ret;
        if (E->trace_all) TLOG(E, "    -> %08x\n", (uint32_t)ret);
    }
}

/* run from start until pc == until is reached (hooked address) */
static void run_until(Emu *E, uint32_t start, uint32_t until, uint64_t max_insns) {
    PPC *s = E->cpu; s->pc = start;
    uint64_t limit = max_insns ? s->icount + max_insns : 0;
    for (;;) {
        uint64_t n = 0;
        if (limit) { if (s->icount >= limit) { char w[200]; emu_fail(E, "instruction limit reached at %s", emu_where(E, w)); } n = limit - s->icount; }
        int rc = ppc_run(s, n);
        if (rc == PPC_STOP_HOOK) {
            uint32_t pc = s->pc;
            if (pc == until) return;
            on_hook(E, pc);
            if (s->pc == pc) { if (ppc_step(s)) emu_fail(E, "fault executing stub at %08x", pc); }
            continue;
        }
        char w[200];
        if (rc == PPC_STOP_COUNT) emu_fail(E, "instruction limit reached at %s", emu_where(E, w));
        if (rc == PPC_STOP_FAULT) emu_fail(E, "unmapped %s at %08x (pc %s)", s->fault_write ? "write" : "read/fetch", s->fault_addr, emu_where(E, w));
        if (rc == PPC_STOP_ILLEGAL) emu_fail(E, "illegal instruction %08x at %s", s->illegal_insn, emu_where(E, w));
        emu_fail(E, "ppc_run returned %d", rc);
    }
}

uint32_t emu_call(Emu *E, uint32_t code, uint32_t toc, const uint32_t *args, int nargs, uint64_t max_insns) {
    uint32_t sp = STACK_TOP - 0x2000;
    E->cpu->gpr[1] = sp; E->cpu->gpr[2] = toc;
    for (int i = 0; i < nargs; i++) { if (i < 8) E->cpu->gpr[3 + i] = args[i]; else wr32(E, sp + 24 + 4 * i, args[i]); }
    E->cpu->lr = SENTINEL;
    run_until(E, code, SENTINEL, max_insns);
    return E->cpu->gpr[3];
}

/* emu_call_nested for our own tools (fghooks.c, popups.c, rawscan.c), counted in own_nest: while it is > 0 the
 * window/scenario must not start the next command from the program's idle (the called code may run event loops). */
uint32_t emu_call_own(Emu *E, uint32_t tvec, const uint32_t *args, int nargs) {
    E->own_nest++; uint32_t r = emu_call_nested(E, tvec, args, nargs); E->own_nest--; return r;
}
/* Call PPC code (TVector) from inside an HLE function and return its r3. Equivalent to the Python
 * reference's call_back(): the callee runs below the caller's stack pointer (sp-0x200, 16-aligned)
 * with its own TOC; afterwards the caller's registers are restored. */
uint32_t emu_call_nested(Emu *E, uint32_t tvec, const uint32_t *args, int nargs) {
    PPC *s = E->cpu;
    uint32_t code = rd32(E, tvec), toc = rd32(E, tvec + 4);
    uint32_t gpr[32], cr = s->cr, xer = s->xer, lr = s->lr, ctr = s->ctr, fpscr = s->fpscr, pc = s->pc; uint64_t fpr[32];
    memcpy(gpr, s->gpr, sizeof gpr); memcpy(fpr, s->fpr, sizeof fpr);
    uint32_t saved_lr = E->saved_lr, saved_r2 = E->saved_r2; const char *hle = E->cur_hle; E->cur_hle = NULL;
    uint32_t ret = E->nest_ret + 4 * (uint32_t)(E->nest_depth++);
    wr32(E, ret, BLR);
    s->gpr[1] = (gpr[1] - 0x200) & ~0xFu;
    for (int i = 0; i < nargs && i < 8; i++) s->gpr[3 + i] = args[i];
    s->gpr[2] = toc; s->lr = ret;
    run_until(E, code, ret, 0);
    uint32_t r3 = s->gpr[3]; memcpy(&E->nest_f1, &s->fpr[1], 8);
    E->nest_depth--;
    memcpy(s->gpr, gpr, sizeof gpr); memcpy(s->fpr, fpr, sizeof fpr);
    s->cr = cr; s->xer = xer; s->lr = lr; s->ctr = ctr; s->fpscr = fpscr; s->pc = pc;
    E->saved_lr = saved_lr; E->saved_r2 = saved_r2; E->cur_hle = hle;
    return r3;
}

void emu_add_bp(Emu *E, uint32_t code_off, BpFn fn, void *ctx) {
    uint32_t a = CODE + code_off, orig = rd32(E, a);
    if ((orig >> 26) == 16 || (orig >> 26) == 18) emu_fail(E, "add_bp on a branch at %06x", code_off);
    if (!E->bp_next) { E->bp_next = BPZONE; map(E, BPZONE, 0x10000); }
    uint32_t slot = E->bp_next, isl = slot + 4; E->bp_next += 12;
    wr32(E, isl, orig); wr32(E, isl + 4, 0x48000002u | (a + 4));
    wr32(E, slot, 0x48000002u | isl);
    wr32(E, a, 0x48000002u | slot);
    E->bps = realloc(E->bps, (E->nbps + 1) * sizeof *E->bps);
    E->bps[E->nbps].slot = slot; E->bps[E->nbps].fn = fn; E->bps[E->nbps].ctx = ctx; E->nbps++;
}
