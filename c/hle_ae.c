/* hle_ae.c - Apple Event Manager and Object Support Library HLE. Port of the AppleEvent section of
 * scripts/mac_hle.py. Descriptors get real handles in the side ('aux') zone; structured descriptors
 * (events, records, lists) map their handle to an AERec. Records are shared, never freed (as in the
 * reference, where Python objects are shared by reference). */
#include "mac.h"
#include <stdlib.h>
#include <string.h>

#define W (E->W)
#define REG(name) static int64_t h_##name(Emu *E)
#define errAEDescNotFound (-1701)
#define errAEHandlerNotFound (-1717)
#define errAEIllegalIndex (-1719)
#define errAEWrongDataType (-1703)

/* ---------------------------------------------------------------- AERec */
AERec *aerec_new(void) { return calloc(1, sizeof(AERec)); }
static AEItem *item_add(AEItem **v, int *n, int *cap) {
    if (*n == *cap) { *cap = *cap * 2 + 8; *v = realloc(*v, *cap * sizeof(AEItem)); }
    AEItem *x = &(*v)[(*n)++]; memset(x, 0, sizeof *x); return x;
}
AEItem *aerec_find(AERec *r, const char *key) {
    for (int i = 0; i < r->n; i++) if (!memcmp(r->it[i].key, key, 4)) return &r->it[i];
    return NULL;
}
AEItem *aerec_put(AERec *r, const char *key, const char *type, const void *d, size_t n, AERec *rec) {
    AEItem *x = r->is_list ? NULL : aerec_find(r, key);
    if (!x) { x = item_add(&r->it, &r->n, &r->cap); memcpy(x->key, key, 4); }
    memset(x->type, 0, 5); if (type && type[0]) memcpy(x->type, type, 4);
    x->rec = rec; if (rec) bytes_set(&x->data, NULL, 0); else bytes_set(&x->data, d, n);
    return x;
}
int aerec_del(AERec *r, const char *key) {
    for (int i = 0; i < r->n; i++) if (!memcmp(r->it[i].key, key, 4)) {
        memmove(&r->it[i], &r->it[i + 1], (r->n - i - 1) * sizeof(AEItem)); r->n--; return 1;
    }
    return 0;
}
AEItem *aerec_attr(AERec *r, const char *key) {
    for (int i = 0; i < r->na; i++) if (!memcmp(r->attr[i].key, key, 4)) return &r->attr[i];
    return NULL;
}
void aerec_set_attr(AERec *r, const char *key, const char *type, const void *d, size_t n) {
    AEItem *x = aerec_attr(r, key);
    if (!x) { x = item_add(&r->attr, &r->na, &r->capa); memcpy(x->key, key, 4); }
    memcpy(x->type, type, 4); bytes_set(&x->data, d, n);
}
static AERec *copy_rec(const AERec *r, int deep) {
    AERec *c = aerec_new(); *c = *r; c->it = NULL; c->n = c->cap = 0; c->attr = NULL; c->na = c->capa = 0;
    for (int i = 0; i < r->n; i++) {
        AEItem *x = item_add(&c->it, &c->n, &c->cap); memcpy(x->key, r->it[i].key, 5); memcpy(x->type, r->it[i].type, 5);
        x->data = bytes_dup(r->it[i].data.b, r->it[i].data.n);
        x->rec = r->it[i].rec ? (deep ? copy_rec(r->it[i].rec, 1) : r->it[i].rec) : NULL;
    }
    for (int i = 0; i < r->na; i++) {
        AEItem *x = item_add(&c->attr, &c->na, &c->capa); memcpy(x->key, r->attr[i].key, 5); memcpy(x->type, r->attr[i].type, 5);
        x->data = bytes_dup(r->attr[i].data.b, r->attr[i].data.n);
    }
    return c;
}
AERec *aerec_copy(const AERec *r) { return copy_rec(r, 0); }
void aerec_free(AERec *r) { (void)r; }

/* ----------------------------------------------------------- descriptors */
static void desc_map(World *w, uint32_t h, AERec *r) {
    for (int i = 0; i < w->ndescs; i++) if (w->descs[i].h == h) { w->descs[i].r = r; return; }
    if (w->ndescs == w->capdescs) { w->capdescs = w->capdescs * 2 + 64; w->descs = realloc(w->descs, w->capdescs * sizeof *w->descs); }
    w->descs[w->ndescs].h = h; w->descs[w->ndescs].r = r; w->ndescs++;
}
static AERec *desc_lookup(World *w, uint32_t h) {
    for (int i = w->ndescs - 1; i >= 0; i--) if (w->descs[i].h == h) return w->descs[i].r;
    return NULL;
}
static void desc_unmap(World *w, uint32_t h) {
    for (int i = 0; i < w->ndescs; i++) if (w->descs[i].h == h) { memmove(&w->descs[i], &w->descs[i + 1], (w->ndescs - i - 1) * sizeof *w->descs); w->ndescs--; return; }
}
uint32_t ae_new_desc(World *w, uint32_t addr, AERec *r, const char *type) {
    Emu *E = w->E; uint32_t h = new_handle(E, 0, NULL, 1); desc_map(w, h, r);
    wr32(E, addr, ostype_i(type)); wr32(E, addr + 4, h); return h;
}
static uint32_t new_simple(World *w, uint32_t addr, const char *type, const void *d, size_t n, int null_data) {
    Emu *E = w->E; uint32_t h = null_data ? 0 : new_handle(E, (uint32_t)n, d, 1);
    wr32(E, addr, ostype_i(type)); wr32(E, addr + 4, h); return h;
}
AERec *ae_desc_rec(World *w, uint32_t addr) { return desc_lookup(w, rd32(w->E, addr + 4)); }
/* (type, rec or bytes) */
static AERec *desc_value(World *w, uint32_t addr, char *type, Bytes *data) {
    Emu *E = w->E; ostype_s(rd32(E, addr), type); uint32_t h = rd32(E, addr + 4);
    AERec *r = desc_lookup(w, h); if (r) return r;
    data->n = 0;
    if (handle_rec(E, h)) { uint32_t n; uint8_t *d = handle_copy(E, h, &n); bytes_set(data, d, n); free(d); }
    return NULL;
}
static void write_desc(World *w, uint32_t addr, const char *type, AERec *r, const Bytes *d) {
    if (r) ae_new_desc(w, addr, r, type); else new_simple(w, addr, type, d->b, d->n, 0);
}
static void put_param(AERec *p, const char *key, const char *type, AERec *r, const Bytes *d) {
    if (r) aerec_put(p, key, type, NULL, 0, r); else aerec_put(p, key, type, d->b, d->n, NULL);
}
static void ensure_attrs(AERec *p) {
    if (p->is_event) {
        if (!aerec_attr(p, "evcl")) aerec_set_attr(p, "evcl", "type", p->cls, 4);
        if (!aerec_attr(p, "evid")) aerec_set_attr(p, "evid", "type", p->id, 4);
    }
}
static void ost(uint32_t v, char *o) { ostype_s(v, o); }
static const char *keyrepr(const AERec *p, char *out) {
    /* python list repr of the parameter names, e.g. ['PRVI', 'ACIN'] */
    int k = 0; out[k++] = '[';
    for (int i = 0; i < p->n; i++) { char r[64]; py_repr_str(p->it[i].key, 4, r, sizeof r); k += sprintf(out + k, "%s%s", i ? ", " : "", r); }
    out[k++] = ']'; out[k] = 0; return out;
}

/* ------------------------------------------------------------- handlers */
static int find_handler(World *w, const char *cls, const char *id, uint32_t *h, uint32_t *refcon) {
    const char *keys[3][2] = { { cls, id }, { cls, "****" }, { "****", "****" } };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < w->naeh; i++)
            if (!memcmp(w->aeh[i].cls, keys[k][0], 4) && !memcmp(w->aeh[i].id, keys[k][1], 4) && w->aeh[i].h) {
                *h = w->aeh[i].h;
                /* refcon: python looks the refcon up under the exact (cls, id) key only */
                *refcon = 0; for (int j = 0; j < w->naeh; j++) if (!memcmp(w->aeh[j].cls, cls, 4) && !memcmp(w->aeh[j].id, id, 4)) *refcon = w->aeh[j].refcon;
                return 1;
            }
    return 0;
}
REG(AEInstallEventHandler) {
    char cls[5], id[5]; ost(A(E, 0), cls); ost(A(E, 1), id);
    int i = 0; for (; i < W->naeh; i++) if (!memcmp(W->aeh[i].cls, cls, 4) && !memcmp(W->aeh[i].id, id, 4)) break;
    if (i == W->naeh) { if (W->naeh == W->capaeh) { W->capaeh = W->capaeh * 2 + 16; W->aeh = realloc(W->aeh, W->capaeh * sizeof *W->aeh); } W->naeh++; memcpy(W->aeh[i].cls, cls, 5); memcpy(W->aeh[i].id, id, 5); }
    W->aeh[i].h = A(E, 2); W->aeh[i].refcon = A(E, 3);
    T(E, "AEInstallEventHandler", "'%s' '%s' -> %08x", cls, id, A(E, 2)); return 0;
}
REG(AEGetEventHandler) {
    char cls[5], id[5]; ost(A(E, 0), cls); ost(A(E, 1), id); uint32_t h, rc;
    if (!find_handler(W, cls, id, &h, &rc)) return errAEHandlerNotFound & 0xFFFF;
    if (A(E, 2)) wr32(E, A(E, 2), h);
    if (A(E, 3)) wr32(E, A(E, 3), rc);
    return 0;
}
int ae_dispatch(World *w, AEQItem *item) {
    Emu *E = w->E; uint32_t h, refcon; char kr[512];
    int found = find_handler(w, item->cls, item->id, &h, &refcon);
    T(E, "AEProcessAppleEvent", "'%s' '%s' %s", item->cls, item->id, keyrepr(item->params, kr));
    if (!found) {
        AERec *rp = aerec_new(); uint8_t e[2] = { 0xF9, 0x54 };       /* -1708 */
        aerec_put(rp, "errn", "shor", e, 2, NULL);
        if (w->on_reply) w->on_reply(w, item->params, rp, -1708);
        return -1708;
    }
    uint32_t evt = new_ptr(E, 16, 1), rep = new_ptr(E, 16, 1);
    AERec *params = aerec_copy(item->params);
    if (!params->is_event) { params->is_event = 1; memcpy(params->cls, item->cls, 5); memcpy(params->id, item->id, 5); }
    ae_new_desc(w, evt, params, "aevt"); AERec *reply = aerec_new(); ae_new_desc(w, rep, reply, "aevt");
    AERec *prev = w->cur_ae; w->cur_ae = params;
    uint32_t args[3] = { evt, rep, refcon };
    uint32_t r = emu_call_nested(E, h, args, 3);
    AERec *rp = ae_desc_rec(w, rep);
    if (w->on_reply) w->on_reply(w, params, rp, (int16_t)r);
    w->cur_ae = prev; dispose_ptr(E, evt); dispose_ptr(E, rep);
    return (int16_t)(r & 0xFFFF);
}
REG(AEProcessAppleEvent) {
    if (!W->nq) return 0;
    AEQItem it = W->q[0]; memmove(&W->q[0], &W->q[1], (W->nq - 1) * sizeof(AEQItem)); W->nq--;
    ae_dispatch(W, &it); return 0;
}
void ae_queue_push(World *w, const char *cls, const char *id, AERec *params) {
    if (w->nq == w->capq) { w->capq = w->capq * 2 + 16; w->q = realloc(w->q, w->capq * sizeof(AEQItem)); }
    AEQItem *x = &w->q[w->nq++]; memcpy(x->cls, cls, 4); x->cls[4] = 0; memcpy(x->id, id, 4); x->id[4] = 0; x->params = params;
}

/* ----------------------------------------------------------- parameters */
REG(AEGetParamPtr) {
    char key[5], want[5]; ost(A(E, 1), key); ost(A(E, 2), want); uint32_t maxs = A(E, 5);
    AERec *p = ae_desc_rec(W, A(E, 0)); if (!p) p = W->cur_ae;
    AEItem *x = p ? aerec_find(p, key) : NULL;
    char shown[140];
    if (!x) strcpy(shown, "None");
    else if (x->rec) strcpy(shown, "struct");
    else { int n = (int)x->data.n > 32 ? 32 : (int)x->data.n; hexs(x->data.b, n, shown); }
    T(E, "AEGetParamPtr", "'%s' '%s' max %u -> %s", key, want, maxs, shown);
    if (!x || x->rec) return errAEDescNotFound & 0xFFFF;
    if (A(E, 3)) wr32(E, A(E, 3), strcmp(want, "****") ? A(E, 2) : ostype_i(x->type[0] ? x->type : "TEXT"));
    uint32_t n = (uint32_t)x->data.n < maxs ? (uint32_t)x->data.n : maxs; emu_write(E, A(E, 4), x->data.b, n);
    if (A(E, 6)) wr32(E, A(E, 6), (uint32_t)x->data.n);
    return 0;
}
REG(AEGetParamDesc) {
    char key[5], want[5]; ost(A(E, 1), key); ost(A(E, 2), want);
    AERec *p = ae_desc_rec(W, A(E, 0)); AEItem *x = p ? aerec_find(p, key) : NULL;
    T(E, "AEGetParamDesc", "'%s' -> %s", key, x ? "found" : "None");
    if (!x) return errAEDescNotFound & 0xFFFF;
    const char *typ = x->type[0] ? x->type : "TEXT";
    write_desc(W, A(E, 3), (!strcmp(want, "****") || !strcmp(want, typ)) ? typ : want, x->rec, &x->data);
    return 0;
}
REG(AEPutParamPtr) {
    char key[5], typ[5]; ost(A(E, 1), key); ost(A(E, 2), typ); uint32_t n = A(E, 4);
    uint8_t *d = malloc(n + 1); emu_read(E, A(E, 3), d, n);
    AERec *p = ae_desc_rec(W, A(E, 0)); if (p) aerec_put(p, key, typ, d, n, NULL);
    free(d); return 0;
}
REG(AEPutParamDesc) {
    char key[5], typ[5]; ost(A(E, 1), key); AERec *p = ae_desc_rec(W, A(E, 0));
    if (p) { Bytes d = {0}; AERec *r = desc_value(W, A(E, 2), typ, &d); put_param(p, key, typ, r, &d); bytes_free(&d); }
    return 0;
}
REG(AEDeleteParam) {
    char key[5]; ost(A(E, 1), key); AERec *p = ae_desc_rec(W, A(E, 0));
    if (p && aerec_del(p, key)) return 0;
    return errAEDescNotFound & 0xFFFF;
}
REG(AEGetAttributePtr) {
    AERec *p = ae_desc_rec(W, A(E, 0)); char key[5]; ost(A(E, 1), key);
    if (!p) return errAEDescNotFound & 0xFFFF;
    ensure_attrs(p); AEItem *x = aerec_attr(p, key);
    if (!x) return errAEDescNotFound & 0xFFFF;
    if (A(E, 3)) wr32(E, A(E, 3), ostype_i(x->type));
    uint32_t n = (uint32_t)x->data.n < A(E, 5) ? (uint32_t)x->data.n : A(E, 5); emu_write(E, A(E, 4), x->data.b, n);
    if (A(E, 6)) wr32(E, A(E, 6), (uint32_t)x->data.n);
    return 0;
}
REG(AEGetAttributeDesc) {
    AERec *p = ae_desc_rec(W, A(E, 0)); char key[5]; ost(A(E, 1), key);
    if (!p) return errAEDescNotFound & 0xFFFF;
    ensure_attrs(p); AEItem *x = aerec_attr(p, key);
    if (!x) return errAEDescNotFound & 0xFFFF;
    new_simple(W, A(E, 3), x->type, x->data.b, x->data.n, 0); return 0;
}
REG(AESizeOfAttribute) {
    AERec *p = ae_desc_rec(W, A(E, 0)); char key[5]; ost(A(E, 1), key);
    if (!p) return errAEDescNotFound & 0xFFFF;
    ensure_attrs(p); AEItem *x = aerec_attr(p, key);
    if (!x) return errAEDescNotFound & 0xFFFF;
    if (A(E, 2)) wr32(E, A(E, 2), ostype_i(x->type));
    if (A(E, 3)) wr32(E, A(E, 3), (uint32_t)x->data.n);
    return 0;
}
REG(AEPutAttributePtr) {
    AERec *p = ae_desc_rec(W, A(E, 0));
    if (p) { char key[5], typ[5]; ost(A(E, 1), key); ost(A(E, 2), typ); uint32_t n = A(E, 4); uint8_t *d = malloc(n + 1); emu_read(E, A(E, 3), d, n); ensure_attrs(p); aerec_set_attr(p, key, typ, d, n); free(d); }
    return 0;
}
REG(AEPutAttributeDesc) {
    AERec *p = ae_desc_rec(W, A(E, 0));
    if (p) { char key[5], typ[5]; ost(A(E, 1), key); Bytes d = {0}; AERec *r = desc_value(W, A(E, 2), typ, &d); if (!r) { ensure_attrs(p); aerec_set_attr(p, key, typ, d.b, d.n); } bytes_free(&d); }
    return 0;
}
REG(AECreateAppleEvent) {
    char cls[5], id[5]; ost(A(E, 0), cls); ost(A(E, 1), id);
    AERec *p = aerec_new(); p->is_event = 1; memcpy(p->cls, cls, 5); memcpy(p->id, id, 5);
    if (A(E, 2)) { char typ[5]; Bytes d = {0}; AERec *r = desc_value(W, A(E, 2), typ, &d); if (!r) aerec_set_attr(p, "addr", typ, d.b, d.n); bytes_free(&d); }
    ae_new_desc(W, A(E, 5), p, "aevt");
    T(E, "AECreateAppleEvent", "'%s' '%s'", cls, id); return 0;
}
REG(AECreateDesc) {
    char typ[5]; ost(A(E, 0), typ); uint32_t n = A(E, 2); uint8_t *d = malloc(n + 1); emu_read(E, A(E, 1), d, n);
    new_simple(W, A(E, 3), typ, d, n, 0); free(d); return 0;
}
REG(AEDuplicateDesc) {
    char typ[5]; Bytes d = {0}; AERec *r = desc_value(W, A(E, 0), typ, &d);
    if (r) r = copy_rec(r, 1);
    write_desc(W, A(E, 1), typ, r, &d); bytes_free(&d); return 0;
}
REG(AECoerceDesc) {
    char typ[5], want[5]; ost(A(E, 1), want); Bytes d = {0}; AERec *r = desc_value(W, A(E, 0), typ, &d); int64_t res = 0;
    if (!strcmp(typ, want) || !strcmp(want, "****")) write_desc(W, A(E, 2), typ, r, &d);
    else if (!r) write_desc(W, A(E, 2), want, NULL, &d);
    else res = -1700 & 0xFFFF;
    bytes_free(&d); return res;
}
REG(AECreateList) {
    int is_rec = A(E, 2) & 0xFF; AERec *p = aerec_new(); p->is_list = !is_rec;
    ae_new_desc(W, A(E, 3), p, is_rec ? "reco" : "list"); return 0;
}
REG(AECountItems) {
    AERec *p = ae_desc_rec(W, A(E, 0));
    wr32(E, A(E, 1), p ? (uint32_t)p->n : 1); return 0;
}
static int64_t put_list(World *w, uint32_t addr, int32_t idx, const char *typ, AERec *r, const Bytes *d) {
    AERec *p = ae_desc_rec(w, addr);
    if (!p || !p->is_list) return errAEWrongDataType & 0xFFFF;
    AEItem *x;
    if (idx == 0 || idx > p->n) x = item_add(&p->it, &p->n, &p->cap); else x = &p->it[idx - 1];
    memcpy(x->key, "****", 5); memcpy(x->type, typ, 4); x->type[4] = 0; x->rec = r;
    if (r) bytes_set(&x->data, NULL, 0); else bytes_set(&x->data, d->b, d->n);
    return 0;
}
REG(AEPutPtr) {
    char typ[5]; ost(A(E, 2), typ); uint32_t n = A(E, 4); Bytes d = {0}; bytes_put(&d, NULL, n); emu_read(E, A(E, 3), d.b, n);
    int64_t r = put_list(W, A(E, 0), (int32_t)A(E, 1), typ, NULL, &d); bytes_free(&d); return r;
}
REG(AEPutDesc) {
    char typ[5]; Bytes d = {0}; AERec *r = desc_value(W, A(E, 2), typ, &d);
    int64_t res = put_list(W, A(E, 0), (int32_t)A(E, 1), typ, r, &d); bytes_free(&d); return res;
}
static AEItem *nth(World *w, uint32_t addr, int32_t idx, const char **key) {
    AERec *p = ae_desc_rec(w, addr); if (!p) return NULL;
    if (idx < 1 || idx > p->n) return NULL;
    *key = p->is_list ? "****" : p->it[idx - 1].key;
    return &p->it[idx - 1];
}
REG(AEGetNthPtr) {
    const char *k; AEItem *x = nth(W, A(E, 0), (int32_t)A(E, 1), &k);
    if (!x || x->rec) return errAEIllegalIndex & 0xFFFF;
    if (A(E, 3)) wr32(E, A(E, 3), ostype_i(k));
    if (A(E, 4)) wr32(E, A(E, 4), ostype_i(x->type[0] ? x->type : "TEXT"));
    uint32_t n = (uint32_t)x->data.n < A(E, 6) ? (uint32_t)x->data.n : A(E, 6); emu_write(E, A(E, 5), x->data.b, n);
    if (A(E, 7)) wr32(E, A(E, 7), (uint32_t)x->data.n);
    return 0;
}
REG(AEGetNthDesc) {
    const char *k; AEItem *x = nth(W, A(E, 0), (int32_t)A(E, 1), &k);
    if (!x) return errAEIllegalIndex & 0xFFFF;
    if (A(E, 3)) wr32(E, A(E, 3), ostype_i(k));
    write_desc(W, A(E, 4), x->type[0] ? x->type : "TEXT", x->rec, &x->data); return 0;
}

/* ----------------------------------------------------------------- send */
static void params_repr(const AERec *p, char *out, int outmax) {
    int k = 0; out[k++] = '{';
    for (int i = 0; i < p->n && k < outmax - 600; i++) {
        char kr[32], vr[1000]; py_repr_str(p->it[i].key, 4, kr, sizeof kr);
        if (p->it[i].rec) strcpy(vr, "{...}");
        else py_repr_bytes(p->it[i].data.b, p->it[i].data.n > 120 ? 120 : (int)p->it[i].data.n, vr, sizeof vr);
        k += snprintf(out + k, outmax - k, "%s%s: %s", i ? ", " : "", kr, vr);
    }
    out[k++] = '}'; out[k] = 0;
}
REG(AESend) {
    AERec *ev = ae_desc_rec(W, A(E, 0)); uint32_t reply_addr = A(E, 1), mode = A(E, 2);
    static AERec empty; if (!ev) ev = &empty;
    if (!memcmp(ev->cls, "SLMS", 4)) {
        AEItem *m = aerec_find(ev, "TMSG");
        char msg[4096] = ""; if (m && !m->rec) { size_t n = m->data.n < sizeof msg - 1 ? m->data.n : sizeof msg - 1; memcpy(msg, m->data.b, n); msg[n] = 0; }
        /* python: msg.rstrip().replace('\r', ' | ').replace('\n', ' | ') if msg.strip() */
        char u[16384]; macroman_to_utf8(msg, -1, u, sizeof u);
        int l = (int)strlen(u); while (l && (u[l - 1] == ' ' || u[l - 1] == '\t' || u[l - 1] == '\r' || u[l - 1] == '\n' || u[l - 1] == '\v' || u[l - 1] == '\f')) l--;
        u[l] = 0;
        int blank = 1; for (int i = 0; i < l; i++) if (!(u[i] == ' ' || u[i] == '\t' || u[i] == '\r' || u[i] == '\n' || u[i] == '\v' || u[i] == '\f')) blank = 0;
        if (!blank) {
            char o[32768]; int k = 0;
            for (int i = 0; i < l && k < (int)sizeof o - 8; i++) { if (u[i] == '\r' || u[i] == '\n') { memcpy(o + k, " | ", 3); k += 3; } else o[k++] = u[i]; }
            o[k] = 0; TLOG(E, "LOG| %s\n", o);
            if (W->on_log) W->on_log(W, o);
        }
        W->log_lines++;
    } else {
        char pr[8192]; params_repr(ev, pr, sizeof pr); T(E, "AESend", "'%s' '%s' %s", ev->cls, ev->id, pr);
    }
    AERec *rp = NULL; int err = 0;
    if (W->route) err = W->route(W, ev, mode, &rp);
    else if (W->ae_reply_for) rp = W->ae_reply_for(W, ev);
    if (err) return err & 0xFFFF;
    if (reply_addr) ae_new_desc(W, reply_addr, rp ? rp : aerec_new(), "aevt");
    return 0;
}
REG(AEDisposeDesc) {
    uint32_t a = A(E, 0);
    if (a) {
        uint32_t h = rd32(E, a + 4);
        desc_unmap(W, h);
        if (h >= AUX_MP && h < AUX_MP + 0x100000 && handle_rec(E, h)) dispose_handle(E, h);
        wr32(E, a, ostype_i("null")); wr32(E, a + 4, 0);
    }
    return 0;
}
REG(AEGetInteractionAllowed) { wr8(E, A(E, 0), 0); return 0; }
REG(zero) { (void)E; return 0; }

/* ---------------------------------------------------- Object Support Library */
static void osl_add(World *w, const char *want, const char *cont, uint32_t proc, uint32_t refcon) {
    for (int i = 0; i < w->nosl; i++) if (!memcmp(w->osl[i].want, want, 4) && !memcmp(w->osl[i].cont, cont, 4)) { w->osl[i].proc = proc; w->osl[i].refcon = refcon; return; }
    if (w->nosl == w->caposl) { w->caposl = w->caposl * 2 + 16; w->osl = realloc(w->osl, w->caposl * sizeof *w->osl); }
    memcpy(w->osl[w->nosl].want, want, 5); memcpy(w->osl[w->nosl].cont, cont, 5); w->osl[w->nosl].proc = proc; w->osl[w->nosl].refcon = refcon; w->nosl++;
}
static int osl_get(World *w, const char *want, const char *cont, uint32_t *proc, uint32_t *refcon) {
    for (int i = 0; i < w->nosl; i++) if (!memcmp(w->osl[i].want, want, 4) && !memcmp(w->osl[i].cont, cont, 4)) { *proc = w->osl[i].proc; *refcon = w->osl[i].refcon; return 1; }
    return 0;
}
REG(AEInstallObjectAccessor) {
    char want[5], cont[5]; ost(A(E, 0), want); ost(A(E, 1), cont); osl_add(W, want, cont, A(E, 2), A(E, 3));
    T(E, "AEInstallObjectAccessor", "'%s' from '%s' -> %08x", want, cont, A(E, 2)); return 0;
}
/* resolve object specifier record spec; returns OSErr and the token descriptor address */
static int osl_resolve(World *w, AERec *spec, uint32_t *tok) {
    Emu *E = w->E; uint32_t ctok;
    AEItem *from = aerec_find(spec, "from");
    if (from && from->rec) { int err = osl_resolve(w, from->rec, &ctok); if (err) return err; }
    else { ctok = new_ptr(E, 8, 1); new_simple(w, ctok, "null", NULL, 0, 1); }
    AEItem *wi = aerec_find(spec, "want"), *fi = aerec_find(spec, "form"), *si = aerec_find(spec, "seld");
    char want[5] = "    ", form[5] = "    ", ctype[5];
    if (wi && wi->data.n >= 4) memcpy(want, wi->data.b, 4);
    if (fi && fi->data.n >= 4) memcpy(form, fi->data.b, 4);
    ostype_s(rd32(E, ctok), ctype);
    uint32_t proc = 0, refcon = 0;
    int found = osl_get(w, want, ctype, &proc, &refcon) || osl_get(w, want, "****", &proc, &refcon) ||
                osl_get(w, "****", ctype, &proc, &refcon) || osl_get(w, "****", "****", &proc, &refcon);
    T(E, "AEResolve", "'%s' from '%s' form '%s' -> %s", want, ctype, form, found ? "accessor" : "none");
    if (!found) return -1728;
    uint32_t seld = new_ptr(E, 8, 1);
    if (si) write_desc(w, seld, si->type[0] ? si->type : "TEXT", si->rec, &si->data);
    else { Bytes z = {0}; write_desc(w, seld, "TEXT", NULL, &z); }
    uint32_t out = new_ptr(E, 8, 1);
    uint32_t args[7] = { ostype_i(want), ctok, ostype_i(ctype), ostype_i(form), seld, out, refcon };
    uint32_t r = emu_call_nested(E, proc, args, 7);
    *tok = out;
    return (int16_t)(r & 0xFFFF);
}
REG(AEResolve) {
    AERec *spec = ae_desc_rec(W, A(E, 0)); uint32_t out = A(E, 2);
    if (!spec) return -1728 & 0xFFFF;
    uint32_t tok = 0; int err = osl_resolve(W, spec, &tok);
    if (!err && tok) { uint8_t b[8]; emu_read(E, tok, b, 8); emu_write(E, out, b, 8); }
    T(E, "AEResolve", "-> err %d", err);
    return err & 0xFFFF;
}
REG(CreateObjSpecifier) {
    AERec *p = aerec_new(); p->is_objspec = 1; ae_new_desc(W, A(E, 5), p, "obj "); return 0;
}

void hle_ae_register(void) {
#define R(n) emu_reg(#n, h_##n)
    R(AEInstallEventHandler); R(AEGetEventHandler); R(AEProcessAppleEvent); R(AEGetParamPtr); R(AEGetParamDesc); R(AEPutParamPtr);
    R(AEPutParamDesc); R(AEDeleteParam); R(AEGetAttributePtr); R(AEGetAttributeDesc); R(AESizeOfAttribute); R(AEPutAttributePtr);
    R(AEPutAttributeDesc); R(AECreateAppleEvent); R(AECreateDesc); R(AEDuplicateDesc); R(AECoerceDesc); R(AECreateList);
    R(AECountItems); R(AEPutPtr); R(AEPutDesc); R(AEGetNthPtr); R(AEGetNthDesc); R(AESend); R(AEDisposeDesc);
    R(AEGetInteractionAllowed); R(AEInstallObjectAccessor); R(AEResolve); R(CreateObjSpecifier);
    static const char *z[] = { "AESetInteractionAllowed", "AEInteractWithUser", "AESuspendTheCurrentEvent", "AEResumeTheCurrentEvent",
        "AEObjectInit", "AESetObjectCallbacks", NULL };
    for (int i = 0; z[i]; i++) emu_reg(z[i], h_zero);
#undef R
}
