/* hle_ui.c - headless Mac Toolbox model for the Smart342(L) front end (MacApp 3) and the driver.
 * Port of scripts/mac_ui.py. Nothing is drawn; everything a program can read back is modelled with
 * real in-memory records (QuickDraw globals and ports, bounding-box regions, windows and the window
 * list, controls incl. the popup CDEF, MENU-format menus, DLOG/DITL dialogs, TextEdit records,
 * aliases). Layouts are from Inside Macintosh (Imaging With QuickDraw; Toolbox Essentials; Text). */
#include "mac.h"
#include "ui_tables.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define W (E->W)
#define U (W->ui)
#define REG(name) static int64_t h_##name(Emu *E)
#define S_TOP 0
#define S_LEFT 0
#define S_BOTTOM 768
#define S_RIGHT 1024
#define MBAR_H 20

typedef struct { int16_t t, l, b, r; } Rect;
static const Rect SCREEN = { 0, 0, 768, 1024 };

typedef struct { int16_t rect[4]; int type; Bytes data; uint32_t h; } DItem;
typedef struct { uint32_t d; DItem *it; int n; Bytes drawn[8]; int ndrawn; } Dlg;
typedef struct { uint32_t k, v; } KV;
typedef struct { uint32_t w; int t, l; } WPos;

typedef struct UI {
    uint32_t qd, port, ctab, wmgr_port, gd, gray, cur_gd, open_pic, te_scrap, scrap, helpmenu;
    int open_rgn_set; Rect open_rgn;
    KV *gwgd; int ngwgd, capgwgd;
    uint32_t *win; int nwin, capwin;          /* front to back */
    KV *var; int nvar, capvar;
    WPos *pos; int npos, cappos;
    uint32_t active;
    struct { uint32_t w; int act; } *aq; int naq, capaq;
    struct { int mid; uint32_t h; } *mbar, *hier; int nmbar, capmbar, nhier, caphier;
    KV *cproc; int ncproc, capcproc;           /* control handle -> procID */
    Dlg *dlg; int ndlg, capdlg;
    Bytes ptext[4];
    int hilite_mode;
} UI;

#define GROW(v, n, cap) do { if ((n) == (cap)) { (cap) = (cap) * 2 + 16; (v) = realloc((v), (cap) * sizeof *(v)); } } while (0)

/* ------------------------------------------------------------- helpers */
static int16_t s16(uint32_t v) { return (int16_t)(v & 0xFFFF); }
static int32_t s32(uint32_t v) { return (int32_t)v; }
static Rect rrect(Emu *E, uint32_t a) { Rect r = { rds16(E, a), rds16(E, a + 2), rds16(E, a + 4), rds16(E, a + 6) }; return r; }
static void wrect(Emu *E, uint32_t a, Rect r) { wr16(E, a, (uint16_t)r.t); wr16(E, a + 2, (uint16_t)r.l); wr16(E, a + 4, (uint16_t)r.b); wr16(E, a + 6, (uint16_t)r.r); }
static Rect R4(int t, int l, int b, int r) { Rect x = { (int16_t)t, (int16_t)l, (int16_t)b, (int16_t)r }; return x; }
static void rpt(uint32_t v, int *pv, int *ph) { *pv = s16(v >> 16); *ph = s16(v); }
static int empty(Rect r) { return r.b <= r.t || r.r <= r.l; }
static int req(Rect a, Rect b) { return a.t == b.t && a.l == b.l && a.b == b.b && a.r == b.r; }
static const char *rrepr(Rect r, char *o) { sprintf(o, "(%d, %d, %d, %d)", r.t, r.l, r.b, r.r); return o; }
static int live(Emu *E, uint32_t h) { return handle_rec(E, h) != NULL; }
static uint8_t *hdata(Emu *E, uint32_t h, uint32_t *n) { return handle_copy(E, h, n); }
static void hset(Emu *E, uint32_t h, const void *d, uint32_t n) { set_handle_size(E, h, n); if (n) emu_write(E, rd32(E, h), d, n); }
static uint32_t RES(World *w, const char *t, int id) { return get_res(w, ostype_i(t), id, 0, NULL); }
static void ost(uint32_t v, char *raw) { ostype_s(v, raw); }
static void repr_mr(const char *s, int n, char *o, int max) { py_repr_str(s, n, o, max); }

/* ================================================================ regions */
static uint32_t new_rgn(Emu *E, Rect r);
static Rect rgn_box(Emu *E, uint32_t h) { return rrect(E, rd32(E, h) + 2); }
static void set_rgn(Emu *E, uint32_t h, Rect r) {
    if (empty(r)) r = R4(0, 0, 0, 0);
    uint32_t p = rd32(E, h); wr16(E, p, 10); wrect(E, p + 2, r);
}
static uint32_t new_rgn(Emu *E, Rect r) { uint32_t h = new_handle(E, 10, NULL, 0); set_rgn(E, h, r); return h; }
static Rect runion(Rect a, Rect b) {
    if (empty(a)) return b;
    if (empty(b)) return a;
    return R4(a.t < b.t ? a.t : b.t, a.l < b.l ? a.l : b.l, a.b > b.b ? a.b : b.b, a.r > b.r ? a.r : b.r);
}
static Rect rsect(Rect a, Rect b) {
    Rect r = R4(a.t > b.t ? a.t : b.t, a.l > b.l ? a.l : b.l, a.b < b.b ? a.b : b.b, a.r < b.r ? a.r : b.r);
    return empty(r) ? R4(0, 0, 0, 0) : r;
}
REG(NewRgn) { return new_rgn(E, R4(0, 0, 0, 0)); }
REG(DisposeRgn) { dispose_handle(E, A(E, 0)); return NORET; }
REG(CopyRgn) { set_rgn(E, A(E, 1), rgn_box(E, A(E, 0))); return NORET; }
REG(SetEmptyRgn) { set_rgn(E, A(E, 0), R4(0, 0, 0, 0)); return NORET; }
REG(SetRectRgn) { set_rgn(E, A(E, 0), R4(s16(A(E, 2)), s16(A(E, 1)), s16(A(E, 4)), s16(A(E, 3)))); return NORET; }
REG(RectRgn) { set_rgn(E, A(E, 0), rrect(E, A(E, 1))); return NORET; }
REG(OpenRgn) { U->open_rgn_set = 1; U->open_rgn = R4(0, 0, 0, 0); return NORET; }
REG(CloseRgn) { set_rgn(E, A(E, 0), R4(0, 0, 0, 0)); U->open_rgn_set = 0; return NORET; }   /* open_rgn is always empty */
REG(OffsetRgn) {
    Rect b = rgn_box(E, A(E, 0)); int dh = s16(A(E, 1)), dv = s16(A(E, 2));
    if (!empty(b)) set_rgn(E, A(E, 0), R4(b.t + dv, b.l + dh, b.b + dv, b.r + dh));
    return NORET;
}
REG(InsetRgn) {
    Rect b = rgn_box(E, A(E, 0)); int dh = s16(A(E, 1)), dv = s16(A(E, 2));
    set_rgn(E, A(E, 0), R4(b.t + dv, b.l + dh, b.b - dv, b.r - dh)); return NORET;
}
REG(UnionRgn) { set_rgn(E, A(E, 2), runion(rgn_box(E, A(E, 0)), rgn_box(E, A(E, 1)))); return NORET; }
REG(SectRgn) { set_rgn(E, A(E, 2), rsect(rgn_box(E, A(E, 0)), rgn_box(E, A(E, 1)))); return NORET; }
REG(DiffRgn) {
    Rect a = rgn_box(E, A(E, 0)), b = rgn_box(E, A(E, 1)), s = rsect(a, b);
    /* exact when b covers a completely; otherwise the bounding box of a (over-approximation) */
    set_rgn(E, A(E, 2), (req(s, a) && !empty(a)) ? R4(0, 0, 0, 0) : a); return NORET;
}
REG(EmptyRgn) { return empty(rgn_box(E, A(E, 0))) ? 1 : 0; }
REG(EqualRgn) { return req(rgn_box(E, A(E, 0)), rgn_box(E, A(E, 1))) ? 1 : 0; }
REG(PtInRgn) { int v, h; rpt(A(E, 0), &v, &h); Rect b = rgn_box(E, A(E, 1)); return (b.t <= v && v < b.b && b.l <= h && h < b.r) ? 1 : 0; }
REG(RectInRgn) { return empty(rsect(rrect(E, A(E, 0)), rgn_box(E, A(E, 1)))) ? 0 : 1; }

/* =================================================================== rects */
REG(SetRect) { wrect(E, A(E, 0), R4(s16(A(E, 2)), s16(A(E, 1)), s16(A(E, 4)), s16(A(E, 3)))); return NORET; }
REG(OffsetRect) {
    uint32_t a = A(E, 0); int dh = s16(A(E, 1)), dv = s16(A(E, 2)); Rect r = rrect(E, a);
    wrect(E, a, R4(r.t + dv, r.l + dh, r.b + dv, r.r + dh)); return NORET;
}
REG(InsetRect) {
    uint32_t a = A(E, 0); int dh = s16(A(E, 1)), dv = s16(A(E, 2)); Rect r = rrect(E, a);
    Rect n = R4(r.t + dv, r.l + dh, r.b - dv, r.r - dh); wrect(E, a, empty(n) ? R4(0, 0, 0, 0) : n); return NORET;
}
REG(SectRect) { Rect s = rsect(rrect(E, A(E, 0)), rrect(E, A(E, 1))); wrect(E, A(E, 2), s); return empty(s) ? 0 : 1; }
REG(UnionRect) { wrect(E, A(E, 2), runion(rrect(E, A(E, 0)), rrect(E, A(E, 1)))); return NORET; }
REG(PtInRect) { int v, h; rpt(A(E, 0), &v, &h); Rect r = rrect(E, A(E, 1)); return (r.t <= v && v < r.b && r.l <= h && h < r.r) ? 1 : 0; }
REG(Pt2Rect) {
    int v1, h1, v2, h2; rpt(A(E, 0), &v1, &h1); rpt(A(E, 1), &v2, &h2);
    wrect(E, A(E, 2), R4(v1 < v2 ? v1 : v2, h1 < h2 ? h1 : h2, v1 > v2 ? v1 : v2, h1 > h2 ? h1 : h2)); return NORET;
}
REG(SetPt) { wr16(E, A(E, 0), A(E, 2)); wr16(E, A(E, 0) + 2, A(E, 1)); return NORET; }
REG(EqualRect) { return req(rrect(E, A(E, 0)), rrect(E, A(E, 1))) ? 1 : 0; }
REG(EmptyRect) { return empty(rrect(E, A(E, 0))) ? 1 : 0; }

/* ============================================================ QD globals */
static void set_port(Emu *E, uint32_t p) { U->port = p; if (U->qd) wr32(E, U->qd, p); }
REG(InitGraf) {
    uint32_t p = A(E, 0); U->qd = p;
    uint8_t pat[8];
    memset(pat, 0, 8); emu_write(E, p - 8, pat, 8);
    memset(pat, 0xFF, 8); emu_write(E, p - 16, pat, 8);
    for (int i = 0; i < 8; i += 2) { pat[i] = 0xAA; pat[i + 1] = 0x55; } emu_write(E, p - 24, pat, 8);
    for (int i = 0; i < 8; i += 2) { pat[i] = 0x88; pat[i + 1] = 0x22; } emu_write(E, p - 32, pat, 8);
    for (int i = 0; i < 8; i += 2) { pat[i] = 0x77; pat[i + 1] = 0xDD; } emu_write(E, p - 40, pat, 8);
    emu_zero(E, p - 108, 68);
    wr32(E, p - 122, 0); wr16(E, p - 118, 128); wrect(E, p - 116, SCREEN);
    wr32(E, p - 126, 1);
    wr32(E, p, U->wmgr_port);
    U->port = U->wmgr_port;
    T(E, "InitGraf", "&thePort=%08x", p);
    return NORET;
}
REG(SetPort) { set_port(E, A(E, 0)); return NORET; }
REG(GetPort) { wr32(E, A(E, 0), U->port); return NORET; }
REG(Random) {
    /* QuickDraw Random: randSeed = randSeed * 16807 mod (2^31 - 1); result is the low word, 0x8000 -> 0 */
    uint64_t seed = U->qd ? rd32(E, U->qd - 126) : 1;
    seed = (seed * 16807u) % 0x7FFFFFFFu;
    if (U->qd) wr32(E, U->qd - 126, (uint32_t)seed);
    uint32_t r = seed & 0xFFFF;
    return r == 0x8000 ? 0 : r;
}

/* ================================================================ ports */
static int is_color(Emu *E, uint32_t p) { return (rd16(E, p + 6) & 0xC000) == 0xC000; }
static void port_bounds_topleft(Emu *E, uint32_t p, int *t, int *l) {
    if (is_color(E, p)) { uint32_t pm = rd32(E, rd32(E, p + 2)); *t = rds16(E, pm + 6); *l = rds16(E, pm + 8); }
    else { *t = rds16(E, p + 2 + 6); *l = rds16(E, p + 2 + 8); }
}
static void set_port_origin_bounds(Emu *E, uint32_t p, Rect r) {
    if (is_color(E, p)) { uint32_t pm = rd32(E, rd32(E, p + 2)); wrect(E, pm + 6, r); }
    else wrect(E, p + 2 + 6, r);
}
static uint32_t new_pixmap(Emu *E, Rect bounds, int depth, uint32_t base, uint32_t rowbytes) {
    uint32_t h = new_handle(E, 50, NULL, 0), pm = rd32(E, h);
    wr32(E, pm, base); wr16(E, pm + 4, 0x8000 | rowbytes); wrect(E, pm + 6, bounds);
    wr16(E, pm + 14, 0); wr16(E, pm + 16, 0); wr32(E, pm + 18, 0);
    wr32(E, pm + 22, 72u << 16); wr32(E, pm + 26, 72u << 16);
    wr16(E, pm + 30, depth > 8 ? 16 : 0); wr16(E, pm + 32, (uint32_t)depth);
    wr16(E, pm + 34, depth > 8 ? 3 : 1); wr16(E, pm + 36, depth == 32 ? 8 : depth == 16 ? 5 : (uint32_t)depth);
    wr32(E, pm + 38, 0); wr32(E, pm + 42, U->ctab); wr32(E, pm + 46, 0);
    return h;
}
static uint32_t new_pixpat(Emu *E) {
    uint32_t h = new_handle(E, 28, NULL, 0), pp = rd32(E, h); wr16(E, pp, 0);   /* patType 0 = old-style */
    wr32(E, pp + 2, 0); wr32(E, pp + 6, new_handle(E, 8, NULL, 0));
    uint8_t ff[8]; memset(ff, 0xFF, 8); emu_write(E, pp + 20, ff, 8);
    return h;
}
static void init_cport(Emu *E, uint32_t p, Rect rect, uint32_t pixmap) {
    emu_zero(E, p, 108);
    wr16(E, p, 0);
    wr32(E, p + 2, pixmap ? pixmap : new_pixmap(E, SCREEN, 8, 0, 0));
    wr16(E, p + 6, 0xC000);
    wr32(E, p + 8, new_handle(E, 36, NULL, 0));       /* grafVars */
    wrect(E, p + 16, rect);
    wr32(E, p + 24, new_rgn(E, rect)); wr32(E, p + 28, new_rgn(E, R4(-32768, -32768, 32767, 32767)));
    wr32(E, p + 32, new_pixpat(E));
    emu_zero(E, p + 36, 6); { uint8_t ff[6]; memset(ff, 0xFF, 6); emu_write(E, p + 42, ff, 6); }
    wr32(E, p + 52, 0x00010001);
    wr16(E, p + 56, 8);                               /* patCopy */
    wr32(E, p + 58, new_pixpat(E)); wr32(E, p + 62, new_pixpat(E));
    wr16(E, p + 66, 0); wr16(E, p + 68, 0); wr16(E, p + 72, 1); wr16(E, p + 74, 0);
    wr32(E, p + 80, 33); wr32(E, p + 84, 30);
    wr16(E, p + 90, 0);
}
static void init_port(Emu *E, uint32_t p, Rect rect) {
    emu_zero(E, p, 108);
    wr32(E, p + 2, 0); wr16(E, p + 6, 128); wrect(E, p + 8, SCREEN);
    wrect(E, p + 16, rect);
    wr32(E, p + 24, new_rgn(E, rect)); wr32(E, p + 28, new_rgn(E, R4(-32768, -32768, 32767, 32767)));
    uint8_t z[8] = { 0 }, ff[8]; memset(ff, 0xFF, 8);
    emu_write(E, p + 32, z, 8); emu_write(E, p + 40, ff, 8); emu_write(E, p + 58, ff, 8);
    wr32(E, p + 52, 0x00010001); wr16(E, p + 56, 8); wr16(E, p + 72, 1);
    wr32(E, p + 80, 33); wr32(E, p + 84, 30);
}
REG(OpenCPort) { init_cport(E, A(E, 0), SCREEN, 0); set_port(E, A(E, 0)); return NORET; }
REG(OpenPort) { init_port(E, A(E, 0), SCREEN); set_port(E, A(E, 0)); return NORET; }
REG(CloseCPort) { uint32_t p = A(E, 0); dispose_handle(E, rd32(E, p + 24)); dispose_handle(E, rd32(E, p + 28)); return NORET; }
REG(GetWMgrPort) { wr32(E, A(E, 0), U->wmgr_port); return NORET; }
REG(PortSize) { uint32_t p = U->port; Rect r = rrect(E, p + 16); wrect(E, p + 16, R4(r.t, r.l, r.t + s16(A(E, 1)), r.l + s16(A(E, 0)))); return NORET; }
REG(MovePortTo) {
    uint32_t p = U->port; Rect r = rrect(E, p + 16); int gl = s16(A(E, 0)), gt = s16(A(E, 1)), bt, bl;
    port_bounds_topleft(E, p, &bt, &bl);
    /* bounds topLeft = portRect.topLeft - global position */
    set_port_origin_bounds(E, p, R4(r.t - gt, r.l - gl, r.t - gt + (S_BOTTOM - S_TOP), r.l - gl + (S_RIGHT - S_LEFT)));
    return NORET;
}
REG(SetOrigin) {
    uint32_t p = U->port; int h = s16(A(E, 0)), v = s16(A(E, 1));
    Rect r = rrect(E, p + 16); int dh = h - r.l, dv = v - r.t;
    wrect(E, p + 16, R4(v, h, r.b + dv, r.r + dh));
    int bt, bl; port_bounds_topleft(E, p, &bt, &bl);
    set_port_origin_bounds(E, p, R4(bt + dv, bl + dh, bt + dv + (S_BOTTOM - S_TOP), bl + dh + (S_RIGHT - S_LEFT)));
    uint32_t rg = rd32(E, p + 24); Rect b = rgn_box(E, rg);   /* visRgn moves with the origin */
    if (!empty(b)) set_rgn(E, rg, R4(b.t + dv, b.l + dh, b.b + dv, b.r + dh));
    return NORET;
}
REG(ClipRect) { set_rgn(E, rd32(E, U->port + 28), rrect(E, A(E, 0))); return NORET; }
REG(SetClip) { set_rgn(E, rd32(E, U->port + 28), rgn_box(E, A(E, 0))); return NORET; }
REG(GetClip) { set_rgn(E, A(E, 0), rgn_box(E, rd32(E, U->port + 28))); return NORET; }
REG(LocalToGlobal) {
    uint32_t a = A(E, 0); int bt, bl; port_bounds_topleft(E, U->port, &bt, &bl);
    wr16(E, a, (uint32_t)(rds16(E, a) - bt)); wr16(E, a + 2, (uint32_t)(rds16(E, a + 2) - bl)); return NORET;
}
REG(GlobalToLocal) {
    uint32_t a = A(E, 0); int bt, bl; port_bounds_topleft(E, U->port, &bt, &bl);
    wr16(E, a, (uint32_t)(rds16(E, a) + bt)); wr16(E, a + 2, (uint32_t)(rds16(E, a + 2) + bl)); return NORET;
}
REG(PenSize) { wr16(E, U->port + 52, A(E, 1)); wr16(E, U->port + 54, A(E, 0)); return NORET; }
REG(PenMode) { wr16(E, U->port + 56, A(E, 0)); return NORET; }
REG(PenNormal) { wr32(E, U->port + 52, 0x00010001); wr16(E, U->port + 56, 8); return NORET; }
REG(HidePen) { wr16(E, U->port + 66, (uint32_t)(rds16(E, U->port + 66) - 1)); return NORET; }
REG(ShowPen) { wr16(E, U->port + 66, (uint32_t)(rds16(E, U->port + 66) + 1)); return NORET; }
REG(MoveTo) { wr16(E, U->port + 48, A(E, 1)); wr16(E, U->port + 50, A(E, 0)); return NORET; }
REG(Move) {
    wr16(E, U->port + 48, (uint32_t)(rds16(E, U->port + 48) + s16(A(E, 1))));
    wr16(E, U->port + 50, (uint32_t)(rds16(E, U->port + 50) + s16(A(E, 0)))); return NORET;
}
REG(GetPenState) {
    uint32_t p = U->port, a = A(E, 0); uint8_t b[18];
    emu_read(E, p + 48, b, 4); emu_read(E, p + 52, b + 4, 4); emu_read(E, p + 56, b + 8, 2); emu_read(E, p + 58, b + 10, 8);
    emu_write(E, a, b, 18); return NORET;
}
REG(SetPenState) {
    uint32_t p = U->port; uint8_t d[18]; emu_read(E, A(E, 0), d, 18);
    emu_write(E, p + 48, d, 4); emu_write(E, p + 52, d + 4, 4); emu_write(E, p + 56, d + 8, 2); return NORET;
}
REG(TextFont) { wr16(E, U->port + 68, A(E, 0)); return NORET; }
REG(TextFace) { wr8(E, U->port + 70, A(E, 0)); return NORET; }
REG(TextMode) { wr16(E, U->port + 72, A(E, 0)); return NORET; }
REG(TextSize) { wr16(E, U->port + 74, A(E, 0)); return NORET; }
REG(ForeColor) { wr32(E, U->port + 80, A(E, 0)); return NORET; }
REG(BackColor) { wr32(E, U->port + 84, A(E, 0)); return NORET; }
REG(RGBForeColor) { if (is_color(E, U->port)) { uint8_t c[6]; emu_read(E, A(E, 0), c, 6); emu_write(E, U->port + 36, c, 6); } return NORET; }
REG(RGBBackColor) { if (is_color(E, U->port)) { uint8_t c[6]; emu_read(E, A(E, 0), c, 6); emu_write(E, U->port + 42, c, 6); } return NORET; }
REG(GetForeColor) { uint8_t c[6] = { 0 }; if (is_color(E, U->port)) emu_read(E, U->port + 36, c, 6); emu_write(E, A(E, 0), c, 6); return NORET; }
REG(GetBackColor) { uint8_t c[6]; memset(c, 0xFF, 6); if (is_color(E, U->port)) emu_read(E, U->port + 42, c, 6); emu_write(E, A(E, 0), c, 6); return NORET; }
/* text metrics: a fixed 6-pixel font (headless; values only steer layout) */
REG(StringWidth) { return 6 * rd8(E, A(E, 0)); }
REG(TextWidth) { return (uint32_t)(6 * s16(A(E, 2))); }
REG(CharWidth) { (void)E; return 6; }
REG(GetFontInfo) { uint32_t a = A(E, 0); wr16(E, a, 9); wr16(E, a + 2, 3); wr16(E, a + 4, 7); wr16(E, a + 6, 2); return NORET; }
REG(GetFNum) {
    char nm[300]; rd_pstr(E, A(E, 0), nm); int v = 0;
    static const struct { const char *n; int v; } F[] = { { "Chicago", 0 }, { "Geneva", 3 }, { "Monaco", 4 }, { "Helvetica", 21 },
        { "Times", 20 }, { "Courier", 22 }, { "New York", 2 } };
    for (size_t i = 0; i < sizeof F / sizeof *F; i++) if (!strcmp(nm, F[i].n)) v = F[i].v;
    wr16(E, A(E, 1), (uint32_t)v); return NORET;
}
REG(GetFontName) {
    int f = s16(A(E, 0)); wr_pstr(E, A(E, 1), f == 0 ? "Chicago" : f == 4 ? "Monaco" : "Geneva", -1); return NORET;
}
REG(zero) { (void)E; return 0; }
REG(one) { (void)E; return 1; }
REG(noop) { (void)E; return NORET; }
REG(GetAppFont) { (void)E; return 3; }
REG(GetDefFontSize) { (void)E; return 12; }
REG(SetStdProcs) { emu_zero(E, A(E, 0), 52); return NORET; }
REG(SetStdCProcs) { emu_zero(E, A(E, 0), 80); return NORET; }
REG(ScrollRect) { if (A(E, 3)) set_rgn(E, A(E, 3), R4(0, 0, 0, 0)); return NORET; }

/* ============================================================ pictures / polys */
REG(OpenPicture) {
    Rect r = rrect(E, A(E, 0)); uint32_t h = new_handle(E, 10, NULL, 0);
    wr16(E, rd32(E, h), 10); wrect(E, rd32(E, h) + 2, r); U->open_pic = h; return h;
}
REG(ClosePicture) { U->open_pic = 0; return NORET; }
REG(KillPicture) { uint32_t h = A(E, 0); if (!hle_handle_is_resource(W, h)) dispose_handle(E, h); return NORET; }
REG(OpenPoly) { uint32_t h = new_handle(E, 10, NULL, 0); wr16(E, rd32(E, h), 10); return h; }
REG(KillPoly) { dispose_handle(E, A(E, 0)); return NORET; }

/* ==================================================================== GWorlds */
static uint32_t gdev(Emu *E) {
    if (!U->gd) {
        uint32_t gd = new_handle(E, 62, NULL, 0), pm = new_pixmap(E, SCREEN, 8, 0, 0);
        uint32_t g = rd32(E, gd); wr16(E, g, 0); wr16(E, g + 2, 0); wr16(E, g + 4, 0);
        wr32(E, g + 22, pm); wr16(E, g + 20, 1 | 0x8000 | 0x2000);   /* gdFlags: color, screenActive, mainScreen */
        wrect(E, g + 34, SCREEN);
        U->gd = gd;
    }
    return U->gd;
}
static int kv_get(KV *v, int n, uint32_t k, uint32_t *out) { for (int i = 0; i < n; i++) if (v[i].k == k) { *out = v[i].v; return 1; } return 0; }
static void kv_set(KV **v, int *n, int *cap, uint32_t k, uint32_t val) {
    for (int i = 0; i < *n; i++) if ((*v)[i].k == k) { (*v)[i].v = val; return; }
    GROW(*v, *n, *cap); (*v)[*n].k = k; (*v)[*n].v = val; (*n)++;
}
static void kv_del(KV *v, int *n, uint32_t k) {
    for (int i = 0; i < *n; i++) if (v[i].k == k) { memmove(&v[i], &v[i + 1], (*n - i - 1) * sizeof *v); (*n)--; return; }
}
REG(NewGWorld) {
    uint32_t out = A(E, 0), rp = A(E, 2); int depth = s16(A(E, 1));
    Rect r = rrect(E, rp); int w = r.r - r.l, h = r.b - r.t;
    if (!depth) depth = 8;
    int64_t rb = (int64_t)w * depth + 31; rb = (rb >= 0 ? rb / 32 : -((-rb + 31) / 32)) * 4;   /* python floor division */
    uint32_t rowbytes = (uint32_t)rb;
    int64_t sz = rb * (h > 1 ? h : 1); if (sz < 16) sz = 16;
    uint32_t base = new_ptr(E, (uint32_t)sz, 0);
    uint32_t pm = new_pixmap(E, r, depth, base, rowbytes);
    uint32_t gw = new_ptr(E, 108, 0); init_cport(E, gw, r, pm);
    wr32(E, out, gw); { uint32_t g = gdev(E); kv_set(&U->gwgd, &U->ngwgd, &U->capgwgd, gw, g); }
    char rr[64]; T(E, "NewGWorld", "depth %d rect %s -> %08x (pixels %08x, rowBytes %d)", depth, rrepr(r, rr), gw, base, (int)rb);
    return 0;
}
REG(DisposeGWorld) {
    uint32_t gw = A(E, 0), pm = rd32(E, gw + 2), base = rd32(E, rd32(E, pm));
    if (base && ptr_size(E, base) >= 0) dispose_ptr(E, base);   /* python: try/except */
    return NORET;
}
REG(GetGWorldPixMap) { return rd32(E, A(E, 0) + 2); }
REG(GetPixBaseAddr) { return rd32(E, rd32(E, A(E, 0))); }
REG(GetGWorld) { wr32(E, A(E, 0), U->port); wr32(E, A(E, 1), U->cur_gd ? U->cur_gd : gdev(E)); return NORET; }
REG(SetGWorld) {
    set_port(E, A(E, 0));
    if (A(E, 1)) U->cur_gd = A(E, 1);
    else { uint32_t dflt = gdev(E), v; U->cur_gd = kv_get(U->gwgd, U->ngwgd, A(E, 0), &v) ? v : dflt; }
    return NORET;
}
REG(GetGDevice) { return U->cur_gd ? U->cur_gd : gdev(E); }
REG(SetGDevice) { U->cur_gd = A(E, 0); return NORET; }
REG(TestDeviceAttribute) { uint32_t a = A(E, 1); return (a == 0 || a == 13 || a == 15) ? 1 : 0; }
REG(GetCTSeed) { (void)E; return 1024; }
REG(gdev) { return gdev(E); }
REG(Index2Color) { emu_zero(E, A(E, 1), 6); return NORET; }
REG(GetGrayRgn) { if (!U->gray) U->gray = new_rgn(E, R4(MBAR_H, 0, S_BOTTOM, S_RIGHT)); return U->gray; }

/* ============================================================== patterns, cursors, icons */
REG(GetPattern) { uint8_t p[8]; for (int i = 0; i < 8; i += 2) { p[i] = 0xAA; p[i + 1] = 0x55; } return new_handle(E, 8, p, 0); }
REG(GetIndPattern) {
    uint32_t h = RES(W, "PAT#", s16(A(E, 1))); int idx = s16(A(E, 2)); uint8_t d[8] = { 0 };
    if (h) {
        uint32_t n; uint8_t *data = hdata(E, h, &n); int cnt = n >= 2 ? (data[0] << 8) | data[1] : 0;
        if (idx >= 1 && idx <= cnt) {
            /* python slice data[2+8(i-1) : 10+8(i-1)] (may be short) */
            uint32_t a = 2 + 8 * (idx - 1); memset(d, 0, 8);
            uint32_t m = a < n ? (n - a < 8 ? n - a : 8) : 0; memcpy(d, data + a, m);
            free(data); emu_write(E, A(E, 0), d, m); return NORET;
        }
        free(data);
    }
    emu_write(E, A(E, 0), d, 8); return NORET;
}
REG(new_pixpat) { return new_pixpat(E); }
REG(GetCursor) { uint32_t h = RES(W, "CURS", s16(A(E, 0))); return h ? h : new_handle(E, 68, NULL, 0); }
REG(GetCCursor) { return new_handle(E, 96, NULL, 0); }
REG(GetIconSuite) { wr32(E, A(E, 0), new_handle(E, 64, NULL, 0)); return 0; }
REG(GetCIcon) { return new_handle(E, 128, NULL, 0); }
REG(GetIcon) { uint32_t h = RES(W, "ICON", s16(A(E, 0))); return h ? h : new_handle(E, 128, NULL, 0); }

/* ============================================================================ windows */
static int win_index(UI *u, uint32_t w) { for (int i = 0; i < u->nwin; i++) if (u->win[i] == w) return i; return -1; }
static void win_insert(UI *u, int at, uint32_t w) {
    GROW(u->win, u->nwin, u->capwin);
    memmove(&u->win[at + 1], &u->win[at], (u->nwin - at) * sizeof(uint32_t)); u->win[at] = w; u->nwin++;
}
static void win_remove(UI *u, uint32_t w) { int i = win_index(u, w); if (i >= 0) { memmove(&u->win[i], &u->win[i + 1], (u->nwin - i - 1) * sizeof(uint32_t)); u->nwin--; } }
static uint32_t front_visible(Emu *E) { for (int i = 0; i < U->nwin; i++) if (rd8(E, U->win[i] + 110)) return U->win[i]; return 0; }
static void aq_push(UI *u, uint32_t w, int act) { GROW(u->aq, u->naq, u->capaq); u->aq[u->naq].w = w; u->aq[u->naq].act = act; u->naq++; }
static void relink(Emu *E) {
    for (int i = 0; i < U->nwin; i++) wr32(E, U->win[i] + 144, i + 1 < U->nwin ? U->win[i + 1] : 0);
    uint32_t fw = front_visible(E);
    if (fw != U->active) {
        if (U->active && win_index(U, U->active) >= 0) { aq_push(U, U->active, 0); wr8(E, U->active + 111, 0); }
        if (fw) { aq_push(U, fw, 1); wr8(E, fw + 111, 1); }
        U->active = fw;
    }
}
static int pos_get(UI *u, uint32_t w, int *t, int *l) {
    for (int i = 0; i < u->npos; i++) if (u->pos[i].w == w) { *t = u->pos[i].t; *l = u->pos[i].l; return 1; }
    *t = 0; *l = 0; return 0;
}
static void pos_set(UI *u, uint32_t w, int t, int l) {
    for (int i = 0; i < u->npos; i++) if (u->pos[i].w == w) { u->pos[i].t = t; u->pos[i].l = l; return; }
    GROW(u->pos, u->npos, u->cappos); u->pos[u->npos].w = w; u->pos[u->npos].t = t; u->pos[u->npos].l = l; u->npos++;
}
static void pos_del(UI *u, uint32_t w) {
    for (int i = 0; i < u->npos; i++) if (u->pos[i].w == w) { memmove(&u->pos[i], &u->pos[i + 1], (u->npos - i - 1) * sizeof *u->pos); u->npos--; return; }
}
static uint32_t win_of_port(Emu *E, uint32_t p) { return win_index(U, p) >= 0 ? p : 0; }
static Rect l2g(Emu *E, uint32_t p, Rect r) {
    int t, l; pos_get(U, p, &t, &l); int pt = rds16(E, p + 16), pl = rds16(E, p + 18);
    return R4(r.t - pt + t, r.l - pl + l, r.b - pt + t, r.r - pl + l);
}
static void inval(Emu *E, uint32_t w, Rect gr) {
    if (!w || empty(gr)) return;
    uint32_t h = rd32(E, w + 122); set_rgn(E, h, runion(rgn_box(E, h), gr));
}
static void inval_content(Emu *E, uint32_t w) { inval(E, w, l2g(E, w, rrect(E, w + 16))); }
REG(InvalRect) { uint32_t w = win_of_port(E, U->port); if (w) inval(E, w, l2g(E, w, rrect(E, A(E, 0)))); return NORET; }
REG(InvalRgn) { uint32_t w = win_of_port(E, U->port); if (w) inval(E, w, l2g(E, w, rgn_box(E, A(E, 0)))); return NORET; }
static void valid(Emu *E, uint32_t w, Rect gr) {
    uint32_t h = rd32(E, w + 122); Rect ub = rgn_box(E, h);
    if (empty(ub)) return;
    /* exact when the validated rectangle covers the update area; otherwise the update area stays */
    if (gr.t <= ub.t && gr.l <= ub.l && gr.b >= ub.b && gr.r >= ub.r) set_rgn(E, h, R4(0, 0, 0, 0));
}
REG(ValidRect) { uint32_t w = win_of_port(E, U->port); if (w) valid(E, w, l2g(E, w, rrect(E, A(E, 0)))); return NORET; }
REG(ValidRgn) { uint32_t w = win_of_port(E, U->port); if (w) valid(E, w, l2g(E, w, rgn_box(E, A(E, 0)))); return NORET; }
int ui_next_event(World *w, uint32_t mask, int remove, uint32_t *what, uint32_t *msg, uint32_t *mods) {
    Emu *E = w->E; UI *u = w->ui;
    if (u->naq && (mask & 0x0100)) {
        uint32_t ww = u->aq[0].w; int act = u->aq[0].act;
        if (remove) { memmove(&u->aq[0], &u->aq[1], (u->naq - 1) * sizeof *u->aq); u->naq--; }
        *what = 8; *msg = ww; *mods = act ? 1 : 0; return 1;
    }
    if (mask & 0x0040)
        for (int i = 0; i < u->nwin; i++) {
            uint32_t ww = u->win[i];
            if (rd8(E, ww + 110) && !empty(rgn_box(E, rd32(E, ww + 122)))) { *what = 6; *msg = ww; *mods = 0; return 1; }
        }
    return 0;
}
static uint32_t make_window(Emu *E, uint32_t storage, Rect bounds, const char *title, int tlen, int visible, int procid,
                            uint32_t behind, int goaway, uint32_t refcon, int kind, int size) {
    uint32_t w = storage ? storage : new_ptr(E, (uint32_t)size, 0);
    int t = bounds.t, l = bounds.l, b = bounds.b, r = bounds.r;
    init_cport(E, w, R4(0, 0, b - t, r - l), 0);
    /* bounds topLeft of the port's pixmap = -(global position) */
    uint32_t pm = rd32(E, rd32(E, w + 2)); wrect(E, pm + 6, R4(-t, -l, S_BOTTOM - t, S_RIGHT - l));
    emu_zero(E, w + 108, (uint32_t)(size - 108));
    wr16(E, w + 108, (uint32_t)kind); wr8(E, w + 110, visible ? 1 : 0); wr8(E, w + 111, 0); wr8(E, w + 112, goaway ? 1 : 0);
    wr32(E, w + 114, new_rgn(E, R4(t - 20, l - 1, b + 1, r + 1)));
    wr32(E, w + 118, new_rgn(E, R4(t, l, b, r)));
    wr32(E, w + 122, new_rgn(E, R4(0, 0, 0, 0)));
    /* windowDefProc = handle of WDEF (procID >> 4) as in the 32-bit Window Manager; variant kept aside */
    { int pid = procid >= 0 ? procid >> 4 : -((-procid + 15) >> 4); uint32_t h = RES(W, "WDEF", pid); if (!h) h = RES(W, "WDEF", 0); wr32(E, w + 126, h); }
    kv_set(&U->var, &U->nvar, &U->capvar, w, (uint32_t)(procid & 0xF));
    wr32(E, w + 130, 0);
    uint8_t tb[260]; tb[0] = (uint8_t)tlen; memcpy(tb + 1, title, tlen);
    uint32_t th = new_handle(E, (uint32_t)tlen + 1, tb, 0);
    wr32(E, w + 134, th); wr16(E, w + 138, (uint32_t)(6 * tlen)); wr32(E, w + 140, 0);
    wr32(E, w + 148, 0); wr32(E, w + 152, refcon);
    pos_set(U, w, t, l);
    if (behind == 0xFFFFFFFFu) win_insert(U, 0, w);
    else if (behind == 0 || win_index(U, behind) < 0) win_insert(U, U->nwin, w);
    else win_insert(U, win_index(U, behind) + 1, w);
    relink(E); set_port(E, w);
    if (rd8(E, w + 110)) inval_content(E, w);
    char tr[1100], br[64]; repr_mr(title, tlen, tr, sizeof tr);
    T(E, "NewWindow", "%s %s proc %d vis %d -> %08x", tr, rrepr(bounds, br), procid, visible, w);
    return w;
}
REG(GetNewCWindow) {
    int rid = s16(A(E, 0)); uint32_t storage = A(E, 1), behind = A(E, 2);
    uint32_t h = RES(W, "WIND", rid);
    if (!h) emu_fail(E, "WIND %d not found", rid);
    uint32_t n; uint8_t *d = hdata(E, h, &n);
    Rect r = R4((int16_t)(d[0] << 8 | d[1]), (int16_t)(d[2] << 8 | d[3]), (int16_t)(d[4] << 8 | d[5]), (int16_t)(d[6] << 8 | d[7]));
    int procid = (int16_t)(d[8] << 8 | d[9]), vis = (int16_t)(d[10] << 8 | d[11]), goaway = (int16_t)(d[12] << 8 | d[13]);
    uint32_t refcon = (uint32_t)d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17];
    int tl = d[18]; char title[256]; memcpy(title, d + 19, tl);
    free(d);
    int v = (vis & 0xFF00) ? (vis & 0xFF00) : (vis & 0xFF);
    return make_window(E, storage, r, title, tl, v, procid, behind, goaway, refcon, 8, 156);
}
REG(NewCWindow) {
    char title[256]; int tl = 0; if (A(E, 2)) tl = rd_pstr(E, A(E, 2), title);
    return make_window(E, A(E, 0), rrect(E, A(E, 1)), title, tl, A(E, 3) & 0xFF, s16(A(E, 4)), A(E, 5), A(E, 6) & 0xFF, A(E, 7), 8, 156);
}
static void close_window(Emu *E, uint32_t w) {
    if (win_index(U, w) >= 0) { win_remove(U, w); relink(E); }
    pos_del(U, w);
    if (U->port == w) set_port(E, U->wmgr_port);
}
REG(CloseWindow) { close_window(E, A(E, 0)); return NORET; }
REG(FrontWindow) { return front_visible(E); }
REG(LMGetWindowList) { return U->nwin ? U->win[0] : 0; }
static void bring_front(Emu *E, uint32_t w) { if (win_index(U, w) >= 0) { win_remove(U, w); win_insert(U, 0, w); relink(E); } }
REG(SelectWindow) { bring_front(E, A(E, 0)); return NORET; }
REG(SendBehind) {
    uint32_t w = A(E, 0), b = A(E, 1);
    if (win_index(U, w) >= 0) {
        win_remove(U, w);
        if (b && win_index(U, b) >= 0) win_insert(U, win_index(U, b) + 1, w); else win_insert(U, U->nwin, w);
        relink(E);
    }
    return NORET;
}
static void show(Emu *E, uint32_t w, int on) {
    int was = rd8(E, w + 110); wr8(E, w + 110, on ? 1 : 0);
    if (on && !was) inval_content(E, w);
    relink(E);
}
REG(ShowWindow) { show(E, A(E, 0), 1); return NORET; }
REG(HideWindow) { show(E, A(E, 0), 0); return NORET; }
REG(ShowHide) { show(E, A(E, 0), A(E, 1) & 0xFF); return NORET; }
REG(HiliteWindow) { wr8(E, A(E, 0) + 111, (A(E, 1) & 0xFF) ? 1 : 0); return NORET; }
REG(GetWRefCon) { return rd32(E, A(E, 0) + 152); }
REG(SetWRefCon) { wr32(E, A(E, 0) + 152, A(E, 1)); return NORET; }
REG(GetWTitle) {
    uint32_t th = rd32(E, A(E, 0) + 134);
    if (live(E, th)) {
        uint32_t n; uint8_t *d = hdata(E, th, &n);
        if (!n) emu_fail(E, "GetWTitle: empty title handle");
        uint32_t m = (uint32_t)d[0] + 1; if (m > n) m = n; emu_write(E, A(E, 1), d, m); free(d);
    } else wr8(E, A(E, 1), 0);
    return NORET;
}
REG(SetWTitle) {
    uint32_t w = A(E, 0); int len = rd8(E, A(E, 1)); uint8_t s[256]; emu_read(E, A(E, 1), s, (uint32_t)len + 1);
    uint32_t th = rd32(E, w + 134);
    if (live(E, th)) hset(E, th, s, (uint32_t)len + 1);
    return NORET;
}
REG(GetWVariant) { uint32_t v; return kv_get(U->var, U->nvar, A(E, 0), &v) ? v : 0; }
REG(SetWindowPic) { wr32(E, A(E, 0) + 148, A(E, 1)); return NORET; }
static Rect win_global_rect(Emu *E, uint32_t w) {
    int t, l; pos_get(U, w, &t, &l); Rect p = rrect(E, w + 16);
    return R4(t, l, t + p.b - p.t, l + p.r - p.l);
}
static void update_win_rgns(Emu *E, uint32_t w) {
    Rect g = win_global_rect(E, w);
    set_rgn(E, rd32(E, w + 118), g); set_rgn(E, rd32(E, w + 114), R4(g.t - 20, g.l - 1, g.b + 1, g.r + 1));
    uint32_t pm = rd32(E, rd32(E, w + 2)); int pt = rds16(E, w + 16), pl = rds16(E, w + 18);
    wrect(E, pm + 6, R4(pt - g.t, pl - g.l, pt - g.t + S_BOTTOM, pl - g.l + S_RIGHT));
}
REG(MoveWindow) {
    uint32_t w = A(E, 0); pos_set(U, w, s16(A(E, 2)), s16(A(E, 1))); update_win_rgns(E, w);
    if (A(E, 3) & 0xFF) bring_front(E, w);
    return NORET;
}
REG(SizeWindow) {
    uint32_t w = A(E, 0); Rect r = rrect(E, w + 16); wrect(E, w + 16, R4(r.t, r.l, r.t + s16(A(E, 2)), r.l + s16(A(E, 1))));
    set_rgn(E, rd32(E, w + 24), rrect(E, w + 16)); update_win_rgns(E, w); return NORET;
}
REG(FindWindow) { wr32(E, A(E, 1), 0); return 0; }   /* inDesk */
REG(DragGrayRgn) { (void)E; return 0x80008000u; }
REG(BeginUpdate) {
    uint32_t w = A(E, 0); char br[64]; T(E, "BeginUpdate", "%08x %s", w, rrepr(rgn_box(E, rd32(E, w + 122)), br));
    set_rgn(E, rd32(E, w + 122), R4(0, 0, 0, 0)); return NORET;
}

/* ============================================================================ menus */
/* MenuInfo: menuID(2) menuWidth(2) menuHeight(2) menuProc(4) enableFlags(4) title(pstr) items...
 * item: text(pstr) icon(1) keyEquiv(1) mark(1) style(1); list ends with a zero length byte. */
typedef struct { uint8_t txt[256]; int ntxt; int f[5]; } MItem;     /* f[1] icon f[2] key f[3] mark f[4] style */
typedef struct { int mid, mw, mh; uint32_t proc, en; uint8_t title[256]; int ntitle; MItem *it; int n, cap; } Menu;
static void menu_parse(Emu *E, uint32_t h, Menu *m) {
    uint32_t n; uint8_t *d = hdata(E, h, &n); memset(m, 0, sizeof *m);
    if (n < 15) emu_fail(E, "menu handle %08x too short (%u)", h, n);
    m->mid = (int16_t)(d[0] << 8 | d[1]); m->mw = (int16_t)(d[2] << 8 | d[3]); m->mh = (int16_t)(d[4] << 8 | d[5]);
    m->proc = (uint32_t)d[6] << 24 | d[7] << 16 | d[8] << 8 | d[9]; m->en = (uint32_t)d[10] << 24 | d[11] << 16 | d[12] << 8 | d[13];
    int tl = d[14]; m->ntitle = tl; memcpy(m->title, d + 15, tl); uint32_t p = 15 + tl;
    while (p < n && d[p]) {
        int k = d[p]; GROW(m->it, m->n, m->cap); MItem *x = &m->it[m->n++]; memset(x, 0, sizeof *x);
        if (p + 1 + k + 4 > n) emu_fail(E, "truncated MENU item");
        memcpy(x->txt, d + p + 1, k); x->ntxt = k; p += 1 + k;
        x->f[1] = d[p]; x->f[2] = d[p + 1]; x->f[3] = d[p + 2]; x->f[4] = d[p + 3]; p += 4;
    }
    free(d);
}
static void menu_build(Emu *E, uint32_t h, Menu *m) {
    uint8_t *o = malloc(16 + 256 + (size_t)m->n * 262); size_t k = 0;
    o[k++] = m->mid >> 8; o[k++] = m->mid; o[k++] = m->mw >> 8; o[k++] = m->mw; o[k++] = m->mh >> 8; o[k++] = m->mh;
    o[k++] = m->proc >> 24; o[k++] = m->proc >> 16; o[k++] = m->proc >> 8; o[k++] = m->proc;
    o[k++] = m->en >> 24; o[k++] = m->en >> 16; o[k++] = m->en >> 8; o[k++] = m->en;
    o[k++] = (uint8_t)m->ntitle; memcpy(o + k, m->title, m->ntitle); k += m->ntitle;
    for (int i = 0; i < m->n; i++) {
        o[k++] = (uint8_t)m->it[i].ntxt; memcpy(o + k, m->it[i].txt, m->it[i].ntxt); k += m->it[i].ntxt;
        o[k++] = (uint8_t)m->it[i].f[1]; o[k++] = (uint8_t)m->it[i].f[2]; o[k++] = (uint8_t)m->it[i].f[3]; o[k++] = (uint8_t)m->it[i].f[4];
    }
    o[k++] = 0;
    hset(E, h, o, (uint32_t)k); free(o); free(m->it); m->it = NULL; m->n = m->cap = 0;
}
static void menu_free(Menu *m) { free(m->it); m->it = NULL; }
static uint32_t new_menu(Emu *E, int mid, const uint8_t *title, int tl) {
    uint32_t h = new_handle(E, 1, NULL, 0); Menu m; memset(&m, 0, sizeof m);
    m.mid = mid; m.en = 0xFFFFFFFFu; memcpy(m.title, title, tl); m.ntitle = tl; menu_build(E, h, &m); return h;
}
REG(NewMenu) { uint8_t t[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1) + 1, t, n); return new_menu(E, s16(A(E, 0)), t, n); }
REG(GetMenu) {
    int rid = s16(A(E, 0)); uint32_t h = RES(W, "MENU", rid);
    if (h) T(E, "GetMenu", "%d -> %08x", rid, h);
    return h;
}
static void mlist_del_h(Emu *E, uint32_t h) {
    for (int i = 0; i < U->nmbar;) if (U->mbar[i].h == h) { memmove(&U->mbar[i], &U->mbar[i + 1], (U->nmbar - i - 1) * sizeof *U->mbar); U->nmbar--; } else i++;
}
REG(DisposeMenu) { uint32_t h = A(E, 0); mlist_del_h(E, h); if (!hle_handle_is_resource(W, h)) dispose_handle(E, h); return NORET; }
static void hier_add(UI *u, int mid, uint32_t h) {
    for (int i = 0; i < u->nhier; i++) if (u->hier[i].h == h) return;
    GROW(u->hier, u->nhier, u->caphier); u->hier[u->nhier].mid = mid; u->hier[u->nhier].h = h; u->nhier++;
}
REG(InsertMenu) {
    uint32_t h = A(E, 0); int before = s16(A(E, 1)), mid = s16(rd16(E, rd32(E, h)));
    if (before == -1) { hier_add(U, mid, h); return NORET; }
    for (int i = 0; i < U->nmbar; i++) if (U->mbar[i].mid == mid) return NORET;
    int idx = U->nmbar; for (int i = 0; i < U->nmbar; i++) if (U->mbar[i].mid == before) { idx = i; break; }
    GROW(U->mbar, U->nmbar, U->capmbar); memmove(&U->mbar[idx + 1], &U->mbar[idx], (U->nmbar - idx) * sizeof *U->mbar);
    U->mbar[idx].mid = mid; U->mbar[idx].h = h; U->nmbar++;
    return NORET;
}
REG(DeleteMenu) {
    int mid = s16(A(E, 0));
    for (int i = 0; i < U->nmbar;) if (U->mbar[i].mid == mid) { memmove(&U->mbar[i], &U->mbar[i + 1], (U->nmbar - i - 1) * sizeof *U->mbar); U->nmbar--; } else i++;
    for (int i = 0; i < U->nhier;) if (U->hier[i].mid == mid) { memmove(&U->hier[i], &U->hier[i + 1], (U->nhier - i - 1) * sizeof *U->hier); U->nhier--; } else i++;
    return NORET;
}
REG(ClearMenuBar) { U->nmbar = 0; return NORET; }
REG(GetMenuHandle) {
    int mid = s16(A(E, 0));
    for (int i = 0; i < U->nmbar; i++) if (U->mbar[i].mid == mid) return U->mbar[i].h;
    for (int i = 0; i < U->nhier; i++) if (U->hier[i].mid == mid) return U->hier[i].h;
    return 0;
}
REG(CalcMenuSize) {
    uint32_t h = A(E, 0); Menu m; menu_parse(E, h, &m);
    int mx = 40; for (int i = 0; i < m.n; i++) if (6 * m.it[i].ntxt + 40 > mx) mx = 6 * m.it[i].ntxt + 40;
    m.mw = mx; m.mh = 16 * m.n; menu_build(E, h, &m); return NORET;
}
REG(CountMItems) { if (!A(E, 0)) return 0; Menu m; menu_parse(E, A(E, 0), &m); int n = m.n; menu_free(&m); return (uint32_t)n; }
REG(GetMenuItemText) {
    Menu m; menu_parse(E, A(E, 0), &m); int i = s16(A(E, 1)); uint8_t b[256]; int n = 0;
    if (i >= 1 && i <= m.n) { n = m.it[i - 1].ntxt; memcpy(b + 1, m.it[i - 1].txt, n); }
    b[0] = (uint8_t)n; emu_write(E, A(E, 2), b, (uint32_t)n + 1); menu_free(&m); return NORET;
}
REG(SetMenuItemText) {
    uint32_t h = A(E, 0); int i = s16(A(E, 1)); Menu m; menu_parse(E, h, &m);
    if (i >= 1 && i <= m.n) { int n = rd8(E, A(E, 2)); emu_read(E, A(E, 2) + 1, m.it[i - 1].txt, n); m.it[i - 1].ntxt = n; menu_build(E, h, &m); }
    else menu_free(&m);
    return NORET;
}
static int64_t set_field(Emu *E, int k) {
    uint32_t h = A(E, 0); int i = s16(A(E, 1)); Menu m; menu_parse(E, h, &m);
    if (i >= 1 && i <= m.n) { m.it[i - 1].f[k] = A(E, 2) & 0xFF; menu_build(E, h, &m); } else menu_free(&m);
    return NORET;
}
REG(SetItemMark) { return set_field(E, 3); }
REG(SetItemCmd) { return set_field(E, 2); }
REG(SetItemIcon) { return set_field(E, 1); }
REG(SetItemStyle) { return set_field(E, 4); }
static int64_t get_field(Emu *E, int k) {
    Menu m; menu_parse(E, A(E, 0), &m); int i = s16(A(E, 1));
    wr16(E, A(E, 2), (i >= 1 && i <= m.n) ? (uint32_t)m.it[i - 1].f[k] : 0); menu_free(&m); return NORET;
}
REG(GetItemMark) { return get_field(E, 3); }
REG(GetItemCmd) { return get_field(E, 2); }
REG(GetItemIcon) { return get_field(E, 1); }
REG(GetItemStyle) { Menu m; menu_parse(E, A(E, 0), &m); int i = s16(A(E, 1)); wr8(E, A(E, 2), (i >= 1 && i <= m.n) ? (uint32_t)m.it[i - 1].f[4] : 0); menu_free(&m); return NORET; }
REG(CheckItem) {
    uint32_t h = A(E, 0); int i = s16(A(E, 1)); Menu m; menu_parse(E, h, &m);
    if (i >= 1 && i <= m.n) { m.it[i - 1].f[3] = (A(E, 2) & 0xFF) ? 0x12 : 0; menu_build(E, h, &m); } else menu_free(&m);
    return NORET;
}
static int64_t enable(Emu *E, int on) {
    uint32_t h = A(E, 0); int i = s16(A(E, 1));
    if (!h) return NORET;
    Menu m; menu_parse(E, h, &m);
    if (i < 0) emu_fail(E, "Enable/DisableItem with negative item %d", i);   /* python: 1 << negative raises */
    if (i < 32) {
        if (on) m.en |= (1u << i); else m.en &= ~(1u << i);
        menu_build(E, h, &m);
    } else menu_free(&m);
    return NORET;
}
REG(EnableItem) { return enable(E, 1); }
REG(DisableItem) { return enable(E, 0); }
/* AppendMenu metacharacters: ; separates items, ^icon !mark <style /key ( disabled */
static void insert_items(Emu *E, uint32_t h, const uint8_t *s, int ns, int after) {
    Menu m; menu_parse(E, h, &m); if (after > m.n) after = m.n; if (after < 0) after = 0; int pos = after;
    int i0 = 0;
    for (int i = 0; i <= ns; i++) {
        if (i < ns && s[i] != ';') continue;
        const uint8_t *part = s + i0; int np = i - i0; i0 = i + 1;
        if (!np) continue;
        MItem it; memset(&it, 0, sizeof it); int dis = 0;
        for (int j = 0; j < np;) {
            int c = part[j];
            if (c == '^' && j + 1 < np) { it.f[1] = (part[j + 1] - 48) & 0xFF; j += 2; continue; }
            if (c == '!' && j + 1 < np) { it.f[3] = part[j + 1]; j += 2; continue; }
            if (c == '<' && j + 1 < np) {
                int ch = part[j + 1]; it.f[4] |= ch == 'B' ? 1 : ch == 'I' ? 2 : ch == 'U' ? 4 : ch == 'O' ? 8 : ch == 'S' ? 16 : 0; j += 2; continue;
            }
            if (c == '/' && j + 1 < np) { it.f[2] = part[j + 1]; j += 2; continue; }
            if (c == '(' && j == 0) { dis = 1; j += 1; continue; }
            it.txt[it.ntxt++] = (uint8_t)c; j++;
        }
        GROW(m.it, m.n, m.cap); memmove(&m.it[pos + 1], &m.it[pos], (m.n - pos) * sizeof(MItem)); m.it[pos] = it; m.n++; pos++;
        /* shift enable bits above the insertion point and set this item's bit */
        uint64_t en = m.en, low, high, bit;
        if (pos < 32) { low = en & ((1ull << pos) - 1); high = (en >> pos) << (pos + 1); bit = dis ? 0 : (1ull << pos); }
        else { low = en; high = 0; bit = 0; }
        m.en = (uint32_t)((low | high | bit) & 0xFFFFFFFFu);
    }
    menu_build(E, h, &m);
}
REG(AppendMenu) { uint8_t s[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1) + 1, s, n); insert_items(E, A(E, 0), s, n, 9999); return NORET; }
REG(InsertMenuItem) { uint8_t s[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1) + 1, s, n); insert_items(E, A(E, 0), s, n, s16(A(E, 2))); return NORET; }
REG(DeleteMenuItem) {
    uint32_t h = A(E, 0); int i = s16(A(E, 1)); Menu m; menu_parse(E, h, &m);
    if (i >= 1 && i <= m.n) {
        memmove(&m.it[i - 1], &m.it[i], (m.n - i) * sizeof(MItem)); m.n--;
        uint64_t en = m.en, lo = i < 64 ? en & ((1ull << i) - 1) : en, hi = (i + 1 < 64) ? (en >> (i + 1)) << i : 0;
        m.en = (uint32_t)((lo | hi) & 0xFFFFFFFFu);
        menu_build(E, h, &m);
    } else menu_free(&m);
    return NORET;
}
REG(GetMBarHeight) { (void)E; return MBAR_H; }

/* ============================================================================ controls */
/* ControlRecord (handle): nextControl 0, contrlOwner 4, contrlRect 8, contrlVis 16, contrlHilite 17,
 * contrlValue 18, contrlMin 20, contrlMax 22, contrlDefProc 24, contrlData 28, contrlAction 32,
 * contrlRfCon 36, contrlTitle 40 (Str255). Size 296. */
static uint32_t new_control(Emu *E, uint32_t owner, Rect rect, const char *title, int tlen, int vis, int val, int mn, int mx, int proc, uint32_t ref) {
    uint32_t h = new_handle(E, 296, NULL, 0), c = rd32(E, h);
    wr32(E, c, 0); wr32(E, c + 4, owner); wrect(E, c + 8, rect); wr8(E, c + 16, vis ? 0xFF : 0); wr8(E, c + 17, 0);
    wr16(E, c + 18, (uint32_t)val); wr16(E, c + 20, (uint32_t)mn); wr16(E, c + 22, (uint32_t)mx);
    { int pid = proc >= 0 ? proc >> 4 : -((-proc + 15) >> 4); uint32_t d = RES(W, "CDEF", pid); if (!d) d = RES(W, "CDEF", 0); wr32(E, c + 24, d); }
    wr32(E, c + 28, 0); wr32(E, c + 32, 0); wr32(E, c + 36, ref);
    if (tlen > 255) tlen = 255;
    uint8_t tb[256]; tb[0] = (uint8_t)tlen; memcpy(tb + 1, title, tlen); emu_write(E, c + 40, tb, (uint32_t)tlen + 1);
    kv_set(&U->cproc, &U->ncproc, &U->capcproc, h, (uint32_t)proc);
    int pid = proc >= 0 ? proc >> 4 : -((-proc + 15) >> 4);
    if (pid == 63) {      /* popupMenuProc (1008..1015): min = MENU id, after init min 1 max nItems */
        int mid = mn; uint32_t mh = 0;
        for (int i = 0; i < U->nmbar; i++) if (U->mbar[i].mid == mid) mh = U->mbar[i].h;
        for (int i = 0; i < U->nhier; i++) if (U->hier[i].mid == mid) mh = U->hier[i].h;
        if (!mh) { mh = RES(W, "MENU", mid); if (mh) hier_add(U, mid, mh); }
        int n = 0; if (mh) { Menu m; menu_parse(E, mh, &m); n = m.n; menu_free(&m); }
        uint32_t pd = new_handle(E, 6, NULL, 0); wr32(E, rd32(E, pd), mh); wr16(E, rd32(E, pd) + 4, (uint32_t)mid);
        wr32(E, c + 28, pd); wr16(E, c + 20, 1); wr16(E, c + 22, (uint32_t)n); wr16(E, c + 18, n ? 1 : 0);
    }
    if (owner) { wr32(E, c, rd32(E, owner + 140)); wr32(E, owner + 140, h); }   /* link at the head of the window's list */
    char tr[1100]; repr_mr(title, tlen, tr, sizeof tr);
    T(E, "NewControl", "%s proc %d val %d [%d,%d] -> %08x", tr, proc, rds16(E, c + 18), rds16(E, c + 20), rds16(E, c + 22), h);
    return h;
}
REG(NewControl) {
    char title[256]; int tl = 0; if (A(E, 2)) tl = rd_pstr(E, A(E, 2), title);
    return new_control(E, A(E, 0), rrect(E, A(E, 1)), title, tl, A(E, 3) & 0xFF, s16(A(E, 4)), s16(A(E, 5)), s16(A(E, 6)), s16(A(E, 7)), A(E, 8));
}
static void cntl_fields(const uint8_t *d, int *val, int *vis, int *mx, int *mn, int *proc, uint32_t *ref) {
    *val = (int16_t)(d[8] << 8 | d[9]); *vis = (int16_t)(d[10] << 8 | d[11]); *mx = (int16_t)(d[12] << 8 | d[13]);
    *mn = (int16_t)(d[14] << 8 | d[15]); *proc = (int16_t)(d[16] << 8 | d[17]); *ref = (uint32_t)d[18] << 24 | d[19] << 16 | d[20] << 8 | d[21];
}
REG(GetNewControl) {
    int rid = s16(A(E, 0)); uint32_t owner = A(E, 1), h = RES(W, "CNTL", rid);
    if (!h) return 0;
    uint32_t n; uint8_t *d = hdata(E, h, &n);
    Rect r = R4((int16_t)(d[0] << 8 | d[1]), (int16_t)(d[2] << 8 | d[3]), (int16_t)(d[4] << 8 | d[5]), (int16_t)(d[6] << 8 | d[7]));
    int val, vis, mx, mn, proc; uint32_t ref; cntl_fields(d, &val, &vis, &mx, &mn, &proc, &ref);
    char title[256]; int tl = d[22]; memcpy(title, d + 23, tl); free(d);
    return new_control(E, owner, r, title, tl, vis, val, mn, mx, proc, ref);
}
static uint32_t cp_(Emu *E, uint32_t h) { return rd32(E, h); }
REG(GetControlValue) { return rd16(E, cp_(E, A(E, 0)) + 18); }
REG(GetControlMinimum) { return rd16(E, cp_(E, A(E, 0)) + 20); }
REG(GetControlMaximum) { return rd16(E, cp_(E, A(E, 0)) + 22); }
REG(SetControlValue) {
    uint32_t c = cp_(E, A(E, 0)); int v = s16(A(E, 1)), mn = rds16(E, c + 20), mx = rds16(E, c + 22);
    if (v > mx) v = mx; if (v < mn) v = mn;
    wr16(E, c + 18, (uint32_t)v); return NORET;
}
REG(SetControlMinimum) {
    uint32_t c = cp_(E, A(E, 0)); int mn = s16(A(E, 1)); wr16(E, c + 20, (uint32_t)mn);
    if (rds16(E, c + 18) < mn) wr16(E, c + 18, (uint32_t)mn);
    return NORET;
}
REG(SetControlMaximum) {
    uint32_t c = cp_(E, A(E, 0)); int mx = s16(A(E, 1)); wr16(E, c + 22, (uint32_t)mx);
    if (rds16(E, c + 18) > mx) wr16(E, c + 18, (uint32_t)mx);
    return NORET;
}
REG(GetControlReference) { return rd32(E, cp_(E, A(E, 0)) + 36); }
REG(SetControlReference) { wr32(E, cp_(E, A(E, 0)) + 36, A(E, 1)); return NORET; }
REG(GetControlTitle) { uint32_t c = cp_(E, A(E, 0)); uint8_t b[256]; int n = rd8(E, c + 40); emu_read(E, c + 40, b, (uint32_t)n + 1); emu_write(E, A(E, 1), b, (uint32_t)n + 1); return NORET; }
REG(SetControlTitle) { uint32_t c = cp_(E, A(E, 0)); uint8_t b[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1), b, (uint32_t)n + 1); emu_write(E, c + 40, b, (uint32_t)n + 1); return NORET; }
REG(GetControlVariant) { uint32_t v; return (kv_get(U->cproc, U->ncproc, A(E, 0), &v) ? v : 0) & 0xF; }
REG(HiliteControl) { wr8(E, cp_(E, A(E, 0)) + 17, A(E, 1)); return NORET; }
REG(ShowControl) { wr8(E, cp_(E, A(E, 0)) + 16, 0xFF); return NORET; }
REG(HideControl) { wr8(E, cp_(E, A(E, 0)) + 16, 0); return NORET; }
REG(MoveControl) {
    uint32_t c = cp_(E, A(E, 0)); Rect r = rrect(E, c + 8); int h = s16(A(E, 1)), v = s16(A(E, 2));
    wrect(E, c + 8, R4(v, h, v + r.b - r.t, h + r.r - r.l)); return NORET;
}
REG(SizeControl) { uint32_t c = cp_(E, A(E, 0)); Rect r = rrect(E, c + 8); wrect(E, c + 8, R4(r.t, r.l, r.t + s16(A(E, 2)), r.l + s16(A(E, 1)))); return NORET; }
static void unlink_ctl(Emu *E, uint32_t h) {
    uint32_t c = cp_(E, h), owner = rd32(E, c + 4);
    if (owner) {
        uint32_t prev = 0, cur = rd32(E, owner + 140);
        while (cur) {
            uint32_t nxt = rd32(E, rd32(E, cur));
            if (cur == h) { if (prev) wr32(E, rd32(E, prev), nxt); else wr32(E, owner + 140, nxt); break; }
            prev = cur; cur = nxt;
        }
    }
}
REG(DisposeControl) { uint32_t h = A(E, 0); unlink_ctl(E, h); kv_del(U->cproc, &U->ncproc, h); dispose_handle(E, h); return NORET; }
REG(KillControls) {
    uint32_t w = A(E, 0), cur = rd32(E, w + 140);
    while (cur) { uint32_t nxt = rd32(E, rd32(E, cur)); kv_del(U->cproc, &U->ncproc, cur); dispose_handle(E, cur); cur = nxt; }
    wr32(E, w + 140, 0); return NORET;
}
REG(FindControl) { wr32(E, A(E, 2), 0); return 0; }
REG(GetAuxiliaryControlRecord) { wr32(E, A(E, 1), 0); return 0; }   /* false: no auxiliary record */

/* ============================================================================ dialogs */
/* DialogRecord = WindowRecord(156) + items(156) textH(160) editField(164) editOpen(166) aDefItem(168); 170 bytes.
 * Item list (handle) = count-1 (2) then per item: handle/proc(4) rect(8) type(1) data(len+bytes, padded even). */
static DItem *parse_ditl(const uint8_t *d, uint32_t nd, int *n) {
    if (nd < 2) { *n = 0; return NULL; }
    int cnt = (int16_t)(d[0] << 8 | d[1]) + 1; if (cnt < 0) cnt = 0;
    DItem *out = calloc(cnt + 1, sizeof(DItem)); uint32_t p = 2;
    for (int i = 0; i < cnt; i++) {
        if (p + 14 > nd) { free(out); *n = -1; return NULL; }
        for (int k = 0; k < 4; k++) out[i].rect[k] = (int16_t)(d[p + 4 + 2 * k] << 8 | d[p + 5 + 2 * k]);
        out[i].type = d[p + 12]; int ln = d[p + 13];
        uint32_t m = p + 14 + ln <= nd ? (uint32_t)ln : nd - (p + 14);
        out[i].data = bytes_dup(d + p + 14, m); out[i].h = 0;
        p += 14 + ln + (ln & 1);
    }
    *n = cnt; return out;
}
static Bytes subst(Emu *E, const Bytes *t) {
    Bytes cur = bytes_dup(t->b, t->n);
    for (int i = 0; i < 4; i++) {
        char pat[3] = { '^', (char)('0' + i), 0 }; Bytes nx = { 0 };
        for (size_t j = 0; j < cur.n;) {
            if (j + 1 < cur.n && cur.b[j] == '^' && cur.b[j + 1] == (uint8_t)pat[1]) { bytes_put(&nx, U->ptext[i].b, U->ptext[i].n); j += 2; }
            else { bytes_put(&nx, cur.b + j, 1); j++; }
        }
        bytes_free(&cur); cur = nx;
    }
    return cur;
}
static Dlg *dlg_get(UI *u, uint32_t d) { for (int i = 0; i < u->ndlg; i++) if (u->dlg[i].d == d) return &u->dlg[i]; return NULL; }
static void dlg_pop(UI *u, uint32_t d) {
    for (int i = 0; i < u->ndlg; i++) if (u->dlg[i].d == d) { for (int k = 0; k < u->dlg[i].ndrawn; k++) bytes_free(&u->dlg[i].drawn[k]); memmove(&u->dlg[i], &u->dlg[i + 1], (u->ndlg - i - 1) * sizeof(Dlg)); u->ndlg--; return; }
}
static uint32_t make_dialog(Emu *E, int rid, uint32_t storage, uint32_t behind, Rect bounds, int procid, int vis, int goaway, uint32_t refcon,
                            const char *title, int tlen, int items_id) {
    uint32_t dh = RES(W, "DITL", items_id); int n = 0; DItem *items = NULL;
    if (dh) { uint32_t nd; uint8_t *dd = hdata(E, dh, &nd); items = parse_ditl(dd, nd, &n); free(dd); if (n < 0) emu_fail(E, "DITL %d truncated", items_id); }
    uint32_t d = make_window(E, storage, bounds, title, tlen, vis, procid, behind, goaway, refcon, 2, 170);
    for (int k = 0; k < n; k++) {
        DItem *it = &items[k]; int t = it->type & 0x7F;
        Rect r = R4(it->rect[0], it->rect[1], it->rect[2], it->rect[3]);
        if (t == 4 || t == 5 || t == 6) it->h = new_control(E, d, r, (const char *)it->data.b, (int)it->data.n, 1, 0, 0, 1, t - 4, 0);
        else if (t == 7) {
            if (it->data.n < 2) emu_fail(E, "DITL control item without CNTL id");
            int cid = (int16_t)(it->data.b[0] << 8 | it->data.b[1]); uint32_t ch = RES(W, "CNTL", cid);
            if (ch) {
                uint32_t nc; uint8_t *cd = hdata(E, ch, &nc); int val, cvis, mx, mn, proc; uint32_t ref; cntl_fields(cd, &val, &cvis, &mx, &mn, &proc, &ref);
                it->h = new_control(E, d, r, (const char *)cd + 23, cd[22], cvis, val, mn, mx, proc, ref); free(cd);
            }
        } else if (t == 8 || t == 16) {
            Bytes txt = t == 8 ? subst(E, &it->data) : bytes_dup(it->data.b, it->data.n);
            it->h = new_handle(E, (uint32_t)txt.n, txt.b, 0); bytes_free(&txt);
        } else if (t == 32) it->h = it->data.n >= 2 ? RES(W, "ICON", (int16_t)(it->data.b[0] << 8 | it->data.b[1])) : 0;
        else if (t == 64) it->h = it->data.n >= 2 ? RES(W, "PICT", (int16_t)(it->data.b[0] << 8 | it->data.b[1])) : 0;
    }
    uint32_t ih = new_handle(E, 1, NULL, 0);
    Bytes ib = { 0 }; uint8_t b2[2] = { (uint8_t)((n - 1) >> 8), (uint8_t)(n - 1) }; bytes_put(&ib, b2, 2);
    for (int k = 0; k < n; k++) {
        uint8_t hd[14]; uint32_t h = items[k].h; hd[0] = h >> 24; hd[1] = h >> 16; hd[2] = h >> 8; hd[3] = h;
        for (int j = 0; j < 4; j++) { hd[4 + 2 * j] = (uint8_t)(items[k].rect[j] >> 8); hd[5 + 2 * j] = (uint8_t)items[k].rect[j]; }
        hd[12] = (uint8_t)items[k].type; hd[13] = (uint8_t)items[k].data.n; bytes_put(&ib, hd, 14); bytes_put(&ib, items[k].data.b, items[k].data.n);
        if (items[k].data.n & 1) { uint8_t z = 0; bytes_put(&ib, &z, 1); }
    }
    hset(E, ih, ib.b, (uint32_t)ib.n); bytes_free(&ib);
    wr32(E, d + 156, ih); wr32(E, d + 160, 0); wr16(E, d + 164, 0xFFFF); wr16(E, d + 166, 0); wr16(E, d + 168, 1);
    for (int k = 0; k < n; k++) if ((items[k].type & 0x7F) == 16) { wr16(E, d + 164, (uint32_t)k); break; }
    Dlg *dl = dlg_get(U, d); if (!dl) { GROW(U->dlg, U->ndlg, U->capdlg); dl = &U->dlg[U->ndlg++]; memset(dl, 0, sizeof *dl); dl->d = d; }
    else { for (int k = 0; k < dl->n; k++) bytes_free(&dl->it[k].data); free(dl->it); for (int k = 0; k < dl->ndrawn; k++) bytes_free(&dl->drawn[k]); dl->ndrawn = 0; }
    dl->it = items; dl->n = n;
    T(E, "GetNewDialog", "%d -> %08x (%d items)", rid, d, n);
    return d;
}
REG(GetNewDialog) {
    int rid = s16(A(E, 0)); uint32_t storage = A(E, 1), behind = A(E, 2), h = RES(W, "DLOG", rid);
    if (!h) emu_fail(E, "DLOG %d not found", rid);
    uint32_t n; uint8_t *d = hdata(E, h, &n);
    Rect r = R4((int16_t)(d[0] << 8 | d[1]), (int16_t)(d[2] << 8 | d[3]), (int16_t)(d[4] << 8 | d[5]), (int16_t)(d[6] << 8 | d[7]));
    int procid = (int16_t)(d[8] << 8 | d[9]), vis = (int16_t)(d[10] << 8 | d[11]), goaway = (int16_t)(d[12] << 8 | d[13]);
    uint32_t refcon = (uint32_t)d[14] << 24 | d[15] << 16 | d[16] << 8 | d[17]; int items_id = (int16_t)(d[18] << 8 | d[19]);
    char title[256]; int tl = d[20]; memcpy(title, d + 21, tl); free(d);
    return make_dialog(E, rid, storage, behind, r, procid, vis, goaway, refcon, title, tl, items_id);
}
REG(NewDialog) { emu_fail(E, "NewDialog not modelled"); }
static Dlg *items_of(Emu *E, uint32_t d) { Dlg *g = dlg_get(U, d); if (!g) emu_fail(E, "not a dialog %08x", d); return g; }
REG(GetDialogItem) {
    uint32_t d = A(E, 0); int n = s16(A(E, 1)); Dlg *g = items_of(E, d);
    if (n < 1 || n > g->n) emu_fail(E, "GetDialogItem %d of %d", n, g->n);
    DItem *it = &g->it[n - 1];
    if (A(E, 2)) wr16(E, A(E, 2), (uint32_t)it->type);
    if (A(E, 3)) wr32(E, A(E, 3), it->h);
    if (A(E, 4)) wrect(E, A(E, 4), R4(it->rect[0], it->rect[1], it->rect[2], it->rect[3]));
    return NORET;
}
REG(SetDialogItem) {
    uint32_t d = A(E, 0); int n = s16(A(E, 1)); Dlg *g = items_of(E, d);
    int i = n - 1; if (i < 0) i += g->n;           /* python negative index */
    if (i < 0 || i >= g->n) emu_fail(E, "SetDialogItem %d of %d", n, g->n);
    DItem *it = &g->it[i]; it->type = A(E, 2) & 0xFF; it->h = A(E, 3);
    Rect r = rrect(E, A(E, 4)); it->rect[0] = r.t; it->rect[1] = r.l; it->rect[2] = r.b; it->rect[3] = r.r;
    return NORET;
}
REG(GetDialogItemText) {
    uint32_t h = A(E, 0); uint8_t b[256]; int n = 0;
    if (live(E, h)) { uint32_t m; uint8_t *d = hdata(E, h, &m); n = m > 255 ? 255 : (int)m; memcpy(b + 1, d, n); free(d); }
    b[0] = (uint8_t)n; emu_write(E, A(E, 1), b, (uint32_t)n + 1); return NORET;
}
REG(SetDialogItemText) {
    uint32_t h = A(E, 0); uint8_t b[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1) + 1, b, n);
    if (live(E, h)) hset(E, h, b, (uint32_t)n);
    return NORET;
}
REG(SelectDialogItemText) { wr16(E, A(E, 0) + 164, (uint32_t)(s16(A(E, 1)) - 1)); return NORET; }
REG(ParamText) {
    for (int i = 0; i < 4; i++) {
        uint32_t a = A(E, i); bytes_free(&U->ptext[i]);
        if (a) { int n = rd8(E, a); uint8_t b[256]; emu_read(E, a + 1, b, n); U->ptext[i] = bytes_dup(b, n); }
    }
    return NORET;
}
REG(CloseDialog) { dlg_pop(U, A(E, 0)); close_window(E, A(E, 0)); return NORET; }
/* Text a program draws itself into a dialog (alert messages drawn by a user item with TETextBox) is not in
 * the item list; it is kept per dialog for the dialog policy (the Windows front end shows it). */
static void dlg_drawn(Emu *E, const uint8_t *t, uint32_t n) {
    Dlg *g = dlg_get(U, U->port); if (!g || !n) return;
    if (g->ndrawn == 8) { bytes_free(&g->drawn[0]); memmove(&g->drawn[0], &g->drawn[1], 7 * sizeof(Bytes)); g->ndrawn--; }
    g->drawn[g->ndrawn++] = bytes_dup(t, n);
}
REG(TETextBox) { uint32_t n = A(E, 1); if (n > 4000) n = 4000; uint8_t *b = malloc(n + 1); emu_read(E, A(E, 0), b, n); dlg_drawn(E, b, n); free(b); return NORET; }
REG(DrawText) { uint32_t n = A(E, 2) & 0xFFFF; uint8_t *b = malloc(n + 1); emu_read(E, A(E, 0) + (A(E, 1) & 0xFFFF), b, n); dlg_drawn(E, b, n); free(b); return NORET; }
REG(DrawString) { uint8_t b[256]; int n = rd8(E, A(E, 0)); emu_read(E, A(E, 0) + 1, b, n); dlg_drawn(E, b, (uint32_t)n); return NORET; }
/* [(item no, kind, text)] with the texts as the program has set them (control titles, item text handles) */
static int describe(Emu *E, uint32_t d, DlgDesc **out) {
    Dlg *g = dlg_get(U, d); int n = 0; *out = NULL;
    if (!g) return 0;
    *out = calloc(g->n + 1, sizeof(DlgDesc));
    for (int k = 0; k < g->n; k++) {
        int t = g->it[k].type & 0x7F; uint32_t h = g->it[k].h; DlgDesc *x = &(*out)[n];
        if ((t == 4 || t == 5 || t == 6 || t == 7) && live(E, h)) {
            uint32_t c = rd32(E, h); int l = rd8(E, c + 40); emu_read(E, c + 41, x->text, l); x->ntext = l;
            x->k = k + 1; x->kind = t == 4 ? "button" : "control"; n++;
        } else if ((t == 8 || t == 16) && live(E, h)) {
            uint32_t m; uint8_t *dd = hdata(E, h, &m); if (m > sizeof x->text) m = sizeof x->text;
            memcpy(x->text, dd, m); x->ntext = (int)m; free(dd);
            x->k = k + 1; x->kind = t == 8 ? "text" : "edit"; n++;
        }
    }
    return n;
}
static int is_space_cp(uint32_t cp) { return (cp >= 9 && cp <= 13) || (cp >= 28 && cp <= 32) || cp == 0x85 || cp == 0xA0; }
/* Headless default: press the button titled OK, otherwise the first button. */
static int default_policy(const DlgDesc *d, int n) {
    int first = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(d[i].kind, "button")) continue;
        if (first < 0) first = d[i].k;
        int a = 0, b = d[i].ntext;
        while (a < b && is_space_cp(macroman_cp((uint8_t)d[i].text[a]))) a++;
        while (b > a && is_space_cp(macroman_cp((uint8_t)d[i].text[b - 1]))) b--;
        char low[2100]; int k = 0; for (int j = a; j < b; j++) low[k++] = (char)tolower((uint8_t)d[i].text[j]); low[k] = 0;
        int ascii = 1; for (int j = a; j < b; j++) if ((uint8_t)d[i].text[j] >= 0x80) ascii = 0;
        if (ascii && (!strcmp(low, "ok") || !strcmp(low, "yes") || !strcmp(low, "continue"))) return d[i].k;
    }
    return first >= 0 ? first : 1;
}
static void desc_repr(const DlgDesc *d, int n, char *o, size_t max) {
    size_t k = 0; o[k++] = '[';
    for (int i = 0; i < n && k + 64 < max; i++) {
        char tr[8300]; repr_mr(d[i].text, d[i].ntext, tr, sizeof tr);
        k += (size_t)snprintf(o + k, max - k, "%s(%d, '%s', %s)", i ? ", " : "", d[i].k, d[i].kind, tr);
        if (k >= max) k = max - 2;
    }
    o[k++] = ']'; o[k] = 0;
}
static int ui_answer(World *w, int *a) {
    if (!w->nui_answers) return 0;
    *a = w->ui_answers[0]; memmove(w->ui_answers, w->ui_answers + 1, (w->nui_answers - 1) * sizeof(int)); w->nui_answers--; return 1;
}
REG(ModalDialog) {
    uint32_t d = front_visible(E); DlgDesc *desc; int n = describe(E, d, &desc), a;
    if (!ui_answer(W, &a)) {
        if (W->dialog_policy) {
            /* the policy also gets the text the program drew itself (kind "drawn", not in the trace line) */
            Dlg *g = dlg_get(U, d); int nd = g ? g->ndrawn : 0;
            DlgDesc *ext = calloc((size_t)(n + nd + 1), sizeof(DlgDesc)); if (n) memcpy(ext, desc, (size_t)n * sizeof(DlgDesc));
            for (int i = 0; i < nd; i++) {
                DlgDesc *x = &ext[n + i]; x->k = 0; x->kind = "drawn";
                size_t m = g->drawn[i].n < sizeof x->text ? g->drawn[i].n : sizeof x->text; memcpy(x->text, g->drawn[i].b, m); x->ntext = (int)m;
            }
            a = W->dialog_policy(W, ext, n + nd); free(ext);
        } else a = default_policy(desc, n);
    }
    wr16(E, A(E, 1), (uint32_t)a);
    char *r = malloc(70000); desc_repr(desc, n, r, 70000);
    T(E, "ModalDialog", "%08x %s -> item %d", d, r, a);
    if (W->log) TLOG(E, "!! DIALOG %s -> item %d\n", r, a);
    free(r); free(desc); return NORET;
}
static int64_t alert(Emu *E, const char *kind) {
    int rid = s16(A(E, 0)); char *txt = malloc(70000); size_t k = 0; txt[k++] = '['; int nt = 0;
    uint32_t h = RES(W, "ALRT", rid);
    if (h) {
        uint32_t n; uint8_t *d = hdata(E, h, &n); int did = n >= 10 ? (int16_t)(d[8] << 8 | d[9]) : 0; free(d);
        if (n < 10) emu_fail(E, "ALRT %d too short", rid);
        uint32_t ditl = RES(W, "DITL", did);
        if (ditl) {
            uint32_t nd; uint8_t *dd = hdata(E, ditl, &nd); int ni; DItem *it = parse_ditl(dd, nd, &ni); free(dd);
            if (ni < 0) emu_fail(E, "DITL %d truncated", did);
            for (int i = 0; i < ni; i++) {
                if ((it[i].type & 0x7F) == 8) {
                    Bytes s = subst(E, &it[i].data); char tr[8300]; repr_mr((const char *)s.b, (int)s.n, tr, sizeof tr);
                    k += (size_t)snprintf(txt + k, 70000 - k, "%s%s", nt++ ? ", " : "", tr); bytes_free(&s);
                }
                bytes_free(&it[i].data);
            }
            free(it);
        }
    }
    txt[k++] = ']'; txt[k] = 0;
    int a; if (!ui_answer(W, &a)) a = 1;
    T(E, kind, "id %d %s -> %d", rid, txt, a);
    if (W->log) TLOG(E, "!! %s %d: %s -> %d\n", kind, rid, txt, a);
    free(txt); return a;
}
REG(Alert) { return alert(E, "Alert"); }
REG(StopAlert) { return alert(E, "StopAlert"); }
REG(NoteAlert) { return alert(E, "NoteAlert"); }
REG(CautionAlert) { return alert(E, "CautionAlert"); }

/* ============================================================================ TextEdit */
/* TERec: destRect 0, viewRect 8, selRect 16, lineHeight 24, fontAscent 26, selPoint 28, selStart 32,
 * selEnd 34, active 36, wordBreak 38, clikLoop 42, clickTime 46, clickLoc 50, caretTime 52, caretState 56,
 * just 58, teLength 60, hText 62, ... txFont 74, txFace 76, txMode 78, txSize 80, inPort 82, nLines 94 */
static uint32_t te_new(Emu *E, Rect dest, Rect view) {
    uint32_t h = new_handle(E, 104 + 64, NULL, 0), t = rd32(E, h);
    wrect(E, t, dest); wrect(E, t + 8, view); wr16(E, t + 24, 12); wr16(E, t + 26, 9);
    wr32(E, t + 62, new_handle(E, 0, NULL, 0)); wr16(E, t + 60, 0); wr32(E, t + 82, U->port); wr16(E, t + 94, 1);
    wr16(E, t + 80, 12); wr16(E, t + 74, 3);
    return h;
}
REG(TENew) { return te_new(E, rrect(E, A(E, 0)), rrect(E, A(E, 1))); }
static uint8_t *te_text(Emu *E, uint32_t h, uint32_t *n) { uint32_t t = rd32(E, h); return hdata(E, rd32(E, t + 62), n); }
static void te_set_text(Emu *E, uint32_t h, const uint8_t *b, uint32_t n, int sel) {
    uint32_t t = rd32(E, h), th = rd32(E, t + 62); set_handle_size(E, th, n);
    if (n) emu_write(E, rd32(E, th), b, n);
    int cr = 0; for (uint32_t i = 0; i < n; i++) if (b[i] == '\r') cr++;
    wr16(E, t + 60, n); wr16(E, t + 94, (uint32_t)cr + 1);
    if (sel >= 0) { wr16(E, t + 32, (uint32_t)sel); wr16(E, t + 34, (uint32_t)sel); }
}
REG(TESetText) {
    uint32_t n = A(E, 1); uint8_t *b = malloc(n + 1); emu_read(E, A(E, 0), b, n);
    te_set_text(E, A(E, 2), b, n, (int)(n & 0xFFFF)); free(b); return NORET;
}
static void te_insert(Emu *E, uint32_t h, const uint8_t *ins, uint32_t ni) {
    uint32_t t = rd32(E, h), n; uint8_t *b = te_text(E, h, &n); uint32_t s = rd16(E, t + 32), e = rd16(E, t + 34);
    uint32_t a1 = s < n ? s : n, e1 = e < n ? e : n;          /* python b[:s] + ins + b[e:] */
    uint8_t *o = malloc(a1 + ni + (n - e1) + 1);
    memcpy(o, b, a1); memcpy(o + a1, ins, ni); memcpy(o + a1 + ni, b + e1, n - e1);
    te_set_text(E, h, o, a1 + ni + (n - e1), (int)((s + ni) & 0xFFFF)); free(o); free(b);
}
REG(TEInsert) { uint32_t n = A(E, 1); uint8_t *b = malloc(n + 1); emu_read(E, A(E, 0), b, n); te_insert(E, A(E, 2), b, n); free(b); return NORET; }
REG(TEStyleInsert) { uint32_t n = A(E, 1); uint8_t *b = malloc(n + 1); emu_read(E, A(E, 0), b, n); te_insert(E, A(E, 3), b, n); free(b); return NORET; }
REG(TEDelete) { te_insert(E, A(E, 0), NULL, 0); return NORET; }
REG(TEKey) {
    uint8_t c = A(E, 0) & 0xFF; uint32_t h = A(E, 1), t = rd32(E, h);
    if (c == 8) {
        uint32_t s = rd16(E, t + 32), e = rd16(E, t + 34);
        if (s == e && s > 0) wr16(E, t + 32, s - 1);
        te_insert(E, h, NULL, 0);
    } else te_insert(E, h, &c, 1);
    return NORET;
}
REG(TESetSelect) {
    uint32_t h = A(E, 2), t = rd32(E, h), n = rd16(E, t + 60);
    wr16(E, t + 32, A(E, 0) < n ? A(E, 0) : n); wr16(E, t + 34, A(E, 1) < n ? A(E, 1) : n); return NORET;
}
REG(TEGetHeight) {
    uint32_t t = rd32(E, A(E, 2)); int a = s16(A(E, 0)), b = s16(A(E, 1)); int d = a - b; if (d < 0) d = -d;
    int lines = d + 1 > 1 ? d + 1 : 1; return (uint32_t)(lines * rds16(E, t + 24));
}
REG(TEGetPoint) {
    /* Point TEGetPoint(short offset, TEHandle): the Point result is returned through a hidden pointer in r3 */
    uint32_t res = A(E, 0), h = A(E, 2); int off = s16(A(E, 1));
    uint32_t t = rd32(E, h), n; uint8_t *b = te_text(E, h, &n);
    uint32_t m = off > 0 ? ((uint32_t)off < n ? (uint32_t)off : n) : 0;
    int line = 0, lastcr = -1; for (uint32_t i = 0; i < m; i++) if (b[i] == '\r') { line++; lastcr = (int)i; }
    int col = (int)m - (lastcr + 1);
    int lh = rds16(E, t + 24), dt = rds16(E, t), dl = rds16(E, t + 2);
    wr16(E, res, (uint32_t)(dt + (line + 1) * lh)); wr16(E, res + 2, (uint32_t)(dl + 6 * col)); free(b);
    return NORET;
}
/* TextStyle = tsFont(2) tsFace(1) filler(1) tsSize(2) tsColor(RGBColor 6): 12 bytes */
static const uint8_t DEFAULT_STYLE[12] = { 0, 3, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0 };
REG(TEGetStyle) {
    /* TEGetStyle(short offset, TextStyle *theStyle, short *lineHeight, short *fontAscent, TEHandle hTE) */
    uint32_t h = A(E, 4), t = rd32(E, h);
    emu_write(E, A(E, 1), DEFAULT_STYLE, 12); wr16(E, A(E, 2), (uint32_t)rds16(E, t + 24)); wr16(E, A(E, 3), (uint32_t)rds16(E, t + 26));
    return NORET;
}
REG(TEContinuousStyle) { emu_write(E, A(E, 1), DEFAULT_STYLE, 12); return 1; }
REG(h32) { return new_handle(E, 32, NULL, 0); }
REG(h2) { return new_handle(E, 2, NULL, 0); }
REG(TESetAlignment) { wr16(E, rd32(E, A(E, 1)) + 58, A(E, 0)); return NORET; }
REG(TESetClickLoop) { wr32(E, rd32(E, A(E, 1)) + 42, A(E, 0)); return NORET; }
REG(TESetWordBreak) { wr32(E, rd32(E, A(E, 1)) + 38, A(E, 0)); return NORET; }
REG(TEActivate) { wr16(E, rd32(E, A(E, 0)) + 36, 1); return NORET; }
REG(TEDeactivate) { wr16(E, rd32(E, A(E, 0)) + 36, 0); return NORET; }
REG(TEDispose) { uint32_t h = A(E, 0); if (live(E, h)) { dispose_handle(E, rd32(E, rd32(E, h) + 62)); dispose_handle(E, h); } return NORET; }
REG(TECut) { te_insert(E, A(E, 0), NULL, 0); return NORET; }
REG(TEScrapHandle) { return U->te_scrap; }

/* ============================================================================ misc */
REG(LMGetCurApRefNum) { return (uint32_t)W->app_res->refnum; }
REG(LMGetCurStackBase) { (void)E; return STACK_TOP; }
REG(GetApplLimit) { (void)E; return STACK_TOP - STACK_SIZE; }
REG(SetApplLimit) { W->mem_err = 0; return NORET; }
REG(r30) { (void)E; return 30; }
REG(GetCaretTime) { (void)E; return 32; }
REG(LMGetHiliteMode) { return (uint32_t)U->hilite_mode; }
REG(LMSetHiliteMode) { U->hilite_mode = A(E, 0) & 0xFF; return NORET; }
REG(LMGetResLoad) { return W->res_load ? 1 : 0; }
REG(ReserveMem) { W->mem_err = 0; return NORET; }
REG(HandleZone) { return W->zone; }
REG(NewEmptyHandle) { uint32_t h = new_empty_handle(E); W->mem_err = 0; return h; }
REG(EmptyHandle) {
    uint32_t h = A(E, 0); HandleRec *r = handle_rec(E, h);
    if (r) { uint32_t blk = r->blk; if (blk) dispose_ptr(E, blk); handle_set_block(E, h, 0, 0); }
    W->mem_err = 0; return NORET;
}
REG(ReallocateHandle) {
    uint32_t h = A(E, 0), n = A(E, 1); HandleRec *r = handle_rec(E, h); uint32_t blk = r ? r->blk : 0;
    if (blk) dispose_ptr(E, blk);
    uint32_t nb = new_ptr(E, n ? n : 1, 0); handle_set_block(E, h, nb, n); W->mem_err = 0; return NORET;
}
REG(InlineGetHandleSize) { uint32_t h = A(E, 0); HandleRec *r = handle_rec(E, h); return r ? r->n : 0; }
static void append_to_handle(Emu *E, uint32_t h, const uint8_t *d, uint32_t n) {
    HandleRec *r = handle_rec(E, h); if (!r) emu_fail(E, "not a handle %08x", h);
    uint32_t old = r->n; set_handle_size(E, h, old + n); if (n) emu_write(E, rd32(E, h) + old, d, n);
}
REG(PtrAndHand) {
    uint32_t n = A(E, 2); uint8_t *d = malloc(n + 1); emu_read(E, A(E, 0), d, n); append_to_handle(E, A(E, 1), d, n); free(d); W->mem_err = 0; return 0;
}
REG(PtrToXHand) {
    uint32_t h = A(E, 1), n = A(E, 2); uint8_t *d = malloc(n + 1); emu_read(E, A(E, 0), d, n); hset(E, h, d, n); free(d); W->mem_err = 0; return 0;
}
REG(HandAndHand) {
    uint32_t n; uint8_t *d = hdata(E, A(E, 0), &n); append_to_handle(E, A(E, 1), d, n); free(d); return 0;
}
REG(NewString) { uint8_t s[256]; int n = rd8(E, A(E, 0)); emu_read(E, A(E, 0), s, (uint32_t)n + 1); return new_handle(E, (uint32_t)n + 1, s, 0); }
REG(SetString) { uint8_t s[256]; int n = rd8(E, A(E, 1)); emu_read(E, A(E, 1), s, (uint32_t)n + 1); hset(E, A(E, 0), s, (uint32_t)n + 1); return NORET; }
/* python bytes.find(pat, start) */
static int64_t bfind(const uint8_t *d, int64_t n, const uint8_t *p, int64_t m, int64_t start) {
    if (start < 0) { start += n; if (start < 0) start = 0; }
    if (start > n) return -1;
    for (int64_t i = start; i + m <= n; i++) if (!m || !memcmp(d + i, p, (size_t)m)) return i;
    return -1;
}
/* python slice bounds for d[a:b] */
static void pyslice(int64_t n, int64_t *a, int64_t *b) {
    if (*a < 0) { *a += n; if (*a < 0) *a = 0; } if (*a > n) *a = n;
    if (*b < 0) { *b += n; if (*b < 0) *b = 0; } if (*b > n) *b = n;
    if (*b < *a) *b = *a;
}
REG(Munger) {
    /* Munger(h, offset, ptr1, len1, ptr2, len2): find ptr1/len1 at or after offset, replace with ptr2/len2 */
    uint32_t h = A(E, 0), p1 = A(E, 2), p2 = A(E, 4); int64_t off = s32(A(E, 1)), l1 = s32(A(E, 3)), l2 = s32(A(E, 5));
    uint32_t n; uint8_t *data = hdata(E, h, &n); int64_t pos;
    if (p1 == 0) { pos = off; if (l1 < 0) l1 = (int64_t)n - off; }
    else {
        if (l1 < 0) emu_fail(E, "Munger: negative len1");
        uint8_t *pat = malloc((size_t)l1 + 1); emu_read(E, p1, pat, (uint32_t)l1);
        pos = bfind(data, n, pat, l1, off); free(pat);
        if (pos < 0) { free(data); return 0xFFFFFFFFu; }
    }
    if (p2 == 0) { free(data); return (uint32_t)pos; }   /* just search */
    if (l2 < 0) emu_fail(E, "Munger: negative len2");
    int64_t a0 = 0, a1 = pos; pyslice(n, &a0, &a1);
    int64_t b0 = pos + l1, b1 = n; pyslice(n, &b0, &b1);
    size_t nn = (size_t)(a1 - a0) + (size_t)l2 + (size_t)(b1 - b0);
    uint8_t *o = malloc(nn + 1); memcpy(o, data + a0, (size_t)(a1 - a0)); emu_read(E, p2, o + (a1 - a0), (uint32_t)l2);
    memcpy(o + (a1 - a0) + l2, data + b0, (size_t)(b1 - b0));
    set_handle_size(E, h, (uint32_t)nn); if (nn) emu_write(E, rd32(E, h), o, (uint32_t)nn);
    free(o); free(data);
    return (uint32_t)(pos + l2);
}
static int64_t floordiv_pow2(int64_t v, int sh) { return v >= 0 ? v >> sh : -((-v + ((int64_t)1 << sh) - 1) >> sh); }
REG(FixMul) { return (uint32_t)floordiv_pow2((int64_t)s32(A(E, 0)) * s32(A(E, 1)), 16); }
REG(Fix2Long) { return (uint32_t)floordiv_pow2((int64_t)s32(A(E, 0)) + 0x8000, 16); }
REG(BitClr) { uint32_t a = A(E, 0), n = A(E, 1); uint8_t b = rd8(E, a + n / 8); wr8(E, a + n / 8, b & ~(0x80 >> (n % 8))); return NORET; }
REG(BitTst) { uint32_t a = A(E, 0), n = A(E, 1); return (rd8(E, a + n / 8) & (0x80 >> (n % 8))) ? 1 : 0; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
REG(StuffHex) {
    char s[300]; int n = rd_pstr(E, A(E, 1), s); uint8_t out[160]; int k = 0;
    for (int i = 0; i < n;) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f') { i++; continue; }
        if (i + 1 >= n || hexv(s[i]) < 0 || hexv(s[i + 1]) < 0) emu_fail(E, "StuffHex: bad hex string");
        out[k++] = (uint8_t)(hexv(s[i]) << 4 | hexv(s[i + 1])); i += 2;
    }
    emu_write(E, A(E, 0), out, (uint32_t)k); return NORET;
}
/* comparison keys: unicode code points of the Mac Roman text, upper-cased when not case sensitive */
static int cmp_key(const uint8_t *b, int n, int casesens, uint32_t *out) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (casesens) out[k++] = macroman_cp(b[i]);
        else for (int j = 0; j < UPR_CP_N[b[i]]; j++) out[k++] = UPR_CP[b[i]][j];
    }
    return k;
}
static int cmp_cp(const uint32_t *a, int na, const uint32_t *b, int nb) {
    for (int i = 0; i < na && i < nb; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return na == nb ? 0 : (na < nb ? -1 : 1);
}
REG(RelString) {
    uint8_t a[256], b[256]; int na = rd8(E, A(E, 0)), nb = rd8(E, A(E, 1)); emu_read(E, A(E, 0) + 1, a, na); emu_read(E, A(E, 1) + 1, b, nb);
    int cs = A(E, 2) & 0xFF; uint32_t ka[800], kb[800]; int la = cmp_key(a, na, cs, ka), lb = cmp_key(b, nb, cs, kb);
    int c = cmp_cp(ka, la, kb, lb); return c == 0 ? 0 : c < 0 ? 0xFFFF : 1;
}
REG(IUMagString) {
    int na = s16(A(E, 2)), nb = s16(A(E, 3)); if (na < 0 || nb < 0) emu_fail(E, "IUMagString negative length");
    uint8_t *a = malloc(na + 1), *b = malloc(nb + 1); emu_read(E, A(E, 0), a, na); emu_read(E, A(E, 1), b, nb);
    uint32_t *ka = malloc((na + 1) * 12), *kb = malloc((nb + 1) * 12);
    int la = cmp_key(a, na, 0, ka), lb = cmp_key(b, nb, 0, kb); int c = cmp_cp(ka, la, kb, lb);
    free(a); free(b); free(ka); free(kb); return c == 0 ? 0 : c < 0 ? 0xFFFF : 1;
}
REG(UprText) {
    uint32_t a = A(E, 0); int n = s16(A(E, 1)); if (n <= 0) return NORET;
    uint8_t *s = malloc(n), *o = malloc((size_t)n * 3); emu_read(E, a, s, n); int k = 0;
    for (int i = 0; i < n; i++) for (int j = 0; j < UPR_MR_N[s[i]]; j++) o[k++] = UPR_MR[s[i]][j];
    emu_write(E, a, o, (uint32_t)(k < n ? k : n)); free(s); free(o); return NORET;
}
REG(CharType) { uint8_t c = rd8(E, A(E, 0) + (uint32_t)s16(A(E, 1))); return CHARTYPE[c]; }
REG(OSEventAvail) { emu_zero(E, A(E, 1), 16); return 0; }
REG(InfoScrap) { return U->scrap; }
/* chain_from_cur */
static int chain_start(World *w) { for (int i = 0; i < w->nrf; i++) if (w->rf[i] == w->cur_res) return i; return 0; }
REG(CountResources) {
    uint32_t t = A(E, 0); int ids[4096], n = 0;
    for (int k = chain_start(W); k < W->nrf; k++) {
        ResFile *rf = W->rf[k];
        for (int i = 0; i < rf->n; i++) if (rf->r[i].type == t) {
            int seen = 0; for (int j = 0; j < n; j++) if (ids[j] == rf->r[i].id) seen = 1;
            if (!seen && n < 4096) ids[n++] = rf->r[i].id;
        }
    }
    W->res_err = 0; return (uint32_t)n;
}
REG(GetIndResource) {
    uint32_t t = A(E, 0); int idx = s16(A(E, 1)), ids[4096], n = 0;
    for (int k = chain_start(W); k < W->nrf; k++) {
        ResFile *rf = W->rf[k];
        for (int i = 0; i < rf->n; i++) if (rf->r[i].type == t) {
            int seen = 0; for (int j = 0; j < n; j++) if (ids[j] == rf->r[i].id) seen = 1;
            if (!seen && n < 4096) ids[n++] = rf->r[i].id;
        }
    }
    if (idx >= 1 && idx <= n) return get_res(W, t, ids[idx - 1], 0, NULL);
    W->res_err = -192; return 0;
}
REG(HomeResFile) {
    int ref; uint32_t t; int id; int ok = hle_res_handle_info(W, A(E, 0), &ref, &t, &id);
    W->res_err = ok ? 0 : -192; return (uint32_t)(ok ? ref : -1) & 0xFFFF;
}
REG(GetResAttrs) {
    int ref; uint32_t t; int id;
    if (!hle_res_handle_info(W, A(E, 0), &ref, &t, &id)) { W->res_err = -192; return 0; }
    W->res_err = 0; ResFile *rf = res_by_ref(W, ref); Res *r = rf ? resfile_get(rf, t, id) : NULL;
    if (!r) emu_fail(E, "GetResAttrs: resource gone");
    return r->attr;
}
REG(SetResAttrs) {
    int ref; uint32_t t; int id;
    if (hle_res_handle_info(W, A(E, 0), &ref, &t, &id)) {
        ResFile *rf = res_by_ref(W, ref); Res *r = rf ? resfile_get(rf, t, id) : NULL;
        if (!r) emu_fail(E, "SetResAttrs: resource gone");
        r->attr = A(E, 1) & 0xFF;
    }
    W->res_err = 0; return NORET;
}
REG(LoadResource) { W->res_err = 0; return NORET; }
REG(GetFrontProcess) { wr32(E, A(E, 0), W->psn_hi); wr32(E, A(E, 0) + 4, W->psn_lo); return 0; }
REG(SameProcess) {
    uint8_t cur[8] = { W->psn_hi >> 24, W->psn_hi >> 16, W->psn_hi >> 8, W->psn_hi, W->psn_lo >> 24, W->psn_lo >> 16, W->psn_lo >> 8, W->psn_lo };
    static const uint8_t kcur[8] = { 0, 0, 0, 0, 0, 0, 0, 2 };
    uint8_t a[8], b[8]; emu_read(E, A(E, 0), a, 8); emu_read(E, A(E, 1), b, 8);
    if (!memcmp(a, kcur, 8)) memcpy(a, cur, 8);
    if (!memcmp(b, kcur, 8)) memcpy(b, cur, 8);
    wr8(E, A(E, 2), memcmp(a, b, 8) ? 0 : 1); return 0;
}
REG(HMGetHelpMenuHandle) {
    if (!U->helpmenu) { uint32_t h = new_handle(E, 1, NULL, 0); Menu m; memset(&m, 0, sizeof m); m.mid = -16490; m.en = 0xFFFFFFFFu; memcpy(m.title, "Help", 4); m.ntitle = 4; menu_build(E, h, &m); U->helpmenu = h; }
    wr32(E, A(E, 0), U->helpmenu); return 0;
}
REG(HMGetIndHelpMsg) { (void)E; return (uint32_t)-850 & 0xFFFF; }
REG(sfget) { wr8(E, A(E, 6), 0); return NORET; }
REG(sfput) { wr8(E, A(E, 4), 0); return NORET; }
REG(z88_a3) { emu_zero(E, A(E, 3), 88); return NORET; }
REG(z88_a2) { emu_zero(E, A(E, 2), 88); return NORET; }
REG(PPCBrowser) { (void)E; return (uint32_t)-128 & 0xFFFF; }
/* Alias Manager. AliasRecord = userType(4) aliasSize(2) + private data; we keep the target's
 * volume-relative path (UTF-8) as the private data. */
static uint32_t make_alias(Emu *E, const char *rel) {
    size_t n = strlen(rel); uint8_t *d = malloc(6 + n); uint32_t sz = (uint32_t)(6 + n);
    d[0] = d[1] = d[2] = d[3] = 0; d[4] = sz >> 8; d[5] = sz; memcpy(d + 6, rel, n);
    uint32_t h = new_handle(E, sz, d, 0); free(d); return h;
}
static void alias_rel(Emu *E, uint32_t h, char *rel) {
    uint32_t n; uint8_t *d = hdata(E, h, &n); size_t m = n > 6 ? n - 6 : 0; if (m > 2000) m = 2000;
    memcpy(rel, d + 6 < d + n ? d + 6 : d, m); rel[m] = 0; free(d);
}
static void put_spec(Emu *E, uint32_t addr, const char *rel) {
    char par[2048], leaf[300]; parent_of(rel, par); int pid = dir_id_for(W, par); leaf_of(W, rel, leaf);
    write_fsspec(W, addr, W->vref, (uint32_t)pid, leaf);
}
REG(NewAlias) {
    char rel[2048], rr[4096]; spec_rel(W, A(E, 1), rel, sizeof rel); wr32(E, A(E, 2), make_alias(E, rel));
    repr_rel(rel, rr, sizeof rr); T(E, "NewAlias", "%s", rr); return 0;
}
REG(NewAliasMinimal) {
    char rel[2048], rr[4096]; spec_rel(W, A(E, 0), rel, sizeof rel); wr32(E, A(E, 1), make_alias(E, rel));
    repr_rel(rel, rr, sizeof rr); T(E, "NewAliasMinimal", "%s", rr); return 0;
}
REG(ResolveAlias) {
    char rel[2048], rr[4096]; alias_rel(E, A(E, 1), rel); put_spec(E, A(E, 2), rel);
    if (A(E, 3)) wr8(E, A(E, 3), 0);
    int ok = file_exists(W, rel); repr_rel(rel, rr, sizeof rr); T(E, "ResolveAlias", "%s -> %s", rr, ok ? "True" : "False");
    return ok ? 0 : ((uint32_t)-43 & 0xFFFF);
}
REG(MatchAlias) {
    /* MatchAlias(fromFile, rulesMask, alias, short *aliasCount, FSSpecArrayPtr, Boolean *needsUpdate, filter, data) */
    char rel[2048], rr[4096]; alias_rel(E, A(E, 2), rel); int ok = file_exists(W, rel);
    wr16(E, A(E, 3), ok ? 1 : 0);
    if (ok) put_spec(E, A(E, 4), rel);
    if (A(E, 5)) wr8(E, A(E, 5), 0);
    repr_rel(rel, rr, sizeof rr); T(E, "MatchAlias", "%s -> %s", rr, ok ? "True" : "False");
    return ok ? 0 : ((uint32_t)-43 & 0xFFFF);
}
REG(m344) { (void)E; return (uint32_t)-344 & 0xFFFF; }      /* smNoMoresRsrcs */
REG(m1850) { (void)E; return (uint32_t)-1850 & 0xFFFF; }    /* badDragRefErr */
/* ColorSync: report "no profile" so callers take their non-ColorSync path (cmProfileError) */
#define CMS(n) REG(n) { T(E, #n, "%s", "not available"); return (uint32_t)-170 & 0xFFFF; }
CMS(CMGetSystemProfile) CMS(CMOpenProfile) CMS(CMGetProfileHeader) CMS(CMGetProfileLocation)
CMS(CMGetColorSyncFolderSpec) CMS(NCWNewColorWorld) CMS(CWMatchBitmap) CMS(CWMatchColors)

/* ---- definition procedures (WDEF/CDEF/MDEF/LDEF): never executed (68k or absent); standard
 * behaviour is emulated when a program calls one through CallUniversalProc. */
static int defproc_of(Emu *E, uint32_t upp, char *kind, int *id) {
    uint32_t h, t; int ref, rid;
    for (int i = 0; hle_res_handle_at(W, i, &h, &ref, &t, &rid); i++) {
        char ts[5]; ostype_s(t, ts);
        if ((!strcmp(ts, "WDEF") || !strcmp(ts, "CDEF") || !strcmp(ts, "MDEF") || !strcmp(ts, "LDEF")) && live(E, h) && rd32(E, h) == upp) {
            strcpy(kind, ts); *id = rid; return 1;
        }
    }
    return 0;
}
/* 'miKi' 2001: driver for the optional SmartDot hardware key (ADB dongle). Emulated as the key being
 * absent: InitKey (selector 0) reports failure in the byte at pb+0x24, check_key (selector 2) writes
 * 0 to the long at pb+4. Only the optional SmartDot feature depends on it. */
static int64_t miki(Emu *E) {
    uint32_t sel = A(E, 2), pb = A(E, 3);
    if (sel == 0) wr8(E, rd32(E, pb + 0x24), 0);
    else if (sel == 2) wr32(E, rd32(E, pb + 4), 0);
    return 0;
}
REG(CallUniversalProc) {
    uint32_t upp = A(E, 0), info = A(E, 1);
    int is68k = 0; for (int i = 0; i < W->nm68k; i++) if (W->m68k[i].p == upp) is68k = 1;
    if (is68k) {
        /* origin: resource key of the handle whose block is upp */
        uint32_t hfound = 0; int have = 0;
        for (uint32_t i = 0; i < E->hmain_cap; i++) if (E->hmain[i].used && E->hmain[i].blk == upp) { hfound = MPBLK + 4 * i; have = 1; break; }
        int ref = 0, rid = 0; uint32_t t = 0; int og = have && world_orig3(W, hfound, &ref, &t, &rid);
        char orr[600], ts[5]; ostype_s(t, ts);
        if (og) { char tr[64]; py_repr_str(ts, 4, tr, sizeof tr); snprintf(orr, sizeof orr, "(%d, %s, %d)", ref, tr, rid); }
        else strcpy(orr, "None");
        int emul = og && !strcmp(ts, "miKi");
        T(E, "CallUniversalProc", "68k code from %s info %x sel %u -> %s", orr, info, A(E, 2), emul ? "emulated" : "NOT EMULATED");
        if (!emul) emu_fail(E, "call into 68k code %s (info %x) is not emulated", orr, info);
        return miki(E);
    }
    char kind[8]; int did = 0;
    if (upp) { if (!defproc_of(E, upp, kind, &did)) return hle_call_universal_default(E); }
    else { strcpy(kind, "NULL"); did = 0; }
    int isw = !strcmp(kind, "WDEF"), isc = !strcmp(kind, "CDEF");
    T(E, "CallUniversalProc", "emulated %s %d info %x msg %d", kind, did, info, (isw || isc) ? s16(A(E, 4)) : s16(A(E, 2)));
    if ((isw || !strcmp(kind, "NULL")) && info == 0x3bb0) {
        uint32_t w = A(E, 3), param = A(E, 5); int msg = s16(A(E, 4));
        if (msg == 1) {
            int v, h; rpt(param, &v, &h);
            Rect c = rgn_box(E, rd32(E, w + 118)); if (c.t <= v && v < c.b && c.l <= h && h < c.r) return 1;
            Rect s = rgn_box(E, rd32(E, w + 114)); if (s.t <= v && v < s.b && s.l <= h && h < s.r) return 2;
            return 0;
        }
        if (msg == 2) update_win_rgns(E, w);
        return 0;
    }
    if (isc && info == 0x3bb0) {
        uint32_t ch = A(E, 3), param = A(E, 5), c = rd32(E, ch); int msg = s16(A(E, 4));
        if (msg == 1) {
            int v, h; rpt(param, &v, &h); Rect r = rrect(E, c + 8);
            if (!(r.t <= v && v < r.b && r.l <= h && h < r.r) || rd8(E, c + 17) == 255) return 0;
            if (did == 0) { uint32_t p; if (!kv_get(U->cproc, U->ncproc, ch, &p)) p = 0; return (p & 0xF) == 0 ? 10 : 11; }   /* inButton / inCheckBox */
            return 1;     /* popup and others: any part */
        }
        if ((msg == 2 || msg == 10) && param) set_rgn(E, param & 0x7FFFFFFF, rrect(E, c + 8));
        return 0;
    }
    return 0;
}

/* ============================================================================ install */
void world_install_ui(World *Wd) {
    Emu *E = Wd->E; UI *u = calloc(1, sizeof(UI)); Wd->ui = u; u->hilite_mode = 0xFF;
    /* Definition procedures live in the System file. They are never executed headless, but programs
     * (MacApp) check that they exist and store their handles in records. */
    static const struct { const char *t; int id; } DP[] = { { "MDEF", 0 }, { "WDEF", 0 }, { "WDEF", 1 }, { "CDEF", 0 }, { "CDEF", 1 }, { "CDEF", 63 }, { "LDEF", 0 } };
    uint8_t code[16] = { 0x4e, 0x75 };
    for (size_t i = 0; i < sizeof DP / sizeof *DP; i++)
        if (!resfile_get(Wd->sys_res, ostype_i(DP[i].t), DP[i].id)) resfile_put(Wd->sys_res, ostype_i(DP[i].t), DP[i].id, NULL, 0, code, 16);
    u->ctab = new_handle(E, 8 + 256 * 8, NULL, 0);
    u->wmgr_port = new_ptr(E, 108, 0); init_cport(E, u->wmgr_port, SCREEN, 0);
    u->te_scrap = new_handle(E, 0, NULL, 0);
    u->scrap = new_ptr(E, 16, 0);
}

void hle_ui_register(void) {
#define R(n) emu_reg(#n, h_##n)
#define RA(alias, n) emu_reg(alias, h_##n)
    R(NewRgn); R(DisposeRgn); R(CopyRgn); R(SetEmptyRgn); R(SetRectRgn); R(RectRgn); R(OpenRgn); R(CloseRgn); R(OffsetRgn); R(InsetRgn);
    R(UnionRgn); RA("XorRgn", UnionRgn); R(SectRgn); R(DiffRgn); R(EmptyRgn); R(EqualRgn); R(PtInRgn); R(RectInRgn);
    R(SetRect); R(OffsetRect); R(InsetRect); R(SectRect); R(UnionRect); R(PtInRect); R(Pt2Rect); R(SetPt); R(EqualRect); R(EmptyRect);
    R(InitGraf); R(SetPort); R(GetPort); R(Random);
    R(OpenCPort); R(OpenPort); RA("InitPort", OpenPort); R(CloseCPort); RA("ClosePort", CloseCPort); R(GetWMgrPort); RA("GetCWMgrPort", GetWMgrPort);
    R(PortSize); R(MovePortTo); R(SetOrigin); R(ClipRect); R(SetClip); R(GetClip); R(LocalToGlobal); R(GlobalToLocal);
    R(PenSize); R(PenMode); R(PenNormal); R(HidePen); R(ShowPen); R(MoveTo); R(Move); RA("LineTo", MoveTo); RA("Line", Move);
    R(GetPenState); R(SetPenState); R(TextFont); R(TextFace); R(TextMode); R(TextSize); R(ForeColor); R(BackColor);
    R(RGBForeColor); R(RGBBackColor); R(GetForeColor); R(GetBackColor);
    R(StringWidth); R(TextWidth); R(CharWidth); R(GetFontInfo); R(GetFNum); R(GetFontName); RA("GetSysFont", zero); R(GetAppFont);
    R(GetDefFontSize); RA("GetOutlinePreferred", zero); RA("SetOutlinePreferred", noop); R(SetStdProcs); R(SetStdCProcs);
    static const char *draw[] = { "DrawChar", "EraseRect", "FrameRect", "PaintRect", "InvertRect", "FillRect",
        "FillCRect", "EraseOval", "FrameOval", "PaintOval", "FillOval", "InvertOval", "EraseRoundRect",
        "FrameRoundRect", "PaintRoundRect", "InvertRoundRect", "FillRoundRect", "FrameRgn", "PaintRgn",
        "FillRgn", "EraseRgn", "InvertRgn", "FramePoly", "PaintPoly", "FillPoly", "ErasePoly", "PenPat",
        "BackPat", "PenPixPat", "BackPixPat", "CopyBits", "CopyMask", "CopyDeepMask", "DrawPicture",
        "PlotIcon", "PlotCIcon", "PlotIconSuite", "PlotIconID", "SetEntries", "CTabChanged",
        "SetMCEntries", "ClipAbove", "DrawGrowIcon", "StdText", "StdLine", "StdRect", "ProtectEntry", "ReserveEntry", NULL };
    for (int i = 0; draw[i]; i++) emu_reg(draw[i], h_noop);
    R(DrawString); R(DrawText);
    R(ScrollRect);
    R(OpenPicture); R(ClosePicture); R(KillPicture); R(OpenPoly); RA("ClosePoly", noop); R(KillPoly);
    R(NewGWorld); RA("UpdateGWorld", zero); R(DisposeGWorld); R(GetGWorldPixMap); R(GetPixBaseAddr); RA("LockPixels", one); RA("UnlockPixels", noop);
    R(GetGWorld); R(SetGWorld); R(GetGDevice); R(SetGDevice); R(TestDeviceAttribute); R(GetCTSeed);
    RA("GetMainDevice", gdev); RA("GetDeviceList", gdev); RA("NewGDevice", gdev); RA("GetNextDevice", zero); RA("DisposeGDevice", noop);
    RA("Color2Index", zero); R(Index2Color); R(GetGrayRgn); RA("LMGetGrayRgn", GetGrayRgn);
    R(GetPattern); R(GetIndPattern); RA("GetPixPat", new_pixpat); RA("NewPixPat", new_pixpat); RA("DisposePixPat", noop);
    R(GetCursor); R(GetCCursor);
    static const char *curs[] = { "SetCursor", "SetCCursor", "DisposeCCursor", "InitCursor", "HideCursor", "ShowCursor", "ObscureCursor", NULL };
    for (int i = 0; curs[i]; i++) emu_reg(curs[i], h_noop);
    R(GetIconSuite); RA("DisposeIconSuite", zero); R(GetCIcon); R(GetIcon); RA("DisposeCIcon", noop);
    R(CallUniversalProc);
    /* windows */
    R(InvalRect); R(InvalRgn); R(ValidRect); R(ValidRgn);
    R(GetNewCWindow); RA("GetNewWindow", GetNewCWindow); R(NewCWindow); RA("NewWindow", NewCWindow);
    R(CloseWindow); RA("DisposeWindow", CloseWindow); R(FrontWindow); R(LMGetWindowList); RA("LMGetGhostWindow", zero);
    R(SelectWindow); RA("BringToFront", SelectWindow); R(SendBehind); R(ShowWindow); R(HideWindow); R(ShowHide); R(HiliteWindow);
    R(GetWRefCon); R(SetWRefCon); R(GetWTitle); R(SetWTitle); R(GetWVariant); R(SetWindowPic); R(MoveWindow); R(SizeWindow);
    RA("ZoomWindow", noop); RA("DragWindow", noop); RA("GrowWindow", zero); RA("TrackBox", zero); RA("TrackGoAway", zero);
    R(FindWindow); R(DragGrayRgn); R(BeginUpdate); RA("EndUpdate", noop); RA("CheckUpdate", zero);
    /* menus */
    R(NewMenu); R(GetMenu); R(DisposeMenu); R(InsertMenu); R(DeleteMenu); R(ClearMenuBar); R(GetMenuHandle); RA("GetMHandle", GetMenuHandle);
    static const char *mnoop[] = { "DrawMenuBar", "InvalMenuBar", "HiliteMenu", "FlashMenuBar", NULL };
    for (int i = 0; mnoop[i]; i++) emu_reg(mnoop[i], h_noop);
    R(CalcMenuSize); R(CountMItems); RA("CountMenuItems", CountMItems); R(GetMenuItemText); R(SetMenuItemText);
    R(SetItemMark); R(SetItemCmd); R(SetItemIcon); R(SetItemStyle); R(GetItemMark); R(GetItemCmd); R(GetItemIcon); R(GetItemStyle);
    R(CheckItem); R(EnableItem); R(DisableItem); RA("EnableMenuItem", EnableItem); RA("DisableMenuItem", DisableItem);
    R(AppendMenu); R(InsertMenuItem); R(DeleteMenuItem); RA("AppendResMenu", noop); RA("AddResMenu", noop);
    RA("MenuSelect", zero); RA("MenuKey", zero); RA("MenuChoice", zero); RA("PopUpMenuSelect", zero); RA("LMGetTheMenu", zero);
    R(GetMBarHeight); RA("LMGetMBarHeight", GetMBarHeight);
    /* controls */
    R(NewControl); R(GetNewControl); R(GetControlValue); R(GetControlMinimum); R(GetControlMaximum);
    RA("GetCtlValue", GetControlValue); RA("GetCtlMin", GetControlMinimum); RA("GetCtlMax", GetControlMaximum);
    R(SetControlValue); R(SetControlMinimum); R(SetControlMaximum); R(GetControlReference); R(SetControlReference);
    R(GetControlTitle); R(SetControlTitle); RA("setcontroltitle", SetControlTitle); R(GetControlVariant); R(HiliteControl);
    R(ShowControl); R(HideControl); R(MoveControl); R(SizeControl); R(DisposeControl); R(KillControls);
    static const char *cnoop[] = { "Draw1Control", "DrawControls", "UpdateControls", "SetControlColor", "SetControlAction", "DrawControlInCurrentPort", NULL };
    for (int i = 0; cnoop[i]; i++) emu_reg(cnoop[i], h_noop);
    RA("TestControl", zero); RA("TrackControl", zero); R(FindControl); R(GetAuxiliaryControlRecord);
    /* dialogs */
    R(GetNewDialog); R(NewDialog); R(GetDialogItem); R(SetDialogItem); R(GetDialogItemText); RA("GetIText", GetDialogItemText);
    R(SetDialogItemText); RA("SetIText", SetDialogItemText); RA("ShowDialogItem", noop); RA("HideDialogItem", noop);
    R(SelectDialogItemText); R(ParamText);
    static const char *dnoop[] = { "DrawDialog", "UpdateDialog", "UpdtDialog", "ResetAlertStage", "ErrorSound", NULL };
    for (int i = 0; dnoop[i]; i++) emu_reg(dnoop[i], h_noop);
    R(CloseDialog); RA("DisposeDialog", CloseDialog); R(ModalDialog); RA("IsDialogEvent", zero); RA("DialogSelect", zero);
    R(Alert); R(StopAlert); R(NoteAlert); R(CautionAlert);
    /* TextEdit */
    R(TENew); RA("TEStyleNew", TENew); RA("TEStylNew", TENew); R(TESetText); R(TEInsert); R(TEStyleInsert); RA("TEStylInsert", TEStyleInsert);
    R(TEDelete); R(TEKey); R(TESetSelect); R(TEGetHeight); RA("TEGetOffset", zero); R(TEGetPoint); RA("TENumStyles", one);
    R(TEGetStyle); R(TEContinuousStyle); RA("TEGetStyleHandle", h32); RA("GetStylHandle", h32); RA("TEGetStyleScrapHandle", h2);
    RA("TEFeatureFlag", zero); R(TESetAlignment); R(TESetClickLoop); R(TESetWordBreak); R(TEActivate); R(TEDeactivate); R(TEDispose);
    static const char *tnoop[] = { "TEUpdate", "TEIdle", "TECalText", "TEScroll", "TEAutoView", "TESelView", "TESetStyle",
        "TEReplaceStyle", "TECustomHook", "TEUseStyleScrap", "TESetStyleHandle", "TEPinScroll", "TEClick", NULL };
    for (int i = 0; tnoop[i]; i++) emu_reg(tnoop[i], h_noop);
    R(TETextBox);
    RA("TEGetHiliteRgn", zero); RA("TECopy", noop); R(TECut); RA("TEPaste", noop); RA("TEInit", noop); R(TEScrapHandle);
    /* misc */
    R(LMGetCurApRefNum); R(LMGetCurStackBase); R(GetApplLimit); R(SetApplLimit); RA("LMGetCrsrBusy", zero); RA("LMGetDoubleTime", r30);
    RA("GetDblTime", r30); R(GetCaretTime); R(LMGetHiliteMode); R(LMSetHiliteMode); R(LMGetResLoad); RA("LMSetROMMapInsert", noop);
    RA("InitZone", noop); RA("SetGrowZone", noop); R(ReserveMem); R(HandleZone); RA("SetA5", zero); RA("SetCurrentA5", zero);
    RA("NSetTrapAddress", noop); RA("VInstall", zero); RA("VRemove", zero); R(NewEmptyHandle); R(EmptyHandle); R(ReallocateHandle);
    R(InlineGetHandleSize); R(PtrAndHand); R(PtrToXHand); R(HandAndHand); R(NewString); R(SetString); R(Munger);
    R(FixMul); R(Fix2Long); R(BitClr); R(BitTst); R(StuffHex); R(RelString); R(IUMagString); R(UprText); RA("CharByte", zero); R(CharType);
    RA("FindWord", noop); RA("FontToScript", zero); RA("GetScriptVariable", zero); RA("GetScriptManagerVariable", zero); RA("KeyScript", noop);
    RA("KeyTranslate", zero); RA("GetSysDirection", zero); RA("SetEventMask", noop); R(OSEventAvail); RA("SystemEdit", zero);
    R(InfoScrap); RA("LoadScrap", zero); RA("UnloadScrap", zero);
    R(CountResources); R(GetIndResource); R(HomeResFile); R(GetResAttrs); R(SetResAttrs); R(LoadResource);
    R(GetFrontProcess); RA("SetFrontProcess", zero); RA("WakeUpProcess", zero); R(SameProcess);
    RA("HMGetBalloons", zero); RA("HMIsBalloon", zero); RA("HMShowBalloon", zero); RA("HMRemoveBalloon", zero);
    R(HMGetHelpMenuHandle); R(HMGetIndHelpMsg);
    RA("PrSetError", noop); RA("PrGeneral", noop); RA("PrValidate", zero); RA("PrJobMerge", noop);
    RA("SFGetFile", sfget); RA("SFPGetFile", sfget); RA("SFPutFile", sfput); RA("StandardGetFile", z88_a3); RA("StandardPutFile", z88_a2);
    RA("CustomGetFile", z88_a3); RA("CustomPutFile", z88_a2); R(PPCBrowser);
    R(NewAlias); R(NewAliasMinimal); R(ResolveAlias); R(MatchAlias);
    RA("SNextTypeSRsrc", m344); RA("SGetCString", m344); RA("GetDCtlEntry", zero); RA("GZSaveHnd", zero);
    static const char *drag[] = { "NewDrag", "AddDragItemFlavor", "SetDragItemFlavorData", "GetFlavorData", "GetFlavorDataSize", "GetFlavorFlags", NULL };
    for (int i = 0; drag[i]; i++) emu_reg(drag[i], h_m1850);
    R(CMGetSystemProfile); R(CMOpenProfile); R(CMGetProfileHeader); R(CMGetProfileLocation); R(CMGetColorSyncFolderSpec);
    R(NCWNewColorWorld); R(CWMatchBitmap); R(CWMatchColors); RA("CMCloseProfile", zero); RA("CWDisposeColorWorld", zero);
#undef R
#undef RA
}
