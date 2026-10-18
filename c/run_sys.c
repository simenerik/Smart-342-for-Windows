/* run_sys.c - run the original front end + ScannerDriver together (headless) against a simulated
 * scanner, driving the front end through its own AppleScript interface ('SC3D' prev/scan, 'core' setd).
 * Port of scripts/run_sys.py. --steps: comma separated
 *    set:PROP=TYPE:VALUE   e.g. set:pcas=enum:t35m  set:pres=sing:11.811  set:psiz.pwth=sing:24
 *    prev | scan | idle:N */
#include "msys.h"
#include "popups.h"
#include "cassette.h"
#include "fsu.h"
#include <stdlib.h>
#include <string.h>

static char *steps[128]; static int nsteps, istep;
static int top = -1, waitn = 0, nidle = 0;
static uint8_t *flash; static size_t nflash; static int target_level = 2, use_guard = 0; static int bp_mirror;
static void mirror_bp(Emu *E, void *ctx) {   /* --bp-mirror: arguments of SC_display_is_mirror_to_PM(a, b) */
    (void)ctx; TLOG(E, "   BP SC_display_is_mirror_to_PM a=%d b=%d override_flag=%d\n", (int16_t)E->cpu->gpr[3], (int16_t)E->cpu->gpr[4], rds16(E, rd32(E, E->toc - 0x74dc)));
}
static int bp_res;
static double fpr_d(Emu *E, int n) { double d; memcpy(&d, &E->cpu->fpr[n], 8); return d; }
static void res_bp(Emu *E, void *ctx) {   /* --bp-res: ChangeResolution at the division value / unit factor */
    (void)ctx; TLOG(E, "   BP ChangeResolution value=%g divisor=%g -> resolution %g\n", fpr_d(E, 31), fpr_d(E, 1), fpr_d(E, 31) / fpr_d(E, 1));
}
static int use_fghooks, bp_valid, dump_popups, step_log; static double hk_res, hk_scale;
static void valid_bp(Emu *E, void *ctx) { (void)ctx; TLOG(E, "   BP ShowCropValidity valid=%d\n", (int)(E->cpu->gpr[4] & 0xFF)); }
static void dbg_pstr(Emu *E, void *ctx) {   /* --dbg-pstr ADDR:REG - log the Pascal string a register points to */
    int reg = (int)(intptr_t)ctx; uint32_t a = E->cpu->gpr[reg]; int n = rd8(E, a); char b[300]; for (int i = 0; i < n; i++) b[i] = (char)rd8(E, a + 1 + i); b[n] = 0;
    TLOG(E, "   DBG r%d -> [%s] (len %d) r3=%08x\n", reg, b, n, E->cpu->gpr[3]);
}
static void dbg_r3(Emu *E, void *ctx) { (void)ctx; TLOG(E, "   DBG r3=%08x r4=%08x\n", E->cpu->gpr[3], E->cpu->gpr[4]); }
static uint32_t dbg_at[8]; static int dbg_reg[8], ndbg;
static void fg_setup(MSys *S, Proc *fg) { (void)S; fghooks_install_frames(fg->E); for (int i = 0; i < ndbg; i++) emu_add_bp(fg->E, dbg_at[i], dbg_reg[i] < 0 ? dbg_r3 : dbg_pstr, (void *)(intptr_t)dbg_reg[i]); if (dump_popups) popups_install(fg->E); if (use_fghooks) fghooks_install(fg->E); if (bp_valid) emu_add_bp(fg->E, 0x0842bc, valid_bp, NULL); if (bp_mirror) emu_add_bp(fg->E, 0x060d08, mirror_bp, NULL); if (bp_res) emu_add_bp(fg->E, 0x08776c, res_bp, NULL); }
static const char *profile, *replay; static ScsiTarget *replay_t; static Emu *bg_emu; static int bt_on_hold;
static void hold_note(const char *m) {    /* --bt-holds: where in the driver each held-back command came from */
    if (!bt_on_hold || !bg_emu) return;
    char bt[48][96]; int n = emu_backtrace(bg_emu, bt, 12);
    TLOG(bg_emu, "   HOLD %s:", m); for (int i = 0; i < n; i++) TLOG(bg_emu, " < %s", bt[i]); TLOG(bg_emu, "\n");
}

static void be16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }


/* --scsi-log FILE: one line per command with the full data-out (hex) - for comparing what different settings send */
typedef struct { ScsiTarget t; ScsiTarget *in; FILE *f; } LogT;
static int logt_exec(ScsiTarget *t, const uint8_t *cdb, int ncdb, const uint8_t *out, size_t nout, int has_out, uint32_t alloc, uint8_t **din, size_t *nin) {
    LogT *l = (LogT *)t; l->in->log = t->log; l->in->world = t->world;
    int st = l->in->execute(l->in, cdb, ncdb, out, nout, has_out, alloc, din, nin);
    t->has_dt = l->in->has_dt; t->last_dt = l->in->last_dt;
    for (int i = 0; i < ncdb; i++) fprintf(l->f, "%02x", cdb[i]);
    fprintf(l->f, " st=%d out=%zu ", st, nout); for (size_t i = 0; i < nout; i++) fprintf(l->f, "%02x", out[i]); fprintf(l->f, "\n");
    return st;
}
static const char *scsi_log_path;
static ScsiTarget *logt_new(ScsiTarget *in) {
    LogT *l = calloc(1, sizeof *l); l->in = in; l->f = fopen(scsi_log_path, "w"); if (!l->f) { perror(scsi_log_path); exit(2); }
    l->t.execute = logt_exec; l->t.log = in->log; l->t.world = in->world; l->t.note = in->note; return &l->t;
}
static int raw_patch; static int noalign_opt, stephold_opt; static long long abort_at; static int abort_done;
/* --bg-bp ADDR: log the driver reaching a code address (r3, r4, f1, f2) - read-only research hook */
static uint32_t bgbp_at[16]; static int nbgbp;
static void bgbp(Emu *E, void *ctx) {
    double f1, f2; memcpy(&f1, &E->cpu->fpr[1], 8); memcpy(&f2, &E->cpu->fpr[2], 8);
    TLOG(E, "   BGBP %06x r3=%08x r4=%08x r8=%08x f1=%g f2=%g lr=%06x\n", (unsigned)(uintptr_t)ctx, E->cpu->gpr[3], E->cpu->gpr[4], E->cpu->gpr[8], f1, f2, (unsigned)(E->cpu->lr - CODE));
}
/* --bg-lens-info: when the driver chooses a lens (required_lens 0x057ab8, crop range f1..f2 in mm across the bed),
   log the CCD (smart_env +0x10 cells, +0xc pitch) and, per lens 0..3, magnification (lens_magnification 0x06b044) and
   the bed range its CCD covers (ccd_cell_board_w_placement 0x0574b0 of cell 0 and the last cell); at 0x057bd4 the
   choice. Read-only research: the calls only compute. */
static int lens_info;
static uint32_t lens_tvec(Emu *E, uint32_t off) { uint32_t tv = ((E->cpu->gpr[1] - 0x200) & ~0xFu); wr32(E, tv, CODE + off); wr32(E, tv + 4, E->toc); return tv; }
static void lens_info_bp(Emu *E, void *ctx) {
    (void)ctx; double a, b; memcpy(&a, &E->cpu->fpr[1], 8); memcpy(&b, &E->cpu->fpr[2], 8);
    uint64_t save[32]; memcpy(save, E->cpu->fpr, sizeof save);
    uint32_t env = rd32(E, rd32(E, E->toc - 0x7208)); int cells = rds16(E, env + 0x10); uint32_t pv = rd32(E, env + 0xc); float pitch; memcpy(&pitch, &pv, 4);
    if (ctx) { uint32_t t = E->cpu->gpr[3]; int lens = rds16(E, E->cpu->gpr[4] + 0x42); TLOG(E, "   LENS motors_to_start_scan_position: lens %d; lens positions", lens);
        for (int l = 0; l < 5; l++) { uint32_t v = rd32(E, t + 8 + l * 0x34); float f; memcpy(&f, &v, 4); TLOG(E, " %d:%g", l, f); } TLOG(E, "\n"); }
    else TLOG(E, "   LENS required_lens(%g, %g)\n", a, b);
    TLOG(E, "   LENS CCD %d cells, pitch %g\n", cells, pitch);
    for (int l = 0; l < 4; l++) {
        uint32_t ar[1] = { (uint32_t)l }; emu_call_own(E, lens_tvec(E, 0x06b044), ar, 1); double m = E->nest_f1;
        uint32_t a0[2] = { 0, (uint32_t)l }; emu_call_own(E, lens_tvec(E, 0x0574b0), a0, 2); double p0 = E->nest_f1;
        uint32_t a1[2] = { (uint32_t)cells, (uint32_t)l }; emu_call_own(E, lens_tvec(E, 0x0574b0), a1, 2); double p1 = E->nest_f1;
        TLOG(E, "   LENS lens %d: magnification %g -> %.1f px/mm (%.0f dpi); covers %.2f .. %.2f mm (%.2f mm)\n", l, m, m / pitch, m / pitch * 25.4, p0 < p1 ? p0 : p1, p0 < p1 ? p1 : p0, p0 < p1 ? p1 - p0 : p0 - p1);
    }
    memcpy(E->cpu->fpr, save, sizeof save);
}
static void lens_choice_bp(Emu *E, void *ctx) { (void)ctx; TLOG(E, "   LENS chosen lens %d\n", (int)(int16_t)E->cpu->gpr[3]); }
/* --bg-cas: the cassette the driver sets up (GEFEN_cassette_type 0x077fd4: r8 = cassette code); its cassette-directory
   entry (table at *(TOC-0x6684), entries of 0x70 bytes at *(TOC-0x6690)) and the lens shorts at entry +0x2a..+0x2e */
static int bg_cas;
static void cas_bp(Emu *E, void *ctx) {
    (void)ctx; int code = (int)(int16_t)E->cpu->gpr[8];
    uint32_t tab = rd32(E, E->toc - 0x6684), base = rd32(E, E->toc - 0x6690);
    int ent = rds16(E, tab + code * 2); uint32_t e = base + ent * 0x70;
    TLOG(E, "   CAS code %d -> directory entry %d, lens shorts %d %d %d, group? %d\n", code, ent, rds16(E, e + 0x2a), rds16(E, e + 0x2c), rds16(E, e + 0x2e), rds16(E, e + 0x0));
}
/* --bg-integ-scale X (simulator research only): every set_integration_time(f1) call gets f1 * X */
static double integ_scale;
static void integ_bp(Emu *E, void *ctx) {
    (void)ctx; double f; memcpy(&f, &E->cpu->fpr[1], 8); double g = f * integ_scale; memcpy(&E->cpu->fpr[1], &g, 8);
    TLOG(E, "   INTEG set_integration_time %g -> %g (research scale)\n", f, g);
}
/* --bg-integ-limits: when the driver builds its integration-time scale (build_integ_scale 0x031d24), log the
   scanner parameters it reads: smart_env (*(*(TOC-0x7208))) +0x22 first value, +0x26 last value, +0x1e6 factor,
   +0x200 special value (floats) - read-only research */
static int integ_limits;
static void integ_limits_bp(Emu *E, void *ctx) {
    (void)ctx; uint32_t env = rd32(E, rd32(E, E->toc - 0x7208)); uint32_t v[4]; float f[4]; static const int off[4] = { 0x22, 0x26, 0x1e6, 0x200 };
    for (int i = 0; i < 4; i++) { v[i] = rd32(E, env + off[i]); memcpy(&f[i], &v[i], 4); }
    double res; { uint32_t a = rd32(E, E->toc - 0x538c), b = rd32(E, E->toc - 0x5390); float fa, fb; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); res = rds16(E, E->toc - 0x53b0) == 0x14 ? fa : fb; }
    TLOG(E, "   INTEG-LIMITS smart_env %08x: first %g last %g factor %g special %g; resolution %g (G[-53b0]=%d)\n", env, f[0], f[1], f[2], f[3], res, rds16(E, E->toc - 0x53b0));
}
/* --bg-msg: log every call of the driver's Msg(level, format, ...) with its caller (LR) and format string -
   read-only research hook (which code prints which driver log line) */
static int bg_msg;
static void msg_bp(Emu *E, void *ctx) {
    (void)ctx; char t[90]; int k = 0; uint32_t a = E->cpu->gpr[4];
    for (; k < 80; k++) { uint8_t c = rd8(E, a + k); if (!c) break; t[k] = (c == '\n' || c == '\r') ? '|' : (c < 32 || c > 126) ? '.' : (char)c; }
    t[k] = 0;
    TLOG(E, "   MSG lr=%06x fmt@%08x \"%s\"\n", (unsigned)(E->cpu->lr - CODE), a, t);
}
static void targets(MSys *S, Proc *bg, ScsiTarget **tg) {
    (void)S;
    if (raw_patch) { raw_patch_driver(bg->E); raw_hdr_install(bg->E); lens_install(bg->E); focus_install(bg->E); noalign_install(bg->E); if (noalign_opt) raw_noalign = 1; stephold_install(bg->E); if (stephold_opt) raw_stephold = 1; }
    if (bg_msg) emu_add_bp(bg->E, 0x00cdbc, msg_bp, NULL);
    if (integ_limits) emu_add_bp(bg->E, 0x031d48, integ_limits_bp, NULL);
    if (integ_scale > 0) { if (replay || !getenv("SIM_ONLY_OK")) { fprintf(stderr, "--bg-integ-scale is for the simulator only (set SIM_ONLY_OK=1)\n"); exit(2); } emu_add_bp(bg->E, 0x032ac8, integ_bp, NULL); }
    if (bg_cas) emu_add_bp(bg->E, 0x077fd4, cas_bp, NULL);
    if (lens_info) { emu_add_bp(bg->E, 0x057ab8, lens_info_bp, NULL); emu_add_bp(bg->E, 0x068248, lens_info_bp, (void *)1); emu_add_bp(bg->E, 0x057bd4, lens_choice_bp, NULL); }
    for (int i = 0; i < nbgbp; i++) emu_add_bp(bg->E, bgbp_at[i], bgbp, (void *)(uintptr_t)bgbp_at[i]);
    if (replay) {
        replay_t = replay_open(replay, bg->E); if (!replay_t) { fprintf(stderr, "cannot read %s\n", replay); exit(2); }
        tg[4] = replay_t; tg[4]->world = bg->W;
        if (use_guard) { tg[4] = guard_new(tg[4]); tg[4]->log = bg->E; tg[4]->world = bg->W; tg[4]->note = hold_note; bg_emu = bg->E; }
        return;
    }
    tg[4] = sim_new(target_level, bg->E, "flash14", flash, nflash); tg[4]->world = bg->W;
    if (profile && sim_load_profile(tg[4], profile)) { fprintf(stderr, "cannot read profile %s\n", profile); exit(2); }
    if (use_guard) { tg[4] = guard_new(tg[4]); tg[4]->log = bg->E; tg[4]->world = bg->W; }
    if (scsi_log_path) tg[4] = logt_new(tg[4]);
}
static int quiet(MSys *S, Proc *fg) {
    Proc *bg = msys_find(S, BG_NAME);
    return !fg->W->nq && (!bg || (!bg->W->nq && (!strcmp(bg->idle_at, "main") || !strcmp(bg->idle_at, "WaitForFirstForgroundEvent"))));
}
/* value(typ, v) of run_sys.py */
static int value(const char *typ, const char *v, uint8_t *out) {
    if (!strcmp(typ, "enum") || !strcmp(typ, "type")) {
        char m[8]; int n = utf8_to_macroman(v, m, sizeof m); for (int i = 0; i < 4; i++) out[i] = (uint8_t)(i < n ? m[i] : ' '); return 4;
    }
    if (!strcmp(typ, "sing")) { float f = (float)strtod(v, NULL); uint32_t b; memcpy(&b, &f, 4); be32(out, b); return 4; }
    if (!strcmp(typ, "long")) { be32(out, (uint32_t)strtol(v, NULL, 10)); return 4; }
    if (!strcmp(typ, "shor")) { be16(out, (int)strtol(v, NULL, 10)); return 2; }
    if (!strcmp(typ, "bool")) { out[0] = (!strcmp(v, "1") || !strcmp(v, "true")) ? 1 : 0; return 1; }
    if (!strcmp(typ, "TEXT")) return utf8_to_macroman(v, (char *)out, 1024);
    if (!strcmp(typ, "qdrt")) { int a[4] = { 0 }; sscanf(v, "%d;%d;%d;%d", &a[0], &a[1], &a[2], &a[3]); for (int i = 0; i < 4; i++) be16(out + 2 * i, a[i]); return 8; }
    if (!strcmp(typ, "fss ")) {   /* FSSpec: vRefNum -1, parID 2 (volume root), file name */
        char m[300]; int n = utf8_to_macroman(v, m, sizeof m); memset(out, 0, 70);
        be16(out, -1); be32(out + 2, 2); out[6] = (uint8_t)n; memcpy(out + 7, m, n); return n + 1 > 64 ? 6 + n + 1 : 70;
    }
    fprintf(stderr, "type %s\n", typ); exit(2);
}
/* objspec_prop: 'pres' (property of the scan picture) or 'psiz.pwth' (property of a pair property) */
static AERec *objspec_prop(const char *prop) {
    AERec *spec = aerec_new(); uint8_t one[4]; be32(one, 1);
    aerec_put(spec, "want", "type", "cscp", 4, NULL); aerec_put(spec, "from", "null", "", 0, NULL);
    aerec_put(spec, "form", "enum", "indx", 4, NULL); aerec_put(spec, "seld", "long", one, 4, NULL);
    char buf[64]; snprintf(buf, sizeof buf, "%s", prop);
    char *parts[8]; int np = 0; for (char *t = strtok(buf, "."); t && np < 8; t = strtok(NULL, ".")) parts[np++] = t;
    for (int k = 0; k < np; k++) {
        AERec *s = aerec_new(); char m[8]; utf8_to_macroman(parts[k], m, sizeof m);
        aerec_put(s, "want", "type", k < np - 1 ? "cpar" : "prop", 4, NULL); aerec_put(s, "from", "obj ", NULL, 0, spec);
        aerec_put(s, "form", "enum", "prop", 4, NULL); aerec_put(s, "seld", "type", m, strlen(m), NULL);
        spec = s;
    }
    return spec;
}
static void rs_ae_done(MSys *S, Proc *p, AERec *req, AERec *rp, int err) {   /* replies with an error, into the trace */
    (void)S;
    char line[1200]; int k = 0;
    if (rp) for (int i = 0; i < rp->n; i++) {
        AEItem *x = &rp->it[i]; if (x->rec) continue;
        if (!memcmp(x->key, "errs", 4)) { k += snprintf(line + k, sizeof line - k, " errs '%.*s'", (int)(x->data.n < 300 ? x->data.n : 300), (const char *)x->data.b); }
        else if (!memcmp(x->key, "errn", 4) && x->data.n >= 2) { int e = (int16_t)(x->data.b[0] << 8 | x->data.b[1]); if (e) k += snprintf(line + k, sizeof line - k, " errn %d", e); }
    }
    if (err || k) TLOG(p->E, "   AE REPLY %s/%s:%s%s\n", req ? req->cls : "?", req ? req->id : "?", k ? line : "", err ? " (failed)" : "");
}
static void fg_idle(MSys *S, Proc *fg) {
    char bt[48][96]; int depth = emu_backtrace(fg->E, bt, 40);
    if (top < 0) top = depth;
    if (depth > top) return;
    /* inside one of our own calls into the front end (emu_call_own: fghooks.c, popups.c, rawscan.c): its event loops (e.g. the progress
       window's status_update) must not start the next command - the stack walk does not see the outer frames */
    if (fg->E->own_nest > 0) return;
    nidle++;
    /* --abort-at TICKS (research): once the clock passes TICKS while the driver is busy, stop the running operation
       the way the original's queue window does (StopScanner -> send_abort_process_command_to_BG 0x048cec in the FG),
       with the driver's own current operation, preview index and action index */
    { static int refused_seen; if (fw_refused != refused_seen) { refused_seen = fw_refused; TLOG(fg->E, "   SAFETY: an image-processor block was held back\n"); } }
    if (abort_at > 0 && !abort_done && S->clock >= abort_at && !quiet(S, fg)) {
        Proc *bg = msys_find(S, BG_NAME); abort_done = 1;
        if (bg) {
            int op = rds16(bg->E, rd32(bg->E, bg->E->toc - 0x61b8)), pi = rds16(bg->E, rd32(bg->E, bg->E->toc - 0x6210)), ai = rds16(bg->E, rd32(bg->E, bg->E->toc - 0x6250));
            TLOG(fg->E, "   ABORT requested at tick %lld: driver operation %d, preview index %d, action index %d\n", (long long)S->clock, op, pi, ai);
            uint32_t tv = ((fg->E->cpu->gpr[1] - 0x40) & ~0xFu); wr32(fg->E, tv, CODE + 0x048cec); wr32(fg->E, tv + 4, fg->E->toc);
            uint32_t a[3] = { (uint32_t)op, (uint32_t)pi, (uint32_t)ai };
            wr16(fg->E, fg->E->toc - 0x34d2, 1);   /* as StopScanner (0x012f54) from DeletFromQue: the item's status after the abort = 1 */
            emu_call_own(fg->E, tv, a, 3);
        }
        return;
    }
    if (!quiet(S, fg)) return;
    if (waitn > 0) { waitn--; return; }
    if (istep >= nsteps) emu_fail(fg->E, "scenario complete");
    char st[256]; snprintf(st, sizeof st, "%s", steps[istep++]);
    fprintf(S->tf, "SYS === STEP %s (top-level idle #%d, ticks %lld)\n", st, nidle, (long long)S->clock);
    char *rest = strchr(st, ':'); if (rest) *rest++ = 0; else rest = "";
    if (!strcmp(st, "idle")) waitn = *rest ? atoi(rest) : 1;
    else if (!strcmp(st, "set")) {
        char *eq = strchr(rest, '='); if (!eq) emu_fail(fg->E, "bad set step");
        *eq = 0; char *tv = eq + 1, *colon = strchr(tv, ':'); if (!colon) emu_fail(fg->E, "bad set value");
        *colon = 0; const char *typ = tv, *v = colon + 1;
        if (step_log) TLOG(fg->E, "   STEP set %s = %s\n", rest, v);
        uint8_t data[1100]; int n = value(typ, v, data);
        AERec *p = aerec_new();
        aerec_put(p, "----", "obj ", NULL, 0, objspec_prop(rest));
        aerec_put(p, "data", typ, data, (size_t)n, NULL);
        ae_queue_push(fg->W, "core", "setd", p);
        waitn = 1;
    } else if (!strcmp(st, "tables")) { int n = popups_read_all(fg->E); TLOG(fg->E, "   STEP tables: %d popups read\n", n); }
    else if (!strcmp(st, "film")) { int ok = popups_select_filmtype(fg->E, rest); TLOG(fg->E, "   STEP film %s: %s\n", rest, ok ? "selected" : "NOT selected"); }
    else if (!strcmp(st, "prevn")) { fghooks_set_frame(atoi(rest)); ae_queue_push(fg->W, "SC3D", "prev", aerec_new()); waitn = 2; }
    else if (!strcmp(st, "frame")) fghooks_start_preview(fg->E, atoi(rest));
    else if (!strcmp(st, "upd")) raw_dump(fg->E, rest);
    else if (!strcmp(st, "maxdet")) { double cx = 0, cy = 0; int w = 0, h = 0; sscanf(rest, "%lf;%lf;%d;%d", &cx, &cy, &w, &h); raw_maxdet(fg->E, cx, cy, w, h); waitn = 2; }
    else if (!strcmp(st, "fgcall")) {   /* research: call a front-end function without arguments, log its result */
        unsigned a = 0; sscanf(rest, "%x", &a); uint32_t tv = ((fg->E->cpu->gpr[1] - 0x40) & ~0xFu); wr32(fg->E, tv, CODE + a); wr32(fg->E, tv + 4, fg->E->toc);
        uint32_t r = emu_call_own(fg->E, tv, NULL, 0); TLOG(fg->E, "   STEP fgcall %06x -> %08x\n", a, r); }
    else if (!strcmp(st, "raw")) { int w = 0, h = 0, r = raw_start(fg->E, &w, &h); TLOG(fg->E, "   STEP raw: %d (%d x %d)\n", r, w, h); waitn = 2; }
    else if (!strcmp(st, "rawaniso")) {   /* rawaniso:RW - RAW with RW px/mm across (pixel shift; rawscan.c raw_res_w) */
        raw_res_w = atof(rest); int w = 0, h = 0, r = raw_start(fg->E, &w, &h); raw_res_w = 0;
        TLOG(fg->E, "   STEP rawaniso: %d (%d x %d)\n", r, w, h); waitn = 2; }
    else if (!strcmp(st, "rawstream")) {   /* streamed RAW (guard.c): the Max Detail block goes to the scanner as cmd 4 */
        int w = 0, h = 0; raw_stream_armed = 1; int r = raw_start(fg->E, &w, &h); if (r != 1) raw_stream_armed = 0;
        TLOG(fg->E, "   STEP rawstream: %d (%d x %d)\n", r, w, h); waitn = 2; }
    else if (!strcmp(st, "hdr")) { raw_hdr_k = atof(rest); raw_hdr_armed = 1; raw_hdr_fired = 0; TLOG(fg->E, "   STEP hdr armed x%g\n", raw_hdr_k); }
    else if (!strcmp(st, "cas")) {   /* cas:TYPE - film holder by cassette-directory type, chosen in the front end's own menu (cassette.c) */
        char m[200]; int r = cassette_select(fg->E, atoi(rest), m, sizeof m); TLOG(fg->E, "   STEP cas %s: %d %s\n", rest, r, m); waitn = 1; }
    else if (!strcmp(st, "pflag")) TLOG(fg->E, "   STEP pflag: prescan_flag %d, cassette %d\n", cassette_prescan_flag(fg->E), cassette_current(fg->E));
    else if (!strcmp(st, "casmenu")) {   /* casmenu - dump the front end's current cassette menu data (read only): item -> directory type, name */
        uint32_t md = rd32(fg->E, fg->E->toc - 0x3498); int n = md ? rds16(fg->E, md) : -1;
        TLOG(fg->E, "   STEP casmenu: %d items (current type %d)\n", n, cassette_current(fg->E));
        for (int k = 1; k <= n && k < 100; k++) { char nm[48]; uint32_t e = rd32(fg->E, md + 2 + 4 * k); int i = 0; for (; i < 40 && e; i++) { uint8_t c = rd8(fg->E, e + i); if (!c) break; nm[i] = (char)c; } nm[i] = 0;
            TLOG(fg->E, "   CASMENU item %d type %d \"%s\"\n", k, rds16(fg->E, md + 0x192 + 2 * k), nm); }
        waitn = 1; }
    else if (!strcmp(st, "casmount")) {   /* casmount:1 transparent / 0 reflective - as the app's 'R' before a 'pstp' */
        char m[200]; int r = cassette_prepare_mount(fg->E, atoi(rest), m, sizeof m); TLOG(fg->E, "   STEP casmount %s: %d %s\n", rest, r, m); waitn = 1; }
    else if (!strcmp(st, "frames")) TLOG(fg->E, "   STEP frames: %d\n", fghooks_num_frames(fg->E));
    else if (!strcmp(st, "prev")) { ae_queue_push(fg->W, "SC3D", "prev", aerec_new()); waitn = 2; }
    else if (!strcmp(st, "scan")) { ae_queue_push(fg->W, "SC3D", "scan", aerec_new()); waitn = 2; }
    else if (!strcmp(st, "quit")) { ae_queue_push(fg->W, "aevt", "quit", aerec_new()); waitn = 1000000; }
    else if (!strcmp(st, "unplug")) { sim_unplug = 1; TLOG(fg->E, "   STEP unplug: the simulated scanner is switched off\n"); }   /* 0.2.0: test of the scanner-off end (guard.c GONE_LIMIT) */
    else emu_fail(fg->E, "unknown step %s", st);
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = u_fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l + 1); if (fread(d, 1, l, f) != (size_t)l) exit(2); fclose(f); *n = (size_t)l; return d;
}
int main(int argc, char **argv) {
    const char *trace = "/home/claude/work/an/sys_trace_c.txt", *prefs = "/home/claude/work/ex/prefs/Smart342.Prefs.rf";
    const char *stepstr = "idle:5", *vol = "/home/claude/work/vol_run_csys", *pristine = "/home/claude/work/vol_pristine";
    uint64_t maxi = 200000000000ull; int copyvol = 1; const char *flashfile = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) { i++; target_level = !strcmp(argv[i], "sim1") ? 1 : 2; }
        else if (!strcmp(argv[i], "--prefs") && i + 1 < argc) prefs = argv[++i];
        else if (!strcmp(argv[i], "--flash") && i + 1 < argc) flashfile = argv[++i];
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) profile = argv[++i];
        else if (!strcmp(argv[i], "--replay") && i + 1 < argc) { replay = argv[++i]; world_extclock = 1; }
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) stepstr = argv[++i];
        else if (!strcmp(argv[i], "--vol") && i + 1 < argc) vol = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxi = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--keepvol")) copyvol = 0;
        else if (!strcmp(argv[i], "--guard")) use_guard = 1;
        else if (!strcmp(argv[i], "--bt-holds")) bt_on_hold = 1;
        else if (!strcmp(argv[i], "--bp-mirror")) bp_mirror = 1;
        else if (!strcmp(argv[i], "--bp-res")) bp_res = 1;
        else if (!strcmp(argv[i], "--bp-valid")) bp_valid = 1;
        else if (!strcmp(argv[i], "--popups")) dump_popups = 1;
        else if (!strcmp(argv[i], "--raw-patch")) raw_patch = 1;
        else if (!strcmp(argv[i], "--noalign")) noalign_opt = 1;   /* the scanner's colour alignment off for every Max Detail (rawscan.c) */
        else if (!strcmp(argv[i], "--stephold")) stephold_opt = 1;   /* step-and-hold for every Max Detail (rawscan.c, 0.1.28) */
        else if (!strcmp(argv[i], "--bg-integ-limits")) integ_limits = 1;
        else if (!strcmp(argv[i], "--bg-integ-scale") && i + 1 < argc) integ_scale = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bg-msg")) bg_msg = 1;
        else if (!strcmp(argv[i], "--bg-lens-info")) lens_info = 1;
        else if (!strcmp(argv[i], "--bg-cas")) bg_cas = 1;
        else if (!strcmp(argv[i], "--abort-at") && i + 1 < argc) abort_at = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--lens-shift") && i + 1 < argc) lens_shift_pulses = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--focus") && i + 1 < argc) { double v = atof(argv[++i]); for (int k = 0; k < 5; k++) focus_off_l[k] = v; }
        else if (!strcmp(argv[i], "--raw-limit") && i + 1 < argc) { sscanf(argv[++i], "%dx%d", (int *)&raw_lim_w, (int *)&raw_lim_h); }
        else if (!strcmp(argv[i], "--bg-bp") && i + 1 < argc && nbgbp < 16) { unsigned a; sscanf(argv[++i], "%x", &a); bgbp_at[nbgbp++] = a; }
        else if (!strcmp(argv[i], "--step-log")) step_log = 1;
        else if (!strcmp(argv[i], "--dbg") && i + 1 < argc && ndbg < 8) { unsigned a; int r = -1; sscanf(argv[++i], "%x:%d", &a, &r); dbg_at[ndbg] = a; dbg_reg[ndbg++] = r; }
        else if (!strcmp(argv[i], "--scsi-log") && i + 1 < argc) scsi_log_path = argv[++i];
        else if (!strcmp(argv[i], "--scan-res") && i + 1 < argc) { use_fghooks = 1; hk_res = strtod(argv[++i], NULL); fghooks_set(hk_res, hk_scale); }
        else if (!strcmp(argv[i], "--scan-scale") && i + 1 < argc) { use_fghooks = 1; hk_scale = strtod(argv[++i], NULL); fghooks_set(hk_res, hk_scale); }
        else if (!strcmp(argv[i], "--realtime")) world_realtime = 1;
        else if (!strcmp(argv[i], "--escape-names")) world_escape_names = 1;
        else if (!strcmp(argv[i], "--pristine") && i + 1 < argc) pristine = argv[++i];
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && cp -r '%s' '%s' && chmod -R u+w '%s'", vol, pristine, vol, vol);
    if (copyvol && system(cmd)) { fprintf(stderr, "volume copy failed\n"); return 2; }
    char *ss = strdup(stepstr);
    for (char *t = strtok(ss, ","); t && nsteps < 128; t = strtok(NULL, ",")) if (*t) steps[nsteps++] = t;
    if (flashfile) flash = slurp(flashfile, &nflash);     /* a flash file as the scanner stores it (header + fork) */
    else { size_t np; uint8_t *pr = slurp(prefs, &np); flash = make_flash_file(pr, np, &nflash); free(pr); }
    MSys *S = msys_new(vol, trace, maxi);
    S->scsi_target = targets;
    S->on_fg_idle = fg_idle; S->fg_setup = fg_setup; S->on_ae_done = rs_ae_done;
    int rc = msys_run_fg(S);
    if (S->fatal) {
        printf("FATAL in %s %s\n", S->fatal_tag, S->fatal_msg);
        for (int i = 0; i < S->nfatal_bt; i++) printf("%s\n", S->fatal_bt[i]);
    } else printf("done; top-level idles %d remaining steps %d\n", nidle, nsteps - istep);
    if (replay_t) { char m[300]; replay_stats(replay_t, m, sizeof m); printf("%s\n", m); }
    for (int i = 0; i < S->np; i++) printf("%s %s %s%s\n", S->p[i]->tag, S->p[i]->name, S->p[i]->alive ? "alive" : "exit: ", S->p[i]->alive ? "" : S->p[i]->exit_reason);
    return rc;
}
