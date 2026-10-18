/*
 * minerbar.c - a pixel-art "miner" progress bar (see minerbar.h).
 *
 * Part 1 is a portable renderer (plain C, no Windows): it draws the scene into
 * a small buffer of "art pixels". Part 2 (_WIN32 only) is the window class that
 * answers the standard progress-bar messages and blows the art up by a whole
 * number of screen pixels per art pixel, so it stays crisp.
 */
#include "minerbar.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* Part 1: portable renderer                                           */
/* ------------------------------------------------------------------ */

#define RGBX(r, g, b) ((uint32_t)(((r) << 16) | ((g) << 8) | (b)))

/* Sprites: 13 x 12, facing right. Characters map to the palette below. */
#define SPR_W 13
#define SPR_H 12

static const char *const SPR_BODY[SPR_H] = {
    "...KKKK......",
    "..KYYYYKK....",
    ".KYYYYYYLK...",
    ".KyyyyyyyyK..",
    ".KSSSSKSK....",
    ".KSSSSSCK....",
    "..KSSSSK.....",
    "..KRRRRK.....",
    ".KRBBBBRK....",
    ".KBBBBBBK....",
    ".KBBKKBBK....",
    ".KOOK.KOOK...",
};
/* Overlays drawn on top of the body ('.' = leave the body as it is). */
static const char *const SPR_SWING_UP[SPR_H] = {   /* pick raised */
    "........mMMMm",
    "..........W..",
    "..........W..",
    "..........W..",
    "..........W..",
    ".........SS..",
    "........R....",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
};
static const char *const SPR_SWING_DOWN[SPR_H] = { /* pick in the rock */
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
    "...........mM",
    "........R...M",
    ".........SWWM",
    "............M",
    "...........mM",
    ".............",
};
static const char *const SPR_REST[SPR_H] = {       /* pick resting on the floor */
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
    "........R..W.",
    ".........S.W.",
    "...........W.",
    "...........W.",
    ".........mMMm",
};
static const char *const SPR_CHEER_A[SPR_H] = {    /* arms up */
    ".............",
    ".............",
    "S........S...",
    "R........R...",
    "R........R...",
    "R........R...",
    ".R......R....",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
};
static const char *const SPR_CHEER_B[SPR_H] = {    /* arms half up */
    ".............",
    ".............",
    ".............",
    ".............",
    "S........S...",
    "R........R...",
    ".R......R....",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
};
static const char *const SPR_SMILE[SPR_H] = {      /* happy face (eye closed ^) */
    ".............",
    ".............",
    ".............",
    ".............",
    "......SKS....",
    "......K.C....",
    "......K......",
    ".............",
    ".............",
    ".............",
    ".............",
    ".............",
};

#define GEM_W 9
#define GEM_H 7
static const char *const SPR_GEM[GEM_H] = {
    ".KKKKKKK.",
    "KwwCCcCCK",
    "KCCCCCCcK",
    ".KCcCCCK.",
    "..KCCcK..",
    "...KCK...",
    "....K....",
};

static uint32_t pal(char c)
{
    switch (c) {
    case 'K': return RGBX(0x2a, 0x1f, 0x1d); /* outline        */
    case 'Y': return RGBX(0xf7, 0xc6, 0x30); /* helmet         */
    case 'y': return RGBX(0xc9, 0x8f, 0x1c); /* helmet brim    */
    case 'L': return RGBX(0xff, 0xfb, 0xd2); /* lamp           */
    case 'S': return RGBX(0xf6, 0xc9, 0xa3); /* skin           */
    case 'C': return RGBX(0xf0, 0x8e, 0x88); /* cheek          */
    case 'R': return RGBX(0xe0, 0x55, 0x4a); /* shirt          */
    case 'B': return RGBX(0x4c, 0x7d, 0xe2); /* overalls       */
    case 'O': return RGBX(0x5c, 0x3b, 0x26); /* boots          */
    case 'W': return RGBX(0xa8, 0x74, 0x3f); /* handle         */
    case 'M': return RGBX(0xd8, 0xdf, 0xe6); /* pick metal     */
    case 'm': return RGBX(0x8d, 0x98, 0xa4); /* pick shade     */
    case 'w': return RGBX(0xff, 0xff, 0xff); /* gem shine      */
    case 'c': return RGBX(0x2c, 0xa6, 0xd9); /* gem deep       */
    }
    return RGBX(0x7b, 0xe6, 0xff);           /* 'C' in the gem is handled below */
}

typedef struct {
    uint32_t *px;
    int w, h, stride;
} canvas;

static unsigned hash3(int x, int y, int z)
{
    unsigned v = (unsigned)x * 374761393u + (unsigned)y * 668265263u + (unsigned)z * 2246822519u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return v ^ (v >> 16);
}

/* Interior only: the 1-pixel frame is drawn last and never covered. */
static void put(canvas *c, int x, int y, uint32_t col)
{
    if (x < 1 || y < 1 || x > c->w - 2 || y > c->h - 2)
        return;
    c->px[y * c->stride + x] = col;
}

static uint32_t get(canvas *c, int x, int y) { return c->px[y * c->stride + x]; }

static uint32_t mix(uint32_t a, uint32_t b, int t256)
{
    int r = (int)((a >> 16) & 255), g = (int)((a >> 8) & 255), bl = (int)(a & 255);
    r += (((int)((b >> 16) & 255) - r) * t256) >> 8;
    g += (((int)((b >> 8) & 255) - g) * t256) >> 8;
    bl += (((int)(b & 255) - bl) * t256) >> 8;
    return RGBX(r, g, bl);
}

static void sprite(canvas *c, const char *const *rows, int n, int x0, int y0, int gem)
{
    for (int y = 0; y < n; y++)
        for (int x = 0; rows[y][x]; x++) {
            char ch = rows[y][x];
            if (ch == '.')
                continue;
            uint32_t col = (gem && ch == 'C') ? RGBX(0x7b, 0xe6, 0xff) : pal(ch);
            if (ch == 'C' && !gem)
                col = RGBX(0xf0, 0x8e, 0x88);
            put(c, x0 + x, y0 + y, col);
        }
}

/* Rock: rounded stones of uneven width in staggered courses, lit from the top,
   with the odd copper speck and crack. */
static uint32_t rock(int x, int y)
{
    const uint32_t seam = RGBX(0x6a, 0x5e, 0x55);
    int row = (y - 1) / 3, ly = (y - 1) % 3;      /* courses 3 rows tall (2 + seam) */
    /* Walk this course's stones (widths 4..8) to the one holding x. */
    int x0 = -(int)(hash3(row, 0, 21) % 8u), k = 0, sw;
    for (;;) {
        sw = 4 + (int)(hash3(row, k, 23) % 5u);
        if (x < x0 + sw) break;
        x0 += sw;
        k++;
    }
    int lx = x - x0;
    unsigned hs = hash3(row, k, 7);
    static const uint32_t tone[4] = {
        RGBX(0x9c, 0x8e, 0x82), RGBX(0x8e, 0x80, 0x75),
        RGBX(0xa8, 0x9a, 0x8d), RGBX(0x95, 0x87, 0x7b)};
    if (lx == sw - 1 || ly == 2)                  /* gap between stones */
        return seam;
    if ((lx == 0 || lx == sw - 2) && (hs & 16))   /* rounded corners */
        if ((lx == 0 && ly == 0) || (lx == sw - 2 && ly == 1))
            return mix(tone[hs & 3], seam, 150);
    uint32_t col = tone[hs & 3];
    if (ly == 0)
        col = mix(col, RGBX(0xff, 0xf4, 0xe6), 60);   /* lit top */
    if ((hs >> 5) % 9 == 0 && lx == sw / 2 && ly == 1)
        col = RGBX(0xe8, 0xa8, 0x52);                 /* copper speck */
    else if ((hs >> 5) % 7 == 0 && lx == 1 && ly == 1)
        col = seam;                                   /* crack */
    return col;
}

static void sparkle(canvas *c, int x, int y, int big)
{
    uint32_t w = RGBX(0xff, 0xff, 0xff), b = RGBX(0xbf, 0xf2, 0xff);
    put(c, x, y, w);
    put(c, x - 1, y, b); put(c, x + 1, y, b);
    put(c, x, y - 1, b); put(c, x, y + 1, b);
    if (big) {
        put(c, x - 2, y, b); put(c, x + 2, y, b);
        put(c, x, y - 2, b); put(c, x, y + 2, b);
    }
}

void minerbar_render(uint32_t *px, int w, int h, int stride, double frac,
                     int scene, unsigned tick, int tint)
{
    canvas cv = {px, w, h, stride}, *c = &cv;
    if (w < 3 || h < 3)
        return;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;

    const int floor_y = h - 2;              /* last interior row = dirt floor */
    const int top = floor_y - SPR_H;        /* miner's top row (feet on floor) */
    /* The tunnel always has room for the miner; at 100 % a 2-pixel rim of rock stays. */
    const int f_start = 1 + SPR_W + 1;
    int f_end = w - 1 - 2;
    if (f_end < f_start) f_end = f_start;
    int face;
    if (scene == MB_SCENE_DONE)
        face = f_end;
    else if (scene == MB_SCENE_MARQUEE || scene == MB_SCENE_REST)
        face = f_start;
    else
        face = f_start + (int)(frac * (f_end - f_start) + 0.5);
    const int gem_x = f_end - GEM_W;        /* the diamond waits at the far end */
    const int gem_y = floor_y - GEM_H;

    /* Miner position: pick tip against the face, but never walking into the diamond. */
    int mx = face - SPR_W;
    if (scene == MB_SCENE_DONE)
        mx = gem_x - SPR_W + 1;
    else if (mx > gem_x - SPR_W + 2)
        mx = gem_x - SPR_W + 2;
    if (mx < 1) mx = 1;
    const int lamp_x = mx + 8, lamp_y = top + 2;
    const int digging = (scene == MB_SCENE_DIG || scene == MB_SCENE_MARQUEE);
    const int phase = (int)(tick % 4u);     /* 0,1 raised; 2,3 struck */

    /* Rock and tunnel. */
    for (int y = 1; y <= h - 2; y++) {
        int jag = (int)(hash3(y, face >> 1, 3) % 3u) - 1;  /* uneven rock face */
        int edge = face + jag;
        for (int x = 1; x <= w - 2; x++) {
            uint32_t col;
            if (x < edge) {
                if (y == floor_y) {
                    unsigned hp = hash3(x, 0, 5);
                    col = (hp % 5 == 0) ? RGBX(0x8a, 0x6b, 0x4e) : RGBX(0x6b, 0x4e, 0x37);
                } else {
                    unsigned hp = hash3(x, y, 9);
                    col = (hp % 7 == 0) ? RGBX(0x45, 0x34, 0x2c) : RGBX(0x3a, 0x2b, 0x25);
                    if (y == 1)
                        col = RGBX(0x2f, 0x23, 0x1e);   /* ceiling shadow */
                }
                /* Warm glow from the helmet lamp. */
                int dx = x - lamp_x, dy = (y - lamp_y) * 2;
                int d2 = dx * dx + dy * dy, r = 14;
                if (d2 < r * r) {
                    int k = (r * r - d2) * 80 / (r * r);
                    col = mix(col, RGBX(0xff, 0xc8, 0x6a), k);
                }
            } else {
                col = rock(x, y);
            }
            c->px[y * stride + x] = col;
        }
    }

    /* Wooden supports in the dug part, well behind the miner. */
    for (int bx = 24; bx + 2 < mx - 3; bx += 30) {
        for (int y = 2; y < floor_y; y++) {
            put(c, bx, y, RGBX(0xa0, 0x6a, 0x36));
            put(c, bx + 1, y, RGBX(0x7a, 0x4e, 0x26));
        }
        for (int x = bx - 3; x <= bx + 4; x++)
            put(c, x, 1, (x == bx - 3 || x == bx + 4) ? RGBX(0x7a, 0x4e, 0x26) : RGBX(0xa0, 0x6a, 0x36));
    }

    /* Rubble at the foot of the face. */
    if (scene != MB_SCENE_DONE)
        for (int i = 1; i <= 3; i++)
            if (hash3(face, i, 13) & 1)
                put(c, face - i, floor_y - 1, rock(face + i, i));

    /* The diamond: only the part already dug free shows. */
    for (int y = 0; y < GEM_H; y++)
        for (int x = 0; x < GEM_W; x++) {
            char ch = SPR_GEM[y][x];
            if (ch == '.' || gem_x + x >= face)
                continue;
            uint32_t col = (ch == 'C') ? RGBX(0x7b, 0xe6, 0xff) : pal(ch);
            put(c, gem_x + x, gem_y + y, col);
        }

    /* The miner. */
    sprite(c, SPR_BODY, SPR_H, mx, top, 0);
    if (scene == MB_SCENE_DONE) {
        int up = (tick / 2u) % 2u == 0;
        sprite(c, up ? SPR_CHEER_A : SPR_CHEER_B, SPR_H, mx, top, 0);
        sprite(c, SPR_SMILE, SPR_H, mx, top, 0);
    } else if (!digging) {
        sprite(c, SPR_REST, SPR_H, mx, top, 0);
    } else {
        sprite(c, phase < 2 ? SPR_SWING_UP : SPR_SWING_DOWN, SPR_H, mx, top, 0);
        if (phase >= 2) {               /* chips fly back from the strike */
            int ix = mx + 11;           /* above the pick head, in front of the brim */
            uint32_t a = RGBX(0xe6, 0xdb, 0xcf), b = RGBX(0xbf, 0xb2, 0xa5);
            if (phase == 2) {
                put(c, ix, top + 4, a);
                put(c, ix + 1, top + 5, b);
            } else {
                put(c, ix - 1, top + 2, a);
                put(c, ix + 1, top + 1, b);
                put(c, ix, top + 4, b);
            }
        }
    }

    /* Twinkles around the found diamond. */
    if (scene == MB_SCENE_DONE) {
        int s = (int)(tick % 6u);
        int cx = gem_x + GEM_W / 2;
        if (s < 2) sparkle(c, cx + 3, gem_y - 3, s == 1);
        else if (s < 4) sparkle(c, cx - 4, gem_y - 1, s == 3);
        else sparkle(c, cx + 1, gem_y + 1, 0);
        put(c, gem_x + 1, gem_y + 1, (tick & 1) ? RGBX(0xff, 0xff, 0xff) : RGBX(0xd8, 0xf8, 0xff));
    }

    /* Frame (dark, red on error, amber when paused). */
    uint32_t fr = tint == MB_TINT_ERROR ? RGBX(0xc0, 0x26, 0x26)
                : tint == MB_TINT_PAUSED ? RGBX(0xd0, 0x98, 0x10)
                : RGBX(0x2a, 0x1f, 0x1d);
    for (int x = 0; x < w; x++) {
        px[x] = fr;
        px[(h - 1) * stride + x] = fr;
    }
    for (int y = 0; y < h; y++) {
        px[y * stride] = fr;
        px[y * stride + w - 1] = fr;
    }
    (void)get;
}

/* ------------------------------------------------------------------ */
/* Part 2: the Win32 control                                          */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#include <commctrl.h>
#include <stdlib.h>

#ifndef PBS_MARQUEE
#define PBS_MARQUEE 0x08
#endif
#ifndef PBM_SETMARQUEE
#define PBM_SETMARQUEE (WM_USER + 10)
#endif
#ifndef PBM_GETSTEP
#define PBM_GETSTEP (WM_USER + 13)
#define PBM_GETBKCOLOR (WM_USER + 14)
#define PBM_GETBARCOLOR (WM_USER + 15)
#define PBM_SETSTATE (WM_USER + 16)
#define PBM_GETSTATE (WM_USER + 17)
#endif
#ifndef PBST_NORMAL
#define PBST_NORMAL 1
#define PBST_ERROR 2
#define PBST_PAUSED 3
#endif

#define MB_TIMER 1
#define MB_FRAME_MS 140           /* swing speed                          */
#define MB_CHEER_TICKS 60         /* celebrate ~8 s, then hold still      */

typedef struct {
    int lo, hi, pos, step;
    int state;                    /* PBST_*                              */
    int marquee_on;               /* PBM_SETMARQUEE TRUE                 */
    UINT marquee_ms;
    unsigned tick;
    unsigned cheer;               /* ticks spent celebrating             */
    int timer;
    uint32_t *buf;
    int bw, bh;
} mbstate;

static int mb_scene(HWND hwnd, mbstate *s)
{
    if (GetWindowLongW(hwnd, GWL_STYLE) & PBS_MARQUEE)
        return MB_SCENE_MARQUEE;
    if (s->hi > s->lo && s->pos >= s->hi)
        return MB_SCENE_DONE;
    if (s->pos <= s->lo)
        return MB_SCENE_REST;
    return MB_SCENE_DIG;
}

/* Animate only while there is something to show: digging, busy, or the first seconds of joy. */
static void mb_update_timer(HWND hwnd, mbstate *s)
{
    int scene = mb_scene(hwnd, s), want;
    if (scene != MB_SCENE_DONE)
        s->cheer = 0;
    if (s->state == PBST_ERROR || s->state == PBST_PAUSED || !IsWindowVisible(hwnd))
        want = 0;
    else if (scene == MB_SCENE_MARQUEE)
        want = s->marquee_on;
    else if (scene == MB_SCENE_DIG)
        want = 1;
    else if (scene == MB_SCENE_DONE)
        want = s->cheer < MB_CHEER_TICKS;
    else
        want = 0;
    if (want && !s->timer) {
        UINT ms = MB_FRAME_MS;
        if (scene == MB_SCENE_MARQUEE && s->marquee_ms)
            ms = s->marquee_ms < 60 ? 60 : s->marquee_ms;
        SetTimer(hwnd, MB_TIMER, ms, NULL);
        s->timer = 1;
    } else if (!want && s->timer) {
        KillTimer(hwnd, MB_TIMER);
        s->timer = 0;
    }
}

static void mb_paint(HWND hwnd, mbstate *s, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0)
        return;
    /* Whole-pixel scale chosen from the height: the art is about 16 rows. */
    int sc = ch / 16;
    if (sc < 1) sc = 1;
    int aw = (cw + sc - 1) / sc, ah = (ch + sc - 1) / sc;
    if (aw != s->bw || ah != s->bh || !s->buf) {
        uint32_t *nb = (uint32_t *)realloc(s->buf, (size_t)aw * ah * sizeof(uint32_t));
        if (!nb)
            return;
        s->buf = nb;
        s->bw = aw;
        s->bh = ah;
    }
    double frac = (s->hi > s->lo) ? (double)(s->pos - s->lo) / (double)(s->hi - s->lo) : 0.0;
    int scene = mb_scene(hwnd, s);
    int tint = s->state == PBST_ERROR ? MB_TINT_ERROR : s->state == PBST_PAUSED ? MB_TINT_PAUSED : MB_TINT_NORMAL;
    unsigned tick = s->tick;
    if (scene == MB_SCENE_DIG && s->state != PBST_NORMAL)
        tick = 0;                         /* paused/error: pick held up */
    if (scene == MB_SCENE_DONE && s->cheer >= MB_CHEER_TICKS)
        tick = 0;                         /* settle on a happy still   */
    minerbar_render(s->buf, aw, ah, aw, frac, scene, tick, tint);
    /* Keep the frame on the true client edge even when the size is not a multiple of the scale. */
    int lastx = cw - 1, lasty = ch - 1;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = aw;
    bi.bmiHeader.biHeight = -ah;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, 0, 0, aw * sc, ah * sc, 0, 0, aw, ah, s->buf, &bi, DIB_RGB_COLORS, SRCCOPY);
    if (cw % sc || ch % sc) {             /* redraw the right/bottom frame lines that fell outside */
        uint32_t fr = s->buf[(ah - 1) * aw];
        HBRUSH br = CreateSolidBrush(RGB((fr >> 16) & 255, (fr >> 8) & 255, fr & 255));
        RECT r1 = {lastx - sc + 1, 0, cw, ch}, r2 = {0, lasty - sc + 1, cw, ch};
        FillRect(dc, &r1, br);
        FillRect(dc, &r2, br);
        DeleteObject(br);
    }
}

static int mb_clamp(mbstate *s, int v)
{
    if (v < s->lo) v = s->lo;
    if (v > s->hi) v = s->hi;
    return v;
}

static void mb_set_pos(HWND hwnd, mbstate *s, int v)
{
    if (v != s->pos) {
        s->pos = v;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    mb_update_timer(hwnd, s);
}

static LRESULT CALLBACK mb_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    mbstate *s = (mbstate *)GetWindowLongPtrW(hwnd, 0);
    switch (msg) {
    case WM_NCCREATE:
        s = (mbstate *)calloc(1, sizeof *s);
        if (!s)
            return FALSE;
        s->lo = 0; s->hi = 100; s->pos = 0; s->step = 10;
        s->state = PBST_NORMAL;
        SetWindowLongPtrW(hwnd, 0, (LONG_PTR)s);
        break;
    case WM_NCDESTROY:
        if (s) {
            if (s->timer) KillTimer(hwnd, MB_TIMER);
            free(s->buf);
            free(s);
            SetWindowLongPtrW(hwnd, 0, 0);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (s) mb_paint(hwnd, s, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        if (s) mb_paint(hwnd, s, (HDC)wp);
        return 0;
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_SHOWWINDOW:
    case WM_STYLECHANGED:
        if (s) {
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            /* WM_SHOWWINDOW arrives before the visibility changes */
            if (msg == WM_SHOWWINDOW && wp)
                PostMessageW(hwnd, WM_USER + 0x7f0, 0, 0);
            else
                mb_update_timer(hwnd, s);
            InvalidateRect(hwnd, NULL, FALSE);
            return r;
        }
        break;
    case WM_USER + 0x7f0:                 /* deferred timer check after showing */
        if (s) mb_update_timer(hwnd, s);
        return 0;
    case WM_TIMER:
        if (s && wp == MB_TIMER) {
            s->tick++;
            if (mb_scene(hwnd, s) == MB_SCENE_DONE)
                s->cheer++;
            InvalidateRect(hwnd, NULL, FALSE);
            mb_update_timer(hwnd, s);
            return 0;
        }
        break;
    }
    if (!s)
        return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case PBM_SETRANGE: {
        LRESULT old = (LRESULT)(DWORD)MAKELONG((WORD)s->lo, (WORD)s->hi);
        s->lo = (int)(WORD)LOWORD(lp);
        s->hi = (int)(WORD)HIWORD(lp);
        if (s->hi < s->lo) s->hi = s->lo;
        s->pos = mb_clamp(s, s->pos);
        InvalidateRect(hwnd, NULL, FALSE);
        mb_update_timer(hwnd, s);
        return old;
    }
    case PBM_SETRANGE32: {
        LRESULT old = (LRESULT)(DWORD)MAKELONG((WORD)s->lo, (WORD)s->hi);
        s->lo = (int)wp;
        s->hi = (int)lp;
        if (s->hi < s->lo) s->hi = s->lo;
        s->pos = mb_clamp(s, s->pos);
        InvalidateRect(hwnd, NULL, FALSE);
        mb_update_timer(hwnd, s);
        return old;
    }
    case PBM_GETRANGE: {
        PBRANGE *r = (PBRANGE *)lp;
        if (r) { r->iLow = s->lo; r->iHigh = s->hi; }
        return wp ? s->lo : s->hi;
    }
    case PBM_SETPOS: {
        int old = s->pos;
        mb_set_pos(hwnd, s, mb_clamp(s, (int)wp));
        return old;
    }
    case PBM_DELTAPOS: {
        int old = s->pos;
        mb_set_pos(hwnd, s, mb_clamp(s, s->pos + (int)wp));
        return old;
    }
    case PBM_SETSTEP: {
        int old = s->step;
        s->step = (int)wp;
        return old;
    }
    case PBM_GETSTEP:
        return s->step;
    case PBM_STEPIT: {                    /* wraps to the start, like the standard bar */
        int old = s->pos, v = s->pos + s->step;
        if (v > s->hi)
            v = s->lo + (v - s->hi) % (s->hi - s->lo + 1);
        else if (v < s->lo)
            v = s->hi - (s->lo - v) % (s->hi - s->lo + 1);
        mb_set_pos(hwnd, s, mb_clamp(s, v));
        return old;
    }
    case PBM_GETPOS:
        return s->pos;
    case PBM_SETMARQUEE:
        s->marquee_on = wp ? 1 : 0;
        if (lp) s->marquee_ms = (UINT)lp;
        if (s->timer) { KillTimer(hwnd, MB_TIMER); s->timer = 0; }  /* pick up a new interval */
        mb_update_timer(hwnd, s);
        InvalidateRect(hwnd, NULL, FALSE);
        return s->marquee_on;
    case PBM_SETSTATE: {
        int old = s->state;
        if (wp == PBST_NORMAL || wp == PBST_ERROR || wp == PBST_PAUSED) {
            s->state = (int)wp;
            InvalidateRect(hwnd, NULL, FALSE);
            mb_update_timer(hwnd, s);
        }
        return old;
    }
    case PBM_GETSTATE:
        return s->state;
    case PBM_SETBARCOLOR:
    case PBM_SETBKCOLOR:
    case PBM_GETBARCOLOR:
    case PBM_GETBKCOLOR:
        return CLR_DEFAULT;               /* colours are part of the art */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL MinerBar_Register(HINSTANCE hInstance)
{
    WNDCLASSEXW wc;
    if (GetClassInfoExW(hInstance, MINERBAR_CLASSW, &wc))
        return TRUE;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = mb_proc;
    wc.cbWndExtra = sizeof(LONG_PTR);
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = MINERBAR_CLASSW;
    return RegisterClassExW(&wc) != 0;
}
#endif /* _WIN32 */
