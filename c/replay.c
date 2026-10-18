/* replay.c - a ScsiTarget that answers from a recorded hardware session (S342TRC1 scsi-trace.bin, as written by
 * spti.c / smart342-tracerelay). Used to re-run a real scanner session offline: the original driver gets exactly the
 * answers the real scanner gave, so files it writes (preview, parameters) can be regenerated and inspected.
 *
 * Matching: the next recorded command must have the same CDB and the same data-out. Polling loops may run a different
 * number of times offline (timing): recorded repeats of the previous command are skipped forward to the next match;
 * an extra repeat offline is answered with the previous answer. Anything else is a divergence: reported once and
 * answered with CHECK CONDITION (the session is then no longer a replay). */
#include "sim.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { uint64_t t0; uint8_t cdb[16]; int ncdb, st; uint8_t *out; uint32_t nout; uint8_t *data; uint32_t ndata, got; int flags; } Rec;
typedef struct { ScsiTarget t; Rec *r; int n, pos, diverged, last; long served, repeats, skipped, soft; } Replay;

static uint32_t le32_at(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int same_cmd(const Rec *r, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout) {
    return r->ncdb == ncdb && !memcmp(r->cdb, cdb, (size_t)ncdb) && r->nout == nout && (!nout || !memcmp(r->out, out, nout));
}
static int answer(Replay *s, const Rec *r, uint8_t **din, size_t *nin) {
    if (r->ndata < r->got) {            /* data not stored in the trace (large final-scan reads): zeros */
        if (s->t.log) TLOG(s->t.log, "!! REPLAY: data of this command was not recorded (%u of %u bytes) - zeros\n", r->ndata, r->got);
    }
    if (r->got) { *din = calloc(r->got + 1, 1); memcpy(*din, r->data, r->ndata < r->got ? r->ndata : r->got); *nin = r->got; }
    return r->st;
}
static int replay_execute(ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out,
                          uint32_t alloc, uint8_t **din, size_t *nin) {
    Replay *s = (Replay *)t; (void)has_out; (void)alloc; *din = NULL; *nin = 0;
    if (s->diverged) return 2;
    for (int p = s->pos; p < s->n; p++) {
        if (same_cmd(&s->r[p], cdb, ncdb, out, nout)) {
            s->skipped += p - s->pos;
            t->last_dt = s->last >= 0 && s->r[p].t0 > s->r[s->last].t0 ? (double)(s->r[p].t0 - s->r[s->last].t0) / 1e6 : 0.001;
            s->pos = p + 1; s->last = p; s->served++;
            return answer(s, &s->r[p], din, nin);
        }
        /* only recorded repeats (polling) may be skipped */
        if (p == 0 || !same_cmd(&s->r[p], s->r[p - 1].cdb, s->r[p - 1].ncdb, s->r[p - 1].out, s->r[p - 1].nout)) break;
    }
    if (s->last >= 0 && same_cmd(&s->r[s->last], cdb, ncdb, out, nout)) { s->repeats++; t->last_dt = 0.002; return answer(s, &s->r[s->last], din, nin); }
    /* same CDB, data-out of the same length but different content (e.g. the BIP command block carries the SCSI rate the
       driver measured with its own clock): soft mismatch - logged, the recorded answer is served */
    if (s->pos < s->n) {
        Rec *q = &s->r[s->pos];
        if (q->ncdb == ncdb && !memcmp(q->cdb, cdb, (size_t)ncdb) && q->nout == nout && nout) {
            s->soft++;
            if (t->log) {
                char a[200] = "", b[200] = "";
                for (size_t i = 0; i < nout && i < 64; i++) { sprintf(a + 2 * i, "%02x", out[i]); sprintf(b + 2 * i, "%02x", q->out[i]); }
                TLOG(t->log, "!! REPLAY soft mismatch at record %d (data-out differs): sent %s recorded %s\n", s->pos, a, b);
            }
            t->last_dt = s->last >= 0 && q->t0 > s->r[s->last].t0 ? (double)(q->t0 - s->r[s->last].t0) / 1e6 : 0.001;
            s->last = s->pos; s->pos++; s->served++;
            return answer(s, q, din, nin);
        }
    }
    s->diverged = 1;
    if (t->log) {
        char a[40] = "", b[40] = "";
        for (int i = 0; i < ncdb && i < 16; i++) sprintf(a + 2 * i, "%02x", cdb[i]);
        if (s->pos < s->n) for (int i = 0; i < s->r[s->pos].ncdb; i++) sprintf(b + 2 * i, "%02x", s->r[s->pos].cdb[i]);
        TLOG(t->log, "!! REPLAY DIVERGED at record %d: got %s (out %zu), recorded %s (out %u)\n", s->pos, a, nout, b, s->pos < s->n ? s->r[s->pos].nout : 0);
    }
    return 2;
}
static void replay_destroy(ScsiTarget *t) {
    Replay *s = (Replay *)t;
    for (int i = 0; i < s->n; i++) { free(s->r[i].out); free(s->r[i].data); }
    free(s->r); free(s);
}
void replay_stats(ScsiTarget *t, char *msg, int max) {
    Replay *s = (Replay *)t;
    snprintf(msg, (size_t)max, "replay: %ld served, %ld extra polls, %ld recorded polls skipped, %ld soft mismatches, at record %d of %d%s",
             s->served, s->repeats, s->skipped, s->soft, s->pos, s->n, s->diverged ? ", DIVERGED" : "");
}
ScsiTarget *replay_open(const char *path, Emu *log) {
    FILE *f = u_fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n); if (fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; } fclose(f);
    if (n < 8 || memcmp(d, "S342TRC1", 8)) { free(d); return NULL; }
    Replay *s = calloc(1, sizeof *s); int cap = 4096; s->r = calloc((size_t)cap, sizeof(Rec)); s->last = -1;
    long p = 8;
    while (p + 24 + 4 + 12 + 16 + 1 + 32 + 1 + 4 <= n) {
        if (le32_at(d + p) != 0x52435453) break;
        uint64_t t0 = le32_at(d + p + 8) | (uint64_t)le32_at(d + p + 12) << 32;
        p += 24;
        int ncdb = d[p], st = d[p + 3]; p += 4;
        uint32_t got = le32_at(d + p + 8); p += 12;
        Rec r; memset(&r, 0, sizeof r); r.t0 = t0; memcpy(r.cdb, d + p, 16); r.ncdb = ncdb; r.st = st; r.got = got; p += 16;
        p += 1 + 32; r.flags = d[p]; p += 1;
        r.nout = le32_at(d + p); p += 4; if (p + r.nout > n) break;
        r.out = malloc(r.nout + 1); memcpy(r.out, d + p, r.nout); p += r.nout;
        if (p + 8 > n) { free(r.out); break; }
        r.ndata = le32_at(d + p); p += 8; if (p + r.ndata > n) { free(r.out); break; }
        r.data = malloc(r.ndata + 1); memcpy(r.data, d + p, r.ndata); p += r.ndata;
        if (s->n == cap) { cap *= 2; s->r = realloc(s->r, (size_t)cap * sizeof(Rec)); }
        s->r[s->n++] = r;
    }
    free(d);
    s->t.execute = replay_execute; s->t.destroy = replay_destroy; s->t.log = log; s->t.has_dt = 1; s->t.last_dt = 0.001;
    return &s->t;
}
