/* ppc.c - 32-bit PowerPC user-mode interpreter.
 *
 * Implements the complete non-privileged 32-bit PowerPC instruction set of the 601/603/604 generation
 * (integer, condition register, branch, load/store incl. update/indexed/multiple/string/byte-reversed,
 * floating point incl. single precision, FPSCR moves). Reference: "PowerPC Microprocessor Family:
 * The Programming Environments for 32-Bit Microprocessors" (IBM/Motorola, MPCFPE32B).
 *
 * Single-precision arithmetic is correctly rounded once (as on the 603/604): the double-precision
 * intermediate is computed with round-to-odd, which makes the final rounding to single exact.
 */
#include "ppc.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>

/* gcc: build with -frounding-math so rounding-mode changes are honoured */

#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)

#define XER_SO 0x80000000u
#define XER_OV 0x40000000u
#define XER_CA 0x20000000u

PPC *ppc_new(void) {
    PPC *p = (PPC *)calloc(1, sizeof(PPC));
    p->msr = 0x2000; /* FP available */
    return p;
}

void ppc_free(PPC *p) {
    if (!p) return;
    for (uint32_t i = 0; i < PPC_NCHUNKS; i++) free(p->chunk[i]);
    free(p);
}

int ppc_map(PPC *p, uint32_t addr, uint32_t size) {
    if (!size) return 0;
    uint64_t end = (uint64_t)addr + size;
    for (uint64_t a = addr & ~(uint64_t)(PPC_CHUNK_SIZE - 1); a < end; a += PPC_CHUNK_SIZE) {
        uint32_t i = (uint32_t)(a >> PPC_CHUNK_BITS);
        if (!p->chunk[i]) { p->chunk[i] = (uint8_t *)calloc(1, PPC_CHUNK_SIZE); if (!p->chunk[i]) return -1; }
    }
    return 0;
}

uint8_t *ppc_host(PPC *p, uint32_t a) {
    uint8_t *c = p->chunk[a >> PPC_CHUNK_BITS];
    return c ? c + (a & (PPC_CHUNK_SIZE - 1)) : NULL;
}

int ppc_read(PPC *p, uint32_t a, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        uint8_t *h = ppc_host(p, a); if (!h) return -1;
        uint32_t room = PPC_CHUNK_SIZE - (a & (PPC_CHUNK_SIZE - 1)); if (room > n) room = n;
        memcpy(d, h, room); d += room; a += room; n -= room;
    }
    return 0;
}

int ppc_write(PPC *p, uint32_t a, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    while (n) {
        uint8_t *h = ppc_host(p, a); if (!h) return -1;
        uint32_t room = PPC_CHUNK_SIZE - (a & (PPC_CHUNK_SIZE - 1)); if (room > n) room = n;
        memcpy(h, s, room); s += room; a += room; n -= room;
    }
    return 0;
}

/* ------------------------------------------------------------------ memory access with fault */
typedef struct { PPC *p; int fault; } Ctx;

static inline int mem_ok(PPC *p, uint32_t a, uint32_t n, int w, int *fault) {
    uint32_t off = a & (PPC_CHUNK_SIZE - 1);
    if (p->chunk[a >> PPC_CHUNK_BITS] && off + n <= PPC_CHUNK_SIZE) return 1;
    /* slow path: crossing chunks or unmapped */
    for (uint32_t k = 0; k < n; k++) {
        if (!p->chunk[(a + k) >> PPC_CHUNK_BITS]) { p->fault_addr = a + k; p->fault_write = w; *fault = 1; return 0; }
    }
    return 2; /* mapped but crosses chunks */
}

static uint32_t ld32(PPC *p, uint32_t a, int *f) {
    int r = mem_ok(p, a, 4, 0, f); if (!r) return 0;
    uint32_t v;
    if (r == 1) memcpy(&v, ppc_host(p, a), 4); else ppc_read(p, a, &v, 4);
    return BSWAP32(v);
}
static uint32_t ld16(PPC *p, uint32_t a, int *f) {
    int r = mem_ok(p, a, 2, 0, f); if (!r) return 0;
    uint16_t v;
    if (r == 1) memcpy(&v, ppc_host(p, a), 2); else ppc_read(p, a, &v, 2);
    return BSWAP16(v);
}
static uint32_t ld8(PPC *p, uint32_t a, int *f) {
    if (!mem_ok(p, a, 1, 0, f)) return 0;
    return *ppc_host(p, a);
}
static uint64_t ld64(PPC *p, uint32_t a, int *f) {
    int r = mem_ok(p, a, 8, 0, f); if (!r) return 0;
    uint64_t v;
    if (r == 1) memcpy(&v, ppc_host(p, a), 8); else ppc_read(p, a, &v, 8);
    return BSWAP64(v);
}
static void st32(PPC *p, uint32_t a, uint32_t v, int *f) {
    int r = mem_ok(p, a, 4, 1, f); if (!r) return;
    v = BSWAP32(v);
    if (r == 1) memcpy(ppc_host(p, a), &v, 4); else ppc_write(p, a, &v, 4);
}
static void st16(PPC *p, uint32_t a, uint32_t v, int *f) {
    int r = mem_ok(p, a, 2, 1, f); if (!r) return;
    uint16_t x = BSWAP16((uint16_t)v);
    if (r == 1) memcpy(ppc_host(p, a), &x, 2); else ppc_write(p, a, &x, 2);
}
static void st8(PPC *p, uint32_t a, uint32_t v, int *f) {
    if (!mem_ok(p, a, 1, 1, f)) return;
    *ppc_host(p, a) = (uint8_t)v;
}
static void st64(PPC *p, uint32_t a, uint64_t v, int *f) {
    int r = mem_ok(p, a, 8, 1, f); if (!r) return;
    v = BSWAP64(v);
    if (r == 1) memcpy(ppc_host(p, a), &v, 8); else ppc_write(p, a, &v, 8);
}

/* ------------------------------------------------------------------ helpers */
static inline double d_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t b_of(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static inline float f_of(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline uint32_t fb_of(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static inline uint32_t mask32(int mb, int me) {
    uint32_t a = 0xFFFFFFFFu >> mb, b = 0xFFFFFFFFu << (31 - me);
    return mb <= me ? (a & b) : (a | b);
}
static inline uint32_t rotl(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline void set_crf(PPC *p, int field, uint32_t v4) {
    int sh = 28 - 4 * field;
    p->cr = (p->cr & ~(0xFu << sh)) | ((v4 & 0xF) << sh);
}
static inline void rec0(PPC *p, uint32_t r) {
    uint32_t v = ((int32_t)r < 0) ? 8 : (r ? 4 : 2);
    if (p->xer & XER_SO) v |= 1;
    set_crf(p, 0, v);
}
static inline void set_ov(PPC *p, int ov) {
    if (ov) p->xer |= XER_OV | XER_SO; else p->xer &= ~XER_OV;
}
static inline void set_ca(PPC *p, int ca) { if (ca) p->xer |= XER_CA; else p->xer &= ~XER_CA; }

/* add with carry/overflow: r = a + b + c */
static inline uint32_t addx(PPC *p, uint32_t a, uint32_t b, uint32_t c, int setca, int oe) {
    uint64_t s = (uint64_t)a + b + c;
    uint32_t r = (uint32_t)s;
    if (setca) set_ca(p, (int)(s >> 32));
    if (oe) set_ov(p, (int)((~(a ^ b) & (a ^ r)) >> 31));
    return r;
}

/* ---------------------------------------------------------------- floating point */
static int fe_mode(uint32_t fpscr) {
    switch (fpscr & 3) { case 1: return FE_TOWARDZERO; case 2: return FE_UPWARD; case 3: return FE_DOWNWARD; default: return FE_TONEAREST; }
}

/* round a double to single precision with the given rounding mode, returned as double */
static double round_single(double x, uint32_t fpscr) {
    if ((fpscr & 3) == 0) return (double)(float)x;
    int old = fegetround(); fesetround(fe_mode(fpscr));
    volatile double vx = x; volatile float f = (float)vx; double r = f;
    fesetround(old); return r;
}

/* Correctly rounded single result of op(a,b[,c]). Compute in double with round-to-odd, then round to single. */
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MADD, OP_MSUB, OP_SQRT };
static double dop(int op, double a, double b, double c) {
    switch (op) {
    case OP_ADD: return a + b; case OP_SUB: return a - b; case OP_MUL: return a * c; case OP_DIV: return a / b;
    case OP_MADD: return fma(a, c, b); case OP_MSUB: return fma(a, c, -b); case OP_SQRT: return sqrt(b);
    }
    return 0;
}
#define QNAN_DEFAULT 0x7FF8000000000000ull
static inline int is_nan_bits(uint64_t x) { return ((x >> 52) & 0x7FF) == 0x7FF && (x & 0x000FFFFFFFFFFFFFull); }
/* PowerPC NaN propagation: first NaN operand in order A, B, C (quieted); else default QNaN for invalid ops. */
static int nan_result(int op, uint64_t a, uint64_t b, uint64_t c, uint64_t *out) {
    uint64_t cand[3]; int n = 0;
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_DIV: cand[n++] = a; cand[n++] = b; break;
    case OP_MUL: cand[n++] = a; cand[n++] = c; break;
    case OP_MADD: case OP_MSUB: cand[n++] = a; cand[n++] = b; cand[n++] = c; break;
    case OP_SQRT: cand[n++] = b; break;
    }
    for (int i = 0; i < n; i++) if (is_nan_bits(cand[i])) { *out = cand[i] | 0x0008000000000000ull; return 1; }
    return 0;
}
static double single_op(PPC *p, int op, double a, double b, double c) {
    int old = fegetround();
    fesetround(FE_TOWARDZERO); feclearexcept(FE_INEXACT);
    volatile double va = a, vb = b, vc = c;
    double r = dop(op, va, vb, vc);
    int inexact = fetestexcept(FE_INEXACT) != 0;
    fesetround(old);
    if (inexact && !isnan(r) && !isinf(r)) { uint64_t bits = b_of(r); bits |= 1; r = d_of(bits); }
    return round_single(r, p->fpscr);
}
static double double_op(PPC *p, int op, double a, double b, double c);
static double round_single(double x, uint32_t fpscr);
/* full PowerPC arithmetic incl. NaN rules; neg = negate result (fnmadd/fnmsub) unless NaN */
static double farith(PPC *p, int op, int single, uint64_t A, uint64_t B, uint64_t Cc, int neg) {
    uint64_t nb;
    if (nan_result(op, A, B, Cc, &nb)) {
        if (single) nb &= 0xFFFFFFFFE0000000ull;   /* rounding a NaN to single drops low mantissa bits */
        return d_of(nb);
    }
    double r = single ? single_op(p, op, d_of(A), d_of(B), d_of(Cc)) : double_op(p, op, d_of(A), d_of(B), d_of(Cc));
    if (isnan(r)) return d_of(QNAN_DEFAULT);
    return neg ? -r : r;
}
static double double_op(PPC *p, int op, double a, double b, double c) {
    if ((p->fpscr & 3) == 0) return dop(op, a, b, c);
    int old = fegetround(); fesetround(fe_mode(p->fpscr));
    volatile double va = a, vb = b, vc = c; double r = dop(op, va, vb, vc);
    fesetround(old); return r;
}

/* PowerPC store-float-single conversion (no rounding: architected truncation of the double) */
static uint32_t dbl_to_single_bits(uint64_t d) {
    uint32_t exp = (uint32_t)(d >> 52) & 0x7FF;
    if (exp > 896 || (d & 0x7FFFFFFFFFFFFFFFull) == 0) {
        /* normal or zero/inf/nan: take bits directly */
        return (uint32_t)(((d >> 32) & 0xC0000000u) | ((d >> 29) & 0x3FFFFFFFu));
    }
    if (exp >= 874) {   /* denormalise */
        uint32_t sign = (uint32_t)(d >> 63);
        uint64_t frac = (d & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
        int sh = 896 - (int)exp + 1;  /* shift so that result exponent becomes 0 */
        frac >>= sh;
        return (sign << 31) | (uint32_t)(frac >> 29);
    }
    return (uint32_t)(d >> 63) << 31; /* undefined by architecture; return signed zero */
}
static uint64_t single_bits_to_dbl(uint32_t s) {
    uint32_t e = (s >> 23) & 0xFF;
    if (e == 0xFF && (s & 0x7FFFFF)) {   /* NaN: copy payload without quieting (architected load conversion) */
        return ((uint64_t)(s >> 31) << 63) | 0x7FF0000000000000ull | ((uint64_t)(s & 0x7FFFFF) << 29);
    }
    return b_of((double)f_of(s));
}

static uint32_t fctiw(PPC *p, double x, int tozero) {
    if (isnan(x)) return 0x80000000u;
    double r;
    if (tozero) r = trunc(x);
    else {
        switch (p->fpscr & 3) {
        case 0: r = nearbyint(x); /* default env is round-to-nearest-even */ break;
        case 1: r = trunc(x); break;
        case 2: r = ceil(x); break;
        default: r = floor(x); break;
        }
    }
    if (r >= 2147483648.0) return 0x7FFFFFFFu;
    if (r < -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)r;
}

static void fcmp(PPC *p, int crf, double a, double b) {
    uint32_t v;
    if (isnan(a) || isnan(b)) v = 1; else if (a < b) v = 8; else if (a > b) v = 4; else v = 2;
    set_crf(p, crf, v);
    p->fpscr = (p->fpscr & ~0xF000u) | (v << 12);
}

static void fprf(PPC *p, double r) {
    /* FPRF (C,FL,FG,FE,FU) in FPSCR bits 15..19 (big-endian) = mask 0x1F000 */
    uint32_t v;
    int c = fpclassify(r);
    if (c == FP_NAN) v = 0x11;
    else if (c == FP_INFINITE) v = signbit(r) ? 0x09 : 0x05;
    else if (c == FP_ZERO) v = signbit(r) ? 0x12 : 0x02;
    else if (c == FP_SUBNORMAL) v = signbit(r) ? 0x18 : 0x14;
    else v = signbit(r) ? 0x08 : 0x04;
    p->fpscr = (p->fpscr & ~0x1F000u) | (v << 12);
}

/* ------------------------------------------------------------------- execute */
#define RD ((insn >> 21) & 31)
#define RA ((insn >> 16) & 31)
#define RB ((insn >> 11) & 31)
#define RC ((insn >> 6) & 31)
#define SIMM ((int32_t)(int16_t)(insn & 0xFFFF))
#define UIMM (insn & 0xFFFF)
#define RCBIT (insn & 1)
#define OEBIT (insn & 0x400)
#define G p->gpr
#define F p->fpr
#define FD(x) d_of(F[x])

static int illegal(PPC *p, uint32_t insn) { p->illegal_insn = insn; return PPC_STOP_ILLEGAL; }

int ppc_step(PPC *p) {
    int f = 0;
    uint32_t pc = p->pc;
    uint32_t insn = ld32(p, pc, &f);
    if (f) return PPC_STOP_FAULT;
    uint32_t npc = pc + 4;
    uint32_t op = insn >> 26;
    uint32_t ea, t;
    switch (op) {
    case 3: /* twi */ {
        int32_t a = (int32_t)G[RA], b = SIMM; uint32_t to = RD;
        if (((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
            ((to & 2) && (uint32_t)a < (uint32_t)b) || ((to & 1) && (uint32_t)a > (uint32_t)b)) return illegal(p, insn);
        break; }
    case 7: G[RD] = (uint32_t)((int32_t)G[RA] * SIMM); break;                         /* mulli */
    case 8: { uint32_t a = G[RA]; G[RD] = addx(p, ~a, (uint32_t)SIMM, 1, 1, 0); break; } /* subfic */
    case 10: { uint32_t a = G[RA], b = UIMM; uint32_t v = a < b ? 8 : (a > b ? 4 : 2); if (p->xer & XER_SO) v |= 1; set_crf(p, RD >> 2, v); break; } /* cmpli */
    case 11: { int32_t a = (int32_t)G[RA], b = SIMM; uint32_t v = a < b ? 8 : (a > b ? 4 : 2); if (p->xer & XER_SO) v |= 1; set_crf(p, RD >> 2, v); break; } /* cmpi */
    case 12: G[RD] = addx(p, G[RA], (uint32_t)SIMM, 0, 1, 0); break;                   /* addic */
    case 13: G[RD] = addx(p, G[RA], (uint32_t)SIMM, 0, 1, 0); rec0(p, G[RD]); break;   /* addic. */
    case 14: G[RD] = (RA ? G[RA] : 0) + (uint32_t)SIMM; break;                          /* addi */
    case 15: G[RD] = (RA ? G[RA] : 0) + (UIMM << 16); break;                            /* addis */
    case 16: { /* bc */
        uint32_t bo = RD, bi = RA; int32_t bd = (int32_t)(int16_t)(insn & 0xFFFC);
        int ctr_ok = 1, cond_ok = 1;
        if (!(bo & 4)) { p->ctr--; ctr_ok = ((p->ctr != 0) ^ ((bo >> 1) & 1)); }
        if (!(bo & 16)) cond_ok = (((p->cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
        if (insn & 1) p->lr = pc + 4;
        if (ctr_ok && cond_ok) npc = (insn & 2) ? (uint32_t)bd : pc + bd;
        break; }
    case 17: return illegal(p, insn); /* sc */
    case 18: { int32_t li = (int32_t)(insn << 6) >> 6; li &= ~3;
        if (insn & 1) p->lr = pc + 4;
        npc = (insn & 2) ? (uint32_t)li : pc + li; break; }
    case 19: {
        uint32_t xo = (insn >> 1) & 0x3FF;
        switch (xo) {
        case 0: { int d = RD >> 2, s = RA >> 2; set_crf(p, d, (p->cr >> (28 - 4 * s)) & 0xF); break; } /* mcrf */
        case 16: case 528: { /* bclr / bcctr */
            uint32_t bo = RD, bi = RA; int ctr_ok = 1, cond_ok = 1;
            uint32_t target = (xo == 16 ? p->lr : p->ctr) & ~3u;
            if (xo == 16 && !(bo & 4)) { p->ctr--; ctr_ok = ((p->ctr != 0) ^ ((bo >> 1) & 1)); }
            if (!(bo & 16)) cond_ok = (((p->cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
            if (insn & 1) p->lr = pc + 4;
            if (ctr_ok && cond_ok) npc = target;
            break; }
        case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449: {
            uint32_t a = (p->cr >> (31 - RA)) & 1, b = (p->cr >> (31 - RB)) & 1, r = 0;
            switch (xo) {
            case 33: r = !(a | b); break; case 129: r = a & !b; break; case 193: r = a ^ b; break;
            case 225: r = !(a & b); break; case 257: r = a & b; break; case 289: r = !(a ^ b); break;
            case 417: r = a | !b; break; case 449: r = a | b; break;
            }
            uint32_t bit = 1u << (31 - RD);
            p->cr = r ? (p->cr | bit) : (p->cr & ~bit);
            break; }
        case 150: break; /* isync */
        default: return illegal(p, insn);
        }
        break; }
    case 20: { uint32_t sh = RB, mb = RC, me = (insn >> 1) & 31; uint32_t m = mask32(mb, me);
        G[RA] = (rotl(G[RD], sh) & m) | (G[RA] & ~m); if (RCBIT) rec0(p, G[RA]); break; } /* rlwimi */
    case 21: { uint32_t sh = RB, mb = RC, me = (insn >> 1) & 31;
        G[RA] = rotl(G[RD], sh) & mask32(mb, me); if (RCBIT) rec0(p, G[RA]); break; }    /* rlwinm */
    case 23: { uint32_t mb = RC, me = (insn >> 1) & 31;
        G[RA] = rotl(G[RD], G[RB] & 31) & mask32(mb, me); if (RCBIT) rec0(p, G[RA]); break; } /* rlwnm */
    case 24: G[RA] = G[RD] | UIMM; break;
    case 25: G[RA] = G[RD] | (UIMM << 16); break;
    case 26: G[RA] = G[RD] ^ UIMM; break;
    case 27: G[RA] = G[RD] ^ (UIMM << 16); break;
    case 28: G[RA] = G[RD] & UIMM; rec0(p, G[RA]); break;
    case 29: G[RA] = G[RD] & (UIMM << 16); rec0(p, G[RA]); break;
    case 31: {
        uint32_t xo = (insn >> 1) & 0x3FF;
        uint32_t a = G[RA], b = G[RB], s = G[RD];
        uint32_t ea0 = (RA ? G[RA] : 0) + G[RB];
        switch (xo) {
        /* --- arithmetic (XO-form: OE in bit 0x400, xo uses 9 bits) */
        case 266: case 266 | 512: G[RD] = addx(p, a, b, 0, 0, OEBIT); if (RCBIT) rec0(p, G[RD]); break;            /* add */
        case 10: case 10 | 512: G[RD] = addx(p, a, b, 0, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break;              /* addc */
        case 138: case 138 | 512: G[RD] = addx(p, a, b, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* adde */
        case 234: case 234 | 512: G[RD] = addx(p, a, 0xFFFFFFFFu, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* addme */
        case 202: case 202 | 512: G[RD] = addx(p, a, 0, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* addze */
        case 40: case 40 | 512: G[RD] = addx(p, ~a, b, 1, 0, OEBIT); if (RCBIT) rec0(p, G[RD]); break;             /* subf */
        case 8: case 8 | 512: G[RD] = addx(p, ~a, b, 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break;               /* subfc */
        case 136: case 136 | 512: G[RD] = addx(p, ~a, b, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* subfe */
        case 232: case 232 | 512: G[RD] = addx(p, ~a, 0xFFFFFFFFu, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* subfme */
        case 200: case 200 | 512: G[RD] = addx(p, ~a, 0, (p->xer >> 29) & 1, 1, OEBIT); if (RCBIT) rec0(p, G[RD]); break; /* subfze */
        case 104: case 104 | 512: G[RD] = addx(p, ~a, 0, 1, 0, OEBIT); if (RCBIT) rec0(p, G[RD]); break;           /* neg */
        case 235: case 235 | 512: { int64_t prod = (int64_t)(int32_t)a * (int32_t)b; G[RD] = (uint32_t)prod;
            if (OEBIT) { set_ov(p, prod != (int64_t)(int32_t)prod); } if (RCBIT) { rec0(p, G[RD]); } break; }              /* mullw */
        case 75: { int64_t prod = (int64_t)(int32_t)a * (int32_t)b; G[RD] = (uint32_t)((uint64_t)prod >> 32); if (RCBIT) rec0(p, G[RD]); break; } /* mulhw */
        case 11: { uint64_t prod = (uint64_t)a * b; G[RD] = (uint32_t)(prod >> 32); if (RCBIT) rec0(p, G[RD]); break; } /* mulhwu */
        case 491: case 491 | 512: { /* divw */
            int ov = (b == 0) || (a == 0x80000000u && b == 0xFFFFFFFFu);
            G[RD] = ov ? (((int32_t)a < 0 && b == 0) ? 0xFFFFFFFFu : 0) : (uint32_t)((int32_t)a / (int32_t)b);
            if (OEBIT) { set_ov(p, ov); } if (RCBIT) { rec0(p, G[RD]); } break; }
        case 459: case 459 | 512: { /* divwu */
            int ov = (b == 0); G[RD] = ov ? 0 : a / b;
            if (OEBIT) { set_ov(p, ov); } if (RCBIT) { rec0(p, G[RD]); } break; }
        /* --- compare */
        case 0: { int32_t x = (int32_t)a, y = (int32_t)b; uint32_t v = x < y ? 8 : (x > y ? 4 : 2); if (p->xer & XER_SO) v |= 1; set_crf(p, RD >> 2, v); break; }
        case 32: { uint32_t v = a < b ? 8 : (a > b ? 4 : 2); if (p->xer & XER_SO) v |= 1; set_crf(p, RD >> 2, v); break; }
        /* --- logical (rA <- rS op rB) */
        case 28: G[RA] = s & b; if (RCBIT) rec0(p, G[RA]); break;
        case 60: G[RA] = s & ~b; if (RCBIT) rec0(p, G[RA]); break;
        case 444: G[RA] = s | b; if (RCBIT) rec0(p, G[RA]); break;
        case 412: G[RA] = s | ~b; if (RCBIT) rec0(p, G[RA]); break;
        case 316: G[RA] = s ^ b; if (RCBIT) rec0(p, G[RA]); break;
        case 476: G[RA] = ~(s & b); if (RCBIT) rec0(p, G[RA]); break;
        case 124: G[RA] = ~(s | b); if (RCBIT) rec0(p, G[RA]); break;
        case 284: G[RA] = ~(s ^ b); if (RCBIT) rec0(p, G[RA]); break;
        case 954: G[RA] = (uint32_t)(int32_t)(int8_t)s; if (RCBIT) rec0(p, G[RA]); break;   /* extsb */
        case 922: G[RA] = (uint32_t)(int32_t)(int16_t)s; if (RCBIT) rec0(p, G[RA]); break;  /* extsh */
        case 26: G[RA] = s ? (uint32_t)__builtin_clz(s) : 32; if (RCBIT) rec0(p, G[RA]); break; /* cntlzw */
        /* --- shifts */
        case 24: { uint32_t n = b & 0x3F; G[RA] = n > 31 ? 0 : s << n; if (RCBIT) rec0(p, G[RA]); break; }  /* slw */
        case 536: { uint32_t n = b & 0x3F; G[RA] = n > 31 ? 0 : s >> n; if (RCBIT) rec0(p, G[RA]); break; } /* srw */
        case 792: { uint32_t n = b & 0x3F; int32_t v = (int32_t)s; uint32_t r; int ca;
            if (n > 31) { r = (uint32_t)(v >> 31); ca = v < 0; }
            else { r = (uint32_t)(v >> n); ca = v < 0 && n && (s & ((1u << n) - 1)); }
            G[RA] = r; set_ca(p, ca); if (RCBIT) rec0(p, r); break; }                                          /* sraw */
        case 824: { uint32_t n = RB; int32_t v = (int32_t)s; G[RA] = (uint32_t)(v >> n);
            set_ca(p, v < 0 && n && (s & ((1u << n) - 1))); if (RCBIT) rec0(p, G[RA]); break; }               /* srawi */
        /* --- special registers */
        case 19: G[RD] = p->cr; break;                                                                         /* mfcr */
        case 144: { uint32_t crm = (insn >> 12) & 0xFF, m = 0;
            for (int i = 0; i < 8; i++) if (crm & (0x80 >> i)) m |= 0xF0000000u >> (4 * i);
            p->cr = (s & m) | (p->cr & ~m); break; }                                                          /* mtcrf */
        case 512: { int d = RD >> 2; set_crf(p, d, p->xer >> 28); p->xer &= 0x0FFFFFFFu; break; }               /* mcrxr */
        case 339: case 371: { uint32_t spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
            if (spr == 1) G[RD] = p->xer; else if (spr == 8) G[RD] = p->lr; else if (spr == 9) G[RD] = p->ctr;
            else if (spr == 268) G[RD] = (uint32_t)p->icount; else if (spr == 269) G[RD] = (uint32_t)(p->icount >> 32);
            else if (spr == 287) G[RD] = 0x00040103; /* PVR: 604 */
            else return illegal(p, insn);
            break; }
        case 467: { uint32_t spr = ((insn >> 16) & 31) | (((insn >> 11) & 31) << 5);
            if (spr == 1) p->xer = s & 0xE000007Fu; else if (spr == 8) p->lr = s; else if (spr == 9) p->ctr = s;
            else return illegal(p, insn);
            break; }
        case 83: G[RD] = p->msr; break; /* mfmsr (supervisor; tolerated) */
        /* --- cache / sync: no-ops */
        case 86: case 54: case 278: case 246: case 982: case 598: case 854: break;
        case 1014: { uint32_t base = ea0 & ~31u; for (int k = 0; k < 32; k += 4) st32(p, base + k, 0, &f); break; } /* dcbz */
        case 4: { /* tw */
            int32_t x = (int32_t)a, y = (int32_t)b; uint32_t to = RD;
            if (((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
                ((to & 2) && (uint32_t)x < (uint32_t)y) || ((to & 1) && (uint32_t)x > (uint32_t)y)) return illegal(p, insn);
            break; }
        /* --- loads / stores, indexed */
        case 23: t = ld32(p, ea0, &f); if (!f) G[RD] = t; break;                                   /* lwzx */
        case 55: ea = a + b; t = ld32(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;        /* lwzux */
        case 87: t = ld8(p, ea0, &f); if (!f) G[RD] = t; break;                                    /* lbzx */
        case 119: ea = a + b; t = ld8(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;        /* lbzux */
        case 279: t = ld16(p, ea0, &f); if (!f) G[RD] = t; break;                                  /* lhzx */
        case 311: ea = a + b; t = ld16(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;       /* lhzux */
        case 343: t = ld16(p, ea0, &f); if (!f) G[RD] = (uint32_t)(int32_t)(int16_t)t; break;     /* lhax */
        case 375: ea = a + b; t = ld16(p, ea, &f); if (!f) { G[RD] = (uint32_t)(int32_t)(int16_t)t; G[RA] = ea; } break; /* lhaux */
        case 151: st32(p, ea0, s, &f); break;                                                      /* stwx */
        case 183: ea = a + b; st32(p, ea, s, &f); if (!f) G[RA] = ea; break;                       /* stwux */
        case 215: st8(p, ea0, s, &f); break;                                                       /* stbx */
        case 247: ea = a + b; st8(p, ea, s, &f); if (!f) G[RA] = ea; break;                        /* stbux */
        case 407: st16(p, ea0, s, &f); break;                                                      /* sthx */
        case 439: ea = a + b; st16(p, ea, s, &f); if (!f) G[RA] = ea; break;                       /* sthux */
        case 534: t = ld32(p, ea0, &f); if (!f) G[RD] = BSWAP32(t); break;                         /* lwbrx */
        case 790: t = ld16(p, ea0, &f); if (!f) G[RD] = BSWAP16((uint16_t)t); break;              /* lhbrx */
        case 662: st32(p, ea0, BSWAP32(s), &f); break;                                             /* stwbrx */
        case 918: st16(p, ea0, BSWAP16((uint16_t)s), &f); break;                                   /* sthbrx */
        case 20: t = ld32(p, ea0, &f); if (!f) G[RD] = t; break;                                   /* lwarx */
        case 150: st32(p, ea0, s, &f); if (!f) set_crf(p, 0, 2 | ((p->xer & XER_SO) ? 1 : 0)); break; /* stwcx. */
        case 535: t = ld32(p, ea0, &f); if (!f) F[RD] = single_bits_to_dbl(t); break;              /* lfsx */
        case 567: ea = a + b; t = ld32(p, ea, &f); if (!f) { F[RD] = single_bits_to_dbl(t); G[RA] = ea; } break; /* lfsux */
        case 599: { uint64_t v = ld64(p, ea0, &f); if (!f) F[RD] = v; break; }                    /* lfdx */
        case 631: { ea = a + b; uint64_t v = ld64(p, ea, &f); if (!f) { F[RD] = v; G[RA] = ea; } break; } /* lfdux */
        case 663: st32(p, ea0, dbl_to_single_bits(F[RD]), &f); break;                              /* stfsx */
        case 695: ea = a + b; st32(p, ea, dbl_to_single_bits(F[RD]), &f); if (!f) G[RA] = ea; break; /* stfsux */
        case 727: st64(p, ea0, F[RD], &f); break;                                                  /* stfdx */
        case 759: ea = a + b; st64(p, ea, F[RD], &f); if (!f) G[RA] = ea; break;                   /* stfdux */
        case 983: st32(p, ea0, (uint32_t)F[RD], &f); break;                                        /* stfiwx */
        case 597: case 533: { /* lswi / lswx */
            uint32_t n = (xo == 597) ? (RB ? RB : 32) : (p->xer & 0x7F);
            ea = (xo == 597) ? (RA ? G[RA] : 0) : ea0;
            uint32_t r = RD - 1, i = 0;
            while (n > 0) {
                if (i == 0) { r = (r + 1) & 31; G[r] = 0; }
                uint32_t byte = ld8(p, ea, &f); if (f) break;
                G[r] |= byte << (24 - i);
                i = (i + 8) & 31; ea++; n--;
            }
            break; }
        case 725: case 661: { /* stswi / stswx */
            uint32_t n = (xo == 725) ? (RB ? RB : 32) : (p->xer & 0x7F);
            ea = (xo == 725) ? (RA ? G[RA] : 0) : ea0;
            uint32_t r = RD - 1, i = 0;
            while (n > 0) {
                if (i == 0) r = (r + 1) & 31;
                st8(p, ea, (G[r] >> (24 - i)) & 0xFF, &f); if (f) break;
                i = (i + 8) & 31; ea++; n--;
            }
            break; }
        default: return illegal(p, insn);
        }
        break; }
    /* --- D-form loads/stores */
    case 32: ea = (RA ? G[RA] : 0) + SIMM; t = ld32(p, ea, &f); if (!f) G[RD] = t; break;           /* lwz */
    case 33: ea = G[RA] + SIMM; t = ld32(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;     /* lwzu */
    case 34: ea = (RA ? G[RA] : 0) + SIMM; t = ld8(p, ea, &f); if (!f) G[RD] = t; break;            /* lbz */
    case 35: ea = G[RA] + SIMM; t = ld8(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;      /* lbzu */
    case 36: ea = (RA ? G[RA] : 0) + SIMM; st32(p, ea, G[RD], &f); break;                           /* stw */
    case 37: ea = G[RA] + SIMM; st32(p, ea, G[RD], &f); if (!f) G[RA] = ea; break;                  /* stwu */
    case 38: ea = (RA ? G[RA] : 0) + SIMM; st8(p, ea, G[RD], &f); break;                            /* stb */
    case 39: ea = G[RA] + SIMM; st8(p, ea, G[RD], &f); if (!f) G[RA] = ea; break;                   /* stbu */
    case 40: ea = (RA ? G[RA] : 0) + SIMM; t = ld16(p, ea, &f); if (!f) G[RD] = t; break;           /* lhz */
    case 41: ea = G[RA] + SIMM; t = ld16(p, ea, &f); if (!f) { G[RD] = t; G[RA] = ea; } break;     /* lhzu */
    case 42: ea = (RA ? G[RA] : 0) + SIMM; t = ld16(p, ea, &f); if (!f) G[RD] = (uint32_t)(int32_t)(int16_t)t; break; /* lha */
    case 43: ea = G[RA] + SIMM; t = ld16(p, ea, &f); if (!f) { G[RD] = (uint32_t)(int32_t)(int16_t)t; G[RA] = ea; } break; /* lhau */
    case 44: ea = (RA ? G[RA] : 0) + SIMM; st16(p, ea, G[RD], &f); break;                           /* sth */
    case 45: ea = G[RA] + SIMM; st16(p, ea, G[RD], &f); if (!f) G[RA] = ea; break;                  /* sthu */
    case 46: { ea = (RA ? G[RA] : 0) + SIMM; uint32_t vals[32]; int n = 32 - RD;               /* lmw */
        for (int i = 0; i < n && !f; i++) vals[i] = ld32(p, ea + 4 * i, &f);
        if (!f) for (int i = 0; i < n; i++) G[RD + i] = vals[i];
        break; }
    case 47: { ea = (RA ? G[RA] : 0) + SIMM; for (uint32_t r = RD; r < 32 && !f; r++, ea += 4) st32(p, ea, G[r], &f); break; } /* stmw */
    case 48: ea = (RA ? G[RA] : 0) + SIMM; t = ld32(p, ea, &f); if (!f) F[RD] = single_bits_to_dbl(t); break;           /* lfs */
    case 49: ea = G[RA] + SIMM; t = ld32(p, ea, &f); if (!f) { F[RD] = single_bits_to_dbl(t); G[RA] = ea; } break;     /* lfsu */
    case 50: { ea = (RA ? G[RA] : 0) + SIMM; uint64_t v = ld64(p, ea, &f); if (!f) F[RD] = v; break; }                 /* lfd */
    case 51: { ea = G[RA] + SIMM; uint64_t v = ld64(p, ea, &f); if (!f) { F[RD] = v; G[RA] = ea; } break; }           /* lfdu */
    case 52: ea = (RA ? G[RA] : 0) + SIMM; st32(p, ea, dbl_to_single_bits(F[RD]), &f); break;                           /* stfs */
    case 53: ea = G[RA] + SIMM; st32(p, ea, dbl_to_single_bits(F[RD]), &f); if (!f) G[RA] = ea; break;                 /* stfsu */
    case 54: ea = (RA ? G[RA] : 0) + SIMM; st64(p, ea, F[RD], &f); break;                                               /* stfd */
    case 55: ea = G[RA] + SIMM; st64(p, ea, F[RD], &f); if (!f) G[RA] = ea; break;                                      /* stfdu */
    /* --- floating point arithmetic */
    case 59: case 63: {
        uint32_t xo5 = (insn >> 1) & 0x1F, xo = (insn >> 1) & 0x3FF;
        int single = (op == 59);
        double a = FD(RA), b = FD(RB), c = FD(RC), r;
        int arith = 1;
        if (xo5 >= 18 || (op == 63 && xo5 == 23)) {
            switch (xo5) {
            case 18: r = farith(p, OP_DIV, single, F[RA], F[RB], 0, 0); break;           /* fdiv(s) */
            case 20: r = farith(p, OP_SUB, single, F[RA], F[RB], 0, 0); break;           /* fsub(s) */
            case 21: r = farith(p, OP_ADD, single, F[RA], F[RB], 0, 0); break;           /* fadd(s) */
            case 22: r = farith(p, OP_SQRT, single, 0, F[RB], 0, 0); break;              /* fsqrt(s) */
            case 23: if (single) return illegal(p, insn); r = (a >= 0.0) ? c : b; break; /* fsel (NaN -> b) */
            case 24: r = farith(p, OP_DIV, single, b_of(1.0), F[RB], 0, 0); break;      /* fres (exact reciprocal) */
            case 25: r = farith(p, OP_MUL, single, F[RA], 0, F[RC], 0); break;           /* fmul(s) */
            case 26: r = 1.0 / sqrt(b); if (single) r = round_single(r, p->fpscr); break; /* frsqrte */
            case 28: r = farith(p, OP_MSUB, single, F[RA], F[RB], F[RC], 0); break;      /* fmsub(s) */
            case 29: r = farith(p, OP_MADD, single, F[RA], F[RB], F[RC], 0); break;      /* fmadd(s) */
            case 30: r = farith(p, OP_MSUB, single, F[RA], F[RB], F[RC], 1); break;      /* fnmsub(s) */
            case 31: r = farith(p, OP_MADD, single, F[RA], F[RB], F[RC], 1); break;      /* fnmadd(s) */
            default: return illegal(p, insn);
            }
            F[RD] = b_of(r);
            if (xo5 != 23) fprf(p, r);
        } else if (op == 63) {
            arith = 0;
            switch (xo) {
            case 0: fcmp(p, RD >> 2, a, b); break;                    /* fcmpu */
            case 32: fcmp(p, RD >> 2, a, b); break;                   /* fcmpo */
            case 12: { double rr; if (is_nan_bits(F[RB])) rr = d_of((F[RB] | 0x0008000000000000ull) & 0xFFFFFFFFE0000000ull);
                       else rr = round_single(b, p->fpscr);
                       F[RD] = b_of(rr); fprf(p, rr); arith = 1; break; } /* frsp */
            /* upper word architecturally undefined; sign-extended like QEMU so whole-register comparisons match */
            case 14: case 15: { uint32_t v = fctiw(p, b, xo == 15);
                int invalid = isnan(b) || v == 0x7FFFFFFFu || v == 0x80000000u;
                F[RD] = invalid ? v : (uint64_t)(int64_t)(int32_t)v; arith = 1; break; }   /* fctiw / fctiwz */
            case 40: F[RD] = F[RB] ^ 0x8000000000000000ull; break;      /* fneg */
            case 72: F[RD] = F[RB]; break;                              /* fmr */
            case 136: F[RD] = F[RB] | 0x8000000000000000ull; break;     /* fnabs */
            case 264: F[RD] = F[RB] & 0x7FFFFFFFFFFFFFFFull; break;     /* fabs */
            case 583: F[RD] = 0xFFF8000000000000ull | p->fpscr; break;  /* mffs */
            case 711: { uint32_t fm = (insn >> 17) & 0xFF, m = 0;      /* mtfsf */
                for (int i = 0; i < 8; i++) if (fm & (0x80 >> i)) m |= 0xF0000000u >> (4 * i);
                p->fpscr = ((uint32_t)F[RB] & m) | (p->fpscr & ~m); break; }
            case 134: { int fld = RD >> 2; uint32_t imm = (insn >> 12) & 0xF; int sh = 28 - 4 * fld;  /* mtfsfi */
                p->fpscr = (p->fpscr & ~(0xFu << sh)) | (imm << sh); break; }
            case 38: p->fpscr |= 1u << (31 - RD); break;               /* mtfsb1 */
            case 70: p->fpscr &= ~(1u << (31 - RD)); break;            /* mtfsb0 */
            case 64: { int d = RD >> 2, s = RA >> 2; uint32_t v = (p->fpscr >> (28 - 4 * s)) & 0xF;  /* mcrfs */
                set_crf(p, d, v); break; }
            default: return illegal(p, insn);
            }
            (void)arith;
        } else return illegal(p, insn);
        if (RCBIT && !(op == 63 && (xo == 0 || xo == 32))) set_crf(p, 1, p->fpscr >> 28);
        break; }
    default: return illegal(p, insn);
    }
    if (f) { return PPC_STOP_FAULT; }
    p->pc = npc;
    p->icount++;
    return 0;
}

int ppc_run(PPC *p, uint64_t max_insns) {
    uint64_t end = max_insns ? p->icount + max_insns : ~0ull;
    while (p->icount < end) {
        if (p->pc - p->hook_lo < p->hook_hi - p->hook_lo) return PPC_STOP_HOOK;
        int r = ppc_step(p);
        if (r) return r;
    }
    return PPC_STOP_COUNT;
}
