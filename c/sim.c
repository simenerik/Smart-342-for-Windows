/* sim.c - behavioural simulator of a Scitex Smart 342(L) as seen over SCSI.
 * Port of scripts/sim1.py (level 1) and scripts/sim2.py (level 2: CCD offset-DAC dark model and DSP
 * statistics block). Log lines are identical to the Python reference. */
#include "sim.h"
#include "fwmem.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>

#define BASE 0xA00000
enum { IDX_DATA = 0x8A00, IDX_STAT = 0x8C00, IDX_CMD = 0x9040, IDX_IRQ_RST = 0x9000 };

/* ------------------------------------------------------------ int map */
typedef struct { int64_t *k; uint32_t *v; uint8_t *used; size_t cap, n; } IMap;
static size_t im_slot(IMap *m, int64_t k) {
    uint64_t h = (uint64_t)k * 0x9E3779B97F4A7C15ull; size_t i = (size_t)(h >> 20) & (m->cap - 1);
    while (m->used[i] && m->k[i] != k) i = (i + 1) & (m->cap - 1);
    return i;
}
static void im_put(IMap *m, int64_t k, uint32_t v);
static void im_grow(IMap *m) {
    IMap o = *m; m->cap = o.cap ? o.cap * 2 : 256; m->n = 0;
    m->k = calloc(m->cap, sizeof(int64_t)); m->v = calloc(m->cap, sizeof(uint32_t)); m->used = calloc(m->cap, 1);
    for (size_t i = 0; i < o.cap; i++) if (o.used[i]) im_put(m, o.k[i], o.v[i]);
    free(o.k); free(o.v); free(o.used);
}
static void im_put(IMap *m, int64_t k, uint32_t v) {
    if ((m->n + 1) * 2 > m->cap) im_grow(m);
    size_t i = im_slot(m, k); if (!m->used[i]) { m->used[i] = 1; m->k[i] = k; m->n++; } m->v[i] = v;
}
static int im_get(IMap *m, int64_t k, uint32_t *v) {
    if (!m->cap) return 0; size_t i = im_slot(m, k); if (!m->used[i]) return 0; *v = m->v[i]; return 1;
}
static uint32_t im_getd(IMap *m, int64_t k, uint32_t d) { uint32_t v; return im_get(m, k, &v) ? v : d; }

/* ------------------------------------------------------------ word deque */
typedef struct { uint32_t *v; size_t head, n, cap; } WQ;
static void wq_push(WQ *q, uint32_t w) {
    if (q->head + q->n == q->cap) {
        if (q->head) { memmove(q->v, q->v + q->head, q->n * sizeof(uint32_t)); q->head = 0; }
        if (q->n == q->cap) { q->cap = q->cap * 2 + 16; q->v = realloc(q->v, q->cap * sizeof(uint32_t)); }
    }
    q->v[q->head + q->n++] = w;
}
static uint32_t wq_pop(WQ *q) { uint32_t w = q->v[q->head]; q->head++; q->n--; if (!q->n) q->head = 0; return w; }

/* ------------------------------------------------------------ byte FIFO */
typedef struct { uint8_t *b; size_t head, n, cap; } BQ;
static void bq_put(BQ *q, const uint8_t *d, size_t n) {
    if (q->head + q->n + n > q->cap) {
        if (q->head) { memmove(q->b, q->b + q->head, q->n); q->head = 0; }
        if (q->n + n > q->cap) { q->cap = (q->n + n) * 2 + 1024; q->b = realloc(q->b, q->cap); }
    }
    memcpy(q->b + q->head + q->n, d, n); q->n += n;
}

typedef struct Sim {
    ScsiTarget t;
    int level;
    /* indexer */
    uint32_t inq[16]; int ninq, expect;
    WQ outq;
    int master_version, main_mj, main_mn, motion_type;
    uint8_t nv[256];
    int irq;
    uint32_t idx_unknown[64]; int nidx_unknown;
    struct { uint32_t op; uint32_t w[16]; int n; } nvstore[32]; int nnvstore;
    int nv_version, nv_status;
    /* bus */
    IMap regs;
    uint8_t unk_r[4096], unk_w[4096];
    int ccd_conf, sharp2, tog_c, tog_1;
    /* SCP */
    char flash_name[64]; uint8_t *flash; size_t nflash;
    int cur;                      /* 1 = file open */
    size_t pos; int seq, err, block, last_cmd;
    /* BIP */
    WQ ints; BQ out;
    int has_block; uint32_t blk[14]; int blk_rows_set; uint32_t blk_rows;
    int mode;                     /* -1 = None */
    int64_t row; int final_done; uint32_t last_ctrl;
    int prescan_ops, cap_wait; uint64_t nseed;   /* test option SIM_TEXTURE: prescans counted, noise generator */
    int m2_pulses;                     /* last motor-2 (lens carriage) target in steps */
    double m2_eff; int m2_seen;        /* the lens's real place with play (SIM_LENS_SHIFT third value: play in steps) */
    /* sim2 */
    uint32_t dac[3];
    /* hardware profile (values observed on a real scanner; see sim_load_profile) */
    int prof, r2438, r141b_lw, r141b_ct, lw_sel, inq4, inq7;
    int chunks;                   /* final scan: keep producing BIP chunks for as long as the driver reads (any image size) */
    /* the controller's memory rule (fwmem.h, from its firmware): a block it has no memory for makes the controller
       program stop - the 0x80 itself is answered, then nothing (hardware session s9) */
    uint32_t fw_reserve; int fw_dead, unplug_sense;
} Sim;

static void note(Sim *s, int important, const char *fmt, ...) PRINTF_LIKE(3, 4);
volatile int sim_fw_dead;
volatile int sim_unplug;     /* developer option --sim-off (app.c): the scanner is switched off - answered as spti.c answers a scanner that stopped answering */    /* the modelled controller program has stopped (memory): the window treats it as a scanner that no longer answers */
static void note(Sim *s, int important, const char *fmt, ...) {
    if (!s->t.log || !s->t.log->trace) return;
    char m[8192]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
    TLOG(s->t.log, "%s%s\n", important ? "!! " : "   ", m);
}
static void hexstr(const uint8_t *d, size_t n, char *out) { for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]); out[2 * n] = 0; }

/* -------------------------------------------------------------- indexer */
static const uint32_t IDX_REPLY[] = { 0x1402, 0x140c, 0x140d, 0x140e, 0x1417, 0x141b, 0x141c, 0x141f, 0x1420, 0x1438, 0x1440,
    0x1443, 0x144d, 0x14fa, 0x2407, 0x2408, 0x2438 };
static const uint32_t NV_WRITES[] = { 0x241d, 0x241e, 0x244c, 0x243f, 0x341a, 0x3421, 0x3429, 0x6423, 0x6424, 0x6425, 0x6426 };
static int in_set(const uint32_t *s, size_t n, uint32_t v) { for (size_t i = 0; i < n; i++) if (s[i] == v) return 1; return 0; }
#define NELEM(a) (sizeof(a) / sizeof *(a))
static int nv_checksum(Sim *s) {
    int sum = 1;
    for (int i = 0; i < s->nnvstore; i++) {
        uint32_t op = s->nvstore[i].op; if (op == 0x241d) continue;
        for (int k = 0; k < s->nvstore[i].n; k++) sum += (int)(s->nvstore[i].w[k] >> 8) + (int)(s->nvstore[i].w[k] & 0xFF);
        if (op >> 12 == 6) sum += 15;
    }
    return sum & 0xFF;
}
static int idx_unknown_add(Sim *s, uint32_t w) {
    for (int i = 0; i < s->nidx_unknown; i++) if (s->idx_unknown[i] == w) return 0;
    if (s->nidx_unknown < 64) s->idx_unknown[s->nidx_unknown++] = w;
    return 1;
}
static void idx_reply(Sim *s, uint32_t w) { wq_push(&s->outq, w & 0xFFFF); }
static void idx_command(Sim *s, const uint32_t *words, int n) {
    char m[200]; int k = sprintf(m, "IDX cmd");
    for (int i = 0; i < n; i++) k += sprintf(m + k, " %04x", words[i]);
    note(s, 0, "%s", m);
    uint32_t w0 = words[0];
    if (in_set(NV_WRITES, NELEM(NV_WRITES), w0)) {
        int i = 0; for (; i < s->nnvstore; i++) if (s->nvstore[i].op == w0) break;
        if (i == s->nnvstore && s->nnvstore < 32) s->nnvstore++;
        s->nvstore[i].op = w0; s->nvstore[i].n = n - 1; for (int j = 1; j < n; j++) s->nvstore[i].w[j - 1] = words[j];
        if (w0 == 0x241d) s->nv_version = (int)(words[1] >> 8);
        return;
    }
    if ((w0 == 0x320f || w0 == 0x320e) && n >= 3) { int32_t v = (int32_t)((words[1] << 16) | words[2]); s->m2_pulses = v >> 8;   /* motor 2 (lens carriage) target, for SIM_LENS_SHIFT (0e = the slow move, 0.1.26) */
        /* play (backlash) model: the lens follows the motor only once the gap of `play` steps is taken up; it stays at
           the side it was last pushed to (test option; 0.1.23 lens approach) */
        double play = 0, ppp; int ref; const char *ls = getenv("SIM_LENS_SHIFT");
        if (ls && sscanf(ls, "%lf:%d:%lf", &ppp, &ref, &play) < 3) play = 0;
        if (!s->m2_seen) { s->m2_eff = s->m2_pulses; s->m2_seen = 1; }
        else if (s->m2_pulses > s->m2_eff + play / 2) s->m2_eff = s->m2_pulses - play / 2;
        else if (s->m2_pulses < s->m2_eff - play / 2) s->m2_eff = s->m2_pulses + play / 2; }
    if (s->prof && (w0 == 0x1441 || w0 == 0x1442)) { s->lw_sel = (w0 == 0x1442); return; }   /* select CT / LW CCD */
    if (s->prof && w0 == 0x141b) { idx_reply(s, (uint32_t)(s->lw_sel ? s->r141b_lw : s->r141b_ct)); return; }
    if (w0 == 0x2438) { idx_reply(s, s->prof ? (uint32_t)s->r2438 : 3); return; }
    if (w0 == 0x140c) { idx_reply(s, 0x2B); return; }
    if (w0 == 0x1402) { idx_reply(s, (uint32_t)s->master_version << 8); return; }
    if (w0 == 0x141b) { idx_reply(s, s->motion_type == 2 ? 8 : 0); return; }
    if (w0 == 0x2407) { int a = (int)(words[1] >> 8); idx_reply(s, (uint32_t)s->nv[a] << 8); return; }
    if (w0 == 0x2406) { s->nv[words[1] >> 8] = words[1] & 0xFF; return; }
    if (w0 == 0x141c) { idx_reply(s, (uint32_t)((s->nv_version << 8) | s->nv_status)); return; }
    if (w0 == 0x141f) {
        uint32_t v = 0; for (int i = 0; i < s->nnvstore; i++) if (s->nvstore[i].op == 0x241e) { v = s->nvstore[i].n ? s->nvstore[i].w[0] : 0; break; }
        idx_reply(s, v); return;
    }
    if (w0 == 0x1443) { idx_reply(s, (uint32_t)nv_checksum(s) << 8); return; }
    if (in_set(IDX_REPLY, NELEM(IDX_REPLY), w0)) {
        if (idx_unknown_add(s, w0)) note(s, 1, "UNK indexer query %04x -> 0", w0);
        idx_reply(s, 0); return;
    }
    if (idx_unknown_add(s, w0)) note(s, 1, "UNK indexer cmd %04x (%d words)", w0, n);
}
static void idx_write_word(Sim *s, uint32_t w) {
    if (!s->ninq) { int n = (int)((w >> 12) & 0xF); s->expect = n > 1 ? n : 1; }
    if (s->ninq < 16) s->inq[s->ninq++] = w;
    if (s->ninq >= s->expect) { uint32_t words[16]; int n = s->ninq; memcpy(words, s->inq, n * sizeof(uint32_t)); s->ninq = 0; idx_command(s, words, n); }
}

/* ------------------------------------------------------------------ SCP */
static void repr_latin1(const uint8_t *s, size_t n, char *out) {
    int has_sq = 0, has_dq = 0;
    for (size_t i = 0; i < n; i++) { if (s[i] == '\'') has_sq = 1; if (s[i] == '"') has_dq = 1; }
    char q = (has_sq && !has_dq) ? '"' : '\''; int k = 0; out[k++] = q;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '\\') { out[k++] = '\\'; out[k++] = '\\'; }
        else if (c == (uint8_t)q) { out[k++] = '\\'; out[k++] = q; }
        else if (c == '\t') k += sprintf(out + k, "\\t");
        else if (c == '\n') k += sprintf(out + k, "\\n");
        else if (c == '\r') k += sprintf(out + k, "\\r");
        else if (c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0xAD) k += sprintf(out + k, "\\x%02x", c);
        else if (c < 0x80) out[k++] = (char)c;
        else { out[k++] = (char)(0xC0 | (c >> 6)); out[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[k++] = q; out[k] = 0;
}
static void scp_packet(Sim *s, const uint8_t *pkt, size_t n) {
    if (n < 4) emu_fail(s->t.log, "SCP packet too short (%zu)", n);
    uint32_t cmd = pkt[2] | (pkt[3] << 8);
    s->last_cmd = (int)cmd;
    if (cmd == 0x13) {
        size_t e = 12; while (e < n && pkt[e]) e++;
        char r[1024]; repr_latin1(pkt + 12, e > 12 ? e - 12 : 0, r);
        int found = (e - 12 == strlen(s->flash_name) && !memcmp(pkt + 12, s->flash_name, e - 12) && s->flash);
        s->cur = found; s->pos = 0; s->seq = 0; s->err = found ? 0 : 2;
        if (found) note(s, 0, "SCP open-get %s -> found %zu bytes", r, s->nflash);
        else note(s, 1, "SCP open-get %s -> MISSING", r);
        return;
    }
    char hx[80]; hexstr(pkt, n < 32 ? n : 32, hx);
    note(s, 1, "UNK SCP packet cmd %04x: %s", cmd, hx);
}
static void put16le(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put32le(uint8_t *p, uint32_t v) { put16le(p, v & 0xFFFF); put16le(p + 2, v >> 16); }
static uint8_t *scp_fetch(Sim *s, int sub, uint32_t n, int param, size_t *nout) {
    uint8_t *r = calloc(n + 64, 1); *nout = n;
    if (sub == '3') {
        uint8_t st[22]; memset(st, 0, sizeof st); size_t l = 12;
        put16le(st, n); put16le(st + 2, (param == 1 && s->cur) ? 2 : 0); put16le(st + 6, (uint32_t)s->err);
        if (s->prof) { put16le(st, 0); put16le(st + 2, s->cur ? 2 : 0); put16le(st + 4, (uint32_t)s->last_cmd); }   /* as the real scanner answers */
        if (param == 1) { put32le(st + 14, s->cur ? (uint32_t)s->nflash : 0); l = 22; }
        memcpy(r, st, l < n ? l : n); return r;
    }
    if (sub == '4') {
        size_t lo = 0, hi = 0;
        if (s->cur) {
            /* python: cur[pos:pos + min(block, n - 10)] */
            int64_t L = (int64_t)s->nflash, a = (int64_t)s->pos, k = (int64_t)n - 10; if (k > s->block) k = s->block;
            int64_t b = a + k;
            if (a > L) a = L;
            if (b < 0) { b += L; if (b < 0) b = 0; }
            if (b > L) b = L;
            lo = (size_t)a; hi = b > a ? (size_t)b : lo;
            s->pos += hi - lo;
        }
        uint8_t hdr[10] = { 0 }; put16le(hdr, s->prof ? 0 : n); put16le(hdr + 6, (uint32_t)s->seq); put16le(hdr + 8, (uint32_t)(hi - lo)); s->seq++;
        size_t k = 0;
        for (size_t i = 0; i < 10 && k < n; i++) r[k++] = hdr[i];
        for (size_t i = lo; i < hi && k < n; i++) r[k++] = s->flash[i];
        return r;
    }
    if (sub == '2') {
        s->cur = 0; uint8_t hdr[12] = { 0 }; put16le(hdr, n);
        if (s->prof) { put16le(hdr, 0); put16le(hdr + 4, '2'); put16le(hdr + 10, 1); s->last_cmd = '2'; memcpy(r, hdr, n < 12 ? n : 12); return r; }
        memcpy(r, hdr, n < 10 ? n : 10); return r;
    }
    char ch[2] = { (char)sub, 0 }, rr[16]; repr_latin1((uint8_t *)ch, 1, rr);
    note(s, 1, "UNK SCP fetch sub %s", rr);
    return r;
}

/* ------------------------------------------------------------------ BIP */
static const char *BLK_NAMES[14] = { "cmd", "l2", "bytes_per_line", "total", "w14", "pixels", "w18", "w20", "w22", "w24", "w26", "w28", "rate", "w32" };
enum { B_CMD = 0, B_BPL = 2, B_TOTAL = 3, B_PIX = 5 };
/* Test option (developer use, environment SIM_TEXTURE=sigma): instead of the ramps, prescan data is a fixed texture
   (value noise at 37, 9 and 3 pixel scales, not periodic, about 2000 +- 870 in 12 bits) seen through a per-operation shift (0.37 x k pixels across,
   -0.21 x k along for the k-th prescan) plus Gaussian noise of the given sigma (12-bit counts). Lets the averaging
   of RAW passes be tested end to end; never used unless the variable is set. */
static double sim_hash(int64_t x, int64_t y, int c) {    /* lattice value in -1..1 */
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ULL ^ (uint64_t)y * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(c + 1) * 0x165667B19E3779F9ULL;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32;
    return (double)(h >> 11) * (2.0 / 9007199254740992.0) - 1;
}
static double sim_vnoise(double x, double y, double cell, int c) {   /* smooth, not periodic */
    x /= cell; y /= cell; double fx = floor(x), fy = floor(y), tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty);
    int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double a = sim_hash(ix, iy, c), b = sim_hash(ix + 1, iy, c), d = sim_hash(ix, iy + 1, c), e = sim_hash(ix + 1, iy + 1, c);
    return (a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty;
}
static int sim_chart_now;   /* SIM_CHART_NOW is set: the simulated chart is in (read once per prescan) */
static double sim_tex(double x, double y, int c) {   /* SIM_TEXTURE_SCALE (default 1) scales it, e.g. 0.1 for a dark picture */
    static double sc = -1; if (sc < 0) { const char *e = getenv("SIM_TEXTURE_SCALE"); sc = e ? atof(e) : 1; if (sc <= 0) sc = 1; }
    /* SIM_TEXTURE_BARS=P (test option, 0.1.24): bars of period P pixels across (nothing else across - a registration
       across is ambiguous by whole periods, as the focus chart's bar fields on 10 Oct) and a faint wave along (period 97
       rows) so that along stays measurable */
    /* SIM_TEXTURE_CHART=P (test option, 0.3.0): the focus chart's two bar blocks, alternating every SIM_TEXTURE_CHART_X
       (default 300) pixels across: bars of period P varying across, tilted 0.002 px per line, then bars varying along,
       tilted 0.03 line per pixel (as on 10 Oct) */
    { static double cp = -1, cx = 300; if (cp < 0) { const char *e = getenv("SIM_TEXTURE_CHART"); cp = e ? atof(e) : 0; e = getenv("SIM_TEXTURE_CHART_X"); if (e) cx = atof(e); }
      if (cp > 0 && sim_chart_now) {   /* only once the program says the chart is in (SIM_CHART_NOW, set by the setup's chart step in tests) */ (void)c; double xm = fmod(fabs(x), 2 * cx), u = xm < cx ? (x + 0.002 * y) : (y + 0.03 * x);   /* blocks alternate every cx pixels across */ return sc * (2000 + 1500 * tanh(2.5 * sin(6.283185307 * u / cp))); } }
    { static double bp = -1; if (bp < 0) { const char *e = getenv("SIM_TEXTURE_BARS"); bp = e ? atof(e) : 0; }
      if (bp > 0) { (void)c; return sc * (2000 + 900 * tanh(3 * sin(6.283185307 * x / bp)) + 300 * sin(6.283185307 * y / 97)); } }
    return sc * (2000 + 500 * sim_vnoise(x, y, 37, c) + 250 * sim_vnoise(x, y, 9, c + 7) + 120 * sim_vnoise(x, y, 3, c + 13));
}
static double sim_gauss(Sim *s) {
    uint64_t *x = &s->nseed; double u, v;
    *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; u = ((*x >> 11) + 1) * (1.0 / 9007199254740993.0);
    *x ^= *x << 13; *x ^= *x >> 7; *x ^= *x << 17; v = (*x >> 11) * (1.0 / 9007199254740992.0);
    return sqrt(-2 * log(u)) * cos(6.283185307 * v);
}
static double sim_prnu(void) { static double pr = -1; if (pr < 0) { const char *e = getenv("SIM_PRNU"); pr = e ? atof(e) : 0; } return pr; }
/* the light reaching CCD photosite p (0 .. 5003) in colour c with SIM_PRNU (the CCD's own pattern, the same in the
   white line and in pictures) */
static double sim_white(int p, int c) {
    double lv = c == 0 ? 21000 : c == 1 ? 22500 : 20500, v = lv * (p & 1 ? 0.985 : 1) * (1 + sim_prnu() * sim_hash(p, 7771, c) * 1.7320508);
    if (p % 397 == 11) v *= 0.96;   /* a few weaker sites */
    return v;
}
/* SIM_STRIP_SPECK=p:depth (test option, 0.4.0): a speck on the KEEP CLEAN strip - only the white line sees it (photosites
   p-3 .. p+3 darker by depth, e.g. 0.008), not the pictures */
static double sim_strip(int p) {
    static int sp = -2; static double sd; if (sp == -2) { const char *e = getenv("SIM_STRIP_SPECK"); sp = -1; if (e && sscanf(e, "%d:%lf", &sp, &sd) != 2) sp = -1; }
    return sp >= 0 && p >= sp - 3 && p <= sp + 3 ? 1 - sd : 1;
}
volatile int sim_dsp_drop = -1, sim_dsp_cut = 32, sim_dsp_wcomp = 1; volatile double sim_dsp_inpol = 1;
/* SIM_EMPTY=level (test option, 0.4.0): prescans (previews and Max Details) are an empty transparent frame - the light
   path of each CCD photosite (sim_white) times the gain vector the driver last wrote (3b to a2 8cc0: 5137 x RGB words),
   normalised to the given level (the scanner's 14-bit samples, as the driver reads them in its 14-bit mode: an empty
   frame gives about 12500 on the hardware, s37b), with a little noise. The CCD photosite of picture column x is DROP + CUT - 1 +
   x x INPOL_WIDTH_STEP x the width compression (previews: 8), from the driver's own prints of this picture (app.c passes it in sim_dsp_*: the real
   controller has it from the image processor's registers). So the streak correction can be checked end to end. */
static int sim_empty_px(Sim *s, uint32_t pix, uint32_t rows, uint8_t *p) {
    static double lvl = -1; if (lvl < 0) { const char *e = getenv("SIM_EMPTY"); lvl = e ? atof(e) : 0; }
    if (!(lvl > 0) || sim_dsp_drop < 0) return 0;
    static double g[5004 * 3]; double m[3] = { 0, 0, 0 };
    for (int q = 0; q < 5004; q++) for (int c = 0; c < 3; c++) { double gv = im_getd(&s->regs, 0x28cc0 + 2 * (3 * q + c), 0x4000);
        g[3 * q + c] = (sim_prnu() > 0 ? sim_white(q, c) : 1) * (gv > 0 ? gv : 1); m[c] += g[3 * q + c] / 5004; }
    if (!s->nseed) s->nseed = 0x9E3779B97F4A7C15ULL;
    double base = sim_dsp_drop + sim_dsp_cut - 1, st = (sim_dsp_inpol > 0.2 && sim_dsp_inpol < 5 ? sim_dsp_inpol : 1) * (sim_dsp_wcomp > 0 ? sim_dsp_wcomp : 1);
    for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < pix; x++) {
        int q = (int)(base + x * st + 0.5); if (q < 0) q = 0; if (q > 5003) q = 5003;
        for (int c = 0; c < 3; c++) { double v = lvl * g[3 * q + c] / m[c] + 8.0 * sim_gauss(s); put16le(p + 2 * c, v < 0 ? 0 : v > 16383 ? 16383 : (uint32_t)(v + 0.5)); }
        p += 6; }
    return 1;
}
/* SIM_NEGATIVE=1 (test option, 0.4.0): prescans are a colour negative of a grey wedge across the CCD (scene log
   exposure 0.1 - 1.1 from photosite 0 to 5003) on C-41-like film: per channel base density 0.47 / 0.92 / 1.25 (the orange
   mask; hardware s39: 16840 / 5950 / 2830 of an empty frame's 50000) plus gamma 0.65 / 0.60 / 0.55 x the exposure; in
   previews (image-processor mode 6) the outer 3 % is holder (opaque). A correct conversion gives a neutral wedge,
   dark on the left */
static int sim_negative_px(Sim *s, uint32_t pix, uint32_t rows, int64_t y0, uint8_t *p) {
    static int on = -1; if (on < 0) on = getenv("SIM_NEGATIVE") != NULL;
    if (!on) return 0;
    static const double bd[3] = { 0.47, 0.92, 1.25 }, gm[3] = { 0.65, 0.60, 0.55 };
    if (!s->nseed) s->nseed = 0x9E3779B97F4A7C15ULL;
    uint32_t mx = pix * 3 / 100, my = rows * 3 / 100; int pv = s->mode == 6;
    for (uint32_t y = 0; y < rows; y++) for (uint32_t x = 0; x < pix; x++) {
        int hold = pv && (x < mx || x >= pix - mx || y < my || y >= rows - my);
        /* across the CCD (the driver's DROP_PIXELS of this picture, as SIM_EMPTY): previews and RAWs of the same place agree */
        double e = sim_dsp_drop >= 0 ? 0.1 + 1.0 * (sim_dsp_drop + sim_dsp_cut - 1 + x * (sim_dsp_inpol > 0.2 && sim_dsp_inpol < 20 ? sim_dsp_inpol : 1) * (sim_dsp_wcomp > 0 ? sim_dsp_wcomp : 1)) / 5004.0 : 0.1 + 1.0 * x / (pix > 1 ? pix - 1 : 1);
        for (int c = 0; c < 3; c++) { double v = hold ? 2 : 12500 * pow(10, -(bd[c] + gm[c] * e)) + 1.0 * sim_gauss(s); put16le(p + 2 * c, v < 0 ? 0 : v > 16383 ? 16383 : (uint32_t)(v + 0.5)); }
        p += 6; }
    (void)y0; return 1;
}
static void make_prescan(Sim *s, uint32_t pix, uint32_t rows, int64_t y0) {
    size_t n = (size_t)pix * rows * 6; uint8_t *d = malloc(n ? n : 1), *p = d;
    const char *tx = getenv("SIM_TEXTURE");
    if (sim_empty_px(s, pix, rows, d) || sim_negative_px(s, pix, rows, y0, d)) { bq_put(&s->out, d, n); free(d); return; }
    if (tx) {
        double sig = atof(tx), ox = 0.37 * s->prescan_ops, oy = -0.21 * s->prescan_ops; sim_chart_now = getenv("SIM_CHART_NOW") != NULL;
        if (getenv("SIM_TEXTURE_NODRIFT")) ox = oy = 0;
        /* SIM_LENS_SHIFT=px:ref (test option): a model of a lens carriage moving ACROSS - the picture shifts by px per
           motor-2 step away from ref (the lens's LPOS in steps); used only to check the lens-shift test's analysis */
        { const char *ls = getenv("SIM_LENS_SHIFT"); double ppp = 0; int ref = 0; if (ls && sscanf(ls, "%lf:%d", &ppp, &ref) == 2) ox += ppp * ((s->m2_seen ? s->m2_eff : s->m2_pulses) - ref); }
        if (!s->nseed) s->nseed = 0x9E3779B97F4A7C15ULL;
        for (int64_t y = y0; y < y0 + rows; y++) for (uint32_t x = 0; x < pix; x++) {
            for (int c = 0; c < 3; c++) { double v = sim_tex(x - ox, y - oy, c) + sig * sim_gauss(s); put16le(p + 2 * c, v < 0 ? 0 : v > 4095 ? 4095 : (uint32_t)(v + 0.5)); }
            p += 6; }
        bq_put(&s->out, d, n); free(d); return;
    }
    uint32_t den = pix > 1 ? pix - 1 : 1;
    for (int64_t y = y0; y < y0 + rows; y++)
        for (uint32_t x = 0; x < pix; x++) {
            uint32_t r = (uint32_t)((uint64_t)x * 4095 / den), g = (uint32_t)((y * 8) & 4095), b = 2048;
            put16le(p, r); put16le(p + 2, g); put16le(p + 4, b); p += 6;
        }
    bq_put(&s->out, d, n); free(d);
}
static void bip_produce(Sim *s) {
    if (!s->has_block) return;
    uint32_t *b = s->blk;
    /* the picture data comes from the image processor's mode, set by the driver (6 prescan, A Max Detail, 7 scan);
       the block's cmd only tells the controller how to buffer it - a Max Detail sent as cmd 4 (streamed RAW) gives
       the same data (firmware: the transfer loop is the same code for both, fwmem.h) */
    if ((s->mode == 6 || s->mode == 0xA) && (b[B_CMD] == 1 || b[B_CMD] == 4) && b[B_BPL]) {
        uint32_t pix = b[B_PIX], rows = b[B_TOTAL] / b[B_BPL];
        make_prescan(s, pix, rows, s->row); s->row += rows;
        note(s, 1, "BIP prescan chunk: %u pixels x %u rows (rows so far %lld)", pix, rows, (long long)s->row);
    } else if (s->mode == 7 && b[B_CMD] == 4 && b[B_BPL] && (!s->final_done || s->chunks)) {
        uint32_t bpl = b[B_BPL], pix = b[B_PIX], rows = b[B_TOTAL] / bpl;
        uint32_t bpp = bpl / (pix > 1 ? pix : 1); if (bpp < 1) bpp = 1;
        size_t n = (size_t)rows * bpl; uint8_t *d = calloc(n ? n : 1, 1);
        uint32_t dx = pix > 1 ? pix - 1 : 1, dy = rows > 1 ? rows - 1 : 1;
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t *p = d + (size_t)y * bpl;
            for (uint32_t x = 0; x < pix; x++) {
                uint8_t v[4] = { (uint8_t)((uint64_t)x * 255 / dx), (uint8_t)((uint64_t)y * 255 / dy), 128, 32 };
                if (s->chunks) { v[1] = (uint8_t)(((uint64_t)(s->row + y) * 3) & 0xFF); v[2] = (uint8_t)((((s->row + y) / 64) & 1) ? 200 : 60); }
                for (uint32_t c = 0; c < bpp; c++) *p++ = c < 4 ? v[c] : 0;
            }
        }
        bq_put(&s->out, d, n); free(d); s->final_done = 1;
        if (s->chunks) s->row += rows;
        note(s, 1, "BIP final scan: %u x %u, %u bytes/pixel -> %zu bytes", pix, rows, bpp, n);
    }
}
static void bip_ctrl(Sim *s, uint32_t v) {
    if ((v & 0x05) == 0x05 && !(s->last_ctrl & 0x04)) wq_push(&s->ints, 0x0001);
    s->last_ctrl = v;
}
static void bip_command(Sim *s, uint32_t c) {
    note(s, 0, "BIP cmd %02x", c);
    if (c < 0x10) { wq_push(&s->ints, (c << 8) | 1); return; }
    uint32_t n = c >> 4;
    if ((c & 0xF) == 0) { wq_push(&s->ints, (n << 8) | c); return; }
    if ((c & 0xF) == 1) {
        s->mode = (int)n; s->row = 0; s->final_done = 0; if (n == 6 || n == 0xA) s->prescan_ops++;
        if (s->chunks) { s->out.n = 0; s->out.head = 0; }     /* nothing left over from the previous operation */
        note(s, 1, "BIP go: mode %x", n);
        wq_push(&s->ints, (n << 8) | 2); wq_push(&s->ints, (n << 8) | c); return;
    }
    note(s, 1, "UNK BIP command %02x", c);
}
static void bip_complex(Sim *s, const uint8_t *data, size_t n) {
    if (n < 34) emu_fail(s->t.log, "BIP complex command too short (%zu)", n);
    uint8_t d[34]; memcpy(d, data, 34);
    for (int k = 0; k + 1 < 34; k += 2) { uint8_t t = d[k]; d[k] = d[k + 1]; d[k + 1] = t; }
    static const int sz[14] = { 2, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 };
    int o = 0;
    for (int i = 0; i < 14; i++) { uint32_t v = 0; for (int j = 0; j < sz[i]; j++) v = (v << 8) | d[o + j]; s->blk[i] = v; o += sz[i]; }
    s->has_block = 1;
    s->blk_rows_set = ((s->blk[B_CMD] == 1 || (s->blk[B_CMD] == 4 && (s->mode == 6 || s->mode == 0xA))) && s->blk[B_BPL]);
    if (s->blk_rows_set) s->blk_rows = s->blk[B_TOTAL] / s->blk[B_BPL];
    char m[1024]; int k = sprintf(m, "{");
    for (int i = 0; i < 14; i++) k += sprintf(m + k, "%s'%s': %u", i ? ", " : "", BLK_NAMES[i], s->blk[i]);
    if (s->blk_rows_set) k += sprintf(m + k, ", 'rows': %u", s->blk_rows);
    sprintf(m + k, "}");
    note(s, 1, "BIP complex cmd %s", m);
    { const char *e = getenv("SIM_CAPTURE_WAIT"); if (e && (s->blk[B_CMD] == 1 || s->blk[B_CMD] == 4)) s->cap_wait = atoi(e); }
    bip_produce(s);
}

/* ------------------------------------------------------------------ bus */
static int unk_first(uint8_t *set, int64_t off) {
    if (off < 0 || off >= 0x100000) return 0;
    size_t i = (size_t)(off >> 8);
    if (set[i]) return 0;
    set[i] = 1; return 1;
}
static uint32_t dark_level(Sim *s, int ch) { int64_t v = 2200 - (int64_t)s->dac[ch]; if (v < 0) v = 0; if (v > 4095) v = 4095; return (uint32_t)v; }
static uint32_t bus_read(Sim *s, int64_t off) {
    /* SIM_PRNU=a (test option, 0.3.1): the white line of a gain calibration (image-processor mode 5, buffer a5) as a
       real CCD gives it - about 22000 with each photosite a little different (+-a, fixed per site), the odd sites 1.5 %
       lower (the two readout channels), positions 5004 - 5136 not light-sensitive (hardware s19). Without it the line
       reads 0 as before. */
    { if (sim_prnu() > 0 && s->mode == 5 && off >= 0x50000 && off < 0x50000 + 30822) {
          int idx = (int)((off - 0x50000) / 2), p = idx / 3, c = idx % 3;
          if (p >= 5004) return 5;
          double v = sim_white(p, c) * sim_strip(p);
          if (!s->nseed) s->nseed = 0x9E3779B97F4A7C15ULL;
          v *= 1 + 0.0005 * sim_gauss(s);   /* each calibration's own noise (hardware: 0.015 - 0.02 % for 256 lines) */
          return (uint32_t)(v + 0.5);
      } }
    if (s->level >= 2) {
        if (off >= 0x10000 && off < 0x20000) return dark_level(s, (int)(((off - 0x10000) / 2) % 3));
        if (off >= 0x21000 && off < 0x21000 + 0x80) {
            switch ((int)(off - 0x21000)) {
            case 0x0C: case 0x0E: case 0x10: case 0x12: case 0x14: case 0x16: return 400;
            case 0x7C: return 12000; case 0x7E: return 12400; case 0x80: return 11800;
            default: return 0;
            }
        }
    }
    if (off == IDX_STAT) return (s->outq.n ? 0x8000u : 0) | (s->irq ? 0x0800u : 0);
    if (off == 0xC000) { s->tog_c = !s->tog_c; return (uint32_t)s->ccd_conf | (s->tog_c ? 0x1000u : 0) | 0x2000u; }
    if (off == 0x1000) { s->tog_1 = !s->tog_1; return (im_getd(&s->regs, 0x1000, 0) & ~0x100u) | (s->tog_1 ? 0x100u : 0); }
    if (off == 0xB2002) return s->sharp2 ? ((im_getd(&s->regs, 0xB2000, 0x1E00) >> 9) & 0xF) << 12 : 0xFFFF;
    if (off == IDX_DATA) return s->outq.n ? wq_pop(&s->outq) : 0;
    if (off == 0xE0002) return 0x0001;
    uint32_t v; if (im_get(&s->regs, off, &v)) return v;
    if (unk_first(s->unk_r, off)) note(s, 1, "UNK read  bus %05llx (first in block)", (long long)off);
    return 0;
}
static void bus_write(Sim *s, int64_t off, uint32_t v) {
    if (s->level >= 2 && (off == 0xA000 || off == 0xA008 || off == 0xA010)) {
        s->dac[(off - 0xA000) / 8] = v & 0xFFF; im_put(&s->regs, off, v); return;
    }
    if (off == IDX_CMD) { idx_write_word(s, v); return; }
    if (off == 0xE0000) bip_ctrl(s, v);
    if (off == 0xE0010) { bip_command(s, v); return; }
    if (off == IDX_IRQ_RST) { s->irq = 0; return; }
    im_put(&s->regs, off, v);
    if (unk_first(s->unk_w, off)) note(s, 1, "UNK write bus %05llx = %04x (first in block)", (long long)off, v);
}

/* ------------------------------------------------------------- firmware */
static uint8_t *dup_n(const void *p, size_t n) { uint8_t *d = malloc(n ? n : 1); if (n) memcpy(d, p, n); return d; }
static uint8_t *le16(uint32_t v, size_t *n) { uint8_t *d = malloc(2); put16le(d, v); *n = 2; return d; }
static int sim_execute(ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out,
                       uint32_t alloc, uint8_t **din, size_t *nin) {
    Sim *s = (Sim *)t; (void)alloc;
    *din = NULL; *nin = 0;
    uint8_t c[16] = { 0 }; memcpy(c, cdb, ncdb > 16 ? 16 : ncdb);
    int op = c[0];
    if (sim_unplug) {
        /* as spti.c with a scanner that no longer answers: the command fails with the autosense ABORTED COMMAND
           (70 00 0B); the next REQUEST SENSE is answered from it (status 0), any other command - also a second
           REQUEST SENSE - fails again (hardware s36: 03 st=0, 03 st=2, ...) */
        if (s->fw_dead != 2) {
            note(s, 1, "the scanner is switched off (simulator, --sim-off): no answer from op %02x on", op);
            if (s->t.note) { char m[400]; snprintf(m, sizeof m, "THE SCANNER IS NOT ANSWERING (SCSI command %02x - simulated: switched off): it may be switched off, "
                "or its cable or adapter came loose. Switch the scanner off, wait 10 seconds, switch it on, wait for the steady green light, then start this program again.", op); s->t.note(m); }
        }
        s->fw_dead = 2; sim_fw_dead = 1;
        if (op == 0x03 && s->unplug_sense) {
            uint32_t n = alloc ? alloc : (ncdb >= 5 ? c[4] : 0); uint8_t *d = calloc(n + 1, 1);
            uint8_t sn[18] = { 0x70, 0, 0x0B, 0, 0, 0, 0, 10 }; memcpy(d, sn, n < 18 ? n : 18);
            s->unplug_sense = 0; *din = d; *nin = n; return 0;
        }
        s->unplug_sense = 1; return 2;
    }
    if (s->fw_dead) { note(s, 1, "no answer to op %02x: the controller program has stopped (memory)", op); return 2; }
    if (op == 0x00) return 0;
    if (op == 0x12) {
        uint8_t inq[36] = { 0 }; inq[0] = 6; inq[4] = 31;
        if (s->prof) { inq[4] = (uint8_t)s->inq4; inq[7] = (uint8_t)s->inq7; }
        memcpy(inq + 8, "SCITEX  ", 8); memcpy(inq + 16, "SMART340        ", 16); memcpy(inq + 32, "1.C8", 4);
        size_t n = c[4] < 36 ? c[4] : 36; *din = dup_n(inq, n); *nin = n; return 0;
    }
    if (op == 0x03) {
        uint8_t sn[8] = { 0xF0, 0, 5, 0, 0, 0, 0, 0 }; size_t n = c[4];
        *din = calloc(n + 1, 1); memcpy(*din, sn, n < 8 ? n : 8); *nin = n; return 0;
    }
    if (op == 0xD0 || op == 0xD4 || op == 0xED) return 0;
    if (op == 0xEE) { *din = le16(1, nin); return 0; }
    if (op == 0x4E) {
        int unit = c[1] >> 5; uint32_t w = unit == 0 ? (uint32_t)s->master_version << 8 : (uint32_t)((s->main_mn << 4) | s->main_mj);
        *din = le16(w, nin); return 0;
    }
    if (op == 0x3B || op == 0x3C) {
        uint32_t addr = ((uint32_t)c[2] << 24) | (c[3] << 16) | (c[4] << 8) | c[5];
        uint32_t n = ((uint32_t)c[6] << 16) | (c[7] << 8) | c[8];
        int64_t off = (int64_t)addr - BASE;
        if (op == 0x3C) {
            uint8_t *d = malloc(n + 2);
            for (uint32_t k = 0; k < n; k += 2) put16le(d + k, bus_read(s, off + k));
            *din = d; *nin = n; return 0;
        }
        for (size_t k = 0; k + 1 < nout; k += 2) bus_write(s, off + (int64_t)k, out[k] | (out[k + 1] << 8));
        return 0;
    }
    if (op == 0x80) {
        FwBip fb;
        if (has_out && fw_bip_check(cdb, ncdb, out, (int)nout, s->fw_reserve, &fb) == 0) {
            if (!fb.ok) { s->fw_dead = 1; sim_fw_dead = 1; note(s, 1, "controller: Non availble mem for BIP activity (cmd %d needs %llu bytes, %llu free) - the controller program stops", fb.cmd, (unsigned long long)fb.need, (unsigned long long)fb.free_before); return 0; }
            s->fw_reserve = fb.reserve_after;
        }
        bip_complex(s, out, nout); return 0;
    }
    if (op == 0x83) {
        uint32_t v = s->ints.n ? wq_pop(&s->ints) : 0;
        if (v) note(s, 0, "BIP int -> %04x", v);
        *din = le16(v, nin); return 0;
    }
    if (op == 0x48) {
        int sel = c[2]; uint32_t n = ((uint32_t)c[6] << 16) | (c[7] << 8) | c[8];
        if (c[5] == 2) {      /* the controller's counters a09a / a09c (0x1e95; guard.c): 4 bytes, little-endian words. The
                                 model produces lines on demand, so it never waits for the host: 0, 0 - SIM_FW_WAITS sets a09c
                                 (and a09a after a colon) to test the reporting */
            const char *e = getenv("SIM_FW_WAITS"); int w = 0, l = 0; if (e) sscanf(e, "%d:%d", &w, &l);   /* "a09c[:a09a]" */
            uint8_t *d = calloc(n + 4, 1); if (n >= 4) { d[0] = (uint8_t)l; d[1] = (uint8_t)(l >> 8); d[2] = (uint8_t)w; d[3] = (uint8_t)(w >> 8); }
            *din = d; *nin = n; return 0;
        }
        if (sel == 1 && c[5] == 1) {
            /* SIM_CAPTURE_WAIT=n (test option, 0.4.0): the first n "data ready?" polls of each picture answer "not yet",
               as a real scanner's slower transfer does - so the program can act while a picture is being sent */
            if (s->cap_wait > 0) { s->cap_wait--; *din = le16(0, nin); return 0; }
            if (!s->out.n) bip_produce(s);
            *din = le16(s->out.n ? 1 : 0, nin); return 0;
        }
        if (sel == 1 || sel == 3) {
            size_t m = s->out.n < n ? s->out.n : n;
            uint8_t *d = calloc(n + 1, 1); memcpy(d, s->out.b + s->out.head, m);
            s->out.head += m; s->out.n -= m; if (!s->out.n) s->out.head = 0;
            if (m < n) note(s, 1, "prescan read beyond produced data (%zu < %u)", m, n);
            *din = d; *nin = n; return 0;
        }
        char hx[600]; hexstr(cdb, (size_t)ncdb, hx);
        note(s, 1, "UNK 48 selector %s", hx);
        *din = calloc(n + 1, 1); *nin = n; return 0;
    }
    if (op == 0x82) { *din = le16(1, nin); return 0; }
    if (op == 0xD5) { *din = le16(0, nin); return 0; }
    if (op == 0x42) { scp_packet(s, out, nout); return 0; }
    if (op == 0x43) { uint32_t n = c[2] | (c[3] << 8); *din = scp_fetch(s, c[1], n, c[4], nin); return 0; }
    char hx[600]; hexstr(cdb, (size_t)ncdb, hx);
    note(s, 1, "UNK SCSI op %s", hx);
    size_t n = 0;
    if (ncdb == 10) n = ((size_t)c[6] << 16) | (c[7] << 8) | c[8]; else if (ncdb == 6) n = c[4];
    if (!has_out) { *din = calloc(n + 1, 1); *nin = n; }
    return 0;
}
static void sim_destroy(ScsiTarget *t) {
    Sim *s = (Sim *)t; free(s->outq.v); free(s->ints.v); free(s->out.b); free(s->regs.k); free(s->regs.v); free(s->regs.used); free(s->flash); free(s);
}

uint8_t *make_flash_file(const uint8_t *rsrc, size_t n, size_t *outn) {
    int32_t ck = 0; for (size_t i = 0; i < n; i++) ck += rsrc[i] > 127 ? (int)rsrc[i] - 256 : rsrc[i];
    uint32_t len = (uint32_t)(16 + n), c = (uint32_t)ck & 0xFFFF;
    uint8_t *d = calloc(16 + n, 1);
    d[0] = len >> 24; d[1] = len >> 16; d[2] = len >> 8; d[3] = len; d[4] = c >> 8; d[5] = c;
    memcpy(d + 16, rsrc, n); *outn = 16 + n; return d;
}

/* Profile file (text): 'nv ADDR VALUE', 'r2438 V', 'r141b_lw V', 'r141b_ct V', 'inq4 V', 'inq7 V' (hex). Lines starting
 * with # are comments. Values come from a hardware trace (hwtools/mkprofile.py). Returns 0 on success. */
int sim_load_profile(ScsiTarget *t, const char *path) {
    Sim *s = (Sim *)t; FILE *f = u_fopen(path, "rb"); if (!f) return -1;
    char line[256]; s->prof = 1; s->chunks = 1; s->block = 0x7FFFFFFF; s->r2438 = 3; s->r141b_lw = s->r141b_ct = 0; s->inq4 = 31; s->inq7 = 0;
    while (fgets(line, sizeof line, f)) {
        char k[32]; unsigned a, v;
        if (line[0] == '#') continue;
        if (sscanf(line, "nv %x %x", &a, &v) == 2 && a < 256) { s->nv[a] = (uint8_t)v; continue; }
        if (sscanf(line, "%31s %x", k, &v) == 2) {
            if (!strcmp(k, "r2438")) s->r2438 = (int)v; else if (!strcmp(k, "r141b_lw")) s->r141b_lw = (int)v;
            else if (!strcmp(k, "r141b_ct")) s->r141b_ct = (int)v; else if (!strcmp(k, "inq4")) s->inq4 = (int)v;
            else if (!strcmp(k, "inq7")) s->inq7 = (int)v;
        }
    }
    fclose(f); return 0;
}

ScsiTarget *sim_new(int level, Emu *log, const char *flash_name, const uint8_t *flash, size_t nflash) {
    Sim *s = calloc(1, sizeof *s);
    s->t.execute = sim_execute; s->t.destroy = sim_destroy; s->t.log = log;
    s->level = level;
    s->master_version = 17; s->main_mj = 11; s->main_mn = 11; s->motion_type = 1;
    s->ccd_conf = 0x0800 | 0x0100; s->sharp2 = 1;
    s->block = 512; s->mode = -1;
    s->dac[0] = s->dac[1] = s->dac[2] = 0x800;
    snprintf(s->flash_name, sizeof s->flash_name, "%s", flash_name ? flash_name : "");
    if (flash) { s->flash = dup_n(flash, nflash); s->nflash = nflash; }
    return &s->t;
}
