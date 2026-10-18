/* run_bg.c - scenario runner: original ScannerDriver under emulation + HLE + simulated scanner.
 * Port of scripts/run.py (driver-only reference runs). usage:
 *   run_bg [--vol DIR] [--fresh] [--trace FILE] [--target sim1|sim2] [--scenario init,preview:2/0/0] [--prefs FILE] [--max N] */
#include "mac.h"
#include "fsu.h"
#include "sim.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define APPDIR "Smart342(L) V2.2.0"
static FILE *tf;
static Emu *E;
static World *Wd;
static char *steps[64]; static int nsteps, istep;
static uint32_t appdir, tmpdir;
static struct Bus *bus;

static void be16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void pref_block(uint8_t *pref) {
    memset(pref, 0, 0x30);
    /* struct.pack_into('>f', pref, 0, 0.0) */
    be32(pref + 6, appdir); be16(pref + 10, -1);
    be32(pref + 0xc, appdir); be16(pref + 0x10, -1);
    be16(pref + 0x12, 1);
    pref[0x14] = 1;
    be32(pref + 0x16, appdir); be16(pref + 0x1a, -1);
}
static void put(AERec *r, const char *k, const void *d, size_t n) { aerec_put(r, k, "", d, n, NULL); }

static void step_init(void) {
    AERec *p = aerec_new(); uint8_t b[8], pref[0x30];
    be32(b, 0); be32(b + 4, 0x2002); put(p, "FSNK", b, 8);
    be16(b, 0); put(p, "DBGK", b, 2);
    pref_block(pref); put(p, "PREF", pref, 0x30);
    be32(b, tmpdir); put(p, "STFL", b, 4);
    ae_queue_push(Wd, "SCTX", "INIT", p);
    ae_queue_push(Wd, "SCTX", "INSC", aerec_new());
}

static void idle(World *w) {
    (void)w;
    char wc[200]; emu_where_caller(E, wc);
    const char *last = strrchr(wc, ' '); last = last ? last + 1 : wc;
    if (strcmp(last, "main") && strcmp(last, "WaitForFirstForgroundEvent")) return;
    if (istep >= nsteps) emu_fail(E, "scenario complete");
    char st[256]; snprintf(st, sizeof st, "%s", steps[istep++]);
    char *rest = strchr(st, ':'); if (rest) *rest++ = 0;
    int a[8], na = 0;
    if (rest) for (char *t = strtok(rest, "/"); t && na < 8; t = strtok(NULL, "/")) if (*t) a[na++] = atoi(t);
    /* python: '=== STEP %s %s\n' % (nm, a) with a a list */
    char al[200]; int k = sprintf(al, "[");
    for (int i = 0; i < na; i++) k += sprintf(al + k, "%s%d", i ? ", " : "", a[i]);
    sprintf(al + k, "]");
    fprintf(tf, "=== STEP %s %s\n", st, al);
    if (!strcmp(st, "init")) step_init();
    else if (!strcmp(st, "preview")) {
        int def[3] = { 2, 0, 0 }; for (int i = na; i < 3; i++) a[i] = def[i];
        int cassette = a[0], media = a[1], pindex = a[2];
        uint32_t p1 = new_ptr(E, 8, 0), p2 = new_ptr(E, 8, 0);
        uint32_t args[3] = { (uint32_t)cassette, p1, p2 };
        uint32_t tv = emu_tvec_of(E, "SC_get_cassete_preview_params");
        if (!tv) emu_fail(E, "no export SC_get_cassete_preview_params");
        emu_call_nested(E, tv, args, 3);
        int sctp = rds16(E, p1), imtp = rds16(E, p2);
        fprintf(tf, "=== preview params from driver: cassette %d -> scan type %d image type %d\n", cassette, sctp, imtp);
        AERec *p = aerec_new(); uint8_t b[2];
        be16(b, cassette); put(p, "CATP", b, 2); be16(b, sctp); put(p, "SCTP", b, 2); be16(b, imtp); put(p, "IMTP", b, 2);
        be16(b, media); put(p, "PONG", b, 2); be16(b, pindex); put(p, "PRVI", b, 2); be16(b, 0); put(p, "SMDT", b, 2);
        ae_queue_push(Wd, "SCTX", "PRVW", p);
    } else if (!strcmp(st, "set")) { /* results only */ }
    else emu_fail(E, "unknown step %s", st);
}

static AERec *reply_for(World *w, AERec *ev) {
    (void)w;
    if (!memcmp(ev->cls, "SCTX", 4) && !memcmp(ev->id, "QUES", 4)) {
        AEItem *q = aerec_find(ev, "SQKY"); char txt[300] = "";
        if (q && !q->rec && q->data.n > 5) {
            size_t n = q->data.b[4]; if (5 + n > q->data.n) n = q->data.n - 5;
            memcpy(txt, q->data.b + 5, n); txt[n] = 0;
        }
        char r[2048]; py_repr_str(txt, -1, r, sizeof r); fprintf(tf, "!! QUES dialog: %s\n", r);
        AERec *rp = aerec_new(); uint8_t b[2]; be16(b, 1); aerec_put(rp, "SRKY", "", b, 2, NULL); return rp;
    }
    if (!memcmp(ev->cls, "SCTX", 4) && !memcmp(ev->id, "FRQU", 4)) fprintf(tf, "!! FRQU (fatal quit request)\n");
    return NULL;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = u_fopen(p, "rb"); if (!f) { fprintf(stderr, "cannot read %s\n", p); exit(2); }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l + 1); if (fread(d, 1, l, f) != (size_t)l) exit(2); fclose(f); *n = (size_t)l; return d;
}

int main(int argc, char **argv) {
    const char *vol = "/home/claude/work/vol", *trace = "/home/claude/work/an/run_trace_c.txt", *target = "sim1";
    const char *scenario = "init", *prefs = "/home/claude/work/ex/prefs/Smart342.Prefs.rf";
    uint64_t maxi = 4000000000ull; int fresh = 0, ui = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--vol") && i + 1 < argc) vol = argv[++i];
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) trace = argv[++i];
        else if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
        else if (!strcmp(argv[i], "--scenario") && i + 1 < argc) scenario = argv[++i];
        else if (!strcmp(argv[i], "--prefs") && i + 1 < argc) prefs = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) maxi = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--fresh")) fresh = 1;
        else if (!strcmp(argv[i], "--ui")) ui = 1;
        else { fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
    }
    static char scratch[512];
    if (fresh) {
        snprintf(scratch, sizeof scratch, "/home/claude/work/vol_run_cbg");
        char cmd[2048];
        snprintf(cmd, sizeof cmd, "rm -rf '%s' && cp -r /home/claude/work/vol_pristine '%s' && chmod -R u+w '%s'", scratch, scratch, scratch);
        if (system(cmd)) { fprintf(stderr, "fresh copy failed\n"); return 2; }
        vol = scratch;
    }
    char *sc = strdup(scenario);
    for (char *t = strtok(sc, ","); t && nsteps < 64; t = strtok(NULL, ",")) steps[nsteps++] = t;

    hle_uistub_register(); hle_core_register(); hle_files_register(); if (ui) hle_ui_register(); hle_scsi_register();
    tf = u_fopen(trace, "wb"); if (!tf) { perror(trace); return 2; }
    char pef[1024], rsrc[1100];
    snprintf(pef, sizeof pef, "%s/%s/ScannerDriver Smart342(L)", vol, APPDIR); snprintf(rsrc, sizeof rsrc, "%s.rsrc", pef);
    E = emu_new(pef, tf, NULL); if (!E) return 2;
    Wd = world_new(E, vol, rsrc, APPDIR, NULL, NULL);   /* python run.py: MacWorld(log=None) */
    if (ui) world_install_ui(Wd);
    emu_bind_imports(E);
    size_t np; uint8_t *pr = slurp(prefs, &np); size_t nf; uint8_t *fl = make_flash_file(pr, np, &nf); free(pr);
    ScsiTarget *tg[8] = { 0 };
    tg[4] = sim_new(!strcmp(target, "sim2") ? 2 : 1, E, "flash14", fl, nf); free(fl);
    tg[4]->world = Wd;
    bus = bus_new(Wd, tg, 1);
    appdir = (uint32_t)Wd->app_dir_id;
    tmpdir = (uint32_t)dir_id_for(Wd, "Temporary Items");
    { char hp[2048]; host_path(Wd, "Temporary Items", hp, sizeof hp); u_mkdir(hp); }
    Wd->on_idle = idle;
    Wd->ae_reply_for = reply_for;

    jmp_buf jb; E->fail_jmp = &jb;
    int rc = 0;
    if (!setjmp(jb)) {
        emu_call(E, emu_addr_of(E, "main"), E->toc, NULL, 0, maxi);
        printf("main returned\n");
    } else if (strstr(E->stopped_reason, "scenario complete")) printf("DONE: scenario complete\n");
    else {
        printf("STOP: %s\n", E->stopped_reason);
        char bt[20][96]; int n = emu_backtrace(E, bt, 20); for (int i = 0; i < n; i++) printf("%s\n", bt[i]);
        rc = 1;
    }
    fclose(tf);
    printf("SCSI commands: %d\n", bus_ncmd(bus));
    return rc;
}
