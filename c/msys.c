/* msys.c - cooperative multi-process system (port of scripts/macsys.py). See msys.h. */
#include "msys.h"
#include "fsu.h"
#include "coro.h"
#include <stdlib.h>
#include <string.h>

#define connectionInvalid (-609)
#define CORO_STACK (64u << 20)

static void yield_from(MSys *S, Proc *p);
static int64_t launch(World *W, const char *rel, uint32_t *hi, uint32_t *lo);
static int route(World *W, AERec *ev, uint32_t mode, AERec **reply);
static void on_reply(World *W, AERec *req, AERec *rp, int err);
static void yield_hook(World *W) { Proc *p = W->user; yield_from(p->S, p); }

MSys *msys_new(const char *vol, const char *trace_path, uint64_t max_insns) {
    MSys *S = calloc(1, sizeof *S);
    snprintf(S->vol, sizeof S->vol, "%s", vol);
    S->tf = u_fopen(trace_path, "wb"); if (!S->tf) { perror(trace_path); exit(2); }
    setvbuf(S->tf, NULL, _IOLBF, 1 << 16);
    S->max_insns = max_insns;
    ProcEnt f = { 0, 0x2000, "Finder", "FNDR", "MACS", 0, 0 };
    S->procs[0] = f; S->nprocs = 1;
    return S;
}

static Proc *spawn(MSys *S, const char *name, uint32_t hi, uint32_t lo, const char *ptype, const char *sig,
                   uint32_t lhi, uint32_t llo, const char *tag) {
    Proc *p = calloc(1, sizeof *p); p->S = S;
    snprintf(p->name, sizeof p->name, "%s", name); snprintf(p->tag, sizeof p->tag, "%s", tag);
    p->psn_hi = hi; p->psn_lo = lo;
    char pef[2048], rsrc[2100];
    snprintf(pef, sizeof pef, "%s/%s/%s", S->vol, APPDIR, name); snprintf(rsrc, sizeof rsrc, "%s.rsrc", pef);
    p->E = emu_new(pef, S->tf, p->tag); if (!p->E) exit(2);
    p->W = world_new(p->E, S->vol, rsrc, APPDIR, S->tf, S->dirs);
    if (!S->dirs) S->dirs = p->W->dirs;            /* one volume: directory IDs are system-wide */
    p->W->clock = &S->clock;
    world_install_ui(p->W);
    emu_bind_imports(p->E);
    p->W->psn_hi = hi; p->W->psn_lo = lo;
    p->W->procs = S->procs; p->W->nprocs = &S->nprocs;
    p->W->user = p;
    p->W->yield_hook = yield_hook;
    p->W->launch_hook = launch;
    p->W->route = route;
    p->W->on_reply = on_reply;
    p->W->dialog_policy = S->dialog_policy;
    p->W->on_log = S->on_log;
    ProcEnt e; memset(&e, 0, sizeof e); e.psn_hi = hi; e.psn_lo = lo; snprintf(e.name, sizeof e.name, "%s", name);
    memcpy(e.type, ptype, 5); memcpy(e.sig, sig, 5); e.lhi = lhi; e.llo = llo;
    S->procs[S->nprocs++] = e;
    S->p[S->np++] = p;
    p->alive = 1;
    return p;
}
Proc *msys_find(MSys *S, const char *name) { for (int i = 0; i < S->np; i++) if (!strcmp(S->p[i]->name, name)) return S->p[i]; return NULL; }

static Proc *next_of(MSys *S, Proc *p) {
    Proc *live[4]; int n = 0, at = -1;
    for (int i = 0; i < S->np; i++) if (S->p[i]->alive) { if (S->p[i] == p) at = n; live[n++] = S->p[i]; }
    if (!n) return NULL;
    if (at < 0) return live[0];
    return live[(at + 1) % n];
}
static void proc_main(void *arg);
static void resume_from(MSys *S, struct Coro *from, Proc *q) {
    if (!q->started) { q->started = 1; q->co = coro_new(proc_main, q, CORO_STACK); }
    (void)S; coro_switch(from, q->co);
}

static void proc_main(void *arg) {
    Proc *p = arg; MSys *S = p->S; Emu *E = p->E;
    jmp_buf jb; E->fail_jmp = &jb;
    if (!setjmp(jb)) {
        emu_call(E, emu_entry(E), E->toc, NULL, 0, S->max_insns);
        snprintf(p->exit_reason, sizeof p->exit_reason, "returned");
    } else {
        snprintf(p->exit_reason, sizeof p->exit_reason, "%s", E->stopped_reason);
        if (!strstr(E->stopped_reason, "ExitToShell") && !strstr(E->stopped_reason, "scenario complete")) {
            S->fatal = 1; snprintf(S->fatal_tag, sizeof S->fatal_tag, "%s", p->tag);
            snprintf(S->fatal_msg, sizeof S->fatal_msg, "%s", E->stopped_reason);
            S->nfatal_bt = emu_backtrace(E, S->fatal_bt, 15);
        }
    }
    E->fail_jmp = NULL;
    p->alive = 0;
    fprintf(S->tf, "%s == process exit: %s\n", p->tag, p->exit_reason);
    for (int i = 0; i < S->nprocs; i++)
        if (S->procs[i].psn_hi == p->psn_hi && S->procs[i].psn_lo == p->psn_lo) { memmove(&S->procs[i], &S->procs[i + 1], (S->nprocs - i - 1) * sizeof(ProcEnt)); S->nprocs--; i--; }
    if (S->fatal || strstr(p->exit_reason, "scenario complete")) { S->done = 1; coro_switch(p->co, S->main); }
    Proc *nxt = next_of(S, p);
    if (!nxt) { S->done = 1; coro_switch(p->co, S->main); }
    resume_from(S, p->co, nxt);
    fprintf(stderr, "exited process resumed\n"); exit(3);
}

/* Called on p's coroutine at a WaitNextEvent/GetNextEvent: run the other processes until it is p's turn. */
static void yield_from(MSys *S, Proc *p) {
    char wc[200]; emu_where_caller(p->E, wc);
    const char *sp = strrchr(wc, ' '); snprintf(p->idle_at, sizeof p->idle_at, "%s", sp ? sp + 1 : wc);
    S->nyield++;
    if (S->nyield % 20000 == 0) {
        char bt[8][96]; int n = emu_backtrace(p->E, bt, 6);
        fprintf(S->tf, "%s .. yield #%ld at", p->tag, S->nyield);
        for (int i = 0; i < n; i++) fprintf(S->tf, "%s%s", i ? " < " : " ", bt[i]);
        fprintf(S->tf, " ae_queue=[");
        for (int i = 0; i < p->W->nq; i++) fprintf(S->tf, "%s('%s', '%s')", i ? ", " : "", p->W->q[i].cls, p->W->q[i].id);
        fprintf(S->tf, "]\n");
    }
    Proc *nxt = next_of(S, p);
    if (world_realtime && (!nxt || nxt == S->p[0])) {
        /* hardware mode: a whole round of processes passed without SCSI traffic -> let the host idle 1 ms
         * (a Mac in WaitNextEvent sleeps when nothing is pending) */
        long act = S->bus ? bus_ncmd(S->bus) : 0;
        if (act == S->last_activity) host_sleep_ms(1);
        S->last_activity = act;
    }
    if (!nxt || nxt == p) return;
    resume_from(S, p->co, nxt);
    if (S->done) { fprintf(stderr, "resumed after stop\n"); exit(3); }
}

/* LaunchApplication: only the scanner driver can be launched */
static int64_t launch(World *W, const char *rel, uint32_t *hi, uint32_t *lo) {
    Proc *self = W->user; MSys *S = self->S;
    const char *nm = strrchr(rel, '/'); nm = nm ? nm + 1 : rel;
    if (strcmp(nm, BG_NAME)) return -43;
    for (int i = 0; i < S->np; i++) if (!strcmp(S->p[i]->name, BG_NAME) && S->p[i]->alive) return -600;
    Proc *bg = spawn(S, BG_NAME, 0, 0x2001, "BGJB", "SC3D", W->psn_hi, W->psn_lo, "BG");
    if (S->scsi_target) {
        ScsiTarget *tg[8] = { 0 }; S->scsi_target(S, bg, tg);
        S->bus = bus_new(bg->W, tg, 1);
    }
    fprintf(S->tf, "SYS launched %s as psn (%u, %u)\n", nm, bg->psn_hi, bg->psn_lo);
    *hi = bg->psn_hi; *lo = bg->psn_lo; return 0;
}

/* ------------------------------------------------------------------ AppleEvents */
/* returns 1 and the target psn, or 0 (python: None) */
static int target_of(MSys *S, Proc *sender, AERec *ev, uint32_t *hi, uint32_t *lo) {
    AEItem *a = aerec_attr(ev, "addr"); if (!a) return 0;
    if (!strcmp(a->type, "psn ") && a->data.n >= 8) {
        const uint8_t *d = a->data.b; *hi = (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3]; *lo = (uint32_t)d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7];
        if (*hi == 0 && *lo == 2) { *hi = sender->psn_hi; *lo = sender->psn_lo; }
        return 1;
    }
    if (!strcmp(a->type, "sign")) {
        /* candidates from the process table in order; prefer one that is not the sender */
        int idx = -1;
        for (int i = 0; i < S->nprocs; i++) if (a->data.n >= 4 && !memcmp(S->procs[i].sig, a->data.b, 4)) {
            int self = S->procs[i].psn_hi == sender->psn_hi && S->procs[i].psn_lo == sender->psn_lo;
            if (!self) { idx = i; break; }
            if (idx < 0) idx = i;
        }
        if (idx < 0) return 0;
        *hi = S->procs[idx].psn_hi; *lo = S->procs[idx].psn_lo; return 1;
    }
    return 0;
}
static int route(World *W, AERec *ev, uint32_t mode, AERec **reply) {
    Proc *sender = W->user; MSys *S = sender->S; uint32_t hi = 0, lo = 0;
    int found = target_of(S, sender, ev, &hi, &lo);
    Proc *q = NULL;
    if (found) for (int i = 0; i < S->np; i++) if (S->p[i]->psn_hi == hi && S->p[i]->psn_lo == lo) q = S->p[i];
    if (!q || !q->alive) {
        char t[64] = "None"; if (found) snprintf(t, sizeof t, "(%u, %u)", hi, lo);
        fprintf(S->tf, "%s AE %s/%s -> %s: no such process\n", sender->tag, ev->cls, ev->id, t);
        return connectionInvalid;
    }
    AESlot *slot = calloc(1, sizeof *slot);
    uint8_t from[8] = { sender->psn_hi >> 24, sender->psn_hi >> 16, sender->psn_hi >> 8, sender->psn_hi,
                        sender->psn_lo >> 24, sender->psn_lo >> 16, sender->psn_lo >> 8, sender->psn_lo };
    /* python: deliver.setdefault('__attr__', {})['from'] = ... also lands in the sender's (shared) attribute dict */
    if (ev->na) aerec_set_attr(ev, "from", "psn ", from, 8);
    AERec *deliver = aerec_copy(ev);
    aerec_set_attr(deliver, "from", "psn ", from, 8);
    deliver->slot = slot;
    if (S->on_route) S->on_route(S, sender, q, ev);
    ae_queue_push(q->W, ev->cls, ev->id, deliver);
    if ((mode & 3) != 3) { *reply = aerec_new(); return 0; }
    /* kAEWaitReply. While waiting, the Apple Event Manager dispatches Apple events that arrive for the
     * sender to its handlers (both applications wait for replies, and each sends events to the other
     * while handling one). */
    for (;;) {
        if (slot->done) { *reply = slot->reply ? slot->reply : aerec_new(); return 0; }
        if (!q->alive) return connectionInvalid;
        if (sender->W->nq) {
            AEQItem it = sender->W->q[0]; memmove(&sender->W->q[0], &sender->W->q[1], (sender->W->nq - 1) * sizeof(AEQItem)); sender->W->nq--;
            ae_dispatch(sender->W, &it);
            continue;
        }
        yield_from(S, sender);
    }
}
static void on_reply(World *W, AERec *req, AERec *rp, int err) {
    if (req && req->slot) { req->slot->reply = rp ? rp : aerec_new(); req->slot->done = 1; }
    else { Proc *p = W->user; if (p && p->S->on_ae_done) p->S->on_ae_done(p->S, p, req, rp, err); }   /* events queued by the host */
}

/* ------------------------------------------------------------------ run */
static void fg_idle(World *W) { Proc *p = W->user; if (p->S->on_fg_idle) p->S->on_fg_idle(p->S, p); }
/* fg_adapt (scripts/fg_adapt.py): answer_alrt(): alerts normally appear as MacApp modeless windows
 * once the application is running (flag byte at TOC-0x2E39). Headless, every alert takes the classic
 * ModalDialog path instead, which the dialog policy answers. The returned answer is the same item. */
static void modal_alerts(Emu *E, void *ctx) { (void)ctx; wr8(E, E->toc - 0x2E39, 0); }

int msys_run_fg(MSys *S) {
    hle_uistub_register(); hle_core_register(); hle_files_register(); hle_ui_register(); hle_scsi_register();
    S->main = coro_self_main();
    Proc *fg = spawn(S, FG_NAME, 0, 0x2002, "APPL", "SC3D", 0, 0x2000, "FG");
    fg->W->on_idle = fg_idle;
    emu_add_bp(fg->E, 0x03F798, modal_alerts, NULL);
    if (S->fg_setup) S->fg_setup(S, fg);
    resume_from(S, S->main, fg);
    fflush(S->tf);
    return S->fatal ? 1 : 0;
}
