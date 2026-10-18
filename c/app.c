/* app.c - Smart342 for Windows: runs the original Scitex Smart342(L) 2.2.0 front end and ScannerDriver
 * (PowerPC, Mac OS 9) on the built-in emulator, connected to the scanner through Windows SCSI
 * pass-through, with a Windows window that drives the original front end through its own scripting
 * interface (the same settings and buttons its AppleScript dictionary offers).
 *
 * Threads: the UI thread owns the window; one emulation thread runs both Mac applications (fibers).
 * UI -> emulation: a command queue read at the front end's top-level idle. Emulation -> UI: posted
 * messages (log lines, state changes) and, for the program's own dialogs, a blocking SendMessage so
 * the user answers them. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wchar.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "msys.h"
#include "fsu.h"
#include "popups.h"
#include "cassette.h"
#include "rawstack.h"
#include "rawhdr.h"
#include "rawtile.h"
#include "rawcmp.h"
#include "rawsr.h"
#include "rawsharp.h"
#include "rawreg.h"
#include "minerbar.h"
#include "prefscopy.h"
#include "rawchart.h"
#include "cleangain.h"
#include "streakcal.h"
#include "negconv.h"

#define APP_TITLE L"Smart342 for Windows"
#define APP_VERSION8 "0.5.0 (2026-10-18)"
#define APP_VERSION L"0.5.0 (2026-10-18)"

enum { WM_APP_LOG = WM_APP + 1, WM_APP_STATE, WM_APP_DIALOG, WM_APP_EXITED, WM_APP_RANGE, WM_APP_TABLES, WM_APP_PROGRESS, WM_APP_FRAMES };
enum { ST_STARTING, ST_READY, ST_BUSY, ST_PREVIEW_DONE, ST_SCAN_DONE, ST_ERROR, ST_JOB_FAILED, ST_RAW_DONE, ST_HOLDER };
enum { IDM_RAWTEST = 900, IDM_RAWLIMITS = 901, IDM_FOCUSTEST = 902, IDM_LENSTEST = 903, IDM_RAWSTREAM = 904, IDM_PSHIFT = 905, IDM_RSH0 = 950 /* .. 953 RAW sharpening off .. strong */, IDM_COLREG = 960, IDM_NOALIGN = 961, IDM_PSN5 = 962, IDM_PSN8 = 963, IDM_PSSLOW = 964, IDM_STEPHOLD = 965, IDM_LOGDETAIL = 966, IDM_ABOUT = 967, IDM_SCSI = 968, IDM_SETUP = 969, IDM_SETUP_OFFER = 970, IDM_CHART_MEASURE = 971, IDM_CHART_SKIP = 972, IDM_FIXSTREAKS = 973, IDM_NEGPOS = 974, IDM_NEGC0 = 975 /* .. 977 */, IDM_NEGAUTO = 978, IDM_NEGLOCK = 979, IDM_NEGROT0 = 980 /* .. 983 */, IDM_FOCUS0 = 920 /* .. 940: -0.50 .. +0.50 mm */ };
enum { IDC_MOUNT = 100, IDC_CASS, IDC_MODE, IDC_MEDIA, IDC_RES, IDC_SCALE, IDC_MIRROR, IDC_FORMAT, IDC_USECROP,
       IDC_CW, IDC_CH, IDC_CX, IDC_CY, IDC_NAME, IDC_PREVIEW, IDC_SCAN, IDC_STOP, IDC_OPENSCANS, IDC_LOG, IDC_STATUS, IDC_PREVWIN,
       IDC_RESMAX, IDC_RESINFO, IDC_SHARP, IDC_COLOR, IDC_FILM, IDC_BLIST, IDC_BADD, IDC_BSCAN, IDC_BCLEAR, IDC_FRAME, IDC_FRAMEINFO, IDC_PASSES, IDC_KEEPPASS, IDC_HDR, IDC_PROGRESS, IDC_GAMMA };

/* ------------------------------------------------------------------ shared state */
typedef struct { char kind; char prop[16]; char type[8]; char value[300]; } Cmd;   /* kind: 's' setd, 'p' prev, 'c' scan, 'q' quit,
                                                                                      'h' scan-time resolution/scale, 'r' probe the allowed resolution range,
                                                                                      't' read the table lists, 'g' set a table by name (checked against the list) */
static CRITICAL_SECTION cs;
static Cmd cmdq[64]; static int ncmd;
static HWND hwnd, hlog, hstatus, hprev;
static wchar_t exedir[MAX_PATH], voldir[MAX_PATH], logdir[MAX_PATH], scandir_[MAX_PATH];
static int simulate, closing, emu_running, autotest, started_ready_once;
static char replay8[MAX_PATH * 3]; static int cass_default = 2, mode_default = 1;   /* holder: cassette-directory type (cassette.c); 2 = what 0.1.x selected for "T 35mm" */
static wchar_t crop_text[4][32]; static int crop_text_on;   /* developer option --crop-text "L;T;W;H": typed into the crop boxes verbatim (0.1.27: decimal commas) */
static double drag_test[4]; static int drag_test_on; static wchar_t auto_res[32]; static int auto_wait_range; static char auto_tab[3][96];   /* --sharp / --color / --film (autotest) */ static int media_default;   /* developer option --drag-mm L,T,W,H (autotest) */   /* developer option --replay <scsi-trace.bin>: re-run a recorded hardware session */
static char vol8[MAX_PATH * 3], trace8[MAX_PATH * 3], btrace8[MAX_PATH * 3], simflash8[MAX_PATH * 3], simbin8[MAX_PATH * 3], simprof8[MAX_PATH * 3];
static ScsiTarget *hw_target;
static char pending_name[300];          /* output file of the running scan */

static void to_utf8(const wchar_t *w, char *o, int max) { WideCharToMultiByte(CP_UTF8, 0, w, -1, o, max, NULL, NULL); }
static void to_w(const char *s, wchar_t *o, int max) { MultiByteToWideChar(CP_UTF8, 0, s, -1, o, max); }
static void post_log(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)_strdup(buf));
}
/* 0.2.0: a detail line - always in ui-log.txt, in the window only with Tools > Detailed log in the window */
static void post_logd(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    PostMessageW(hwnd, WM_APP_LOG, 1, (LPARAM)_strdup(buf));
}
/* safety-interlock messages: consecutive identical ones are shown once with a count (emulation thread only) */
static char note_last[300]; static long note_rep;
static void note_flush(void) {
    if (note_rep > 1) post_logd("SAFETY INTERLOCK: (%ld times in a row)", note_rep);
    note_last[0] = 0; note_rep = 0;
}
static void post_state(int st) { note_flush(); PostMessageW(hwnd, WM_APP_STATE, (WPARAM)st, 0); }
static void post_state_l(int st, LPARAM l) { note_flush(); PostMessageW(hwnd, WM_APP_STATE, (WPARAM)st, l); }

/* ------------------------------------------------------------------ emulation side */
static int top = -1, waitn = 0, job = 0;   /* job: 0 none, 'p' preview running, 'c' scan running */
/* A job is finished only when the driver says so: it sends the front end 'SCTX'/'FPRV' after a preview and
   'SCTX'/'FSCN' after a final scan; 'FABT' (aborted) or 'FRQU' (fatal) end either. (0.1.3 decided from idleness
   alone, which in real time can happen while the driver is still between steps.) */
static char job_end[5];                    /* id of the driver's event that ended the job, "" = not yet */
/* frames (multi-preview formats such as 4 T35mm H): the preview job covers one frame (job_frame, 0 = any); after a
   preview the front end's current preview is unknown (cur_frame 0) until start_preview is called for a frame */
static int job_frame, cur_frame, last_fprv;
static volatile int raw_patch_ok, raw_hdr_ok, raw_req_w, raw_req_h;
static volatile int lens_ok, focus_ok;   /* the lens / focus hooks are installed (rawscan.c) */
static int noalign_ok, stephold_ok, ft_on, lt_on;   /* forward (defined below) */
#define OPT_REQ(d) ((d) * (1 - 1e-5))   /* 0.1.28: the request for "the CCD's own sampling" - a hair below it (see do_raw) */
#define OPT_SHOW(d) (floor((d) * 10) / 10)   /* the CCD dpi as shown / to type: rounded DOWN to 0.1 dpi (2662.1, 1349.0, 781.3) */
#define OPT_NEAR(x, d) ((d) > 0 && fabs((x) - (d)) < fmax(0.15, 2e-4 * (d)))   /* "the CCD's own sampling": within 0.15 dpi or 0.02 % (review: 781 for L67 was outside 2e-4) */
static int pv_lens = -1, lens_seq_seen; static double pv_opt_dpi;   /* lens of the last prescan and its optical sampling across (dpi) */
extern volatile int spti_dead;    /* spti.c: the scanner stopped answering on SCSI */
#define SCANNER_DEAD (spti_dead || sim_fw_dead)   /* (the simulator models a stopped controller since 0.1.19) */
/* RAW size (0.1.19): the scanner's controller keeps a whole Max Detail in its memory; colour RAW = 6 bytes per pixel
   in a pool of 7,077,888 bytes, so at most 1,179,648 pixels (width x lines) per Max Detail - found in its firmware
   (fwmem.h; it explains session s9, and every Max Detail that worked was under it). A RAW block is kept to raw_area
   pixels (default and maximum 1,000,000 - see RAW_AREA_MAX; smart342.ini [raw] area can only lower it). Larger
   crops are made of blocks (rawtile.c) or, with Tools > Streamed RAW (experimental), in one request that the
   controller streams through as many blocks as fit, as it does for final scans (guard.c). raw_max_w/h: developer
   option --raw-limit WxH (a rectangle instead of the area, for the block tests). */
#define RAW_AREA_FW 1179648L       /* the controller's whole-picture limit for a colour Max Detail */
#define RAW_AREA_MAX 1000000L      /* our ceiling: a final scan right after such a RAW still fits (its blocks are up to
                                      307,200 bytes, SCSI_max_buf_for_scan 0x07e228, and it needs 3 of them: <= 1,026,048 pixels; review) */
static long raw_area = RAW_AREA_MAX;
static int raw_max_w, raw_max_h, raw_max_w2, raw_max_h2;
static int raw_stream_on = 1;             /* Tools > Diagnostics > Streamed RAW for large crops; 0.4.0: on by default and saved (hardware s10, s12, s20,
                                             s36, s39: every streamed RAW complete, counters 0); autotest: off unless --raw-stream (the block tests) */
static volatile int raw_stream_req;       /* the next RAW request ('m') is streamed: set before it is queued */
static volatile int rt_bypass, raw_start_result;
static volatile int stop_req, user_stopped;   /* Stop button: request (taken by the emulator thread); the job ended by the user */    /* 1 while a RAW size test step is requested */   /* RAW: driver patched; size of the running request */
static volatile int fg_frames_extra;        /* FPRVs that arrived outside a job (frames the original previews by itself) */
static void on_route(MSys *S, Proc *from, Proc *to, const AERec *ev) {
    (void)S; (void)to;
    if (strcmp(from->name, BG_NAME) || strcmp(ev->cls, "SCTX")) return;
    /* the driver's progress for the original's Progress window: STUP SVAL = percent, SHSM SHKY = message, CLSM = close */
    if (!strcmp(ev->id, "STUP")) {
        for (int i = 0; i < ev->n; i++) if (!memcmp(ev->it[i].key, "SVAL", 4) && ev->it[i].data.n >= 2)
            PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)(ev->it[i].data.b[0] << 8 | ev->it[i].data.b[1]), 0);
    } else if (!strcmp(ev->id, "SHSM")) {
        for (int i = 0; i < ev->n; i++) if (!memcmp(ev->it[i].key, "SHKY", 4) && ev->it[i].data.n >= 1) {
            int n = ev->it[i].data.b[0]; if (n > (int)ev->it[i].data.n - 1) n = (int)ev->it[i].data.n - 1;
            char t[300]; macroman_to_utf8((const char *)ev->it[i].data.b + 1, n, t, sizeof t);
            PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)-1, (LPARAM)_strdup(t));
        }
    }
    if (!job) return;
    if (!strcmp(ev->id, "FPRV")) {
        int f = 0; for (int i = 0; i < ev->n; i++) if (!memcmp(ev->it[i].key, "PRVI", 4) && ev->it[i].data.n >= 2) f = ev->it[i].data.b[0] << 8 | ev->it[i].data.b[1];
        last_fprv = f; cur_frame = 0;
        if (job != 'p') { fg_frames_extra++; post_state_l(ST_PREVIEW_DONE, f | 0x10000); return; }   /* previewed by the original on its own (Automatic) */
        if (job_frame && f && f != job_frame) return;
    }
    if (!job) return;
    if ((job == 'p' && !strcmp(ev->id, "FPRV")) || (job == 'c' && !strcmp(ev->id, "FSCN")) || (job == 'm' && !strcmp(ev->id, "FMXD")) || !strcmp(ev->id, "FABT") || !strcmp(ev->id, "FRQU"))
        snprintf(job_end, sizeof job_end, "%s", ev->id);
}
static int started;
static int quiet(MSys *S, Proc *fg) {
    Proc *bg = msys_find(S, BG_NAME);
    return !fg->W->nq && (!bg || (!bg->W->nq && (!strcmp(bg->idle_at, "main") || !strcmp(bg->idle_at, "WaitForFirstForgroundEvent"))));
}
static void be16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static AERec *objspec_prop(const char *prop) {
    AERec *spec = aerec_new(); uint8_t one[4]; be32(one, 1);
    aerec_put(spec, "want", "type", "cscp", 4, NULL); aerec_put(spec, "from", "null", "", 0, NULL);
    aerec_put(spec, "form", "enum", "indx", 4, NULL); aerec_put(spec, "seld", "long", one, 4, NULL);
    char buf[64]; snprintf(buf, sizeof buf, "%s", prop);
    char *parts[8]; int np = 0; for (char *t = strtok(buf, "."); t && np < 8; t = strtok(NULL, ".")) parts[np++] = t;
    for (int k = 0; k < np; k++) {
        AERec *s = aerec_new();
        aerec_put(s, "want", "type", k < np - 1 ? "cpar" : "prop", 4, NULL); aerec_put(s, "from", "obj ", NULL, 0, spec);
        aerec_put(s, "form", "enum", "prop", 4, NULL); aerec_put(s, "seld", "type", parts[k], 4, NULL);
        spec = s;
    }
    return spec;
}
static int value(const char *typ, const char *v, uint8_t *out) {
    if (!strcmp(typ, "enum")) { for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v[i] ? v[i] : ' '); return 4; }
    if (!strcmp(typ, "sing")) { float f = (float)strtod(v, NULL); uint32_t b; memcpy(&b, &f, 4); be32(out, b); return 4; }
    if (!strcmp(typ, "bool")) { out[0] = (!strcmp(v, "1") || !strcmp(v, "true")) ? 1 : 0; return 1; }
    if (!strcmp(typ, "TEXT")) return utf8_to_macroman(v, (char *)out, 255);
    if (!strcmp(typ, "fss ")) {
        char m[300]; int n = utf8_to_macroman(v, m, sizeof m); if (n > 63) n = 63; memset(out, 0, 70);
        be16(out, -1); be32(out + 2, 2); out[6] = (uint8_t)n; memcpy(out + 7, m, n); return 70;
    }
    return 0;
}
/* ---- allowed resolution range, decided by the original front end itself ----
   TSetUp::ShowCropValidity(valid) (0x0842bc) is called by check_crop_validity after every resolution change
   (ChangeResolution -> check_crop_validity -> valid_crop). A probe sets candidate resolutions through the scripting
   interface and reads that flag; front-end only (no event to the driver, no SCSI - verified in the trace). */
static volatile int crop_valid_flag, crop_valid_seq;
static void valid_bp(Emu *E, void *ctx) { (void)ctx; crop_valid_flag = (int)(E->cpu->gpr[4] & 0xFF); crop_valid_seq++; }
static void fg_setup(MSys *S, Proc *fg) { (void)S; fghooks_install(fg->E); fghooks_install_frames(fg->E); emu_add_bp(fg->E, 0x0842bc, valid_bp, NULL); }
static struct { int active, phase, waiting, seq0, n; double cur, lo, hi, ok, min, max; double tries[8]; int ntry, itry; } pr;
static void send_setd(Proc *fg, const char *prop, const char *type, const char *val) {
    uint8_t data[300]; int n = value(type, val, data);
    AERec *p = aerec_new();
    aerec_put(p, "----", "obj ", NULL, 0, objspec_prop(prop));
    aerec_put(p, "data", type, data, (size_t)n, NULL);
    ae_queue_push(fg->W, "core", "setd", p);
}
static void probe_try(Proc *fg, double r) {
    char v[64]; snprintf(v, sizeof v, "%.6f", r); pr.cur = r; pr.seq0 = crop_valid_seq; pr.waiting = 1; pr.n++;
    send_setd(fg, "pres", "sing", v); waitn = 1;
}
static void probe_done(Proc *fg, int ok) {
    pr.active = 0;
    if (pr.ntry && pr.tries[0] > 0) { char v[64]; snprintf(v, sizeof v, "%.6f", pr.tries[0]); send_setd(fg, "pres", "sing", v); waitn = 1; }   /* back to the entered value */
    PostMessageW(hwnd, WM_APP_RANGE, (WPARAM)(ok ? (LONG_PTR)(pr.min * 1000 + 0.5) : 0), (LPARAM)(ok ? (LONG_PTR)(pr.max * 1000 + 0.5) : 0));
}
/* one step per quiet front-end idle; returns 1 while the probe is running */
static int probe_step(Proc *fg) {
    if (!pr.active) return 0;
    int v = -1;
    if (pr.waiting) { pr.waiting = 0; v = crop_valid_seq != pr.seq0 ? crop_valid_flag : -1; if (v < 0) { probe_done(fg, 0); return 1; } }
    if (pr.phase == 0) {                       /* find any valid resolution */
        if (v == 1) { pr.ok = pr.cur; pr.phase = 1; pr.lo = 0.5; pr.hi = pr.ok; probe_try(fg, pr.lo); return 1; }
        if (pr.itry < pr.ntry) { probe_try(fg, pr.tries[pr.itry++]); return 1; }
        probe_done(fg, 0); return 1;
    }
    if (pr.phase == 1) {                       /* minimum: lo invalid, hi valid */
        if (v == 1 && pr.cur == 0.5) { pr.min = 0.5; pr.phase = 2; pr.lo = pr.ok; pr.hi = 800; probe_try(fg, pr.hi); return 1; }
        if (pr.cur != 0.5) { if (v == 1) pr.hi = pr.cur; else pr.lo = pr.cur; }
        if (pr.hi - pr.lo > fmax(0.004 * pr.hi, 0.02)) { probe_try(fg, (pr.lo + pr.hi) / 2); return 1; }
        pr.min = pr.hi; pr.phase = 2; pr.lo = pr.ok; pr.hi = 800; probe_try(fg, pr.hi); return 1;
    }
    if (pr.phase == 2) {                       /* maximum: lo valid, hi invalid */
        if (pr.cur == 800 && v == 1) { pr.max = 800; probe_done(fg, 1); return 1; }
        if (pr.cur != 800) { if (v == 1) pr.lo = pr.cur; else pr.hi = pr.cur; }
        if (pr.hi - pr.lo > fmax(0.004 * pr.lo, 0.02)) { probe_try(fg, (pr.lo + pr.hi) / 2); return 1; }
        pr.max = pr.lo; probe_done(fg, 1); return 1;
    }
    return 0;
}
/* ---- table choices (sharpness, colour): the names the original's own Setup popups hold (popups.c) ---- */
static PopList ui_lists[POP_N];          /* copy for the window, under cs */
/* The original's scripting answers a sharpness or colour table it ACCEPTED with "Not found name of Sharp/Color Table":
   SetScanPictureProperty tests the result of DoSharpPopItem (0x039380) and DoColorPopItem (0x03948c) the wrong way
   round (beq where the gradation branch at 0x039274 has bne). The name was checked against the popup before sending,
   so that answer is expected and not shown as an error. */
static volatile int tbl_quirk;
static void tables_post(Proc *fg) {
    popups_read_all(fg->E);
    EnterCriticalSection(&cs); memcpy(ui_lists, pop_lists, sizeof ui_lists); LeaveCriticalSection(&cs);
    PostMessageW(hwnd, WM_APP_TABLES, 0, 0);
}
static void table_set(Proc *fg, const char *prop, const char *name) {
    int k = !strcmp(prop, "psrp") ? POP_SHARP : !strcmp(prop, "pclr") ? POP_COLOR : !strcmp(prop, "pgrd") ? POP_GRAD : -1;
    if (k < 0) return;
    popups_read_all(fg->E);                 /* the list as it is now (it follows Mode and Media) */
    const PopList *L = &pop_lists[k]; int found = 0;
    for (int i = 0; i < L->n; i++) if (!strcmp(L->item[i], name)) found = 1;
    if (!L->n) { post_log("Table %s: the original offers no list here - not changed.", popup_name(k)); return; }
    /* a name the original does not offer would make it fail at scan time ("Cannot open file"), so never send one */
    const char *use = found ? name : L->item[0];
    if (!found) post_log("Table %s: \"%s\" is not offered for this mode/media; using \"%s\".", popup_name(k), name, use);
    tbl_quirk = (k == POP_SHARP || k == POP_COLOR);
    send_setd(fg, prop, "TEXT", use); waitn = 1;
    post_logd("Table sent: %s = %s", popup_name(k), use);
}
static void run_cmd(MSys *S, Proc *fg, Cmd *c) {
    (void)S;
    if (c->kind == 's') {
        uint8_t data[300]; int n = value(c->type, c->value, data);
        AERec *p = aerec_new();
        aerec_put(p, "----", "obj ", NULL, 0, objspec_prop(c->prop));
        aerec_put(p, "data", c->type, data, (size_t)n, NULL);
        ae_queue_push(fg->W, "core", "setd", p);
        waitn = 1;
    } else if (c->kind == 'p' || c->kind == 'P') {       /* 'P' value = frame: the scripted Preview covers that frame (fghooks.c) */
        raw_hdr_armed = 0;                                 /* the longer exposure is only ever for the RAW pass it was armed for */
        raw_stream_armed = 0;
        job_frame = c->kind == 'P' ? atoi(c->value) : 0; fghooks_set_frame(job_frame);
        ae_queue_push(fg->W, "SC3D", "prev", aerec_new()); waitn = 2; job = 'p'; job_end[0] = 0; PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)-2, 'p'); }
    else if (c->kind == 'm') {                              /* RAW: the original's Max Detail of exactly the crop (rawscan.c) */
        double hk = atof(c->value);                        /* > 1: experimental HDR - this Max Detail with k x the integration time */
        double rw = 0;                                     /* "k s rw": s = 1 streamed RAW; rw > 0: px/mm across (pixel shift) */
        { const char *sp = strchr(c->value, ' '); raw_stream_req = sp && atoi(sp + 1) == 1; const char *sp2 = sp ? strchr(sp + 1, ' ') : NULL; if (sp2) rw = atof(sp2 + 1); }
        raw_hdr_k = hk > 1 ? hk : 0; raw_hdr_armed = hk > 1; raw_hdr_fired = 0; raw_last_range = -9; raw_hdr_dark = -9;
        int w = 0, h = 0; raw_inpol14 = -1;
        int stream = raw_stream_req; raw_stream_req = 0;
        raw_lim_w = raw_lim_h = raw_lim_w2 = raw_lim_h2 = 0; raw_lim_area = 0;
        if (rt_bypass || stream) { }                      /* streamed: the controller only needs a few blocks (guard.c checks) */
        else if (raw_max_w > 0) { raw_lim_w = raw_max_w; raw_lim_h = raw_max_h; raw_lim_w2 = raw_max_w2; raw_lim_h2 = raw_max_h2; }
        else raw_lim_area = raw_area > 0 ? raw_area + 20000 : 0;   /* the driver's own rounding of the crop can add a row and a column (review): 2 % slack, still <= 1,026,048 */
        raw_stream_armed = stream;
        raw_res_w = rw > 0 ? rw : 0;
        int r = raw_start(fg->E, &w, &h); raw_req_w = w; raw_req_h = h; raw_start_result = r;
        raw_res_w = 0;
        if (r != 1) { raw_hdr_armed = 0; raw_stephold = 0; raw_stream_armed = 0; }
        if (r == 1) { PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)-2, 'm'); post_logd("RAW: %d x %d pixels requested from the scanner software (12-bit prescan at the scan resolution).", w, h); job = 'm'; job_end[0] = 0; waitn = 2; }
        else if (r == -100) {
            if (raw_max_w > 0) post_log("RAW refused before anything was sent: %d x %d pixels is more than the RAW block size set with --raw-limit (%d x %d pixels).", w, h, raw_max_w, raw_max_h);
            else post_log("RAW refused before anything was sent: %d x %d = %lld pixels is more than one RAW block (%ld pixels): the scanner keeps a whole RAW block in its memory, and a larger one makes it stop (as on 8 Oct).", w, h, (long long)w * h, raw_area);
            post_state(ST_JOB_FAILED); }
        else { post_log("RAW: the scanner software did not accept the area (result %d).", r); post_state(ST_JOB_FAILED); } }
    else if (c->kind == 'k') {                              /* film holder: chosen in the original's own cassette menu (cassette.c) */
        static int logged = -1; char m[200]; int t = atoi(c->value);
        int r = cassette_select(fg->E, t, m, sizeof m);
        if (r != 1 || t != logged) {                        /* 0.2.0: the menu details as a detail line, the holder's name in the window */
            const CasHolder *hh = cas_holder(t);
            if (r >= 1 && hh) { post_logd("%s", m); post_log("Holder: %s%s.", hh->name, hh->transparent ? "" : " (reflective)"); } else post_log("%s", m); }
        logged = r >= 1 ? t : -1;
        if (r == 2) cur_frame = 0;                          /* the original's Restart: no preview is the current one any more */
        /* the window learns the holder the front end really has, and whether its previews were discarded */
        post_state_l(ST_HOLDER, (LPARAM)(((r == 2) << 16) | (cassette_current(fg->E) & 0xFFFF)));
        if (r == 2) {                                       /* a scan or RAW needs a preview of the new holder first */
            int scan_queued = 0; EnterCriticalSection(&cs);
            for (int i = 0; i < ncmd; i++) if (strchr("cm", cmdq[i].kind)) scan_queued = 1;
            if (scan_queued) ncmd = 0;
            LeaveCriticalSection(&cs);
            if (scan_queued) { post_log("The scan was not started: make a preview with the new holder first."); post_state(ST_JOB_FAILED); } }
        if (r < 1) {                                        /* never start a preview/scan/RAW with a different holder than the one chosen */
            int job_queued = 0; EnterCriticalSection(&cs);
            for (int i = 0; i < ncmd; i++) if (strchr("pPcm", cmdq[i].kind)) job_queued = 1;
            if (job_queued) ncmd = 0;
            LeaveCriticalSection(&cs);
            if (job_queued) { post_log("The preview/scan was not started: the scanner software could not be set to this holder."); post_state(ST_JOB_FAILED); } } }
    else if (c->kind == 'R') {                              /* before a mount: the original's Restart if a preview exists (cassette.c) */
        char m[200]; int r = cassette_prepare_mount(fg->E, atoi(c->value) == 0, m, sizeof m);
        if (m[0]) post_log("%s", m);
        if (r == 2) cur_frame = 0;
        if (r != 1) post_state_l(ST_HOLDER, (LPARAM)(((r == 2) << 16) | (cassette_current(fg->E) & 0xFFFF)));
        if (r < 0) {                                        /* the mount stays: nothing queued after it may run (it would ask again) */
            int job_queued = 0; EnterCriticalSection(&cs);
            for (int i = 0; i < ncmd; i++) if (strchr("pPcm", cmdq[i].kind)) job_queued = 1;
            ncmd = 0; LeaveCriticalSection(&cs);
            if (job_queued) { post_log("The preview/scan was not started: the scanner software keeps the mount of its previews."); post_state(ST_JOB_FAILED); } } }
    else if (c->kind == 'n') {                              /* number of frames of the current format */
        int n = fghooks_num_frames(fg->E); PostMessageW(hwnd, WM_APP_FRAMES, (WPARAM)n, atoi(c->value)); }
    else if (c->kind == 'F') {                              /* make frame n the current preview (as opening its thumbnail) */
        int f = atoi(c->value);
        if (f > 0 && f != cur_frame) { fghooks_start_preview(fg->E, f); cur_frame = f; waitn = 1; } }
    else if (c->kind == 'c') { raw_hdr_armed = 0; raw_stream_armed = 0; ae_queue_push(fg->W, "SC3D", "scan", aerec_new()); waitn = 2; job = 'c'; job_end[0] = 0; PostMessageW(hwnd, WM_APP_PROGRESS, (WPARAM)-2, 'c'); }
    else if (c->kind == 'q') { ae_queue_push(fg->W, "aevt", "quit", aerec_new()); waitn = 1000000; }
    else if (c->kind == 't') tables_post(fg);
    else if (c->kind == 'g') table_set(fg, c->prop, c->value);
    else if (c->kind == 'f') {
        int ok = popups_select_filmtype(fg->E, c->value);
        if (ok) post_log("Film type set: %s", c->value); else post_log("Film type \"%s\" is not offered now - not changed.", c->value);
        tables_post(fg);
    }
    else if (c->kind == 'h') { double r = 0, sc = 0; sscanf(c->value, "%lf %lf", &r, &sc); fghooks_set(r, sc); }
    else if (c->kind == 'r') {
        memset(&pr, 0, sizeof pr); pr.active = 1;
        double start = atof(c->value); double cand[] = { start, 11.811, 40, 100, 7, 200, 3, 300 };
        for (int i = 0; i < 8; i++) if (cand[i] > 0) pr.tries[pr.ntry++] = cand[i];
        probe_step(fg);
    }
}
static void fg_idle(MSys *S, Proc *fg) {
    char bt[48][96]; int depth = emu_backtrace(fg->E, bt, 40);
    if (top < 0) top = depth;
    if (depth > top) return;
    /* inside one of our own calls into the front end (emu_call_own: fghooks.c, popups.c, rawscan.c): its event loops (e.g. the progress
       window's status_update) must not start the next command - the stack walk does not see the outer frames */
    if (fg->E->own_nest > 0) return;
    /* Stop: the way the original's queue window stops a running item (DeletFromQue -> StopScanner(no question) ->
       send_abort_process_command_to_BG 0x048cec in the front end, item status after the abort = 1 at TOC-0x34d2),
       with the driver's own current operation, preview index and action index (its 'ABRT' handler 0x09c4e0 marks the
       abort only when they match). The driver stops all motors, resets and re-initialises the scanner (handle_Abort
       0x0981fc) and answers 'FABT'. Verified in the simulator (run_sys --abort-at): the scan is not saved, the next
       preview and scan work. Commands still waiting (e.g. the next frames of a preview) are dropped. */
    /* the safety interlock held back an image-processor block (it would have stopped the scanner's controller, or it
       had an unexpected form): the driver aborts the operation by itself (guard.c, sense 0x00e3) and shows its alert
       "Motion board S/W loading failed" - explained here (review 0.1.19) */
    { static int refused_seen;
      if (fw_refused != refused_seen) {
          refused_seen = fw_refused;
          post_log("SAFETY: an image-processor command was held back - it would have stopped the scanner. Nothing that could harm the scanner was sent; the scanner software stops this operation. Its message \"Motion board S/W loading failed\" comes from this safety stop, not from the motion board. Please send the Logs folder.");
          { wchar_t p[MAX_PATH * 2]; swprintf(p, MAX_PATH * 2, L"%ls\\smart342.ini", exedir); WritePrivateProfileStringW(L"rawcheck", L"held", L"1", p); } } }
    if (stop_req) {
        Proc *bg = msys_find(S, BG_NAME);
        int op = bg ? rds16(bg->E, rd32(bg->E, bg->E->toc - 0x61b8)) : 0;
        static DWORD stop_t0; static int st_op = -1, st_cap = -1;
        if (op != st_op || guard_capture != st_cap) { st_op = op; st_cap = guard_capture; post_logd("Stop pending: scanner software operation %d, picture being sent %d, job %c.", op, (int)guard_capture, job ? job : '-'); }
        if (job && job_end[0]) {             /* this step had just ended: nothing to abort, but nothing further is started (blocks, passes, frames, list) */
            stop_req = 0; stop_t0 = 0; EnterCriticalSection(&cs); ncmd = 0; LeaveCriticalSection(&cs);
            post_log("Stop: the current step had just finished - nothing further is started."); goto stop_done; }
        if (job && op == 0) {                /* handed to the front end, not started by the driver yet: wait for it (at most 20 s) */
            if (!stop_t0) stop_t0 = GetTickCount();
            if (GetTickCount() - stop_t0 < 20000) return;
            post_log("Stop: the scanner software did not start the job within 20 s - it will run; press Stop again if needed.");
            stop_req = 0; user_stopped = 0; stop_t0 = 0; goto stop_done; }
        stop_t0 = 0;
        /* 0.4.0: not while the scanner sends a picture (guard_capture: from the picture's image-processor block to its end
           handshake). The driver's abort stops the motors but not the controller's transfer; on the hardware (12 Oct,
           19:51) the next calibration then got no CCD scan signal and the scanner software ended with "CCD timeout"
           (B01018; power cycle needed). Every Stop during calibrations has been fine. So the Stop waits until the picture
           is in (seconds for a preview frame, up to a few minutes for a large RAW), then the job ends as Stop expects:
           a step that has just ended starts nothing further (above); one still running is aborted (below). */
        { static DWORD cap_t0; static int cap_said;
          if (guard_capture && !scsi_gone && !sim_fw_dead && bg && job) {
              if (!cap_t0) cap_t0 = GetTickCount();
              if (!cap_said) { cap_said = 1;
                  /* 0.5.0 (hardware 18 Oct): Stop 8 s into a streamed full-frame RAW waited 87 s and then saved it - say so up front */
                  post_log(raw_stream_armed ? "Stop: the scanner is sending the picture. A streamed RAW is ONE picture of the whole crop, so it is finished first (up to about 2 minutes for a full 35 mm frame at 2662 dpi) and saved; then nothing further starts. Stopping in the middle of a picture leaves the scanner needing a power cycle (B01018)."
                                            : "Stop: the scanner is sending the picture - the job stops as soon as it is in (stopping in the middle of a picture leaves the scanner needing a power cycle, B01018)."); }
              if (GetTickCount() - cap_t0 < 15 * 60000) return;
              post_log("Stop: the picture did not finish within 15 minutes - stopped now; if the next job reports a CCD timeout, switch the scanner off and on.");
          }
          cap_t0 = 0; cap_said = 0; }
        stop_req = 0;
        EnterCriticalSection(&cs); ncmd = 0; LeaveCriticalSection(&cs);
        if (!bg || !job) { post_log("Stop: nothing was running in the scanner software yet - the waiting steps are dropped."); post_state(ST_JOB_FAILED); }   /* (review: without this the window stayed busy) */
        else {
            int pi = rds16(bg->E, rd32(bg->E, bg->E->toc - 0x6210)), ai = rds16(bg->E, rd32(bg->E, bg->E->toc - 0x6250));
            post_logd("Stop: asking the scanner software to stop (operation %d, frame %d).", op, pi);
            uint32_t tv = ((fg->E->cpu->gpr[1] - 0x40) & ~0xFu); wr32(fg->E, tv, CODE + 0x048cec); wr32(fg->E, tv + 4, fg->E->toc);
            uint32_t a[3] = { (uint32_t)op, (uint32_t)pi, (uint32_t)ai };
            wr16(fg->E, fg->E->toc - 0x34d2, 1);
            emu_call_own(fg->E, tv, a, 3);
        }
        return;
    }
stop_done:
    if (!quiet(S, fg)) return;
    if (waitn > 0) { waitn--; return; }
    if (!started) { started = 1; post_state(ST_READY); }
    if (job) {
        if (!job_end[0]) return;                 /* the driver has not reported the end yet */
        raw_stream_armed = 0;                    /* never left armed after a job (review) */
        if (!strcmp(job_end, "FPRV")) post_state_l(ST_PREVIEW_DONE, last_fprv ? last_fprv : 1);
        else if (!strcmp(job_end, "FSCN")) post_state(ST_SCAN_DONE);
        else if (!strcmp(job_end, "FMXD")) { cur_frame = 0; post_state(ST_RAW_DONE); }   /* the front end now shows the max detail */
        else if (!strcmp(job_end, "FABT") && user_stopped) { post_log("Stopped: the %s was stopped as you asked; nothing was saved from it.", job == 'p' ? "preview" : job == 'm' ? "RAW scan" : "scan"); post_state(ST_JOB_FAILED); }
        else { post_log("The scanner software ended the %s without finishing it (code %s).", job == 'p' ? "preview" : job == 'm' ? "RAW scan" : "scan", job_end); post_state(ST_JOB_FAILED); }
        job = 0;
    }
    if (probe_step(fg)) return;
    Cmd c; int have = 0;
    EnterCriticalSection(&cs);
    if (ncmd) { c = cmdq[0]; memmove(cmdq, cmdq + 1, (ncmd - 1) * sizeof(Cmd)); ncmd--; have = 1; }
    LeaveCriticalSection(&cs);
    if (have) run_cmd(S, fg, &c);
}
/* 0.4.0: the driver's own print of each picture's processing (bip_calc_dsp_var): "DROP_PIXELS:%d ... OUT_ROWS/COLUMNS:%d %d
   ... CUT_PIX:%d", then "INPOL_WIDTH_STEP: %f". The picture's first column (PM order) is CCD photosite DROP + CUT - 1
   (hardware s34, s37b: best correlation of the column pattern with the white line). Calibrations print 1 row - not kept. */
static volatile int dsp_seq, dsp_drop = -1, dsp_cut = -1, dsp_rows, dsp_cols, dsp_pend, dsp_wcomp, dsp_wcomp_next; static volatile double dsp_inpol;
static void dsp_parse(const char *m) {
    const char *p = strstr(m, "DROP_PIXELS:"), *q;
    /* "HEIGHT & WIDTH COMPRESSION: %d %d" comes just before: the CCD photosites summed per picture column (previews 8) */
    if ((q = strstr(m, "HEIGHT & WIDTH COMPRESSION:"))) { int hc = 0, wc = 0; dsp_wcomp_next = sscanf(q + 27, "%d %d", &hc, &wc) == 2 ? wc : 0; return; }
    if (p) { int r = 0, c = 0, cut = -1, d = atoi(p + 12);
        if ((q = strstr(m, "OUT_ROWS/COLUMNS:"))) { if (sscanf(q + 17, "%d %d", &r, &c) != 2) r = c = 0; }
        if ((q = strstr(m, "CUT_PIX:"))) cut = atoi(q + 8);
        if (r > 1 && c > 1 && d >= 0 && cut >= 0) { dsp_drop = d; dsp_cut = cut; dsp_rows = r; dsp_cols = c; dsp_inpol = 0; dsp_wcomp = dsp_wcomp_next; dsp_pend = 1; dsp_seq++;
            if (simulate) { sim_dsp_drop = d; sim_dsp_cut = cut; sim_dsp_inpol = 1; sim_dsp_wcomp = dsp_wcomp > 0 ? dsp_wcomp : 1; } }
        else dsp_pend = 0;
        return; }
    if (dsp_pend && (p = strstr(m, "INPOL_WIDTH_STEP:"))) { dsp_inpol = atof(p + 17); dsp_pend = 0; if (simulate) sim_dsp_inpol = dsp_inpol; }
}
static void on_log(World *W, const char *msg) { (void)W; note_flush(); post_logd("%s", msg); dsp_parse(msg); }   /* the scanner software's own prints: detail */
static void on_ae_done(MSys *S, Proc *p, AERec *req, AERec *rp, int err) {
    (void)S; (void)p;
    char line[1200] = ""; int k = 0;
    if (rp) for (int i = 0; i < rp->n; i++) {
        AEItem *x = &rp->it[i]; if (x->rec) continue;
        if (!memcmp(x->key, "errs", 4) || !memcmp(x->type, "TEXT", 4)) {
            char t[600]; macroman_to_utf8((const char *)x->data.b, (int)(x->data.n < 500 ? x->data.n : 500), t, sizeof t);
            k += snprintf(line + k, sizeof line - k, " %s", t);
        } else if (!memcmp(x->key, "errn", 4) && x->data.n >= 2) {
            int e = (int16_t)(x->data.b[0] << 8 | x->data.b[1]); if (e) k += snprintf(line + k, sizeof line - k, " (error %d)", e);
        }
    }
    if (tbl_quirk && req && !strcmp(req->id, "setd") && (strstr(line, "Not found name of Sharp Table") || strstr(line, "Not found name of Color Table"))) { tbl_quirk = 0; return; }
    if (req && !strcmp(req->id, "setd")) tbl_quirk = 0;
    /* ChangeCropHorW (0x0871d8) refuses a width/height equal to the current one with this text (checked: same value
       refused, a different one accepted); the crop is sent once for the range check and again with the scan */
    if (req && !strcmp(req->id, "setd") && (!strcmp(line, " change of width not legal") || !strcmp(line, " change of height not legal") ||
        !strcmp(line, " (error -1) change of width not legal") || !strcmp(line, " (error -1) change of height not legal"))) return;
    if (err || k) post_logd("Front end: %s/%s%s%s", req ? req->cls : "?", req ? req->id : "?", k ? " ->" : "", k ? line : (err ? " failed" : ""));
    if (err) post_log("Scanner software:%s", k ? line : " a setting was not accepted (see the Logs folder).");   /* 0.2.0 (review): an error reply stays visible */
}
typedef struct { const DlgDesc *d; int n; int result; } DlgReq;
static int dialog_policy(World *W, const DlgDesc *d, int n) {
    (void)W; DlgReq r = { d, n, 1 };
    SendMessageW(hwnd, WM_APP_DIALOG, 0, (LPARAM)&r);
    return r.result;
}
static uint8_t *slurp8(const char *path, size_t *n) {
    FILE *f = u_fopen(path, "rb"); uint8_t *d = NULL; *n = 0;
    if (f) { fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET); d = malloc((size_t)l + 1);
             if (fread(d, 1, (size_t)l, f) == (size_t)l) *n = (size_t)l; else { free(d); d = NULL; } fclose(f); }
    return d;
}
static void guard_note(const char *m);
static void scsi_note(const char *m);
/* 0.3.0: the scanner-number check of the scanner software (resource_of_current_machine 0x068658): at 0x068718
   (cmpw r31, r5) r31 = the number in the scanner's parameter file (get_flash_scanner_no, read from the scanner at
   every start), r5 = the number in Smart342.Prefs (G[-4ce0], STR# 300); G[-4c48] = lw_butting_configured (set
   earlier at start-up by motion_lw_butting_exist: motion configuration 141b bit 12 - what the original Calibration
   uses to copy STR# 399 / 410). When the numbers differ the software shows alert 7012 ("Incompatibility between
   scanner number in scanner (...) and scanner number in Smart342.Prefs (...)") and stops; this program then offers
   the Scanner(Flash Disk) -> Smart342.Prefs copy (prefs_offer). */
static volatile int serial_mm, serial_lw = -1; static volatile uint32_t serial_flash_no, serial_prefs_no;
static void serial_bp(Emu *E, void *ctx) {
    (void)ctx; uint32_t f = E->cpu->gpr[31], p = E->cpu->gpr[5];
    serial_lw = rds16(E, E->toc - 0x4c48); serial_flash_no = f;   /* the connected scanner's number (Set up this scanner keys its values by it) */
    if (f != p) { serial_mm = 1; serial_flash_no = f; serial_prefs_no = p;
        post_logd("Scanner-number check: the scanner says %u, Smart342.Prefs says %u (line-work butting %d).", (unsigned)f, (unsigned)p, (int)serial_lw); }
    else post_logd("Scanner-number check: %u in the scanner and in Smart342.Prefs.", (unsigned)f);
}
/* ---- the streaks along the scan (cleangain.c, 0.3.1; streakcal.c, 0.4.0) ----
   At every gain calibration (leagal_gain_vector 0x05ca80): at 0x05cca4 (mr r5,r30 - r3 the white line just measured,
   r4 nonzero = transparent, r30 = lens) the line is copied; at 0x05ccb0 (sth r3,0x138(r1), right after
   correct_gefen_gain_vec; r27 still the line) the corrected line. before / after is what every scan of this calibration
   carries (the gain is K / after): logged. 0.4.0: the number of lines the calibration averaged comes from calibrate_gain
   0x047b7c (li r3,0 right after lhax r27,r28,r4: r27 = the line count from the lens table, 16 for a preview, 256 for a
   scan or RAW - STR# 1202 "16 64" / "256 256", RE notes s37); the latest 256-line white line is kept for Tools > Fix
   streaks (the preview's 16-line ones are noisier and carry a fixed pattern of their sample spots - not used). */
static uint16_t cg_before[CG_N], cg_after[CG_N]; static uint32_t cg_ptr; static int cg_lens = -1, cg_refl, cg_nl;
static volatile double cg_last_res = -1, cg_last_pat = -1; static volatile int cg_note_due, cg_last_lens = -1, cg_last_refl;
static volatile int cg_nlines;            /* set at 0x047b7c for the calibration being made */
static volatile int cg_hooks_ok, cg_lines_ok;   /* the hooks are installed (this driver version) */
static uint16_t fs_wline[CG_N]; static volatile int fs_wseq, fs_wlens = -1, fs_wrefl, fs_wn;   /* the latest white line of >= 256 lines */
static void cg_lines_bp(Emu *E, void *ctx) { (void)ctx; cg_nlines = (int)(int16_t)E->cpu->gpr[27]; }
static uint32_t cg_smart_env(Emu *E) { uint32_t h = rd32(E, E->toc - 0x7208); return h ? rd32(E, h) : 0; }
static void gain_bp_before(Emu *E, void *ctx) {
    (void)ctx; cg_ptr = 0; cg_nl = cg_nlines; cg_nlines = 0; uint32_t se = cg_smart_env(E); if (!se) return;
    int npos = rds16(E, se + 0x14) - 1;
    if (npos != CG_NPIX) { post_logd("Gain calibration: %d positions per line (expected %d) - not measured.", npos, CG_NPIX); return; }
    cg_ptr = E->cpu->gpr[3]; cg_refl = E->cpu->gpr[4] == 0; cg_lens = (int)E->cpu->gpr[30];
    for (int i = 0; i < CG_N; i++) cg_before[i] = rd16(E, cg_ptr + 2 * i);
}
static void gain_bp_after(Emu *E, void *ctx) {
    (void)ctx; if (!cg_ptr || E->cpu->gpr[27] != cg_ptr) return;
    for (int i = 0; i < CG_N; i++) cg_after[i] = rd16(E, cg_ptr + 2 * i);
    double lv = 0; for (int p = 0; p < CG_LIGHT; p++) lv += cg_before[p * 3 + 1]; lv /= CG_LIGHT;
    static const char *LN[5] = { "L67 4x5", "L89 6x6", "L110 35 mm", "L60 8x10", "L80 line-work" };
    char nl[40] = ""; if (cg_nl > 0) snprintf(nl, sizeof nl, ", %d lines", cg_nl);
    if (cg_lens < 0 || cg_lens > 4 || lv < 1000) { post_logd("Gain calibration (lens %d%s): white level %.0f - not used.", cg_lens, nl, lv); cg_ptr = 0; return; }
    double pat = cg_pattern(cg_before), res = cg_residual(cg_before, cg_after);
    int same = !memcmp(cg_before, cg_after, sizeof cg_before);
    post_logd("Gain calibration, %s lens, %s%s: white level %.0f; this CCD's pixel pattern %.2f %%; left in the scans after the scanner software's clean-gain correction %.2f %%%s.",
              LN[cg_lens], cg_refl ? "reflective" : "transparent", nl, lv, pat * 100, res * 100, same ? " (no correction made)" : "");
    cg_last_res = res; cg_last_pat = pat; cg_last_lens = cg_lens; cg_last_refl = cg_refl; if (res > 0.003) cg_note_due = 1;
    if (cg_nl >= 256) { memcpy(fs_wline, cg_before, sizeof fs_wline); fs_wlens = cg_lens; fs_wrefl = cg_refl; fs_wn = cg_nl; fs_wseq++; }
    cg_ptr = 0;
}
static void targets(MSys *S, Proc *bg, ScsiTarget **tg) {
    (void)S;
    if (simulate && replay8[0]) {
        ScsiTarget *r = replay_open(replay8, bg->E);
        if (!r) { post_log("Cannot read the recorded session %s", replay8); r = sim_new(2, bg->E, "flash14", NULL, 0); }
        r->world = bg->W; tg[4] = guard_new(r); tg[4]->note = guard_note; tg[4]->log = bg->E;
    } else if (simulate) {
        /* Simulator\flash14.bin: the parameter file exactly as the real scanner sends it (rebuilt from a hardware log);
           Simulator\profile.txt: answers the real scanner gave. Older packages: Simulator\flash.rf (a prefs fork). */
        size_t n = 0, nf = 0; uint8_t *fl = slurp8(simbin8, &n);
        if (fl) nf = n;
        else { uint8_t *d = slurp8(simflash8, &n); fl = d ? make_flash_file(d, n, &nf) : NULL; free(d); }
        ScsiTarget *sim = sim_new(2, bg->E, "flash14", fl, nf); free(fl);
        sim_load_profile(sim, simprof8);     /* no profile file: the built-in default answers */
        sim->world = bg->W; sim->note = scsi_note;   /* 0.2.0: a simulated "switched off" scanner reports as spti.c does */
        /* the same safety interlock as with the real scanner, so the simulation takes the same path */
        tg[4] = guard_new(sim); tg[4]->note = guard_note; tg[4]->log = bg->E;
    } else { tg[4] = hw_target; hw_target->log = bg->E; }
    if (rd32(bg->E, CODE + 0x068718) == 0x7C1F2800u && rd32(bg->E, CODE + 0x068714) == 0x80A2B320u) emu_add_bp(bg->E, 0x068718, serial_bp, NULL);   /* cmpw r31,r5 / lwz r5,-0x4ce0(r2) */
    else post_logd("Note: the scanner-number check of this scanner software version is not known - a Smart342.Prefs of another scanner is not detected.");
    if (rd32(bg->E, CODE + 0x05cca4) == 0x7FC5F378u && rd32(bg->E, CODE + 0x05ccb0) == 0xB0610138u) { emu_add_bp(bg->E, 0x05cca4, gain_bp_before, NULL); emu_add_bp(bg->E, 0x05ccb0, gain_bp_after, NULL); cg_hooks_ok = 1; }   /* mr r5,r30 / sth r3,0x138(r1) */
    else post_logd("Note: the gain calibration of this scanner software version is not known - the streak check is off.");
    if (cg_hooks_ok && rd32(bg->E, CODE + 0x047b78) == 0x7F7C22AEu && rd32(bg->E, CODE + 0x047b7c) == 0x38600000u) { emu_add_bp(bg->E, 0x047b7c, cg_lines_bp, NULL); cg_lines_ok = 1; }   /* lhax r27,r28,r4 / li r3,0 */
    else post_logd("Note: the line count of the gain calibration is not known in this scanner software version - Fix streaks is not available.");
    raw_patch_ok = raw_patch_driver(bg->E);
    raw_hdr_ok = raw_patch_ok && raw_hdr_install(bg->E);
    lens_ok = lens_install(bg->E);
    focus_ok = focus_install(bg->E);
    noalign_ok = noalign_install(bg->E);
    stephold_ok = stephold_install(bg->E);
    if (!raw_patch_ok) post_log("Note: RAW scans are not available (the scanner software is not the expected version).");
    tg[4]->world = bg->W;
}
static void scsi_note(const char *m) { note_flush(); post_log("%s", m); }
static void guard_note(const char *m) {
    if (!strcmp(m, note_last)) { note_rep++; return; }
    note_flush(); snprintf(note_last, sizeof note_last, "%s", m); note_rep = 1;
    post_logd("SAFETY INTERLOCK: %s", m);
}
static DWORD WINAPI emu_thread(LPVOID arg) {
    (void)arg;
    world_realtime = !simulate;
    MSys *S = msys_new(vol8, trace8, 0);
    S->scsi_target = targets; S->on_fg_idle = fg_idle; S->dialog_policy = dialog_policy; S->on_log = on_log; S->on_ae_done = on_ae_done; S->on_route = on_route; S->fg_setup = fg_setup;
    msys_run_fg(S);
    note_flush();
    int gone = S->fatal && strstr(S->fatal_msg, GONE_TAG) != NULL;   /* guard.c: the scanner no longer answers (0.2.0) */
    if (S->fatal) {
        post_logd("STOPPED: %s: %s", S->fatal_tag, S->fatal_msg);
        for (int i = 0; i < S->nfatal_bt; i++) post_logd("    %s", S->fatal_bt[i]);
        if (!gone && !serial_mm) post_log("The scanner software stopped because of an internal error. Close the window and send the Logs folder.");
        post_state(ST_ERROR);
    }
    fflush(S->tf);
    PostMessageW(hwnd, WM_APP_EXITED, gone ? 2 : S->fatal ? 1 : 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ UI helpers */
static HFONT font;
/* layout is designed at 96 dpi and scaled to the display (the program is per-monitor DPI aware) */
static int dpi = 96;
static int S(int v) { return MulDiv(v, dpi, 96); }
/* 0.4.0: the miner bar's art is 16 pixels tall and is enlarged by whole pixels only (crisp): its height is the multiple
   of 16 nearest to the old bar's 18 px at this display scaling (100 - 125 %: 16 px, 150 - 200 %: 32, 250 - 300 %: 48) */
static int bar_h(void) { int k = (S(18) + 8) / 16; if (k < 1) k = 1; return 16 * k; }
static int window_dpi(HWND h) {
    typedef UINT (WINAPI *GDFW)(HWND);
    GDFW f = (GDFW)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (f && h) { UINT d = f(h); if (d) return (int)d; }
    HDC dc = GetDC(NULL); int d = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc); return d > 0 ? d : 96;
}
static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExW(!wcscmp(cls, L"EDIT") || !wcscmp(cls, L"LISTBOX") ? WS_EX_CLIENTEDGE : 0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE); return c;
}
static void combo(int id, const wchar_t *const *items, int sel) {
    HWND c = GetDlgItem(hwnd, id); for (int i = 0; items[i]; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(c, CB_SETCURSEL, sel, 0);
}
static int log_detail;   /* 0.2.0: Tools > Detailed log in the window (saved, off): detail lines (the scanner software's own
                            prints, the interlock, measurements) go only to ui-log.txt; the window shows the program's own messages */
static void log_line(const wchar_t *s, int detail) {
    if (!detail || log_detail) { int n = (int)SendMessageW(hlog, LB_ADDSTRING, 0, (LPARAM)s); SendMessageW(hlog, LB_SETTOPINDEX, n, 0); }
    /* the same lines go to ui-log.txt in the session's log folder */
    static FILE *f; if (!f && logdir[0]) { wchar_t p[MAX_PATH * 2]; swprintf(p, MAX_PATH * 2, L"%ls\\ui-log.txt", logdir); f = _wfopen(p, L"wb"); }
    if (f) { char u[8200]; to_utf8(s, u, sizeof u); SYSTEMTIME t; GetLocalTime(&t); fprintf(f, "%02d:%02d:%02d  %s\r\n", t.wHour, t.wMinute, t.wSecond, u); fflush(f); }
}
static void log_w(const wchar_t *s) { log_line(s, 0); }
static void log_d(const wchar_t *s) { log_line(s, 1); }
static void log8(const char *s) { wchar_t w[2100]; to_w(s, w, 2100); log_w(w); }
static void log8d(const char *s) { wchar_t w[2100]; to_w(s, w, 2100); log_d(w); }
/* script codes, in menu order (verified in SetScanPictureProperty: enum -> item of the front end's own menus) */
static const char *MOUNT_C[] = { "trnp", "rflc" };
static const wchar_t *MOUNT_L[] = { L"Transparent", L"Reflective", NULL };
/* Film holders: NOT through the scripting property 'pcas' - its codes are one off the 2.2.0 cassette directory ('t35m'
   selected T 6 x 6, 'abrd' Strip 6 x 7, 't6x8' was refused) and cannot reach Automatic, Strip 4 x 5, All Board or
   Framed 8x35mm. The holder is chosen in the original's own cassette menu the way a user does (cassette.c); the combo
   lists the holders that menu offers for the chosen mount, each item's data = the cassette-directory type. */
static const char *MODE_C[] = { "colr", "blwh", "lnar" };
/* The front end's scripting enums map to the Setup "Mode" menu items 1-3 (SetScanPictureProperty -> 0x10e = 1/2/3 ->
   DoModePopItem via get_AE_selected_mode). In 2.2.0 that menu is Color CMYK | Color RGB | B/W | Line-art | SmartDOT,
   so 'blwh' selects Color RGB and 'lnar' selects B/W (8-bit grey) - verified: TIFF photometric 2 (3 x 8 bit) and
   1 (8 bit). Line-art (item 4) is not reachable through the script. */
static const wchar_t *MODE_L[] = { L"Color CMYK", L"Color RGB", L"Greyscale (B/W)", NULL };
static const char *MEDIA_C[] = { "pstv", "negt" };
static const wchar_t *MEDIA_L[] = { L"Positive", L"Negative", NULL };
static const char *FMT_C[] = { "TIFF", "CT..", "EPSF", "PICT", "PSIm", "PHOT", "TIFF" };
static const wchar_t *FMT_L[] = { L"TIFF", L"Scitex CT", L"EPSF", L"PICT", L"PSImage", L"Photoshop", L"RAW 16-bit TIFF", NULL };
static const int PASSES_V[] = { 1, 2, 3, 4, 6, 8, 12, 16 };   /* RAW passes choices (averaged) */
static const wchar_t *LENS_NAME[5] = { L"4x5 lens (L67)", L"6x6/6x7 lens (L89)", L"35 mm lens (L110)", L"A3 / 8x10 lens (L60)", L"line-work lens (L80)" };
/* pixel shift (Tools menu, experimental, 0.1.21): pass k with the lens carriage PS_STEPS[k] motor steps from the
   driver's position (always the same direction: hardware s11 showed about half a step of play when the direction
   changes). At least PS_MIN passes; then passes are added until every fine column has a sample near it (rawsr.c
   griddist <= SR_GRIDDIST_OK, the way sr_combine places samples - a "largest phase gap" rule let phases 0, .25, .75
   through, which leave every second column empty: review) or PS_MAX passes were made. s11 (L89): steps 0, 1, 2 moved
   the picture 0, 0.44, 1.59 px across - phases 0, .56, .41, grid distance 0.112. Why 3 at least: in rawsr_test with
   those offsets, 2 passes give an error of 166 and 3 passes 46 (one pass 1108) - the third pass puts a second sample
   on the other side of each new column. Light at +2 steps: 98.9 % (matched per pass, rawsr.c). */
#define PS_MAX 8                       /* array size: the 8-pass setting (0.1.23) */
#define PS_MIN 3
#define PS_APPROACH 8                  /* approach from above (rawscan.c lens_shift_approach): 8 steps above, then down, before each pass */
static const int PS_STEPS[PS_MAX] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static int ps_slow = 1;                 /* 0.1.26: Tools > Pixel shift: slow lens moves (rawscan.c lens_shift_slow); saved; 0.2.0: on by default (the lens shift test of 11 Oct) */
static int ps_approach = PS_APPROACH;  /* smart342.ini [window] pixel_shift_approach (not in the menu): 8 = from above (default), 0 = straight as in 0.1.22 */
static int ps_npass = 5;               /* Tools > Pixel shift passes: 5 = up to 5, stop when the grid is covered (default); 8 = all 8 (less noise); saved */
static int rsh_level, rsh_opt = -1;   /* Tools > RAW sharpening: 0 off, 1 light, 2 medium, 3 strong (saved); developer option --raw-sharpen N */
static const double RSH_K[4] = { 0, 0.023, 0.010, 0.0043 };   /* 0.1.24: the same white-noise gains as 0.1.22 (x1.52 / 2.19 / 3.14) with the chart-fitted model at 2662 x 5324 dpi (35 mm lens, 2x along); a pixel-shift result (5324 x 5324) gets x1.08 / 1.64 / 2.49 (0.1.22: 1.19 / 1.87 / 2.85) - review */
static const wchar_t *RSH_NAME[4] = { L"off", L"light", L"medium", L"strong" };
static HMENU rsh_menu, psn_menu, neg_menu;
static int psn_opt;   /* developer option --ps-passes 5|8 */
static int log_detail_opt; static int psslow_opt;   /* developer option --ps-slow (1) / --no-ps-slow (-1): Tools > Pixel shift: slow lens moves (not saved) */
static void psn_check(void) { if (psn_menu) CheckMenuRadioItem(psn_menu, IDM_PSN5, IDM_PSN8, ps_npass == 8 ? IDM_PSN8 : IDM_PSN5, MF_BYCOMMAND); }
static int colreg_on = 1, colreg_opt = -1, noalign_on, noalign_opt, stephold_on = 1, stephold_opt;   /* 0.2.0: step-and-hold saved, on by default; --stephold / --no-stephold */   /* + Tools > scanner colour alignment off (research, not saved); --noalign */   /* Tools > RAW colour registration (saved, on by default); developer option --colreg 0/1 */
static void rsh_check(void) { if (rsh_menu) CheckMenuRadioItem(rsh_menu, IDM_RSH0, IDM_RSH0 + 3, IDM_RSH0 + rsh_level, MF_BYCOMMAND); }
static int ps_on, ps_opt;   /* Tools > Pixel shift (not saved); developer option --pixel-shift */
static const double HDR_K[] = { 0, 2, 4, 2, 4 };                /* RAW exposure: normal, HDR +1 / +2 stops, long x2 / x4 (experimental) */
static const int HDR_LONG[] = { 0, 0, 0, 1, 1 };                 /* 0.5.0: one pass with k x the integration time, no merge - for dense film (negatives) */
static const wchar_t *GAMMA_L[] = { L"Linear (as scanned)", L"Gamma 2.2", NULL };   /* 0.2.0: RAW output encoding (saved) */
static const wchar_t *HDR_L[] = { L"Normal", L"HDR +1 stop (experimental)", L"HDR +2 stops (experimental)", L"Long x2 - dense film, one pass", L"Long x4 - dense film, one pass", NULL };
#define FMT_RAW 6                          /* not a format of the original: its 12-bit Max Detail prescan, saved by us */
static int sel(int id) { int s = (int)SendMessageW(GetDlgItem(hwnd, id), CB_GETCURSEL, 0, 0); return s < 0 ? 0 : s; }
static int cass_type(void) { HWND c = GetDlgItem(hwnd, IDC_CASS); int s = (int)SendMessageW(c, CB_GETCURSEL, 0, 0); return s < 0 ? -1 : (int)SendMessageW(c, CB_GETITEMDATA, s, 0); }
static void cass_fill(int mount, int type) {     /* mount 0 transparent, 1 reflective; selects `type` if listed, else the first (Automatic) */
    HWND c = GetDlgItem(hwnd, IDC_CASS); SendMessageW(c, CB_RESETCONTENT, 0, 0); int pick = 0;
    for (int i = 0; i < CAS_NHOLDERS; i++) if (CAS_HOLDERS[i].transparent == (mount == 0)) {
        wchar_t w[64]; to_w(CAS_HOLDERS[i].name, w, 64); int k = (int)SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w);
        SendMessageW(c, CB_SETITEMDATA, k, CAS_HOLDERS[i].type); if (CAS_HOLDERS[i].type == type) pick = k; }
    SendMessageW(c, CB_SETCURSEL, pick, 0);
}
static void text8(int id, char *o, int max) { wchar_t w[300]; GetWindowTextW(GetDlgItem(hwnd, id), w, 300); to_utf8(w, o, max); }
/* 0.1.27: a number field as text for the scanner software, a decimal comma read as a point (review: "6,12" in a crop box
   was sent as 6 mm - the driver side reads the text with strtod in the C locale; edit_num / cur_dpi already did this) */
static void num8(int id, char *o, int max) { text8(id, o, max); for (char *c = o; *c; c++) if (*c == ',') *c = '.'; }
static void push(char kind, const char *prop, const char *type, const char *val) {
    EnterCriticalSection(&cs);
    if (ncmd < 64) { Cmd *c = &cmdq[ncmd++]; memset(c, 0, sizeof *c); c->kind = kind; snprintf(c->prop, sizeof c->prop, "%s", prop ? prop : "");
        snprintf(c->type, sizeof c->type, "%s", type ? type : ""); snprintf(c->value, sizeof c->value, "%s", val ? val : ""); }
    LeaveCriticalSection(&cs);
}
static void push_holder(int type) { char v[16]; snprintf(v, sizeof v, "%d", type); push('k', NULL, NULL, v); }
static void push_mount(int m) { char v[8]; snprintf(v, sizeof v, "%d", m); push('R', NULL, NULL, v); push('s', "pstp", "enum", MOUNT_C[m]); }
static int busy_now, started_ui;   /* started_ui: the scanner software is ready (Stop makes sense) */
static int job_ui;      /* 'p' / 'c' while a preview / scan runs (window side) */
static void bq_show(void);
static void pg_end_job(void);
static void set_busy(int busy) {
    busy_now = busy; if (!busy) { job_ui = 0; pg_end_job(); }
    /* the holder (and the mount, which selects the holder list) only changes between jobs: a change after a preview
       restarts the scanner software (cassette.c), which must not happen in the middle of a preview, scan or RAW */
    EnableWindow(GetDlgItem(hwnd, IDC_CASS), !busy); EnableWindow(GetDlgItem(hwnd, IDC_MOUNT), !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_BADD), !busy); if (GetDlgItem(hwnd, IDC_BLIST)) bq_show();
    EnableWindow(GetDlgItem(hwnd, IDC_PREVIEW), !busy); EnableWindow(GetDlgItem(hwnd, IDC_SCAN), !busy);
    EnableWindow(GetDlgItem(hwnd, IDC_STOP), busy && started_ui);
}
static void status(const wchar_t *s) { SetWindowTextW(hstatus, s); }
/* ------------------------------------------------------------------ progress (0.2.0)
   One bar for the whole job: a preview of several frames, a scan, a RAW of several passes (averaging, HDR, pixel shift,
   blocks), a test of several RAWs. A job is made of units - each one preview / scan / Max Detail that the scanner
   software runs (the emulation thread reports when it starts one: WM_APP_PROGRESS -2). Within a unit the scanner
   software reports its own percent (STUP, 5 % steps; hardware s36: a full-frame Max Detail pass = about 25-35 s of
   calibration without a report, then 5 % every 10.3 s, 90 -> 100 in 4.7 s, then about 4 s to the end) - its rate
   gives the unit's length; a unit's length is measured from its start to the next unit's start, so the program's own
   work between passes (registration, combining, saving) is included in the estimate of the passes still to come.
   Before anything is known the bar runs as a marquee and says "estimating". The fraction shown only grows within a job
   (pixel shift may add passes: the bar then waits until the work catches up). */
static HWND hprog;   /* the bar in use for the current job: one of the two below */
/* 0.4.1: the miner bar is an easter egg - each job (preview, scan, RAW, test) shows it about 1 time in MINER_ODDS,
   otherwise the standard Windows progress bar. --miner 1 / 0 forces it either way (tests). */
static HWND hprog_std, hprog_miner; static int miner_opt = -1;
#define MINER_ODDS 15
static int miner_pick(void) {
    static unsigned long long r;
    if (!r) r = GetTickCount64() * 0x9E3779B97F4A7C15ULL ^ ((unsigned long long)GetCurrentProcessId() << 32) ^ 0xD1B54A32D192ED03ULL;
    r ^= r << 13; r ^= r >> 7; r ^= r << 17;
    return (int)((r >> 33) % MINER_ODDS) == 0;
}
static int sim_off_unit, sim_off_at_quit;   /* developer options (simulator): the scanner is switched off as unit K of a job starts / before the quit */
static struct {
    int on, total, started, upto, locked, marquee;
    double w[64], done_t, done_w, tail, disp;
    double t_unit;                     /* seconds (pg_now) when the current unit started */
    int nst; double st_t0, st_p0, st_t1, st_p1, t100;
    wchar_t what[64], unit[16], prefix[48];
} pg;
static double pg_now(void) { return GetTickCount64() / 1000.0; }
/* 0.4.0: a job that finished with a result (picture saved, preview loaded) ends with the miner finding the diamond:
   the bar stays at 100 % for PG_DONE_MS, then hides (timer 9); stopped or failed jobs hide the bar at once.
   0.4.1: only when the miner bar was shown (an easter egg, see pg_begin); the standard bar hides at once. */
static int pg_ok;
#define PG_DONE_MS 5000
static void pg_done(void) { pg_ok = 1; }
static void pg_marquee(int on) {
    if (!hprog || pg.marquee == on) return;
    pg.marquee = on; LONG st = GetWindowLongW(hprog, GWL_STYLE);
    SetWindowLongW(hprog, GWL_STYLE, on ? (st | PBS_MARQUEE) : (st & ~PBS_MARQUEE));
    SendMessageW(hprog, PBM_SETMARQUEE, on, 40);
    if (!on) SendMessageW(hprog, PBM_SETRANGE32, 0, 1000);
}
static void pg_tick(void);
static int pg_estimate(double *rem_out, double *el_out);
/* what: "Preview", "Scan", "RAW", ...; unit: "frame", "pass", "picture" ... ("" for a single unit); total: units planned */
static void pg_begin(const wchar_t *what, const wchar_t *unit, int total) {
    if (pg.locked && pg.on) return;      /* a test of several RAWs owns the bar (lens shift / focus test) */
    double keep_tail = pg.tail; wchar_t pre[48]; wcscpy(pre, pg.prefix);
    memset(&pg.w, 0, sizeof pg.w); pg.done_t = pg.done_w = 0; pg.disp = 0; pg.started = 0; pg.upto = 0; pg.nst = 0; pg.t100 = -1;
    pg.tail = keep_tail > 0 ? keep_tail : -1; pg.total = total < 1 ? 1 : total; pg.on = 1;   /* (weights kept for the first 64 units; later ones weigh 1) */
    for (int i = 0; i < 64; i++) pg.w[i] = 1;
    swprintf(pg.what, 64, L"%ls", what); swprintf(pg.unit, 16, L"%ls", unit); wcscpy(pg.prefix, pre);
    pg_ok = 0; KillTimer(hwnd, 9);
    { HWND want = hprog_std;          /* 0.4.1: the miner only now and then (easter egg) */
      if (hprog_miner && (miner_opt >= 0 ? miner_opt : miner_pick())) want = hprog_miner;
      if (!want) want = hprog_miner;
      if (hprog && hprog != want) { SendMessageW(hprog, PBM_SETMARQUEE, 0, 0); SetWindowLongW(hprog, GWL_STYLE, GetWindowLongW(hprog, GWL_STYLE) & ~PBS_MARQUEE); SendMessageW(hprog, PBM_SETPOS, 0, 0); ShowWindow(hprog, SW_HIDE); }
      hprog = want;
      if (hprog && hprog == hprog_miner) log_d(L"Progress: the miner digs for this one.");
    }
    if (hprog) { ShowWindow(hprog, SW_SHOW); pg.marquee = -1; pg_marquee(1); }
    SetTimer(hwnd, 7, 500, NULL); pg_tick();
}
static void pg_set_total(int n, int upto) { if (!pg.on) return; if (n > pg.total || n >= pg.started) pg.total = n < 1 ? 1 : n; pg.upto = upto; }
static void pg_weight(int unit1, double w) { if (unit1 >= 1 && unit1 <= 64 && w > 0) pg.w[unit1 - 1] = w; }
static void pg_unit_start(void) {
    if (!pg.on) pg_begin(L"Working", L"", 1);
    double now = pg_now();
    if (pg.started > 0) {                /* the previous unit ends here */
        double dur = now - pg.t_unit;
        pg.done_t += dur; pg.done_w += pg.started <= 64 ? pg.w[pg.started - 1] : 1;
        if (pg.t100 >= 0 && dur >= pg.t100) { double tl = dur - pg.t100; pg.tail = pg.tail < 0 ? tl : 0.5 * (pg.tail + tl); }
    }
    pg.started++; if (pg.started > pg.total) pg.total = pg.started;
    if (simulate && sim_off_unit > 0 && pg.started == sim_off_unit && !sim_unplug) { sim_unplug = 1; log_d(L"Developer option --sim-off-unit: the simulated scanner is switched off now."); }
    pg.t_unit = now; pg.nst = 0; pg.t100 = -1;
    { double rem = 0, el = 0; int known = pg_estimate(&rem, &el); wchar_t m[200];
      if (known) swprintf(m, 200, L"Progress: %ls %ls %d of %d started at %.0f s; estimated total %.0f s.", pg.what, pg.unit[0] ? pg.unit : L"unit", pg.started, pg.total, el, el + rem);
      else swprintf(m, 200, L"Progress: %ls %ls %d of %d started at %.0f s; no estimate yet.", pg.what, pg.unit[0] ? pg.unit : L"unit", pg.started, pg.total, el);
      log_d(m); }
    pg_tick();
}
static void pg_stup(int p) {
    if (!pg.on || pg.started < 1 || p < 0 || p > 100) return;
    double t = pg_now() - pg.t_unit;
    if (pg.nst == 0 || p < pg.st_p1) { pg.st_t0 = t; pg.st_p0 = p; pg.nst = 0; }   /* (a new percent series in the same unit starts over) */
    pg.st_t1 = t; pg.st_p1 = p; pg.nst++;
    if (p >= 100 && pg.t100 < 0) pg.t100 = t;
    pg_tick();
}
static void pg_end(void) {
    if (!pg.on) { pg.prefix[0] = 0; pg.locked = 0; return; }
    if (pg.started) { wchar_t m[160]; swprintf(m, 160, L"Progress: %ls ended after %.0f s (%d unit%ls).", pg.what, pg.done_t + pg_now() - pg.t_unit, pg.started, pg.started == 1 ? L"" : L"s"); log_d(m); }
    pg.on = 0; pg.locked = 0; pg.prefix[0] = 0; KillTimer(hwnd, 7);
    if (hprog && pg_ok && hprog == hprog_miner) { pg_marquee(0); SendMessageW(hprog, PBM_SETPOS, 1000, 0); SetTimer(hwnd, 9, PG_DONE_MS, NULL); }   /* the miner finds the diamond */
    else if (hprog) { pg_marquee(0); SendMessageW(hprog, PBM_SETPOS, 0, 0); ShowWindow(hprog, SW_HIDE); }
    pg_ok = 0;
}
static void pg_end_job(void) { if (!pg.locked) pg_end(); else pg_ok = 0; }   /* a test of several RAWs keeps its bar between its RAWs (0.4.0 review: and a step's success is not the test's) */
static void pg_fmt_left(double s, wchar_t *o, int n) {
    if (s < 60) swprintf(o, n, L"less than a minute left");
    else if (s < 3600) swprintf(o, n, L"about %d min left", (int)ceil(s / 60));
    else { int m = (int)ceil(s / 60); swprintf(o, n, L"about %d h %02d min left", m / 60, m % 60); }
}
/* the estimate: 1 = known; *rem = seconds still to go, *el = seconds since the first unit started */
static int pg_estimate(double *rem_out, double *el_out) {
    double t = pg.started ? pg_now() - pg.t_unit : 0;
    int cur = pg.started ? pg.started - 1 : 0; double wc = cur < 64 ? pg.w[cur] : 1;
    double ubase = pg.done_w > 0 ? pg.done_t / pg.done_w : -1, tl = pg.tail >= 0 ? pg.tail : 5;
    double ucur = -1;
    if (pg.started && pg.nst >= 2 && pg.st_p1 > pg.st_p0 && pg.st_t1 > pg.st_t0) {
        double r = (pg.st_p1 - pg.st_p0) / (pg.st_t1 - pg.st_t0);     /* percent per second */
        ucur = pg.t100 >= 0 ? pg.t100 + tl : pg.st_t1 + (pg.st_p1 < 90 ? (90 - pg.st_p1) / r : 0) + 4.7 + tl;
    } else if (pg.started && ubase > 0) ucur = ubase * wc;
    double wr = 0; for (int i = pg.started; i < pg.total; i++) wr += i < 64 ? pg.w[i] : 1;
    if (!pg.started) wr = 0, ucur = -1;
    double uw = ubase > 0 ? ubase : ucur > 0 ? ucur / wc : -1;
    int known = ucur > 0 && (wr <= 0 || uw > 0);
    double rc = ucur - t; if (rc < 1) rc = 1;                         /* running over the estimate: almost done */
    *rem_out = known ? rc + wr * uw : 0; *el_out = pg.done_t + t;
    return known;
}
static void pg_tick(void) {
    if (!pg.on || !hprog) return;
    double rem = 0, el = 0, frac = 0; int known = pg_estimate(&rem, &el);
    if (known) {
        frac = el + rem > 0 ? el / (el + rem) : 0;
        if (frac > 0.99) frac = 0.99;
        if (frac > pg.disp) pg.disp = frac;
        pg_marquee(0); SendMessageW(hprog, PBM_SETPOS, (WPARAM)(int)(pg.disp * 1000 + 0.5), 0);
    } else pg_marquee(1);
    wchar_t m[300], part[80] = L"", left[64];
    if (pg.unit[0] && pg.total > 1) {
        int k = pg.started ? pg.started : 1;
        if (pg.upto > pg.total) swprintf(part, 80, L" - %ls %d of %d (up to %d)", pg.unit, k, pg.total, pg.upto);
        else swprintf(part, 80, L" - %ls %d of %d", pg.unit, k, pg.total);
    }
    /* the time left is the current estimate; the bar never goes back - so no percent in the text, which could
       contradict a raised estimate (e.g. "93 %" with "about 2 min left") */
    if (known) { pg_fmt_left(rem, left, 64); swprintf(m, 300, L"%ls%ls%ls - %ls", pg.prefix, pg.what, part, left); }
    else swprintf(m, 300, L"%ls%ls%ls - %ls", pg.prefix, pg.what, part, pg.started ? L"estimating the time..." : L"starting...");
    status(m);
}
static void passes_enable(void) { int raw = sel(IDC_FORMAT) == FMT_RAW; EnableWindow(GetDlgItem(hwnd, IDC_PASSES), raw && sel(IDC_HDR) == 0); EnableWindow(GetDlgItem(hwnd, IDC_KEEPPASS), raw); EnableWindow(GetDlgItem(hwnd, IDC_HDR), raw); EnableWindow(GetDlgItem(hwnd, IDC_GAMMA), raw); }
static void push_film(void);
static void push_original_settings(void) {
    push_mount(sel(IDC_MOUNT));
    push_holder(cass_type());
    push('s', "pmod", "enum", MODE_C[sel(IDC_MODE)]);
    push('s', "pmed", "enum", MEDIA_C[sel(IDC_MEDIA)]);
    push_film();
}
static int valid_name(const char *n) {
    if (!*n || strlen(n) > 31) return 0;
    for (const char *p = n; *p; p++) if (*p == ':' || *p == '/' || *p == '\\' || (unsigned char)*p < 32) return 0;
    return 1;
}

/* ------------------------------------------------------------------ preview */
static BITMAPINFO pbi; static uint8_t *pbits; static int pw, ph;
/* Preview geometry from the driver's B2F_PreviewParameters_<n> (read by the front end's read_prescan_parameters):
   float at +20 = extent along the scan lines' direction (rows, the crop's Height axis) in mm, float at +24 = extent
   across (columns, Width axis) in mm, short at +52 / +54 = rows / columns. The front end sets the preview's crop start
   to 0,0 (constant 0.0), so the crop offsets Left/Top are millimetres from the preview's top-left corner. */
static double pv_mm_rows, pv_mm_cols;     /* 0 = unknown: drawing a crop is disabled */
/* The original front end shows the preview mirrored left-right relative to the PM data when
   display_is_mirror_to_PM(mount, b) is true: (mount == 1 transparent) XOR (b == 1); b was 0 in every recorded session.
   Crop coordinates are those of that display (verified on two real scans: Left 0 = the PM data's right edge). */
static int pv_mount = 0, pv_mirror;       /* mount (0 transparent, 1 reflective) of the last preview request */
static int pv_negative;                   /* the last preview was requested with Media = Negative */
/* 0.4.0: the shown preview's samples (PM order: not mirrored; host-order 12-bit as the driver wrote them) - for the
   Fix streaks empty-frame check and the negative conversion of the preview */
static uint16_t *pv_r12; static int pv_r12_w, pv_r12_h, pv_r12_frame;
/* scan list: several crops of the same preview, scanned one after the other with the window's other settings */
typedef struct { double w, h, x, y; wchar_t res[32], scale[32], name[64]; int frame, fmt, passes, hdr; } BItem;
static BItem bq[40]; static int nbq, bq_running, bq_idx, bq_wait_range;
static HBITMAP pv_cache; static int pv_cache_w, pv_cache_h;    /* the preview scaled to the window, drawn once per size */
static int drag_on; static POINT drag_a, drag_b;
/* The driver stores the preview size in the PM file's resource fork: 'Cprm' 1 "DEBUG MODE PICT PARAMS" (16 bytes),
   first two big-endian words = number of scan lines, pixels per line (add_disp_params_to_PM_rsrc_fork). The data fork holds
   the scan lines one after another: per pixel 16-bit big-endian R, G, B. Returns 0 and fills lines and pix when found. */
static int pm_size(const wchar_t *rsrc_path, int *lines, int *pix) {
    FILE *f = _wfopen(rsrc_path, L"rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 1); int ok = (long)fread(d, 1, (size_t)n, f) == n; fclose(f);
    int rc = -1;
    #define BE16(p) ((uint32_t)(p)[0] << 8 | (p)[1])
    #define BE32(p) ((uint32_t)(p)[0] << 24 | (uint32_t)(p)[1] << 16 | (uint32_t)(p)[2] << 8 | (p)[3])
    if (ok && n >= 16) {
        uint32_t doff = BE32(d), moff = BE32(d + 4), mlen = BE32(d + 12);
        if (moff + 28 <= (uint32_t)n && moff + mlen <= (uint32_t)n) {
            const uint8_t *m = d + moff; uint32_t tlo = BE16(m + 24); const uint8_t *tl = m + tlo;
            if (moff + tlo + 2 > (uint32_t)n) { free(d); return -1; }   /* damaged resource file */
            int ntypes = (int)(int16_t)BE16(tl) + 1;
            for (int t = 0; t < ntypes && rc; t++) {
                const uint8_t *te = tl + 2 + 8 * t; if (te + 8 > d + n) break;
                if (memcmp(te, "Cprm", 4)) continue;
                int cnt = (int)(int16_t)BE16(te + 4) + 1; uint32_t ro = BE16(te + 6);
                for (int k = 0; k < cnt; k++) {
                    const uint8_t *re = tl + ro + 12 * k; if (re + 12 > d + n) break;
                    if ((int16_t)BE16(re) != 1) continue;
                    uint32_t off = BE32(re + 4) & 0xFFFFFF, at = doff + off;
                    if (at + 4 + 4 > (uint32_t)n || BE32(d + at) < 4) break;
                    *lines = (int)BE16(d + at + 4); *pix = (int)BE16(d + at + 6); rc = 0; break;
                }
            }
        }
    }
    #undef BE16
    #undef BE32
    free(d); return rc;
}
/* ---- 0.4.0: negatives -> a positive (negconv.c) ----
   With Media = Negative the preview is shown converted as the positive file of a RAW will be, and every RAW of a negative
   also gets a positive TIFF next to it (" POS16"): 16-bit, gamma 2.2, levels per channel from the preview's whole frame
   (holder left out) when its brightness matches the RAW's, else from the RAW itself; or the levels kept for a roll.
   Settings (Tools > Negatives, saved): the positive on / off, contrast (film gamma 0.70 / 0.60 / 0.50), levels kept,
   rotation of the positive. The RAW itself is never changed. */
static int neg_pos_on = 1, neg_contrast = 1, neg_rot, neg_lock;   /* contrast 0 softer, 1 normal, 2 harder; rotation k x 90 clockwise */
static NcLevels neg_lock_lv;                                       /* the levels kept for a roll (neg_lock) */
static NcLevels pv_nlv; static int pv_nlv_frame;                   /* the levels of the shown preview (frame; 0 = none) */
static const double NEG_GAMMA[3] = { 0.70, 0.60, 0.50 };
static const wchar_t *NEG_CNAME[3] = { L"softer", L"normal", L"harder" };
static void neg_params(NcParams *p) { nc_defaults(p); p->gamma = NEG_GAMMA[neg_contrast < 0 || neg_contrast > 2 ? 1 : neg_contrast]; }
static void neg_fix_range(NcLevels *lv) { NcParams p; neg_params(&p); int g = lv->ch == 3 ? 1 : 0; lv->range = (lv->hi[g] - lv->lo[g]) / p.gamma; }
/* a preview sample (12-bit, as the driver wrote the PM file) in the RAW's 16-bit scale: the driver's 14 -> 12 bit table
   undone (as raw_read does), x 4 (hardware s39: RAW / preview 1.001 / 1.001 / 1.005); x 16 without the 14-bit mode */
static double pv_v16(int v) {
    double sl = raw_tab_slope, of = raw_tab_off;
    if (raw_inpol14 != 1 || !(sl > 0.01 && sl < 1 && of >= 0 && of < 4096)) return v * 16.0;
    double v14 = v < of ? v : of + (v - of + 0.5) / sl; if (v14 > 16383) v14 = 16383;
    return v14 * 4;
}
static uint16_t *pv_to16(void) {   /* the shown preview in the RAW's scale (malloc; PM order) */
    if (!pv_r12) return NULL;
    static uint16_t lut[4096]; for (int v = 0; v < 4096; v++) { double o = pv_v16(v); lut[v] = (uint16_t)(o > 65535 ? 65535 : o + 0.5); }
    size_t n = (size_t)pv_r12_w * pv_r12_h * 3; uint16_t *o = malloc(n * sizeof *o); if (!o) return NULL;
    for (size_t i = 0; i < n; i++) o[i] = lut[pv_r12[i] & 4095];
    return o;
}
/* the levels of the shown preview's frame (holder left out); 0 = ok */
static int pv_neg_levels(const uint16_t *p16, NcLevels *lv) {
    NcParams np; neg_params(&np); int x0, y0, x1, y1;
    if (nc_area(p16, pv_r12_w, pv_r12_h, 3, &x0, &y0, &x1, &y1)) return -1;
    return nc_levels(p16, pv_r12_w, pv_r12_h, 3, x0, y0, x1, y1, &np, lv);
}
static void pv_render(int first) {
    if (!pv_r12) return;
    int w = pv_r12_w, hgt = pv_r12_h; size_t ns = (size_t)w * hgt * 3;
    if (pv_cache) { DeleteObject(pv_cache); pv_cache = NULL; }
    free(pbits); pw = w; ph = hgt; int stride = (w * 3 + 3) & ~3; pbits = malloc((size_t)stride * hgt);
    if (!pbits) return;
    uint16_t *pos = NULL; int kept = 0;
    if (pv_negative) {
        uint16_t *p16 = pv_to16(); NcLevels lv; NcParams np; neg_params(&np); int ok = 0;
        if (p16) {
            if (neg_lock && neg_lock_lv.ok && neg_lock_lv.ch == 3) { lv = neg_lock_lv; neg_fix_range(&lv); ok = 1; kept = 1; }
            else ok = pv_neg_levels(p16, &lv) == 0;
            if (pv_neg_levels(p16, &pv_nlv) == 0) pv_nlv_frame = pv_r12_frame; else pv_nlv_frame = 0;
            if (ok && (pos = malloc(ns * sizeof *pos)) != NULL && nc_convert(p16, (size_t)w * hgt, 3, &lv, &np, pos)) { free(pos); pos = NULL; }
        }
        free(p16);
        if (first || !pos) { wchar_t m[400];
            if (pos) swprintf(m, 400, L"Negative: the preview is shown as a positive (%ls levels: density %.2f - %.2f red, %.2f - %.2f green, %.2f - %.2f blue; contrast %ls). RAWs of it also get a positive TIFF (Tools > Negatives).",
                              kept ? L"the kept" : L"this frame's", lv.lo[0], lv.hi[0], lv.lo[1], lv.hi[1], lv.lo[2], lv.hi[2], NEG_CNAME[neg_contrast]);
            else swprintf(m, 400, L"Negative: the preview could not be converted (no density range found - is it a negative?) - shown inverted.");
            log_w(m); }
    }
    if (pos) {
        for (int y = 0; y < hgt; y++) for (int x = 0; x < w; x++) {
            const uint16_t *s = pos + ((size_t)y * w + (pv_mirror ? w - 1 - x : x)) * 3; uint8_t *d = pbits + (size_t)y * stride + x * 3;
            d[0] = (uint8_t)(s[2] >> 8); d[1] = (uint8_t)(s[1] >> 8); d[2] = (uint8_t)(s[0] >> 8); }
        free(pos);
    } else if (pv_negative) {   /* not convertible: each channel inverted in density between its 0.5 and 99.5 percentiles (0.3.x) */
        static uint8_t nlut[3][4096];
        for (int c = 0; c < 3; c++) {
            static uint32_t hc[4096]; memset(hc, 0, sizeof hc); size_t np = (size_t)w * hgt;
            for (size_t i = 0; i < np; i++) hc[pv_r12[3 * i + c] & 4095]++;
            int lo = 0, hi = 4095; size_t acc = 0;
            for (int v = 0; v < 4096; v++) { acc += hc[v]; if (acc > np / 200) { lo = v; break; } }
            acc = 0; for (int v = 4095; v >= 0; v--) { acc += hc[v]; if (acc > np / 200) { hi = v; break; } }
            if (lo < 1) lo = 1; if (hi <= lo) hi = lo + 1;
            double dmax = log((double)hi / lo);
            for (int v = 0; v < 4096; v++) { double d = v <= lo ? dmax : v >= hi ? 0 : log((double)hi / v); nlut[c][v] = (uint8_t)(255.0 * pow(d / dmax, 1 / 1.6) + 0.5); }
        }
        for (int y = 0; y < hgt; y++) for (int x = 0; x < w; x++) {
            const uint16_t *s = pv_r12 + ((size_t)y * w + (pv_mirror ? w - 1 - x : x)) * 3; uint8_t *d = pbits + (size_t)y * stride + x * 3;
            d[0] = nlut[2][s[2] & 4095]; d[1] = nlut[1][s[1] & 4095]; d[2] = nlut[0][s[0] & 4095]; }
    } else {                    /* positive: white point at the 99.5th percentile of all samples, gamma 2.2 (display only) */
        static uint32_t hist[4096]; memset(hist, 0, sizeof hist); for (size_t i = 0; i < ns; i++) hist[pv_r12[i] & 4095]++;
        int full = 4095; { size_t acc = 0, lim = ns - ns / 200; for (int v = 0; v < 4096; v++) { acc += hist[v]; if (acc >= lim) { full = v; break; } } }
        if (full < 1) full = 1;
        static uint8_t lut[4096]; for (int i = 0; i < 4096; i++) lut[i] = i >= full ? 255 : (uint8_t)(255.0 * pow((double)i / full, 1 / 2.2) + 0.5);
        for (int y = 0; y < hgt; y++) for (int x = 0; x < w; x++) {
            const uint16_t *s = pv_r12 + ((size_t)y * w + (pv_mirror ? w - 1 - x : x)) * 3; uint8_t *d = pbits + (size_t)y * stride + x * 3;
            d[0] = lut[s[2] & 4095]; d[1] = lut[s[1] & 4095]; d[2] = lut[s[0] & 4095]; }
    }
    memset(&pbi, 0, sizeof pbi); pbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); pbi.bmiHeader.biWidth = w; pbi.bmiHeader.biHeight = -hgt;
    pbi.bmiHeader.biPlanes = 1; pbi.bmiHeader.biBitCount = 24; pbi.bmiHeader.biCompression = BI_RGB;
    InvalidateRect(hprev, NULL, TRUE);
}
static int neg_c_opt = -1, neg_rot_opt = -1, neg_keep_opt;   /* developer options (autotest) */
static void neg_check(void) {
    if (!neg_menu) return;
    CheckMenuItem(neg_menu, IDM_NEGPOS, MF_BYCOMMAND | (neg_pos_on ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(neg_menu, IDM_NEGC0, IDM_NEGC0 + 2, IDM_NEGC0 + neg_contrast, MF_BYCOMMAND);
    CheckMenuRadioItem(neg_menu, IDM_NEGAUTO, IDM_NEGLOCK, neg_lock ? IDM_NEGLOCK : IDM_NEGAUTO, MF_BYCOMMAND);
    CheckMenuRadioItem(neg_menu, IDM_NEGROT0, IDM_NEGROT0 + 3, IDM_NEGROT0 + neg_rot, MF_BYCOMMAND);
}
static void settings_save(void);
/* keep the shown preview's levels for the next frames (a roll scanned with the same levels) */
static void neg_keep(void) {
    if (!pv_negative || !pv_r12 || pv_nlv_frame != pv_r12_frame || !pv_nlv.ok) {
        neg_check(); const wchar_t *m = L"Keep the colour levels: first make a preview of a negative frame (Media: Negative) whose levels should be used for the roll - its levels are then kept.";
        log_w(m); if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); return; }
    neg_lock_lv = pv_nlv; neg_lock = 1; neg_check(); settings_save();
    wchar_t m[400]; swprintf(m, 400, L"Negatives: the colour levels of frame %d are kept for the next frames (density %.3f - %.3f red, %.3f - %.3f green, %.3f - %.3f blue). Saved; Colour levels: each frame its own goes back to automatic.",
                             pv_r12_frame, neg_lock_lv.lo[0], neg_lock_lv.hi[0], neg_lock_lv.lo[1], neg_lock_lv.hi[1], neg_lock_lv.lo[2], neg_lock_lv.hi[2]);
    log_w(m); pv_render(0);
}
/* frames of the current format (1 for single-frame masks; 4 for 4 T35mm H ...): each has its own preview file
   ScitexScanPreviewPM_<n> and B2F_PreviewParameters_<n> */
static int frames_total = 1, frames_done, frame_sel = 1, frames_auto_checked, frame_ready[64];
static int pv_invalid;   /* the scanner software was restarted for another holder after the last preview (cassette.c) */
static void load_preview_frame(int best) {
    wchar_t dir[MAX_PATH * 2]; swprintf(dir, MAX_PATH * 2, L"%ls\\Smart342(L) V2.2.0\\Smart342(L)_DataFolder!!!", voldir);
    if (best < 1) { log_w(L"Preview file not found."); return; }
    wchar_t path[MAX_PATH * 2]; swprintf(path, MAX_PATH * 2, L"%ls\\ScitexScanPreviewPM_%d", dir, best);
    FILE *f = _wfopen(path, L"rb"); if (!f) { log_w(L"Cannot open the preview file."); return; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    /* size from the driver's own record; it must account for the whole file */
    wchar_t rpath[MAX_PATH * 2 + 8]; swprintf(rpath, MAX_PATH * 2 + 8, L"%ls.rsrc", path);
    int lines = 0, pix = 0;
    if (pm_size(rpath, &lines, &pix) || lines <= 0 || pix <= 0 || (long)lines * pix * 6 != n) {
        fclose(f); wchar_t m[200]; swprintf(m, 200, L"Preview size unknown (%d x %d, file %ld bytes) - not shown.", pix, lines, n); log_w(m); return;
    }
    int w = pix, hgt = lines;
    pv_mm_rows = pv_mm_cols = 0;
    { wchar_t bp[MAX_PATH * 2]; swprintf(bp, MAX_PATH * 2, L"%ls\\B2F_PreviewParameters_%d", dir, best);
      FILE *bf = _wfopen(bp, L"rb"); uint8_t b[64];
      if (bf) { if (fread(b, 1, 56, bf) == 56) {
          uint32_t r = (uint32_t)b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23], c = (uint32_t)b[24] << 24 | b[25] << 16 | b[26] << 8 | b[27];
          float fr, fc; memcpy(&fr, &r, 4); memcpy(&fc, &c, 4);
          int nr = (int16_t)(b[52] << 8 | b[53]), nc = (int16_t)(b[54] << 8 | b[55]);
          if (nr == lines && nc == pix && fr > 1 && fr < 1000 && fc > 1 && fc < 1000) { pv_mm_rows = fr; pv_mm_cols = fc; }
        } fclose(bf); }
      if (pv_mm_rows > 0) { wchar_t m[200]; swprintf(m, 200, L"Preview covers %.2f mm (width) x %.2f mm (height). Drag on it to set the crop.", pv_mm_cols, pv_mm_rows); log_w(m); }
      else log_w(L"Preview size in mm unknown - drawing a crop on it is not available for this preview.");
    }
    uint8_t *raw = malloc((size_t)w * hgt * 6); size_t got = raw ? fread(raw, 1, (size_t)w * hgt * 6, f) : 0; fclose(f);
    if (got != (size_t)w * hgt * 6) { free(raw); log_w(L"Preview file is short."); return; }
    free(pv_r12); pv_r12 = malloc((size_t)w * hgt * 3 * sizeof(uint16_t)); pv_r12_w = pv_r12_h = 0; pv_r12_frame = 0;
    if (!pv_r12) { free(raw); log_w(L"Not enough memory to show the preview."); return; }
    int maxv = 1;
    for (size_t i = 0; i < (size_t)w * hgt * 3; i++) { int v = raw[2 * i] << 8 | raw[2 * i + 1]; if (v > maxv) maxv = v; pv_r12[i] = (uint16_t)(v > 4095 ? 4095 : v); }   /* 12-bit; clipped, never wrapped (review) */
    pv_r12_w = w; pv_r12_h = hgt; pv_r12_frame = best; free(raw);
    pv_mirror = (pv_mount == 0);
    pv_render(1);
    wchar_t m[200]; swprintf(m, 200, L"Preview loaded: ScitexScanPreviewPM_%d, %d pixels x %d scan lines, max value %d.", best, w, hgt, maxv); log_d(m);
}
static void load_preview(void) { load_preview_frame(frame_sel); }
/* where the preview is drawn in the window: the picture keeps its true proportions (mm) */
static int prev_geom(HWND h, int *ox, int *oy, int *dw, int *dh) {
    RECT rc; GetClientRect(h, &rc); if (!pbits) return 0;
    double aw = pv_mm_cols > 0 ? pv_mm_cols : pw, ah = pv_mm_rows > 0 ? pv_mm_rows : ph;
    double s = fmin((double)rc.right / aw, (double)rc.bottom / ah);
    *dw = (int)(aw * s); *dh = (int)(ah * s); *ox = (rc.right - *dw) / 2; *oy = (rc.bottom - *dh) / 2;
    return *dw > 0 && *dh > 0;
}
static double edit_num(int id) { wchar_t w[64]; GetWindowTextW(GetDlgItem(hwnd, id), w, 64); for (wchar_t *c = w; *c; c++) if (*c == L',') *c = L'.'; return _wtof(w); }
static void set_edit_mm(int id, double v) { wchar_t w[32]; swprintf(w, 32, L"%.2f", v); SetWindowTextW(GetDlgItem(hwnd, id), w); }
static LRESULT CALLBACK prev_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    int ox, oy, dw, dh;
    if (msg == WM_ERASEBKGND) return 1;          /* everything is painted in WM_PAINT (no flicker) */
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps; HDC wdc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        /* draw into an off-screen bitmap and copy it to the window in one step */
        HDC dc = CreateCompatibleDC(wdc); HBITMAP back = CreateCompatibleBitmap(wdc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
        HGDIOBJ oldb = SelectObject(dc, back);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(DKGRAY_BRUSH));
        if (pbits && prev_geom(h, &ox, &oy, &dw, &dh)) {
            if (!pv_cache || pv_cache_w != dw || pv_cache_h != dh) {   /* the slow, high-quality scaling only when the size changes */
                if (pv_cache) DeleteObject(pv_cache);
                pv_cache = CreateCompatibleBitmap(wdc, dw, dh); pv_cache_w = dw; pv_cache_h = dh;
                HDC cdc = CreateCompatibleDC(wdc); HGDIOBJ oc = SelectObject(cdc, pv_cache);
                SetStretchBltMode(cdc, HALFTONE); SetBrushOrgEx(cdc, 0, 0, NULL);
                StretchDIBits(cdc, 0, 0, dw, dh, 0, 0, pw, ph, pbits, &pbi, DIB_RGB_COLORS, SRCCOPY);
                SelectObject(cdc, oc); DeleteDC(cdc);
            }
            HDC cdc = CreateCompatibleDC(wdc); HGDIOBJ oc = SelectObject(cdc, pv_cache);
            BitBlt(dc, ox, oy, dw, dh, cdc, 0, 0, SRCCOPY);
            SelectObject(cdc, oc); DeleteDC(cdc);
            RECT r; int show = 0;
            if (drag_on) { r.left = min(drag_a.x, drag_b.x); r.right = max(drag_a.x, drag_b.x); r.top = min(drag_a.y, drag_b.y); r.bottom = max(drag_a.y, drag_b.y); show = 1; }
            else if (pv_mm_rows > 0 && SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED) {
                double x = edit_num(IDC_CX), y = edit_num(IDC_CY), cw = edit_num(IDC_CW), chh = edit_num(IDC_CH);
                r.left = ox + (int)(x / pv_mm_cols * dw + 0.5); r.right = ox + (int)((x + cw) / pv_mm_cols * dw + 0.5);
                r.top = oy + (int)(y / pv_mm_rows * dh + 0.5); r.bottom = oy + (int)((y + chh) / pv_mm_rows * dh + 0.5);
                show = cw > 0 && chh > 0;
            }
            if (pv_mm_rows > 0 && nbq) {           /* the scan list's crops, numbered */
                HPEN lp2 = CreatePen(PS_DOT, 1, RGB(0, 220, 255)); HGDIOBJ op2 = SelectObject(dc, lp2), ob2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(0, 220, 255)); SelectObject(dc, font);
                for (int i = 0; i < nbq; i++) {
                    if (bq[i].frame != frame_sel) continue;
                    RECT q = { ox + (int)(bq[i].x / pv_mm_cols * dw + 0.5), oy + (int)(bq[i].y / pv_mm_rows * dh + 0.5),
                               ox + (int)((bq[i].x + bq[i].w) / pv_mm_cols * dw + 0.5), oy + (int)((bq[i].y + bq[i].h) / pv_mm_rows * dh + 0.5) };
                    Rectangle(dc, q.left, q.top, q.right + 1, q.bottom + 1);
                    wchar_t t[8]; swprintf(t, 8, L"%d", i + 1); TextOutW(dc, q.left + 3, q.top + 2, t, (int)wcslen(t));
                }
                SelectObject(dc, op2); SelectObject(dc, ob2); DeleteObject(lp2);
            }
            if (show) {
                HPEN pen = CreatePen(PS_SOLID, S(2), RGB(255, 210, 0)); HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, r.left, r.top, r.right + 1, r.bottom + 1);
                SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
            }
        } else if (!pbits) {
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(220, 220, 220)); SelectObject(dc, font);
            DrawTextW(dc, L"Preview appears here", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldb); DeleteObject(back); DeleteDC(dc);
        EndPaint(h, &ps); return 0;
    }
    if (msg == WM_LBUTTONDOWN && pbits && pv_mm_rows > 0 && prev_geom(h, &ox, &oy, &dw, &dh)) {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (p.x < ox || p.y < oy || p.x > ox + dw || p.y > oy + dh) return 0;
        drag_on = 1; drag_a = drag_b = p; SetCapture(h); return 0;
    }
    if (msg == WM_MOUSEMOVE && drag_on && prev_geom(h, &ox, &oy, &dw, &dh)) {
        POINT p = { (short)LOWORD(lp), (short)HIWORD(lp) };
        p.x = max(ox, min(ox + dw, p.x)); p.y = max(oy, min(oy + dh, p.y));
        drag_b = p; InvalidateRect(h, NULL, FALSE); return 0;
    }
    if (msg == WM_LBUTTONUP && drag_on) {
        drag_on = 0; ReleaseCapture();
        if (prev_geom(h, &ox, &oy, &dw, &dh) && abs(drag_b.x - drag_a.x) >= 3 && abs(drag_b.y - drag_a.y) >= 3) {
            int x0 = min(drag_a.x, drag_b.x) - ox, x1 = max(drag_a.x, drag_b.x) - ox, y0 = min(drag_a.y, drag_b.y) - oy, y1 = max(drag_a.y, drag_b.y) - oy;
            double mx = pv_mm_cols / dw, my = pv_mm_rows / dh;
            set_edit_mm(IDC_CX, x0 * mx); set_edit_mm(IDC_CY, y0 * my); set_edit_mm(IDC_CW, (x1 - x0) * mx); set_edit_mm(IDC_CH, (y1 - y0) * my);
            SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
        }
        InvalidateRect(h, NULL, FALSE); return 0;
    }
    if (msg == WM_SIZE) { InvalidateRect(h, NULL, FALSE); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------------ the program's own dialogs */
static int dlg_result; static wchar_t dlg_edit[64];
static int rawj_running;   /* a RAW scan (one or more Max Details) is in progress */
static LRESULT CALLBACK dlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED && LOWORD(wp) >= 1000) {
        HWND e = GetDlgItem(h, 999); if (e) GetWindowTextW(e, dlg_edit, 64);   /* 0.3.0: ask_box's input field */
        dlg_result = LOWORD(wp) - 1000; DestroyWindow(h); return 0; }
    if (msg == WM_CLOSE) return 0;     /* an answer is required */
    return DefWindowProcW(h, msg, wp, lp);
}
static int auto_answer(DlgReq *r) {
    int first = -1;
    for (int i = 0; i < r->n; i++) if (!strcmp(r->d[i].kind, "button")) {
        if (first < 0) first = r->d[i].k;
        char t[64]; int n = r->d[i].ntext < 63 ? r->d[i].ntext : 63; memcpy(t, r->d[i].text, n); t[n] = 0;
        if (!_stricmp(t, "OK") || !_stricmp(t, "Yes") || !_stricmp(t, "Continue")) return r->d[i].k;
    }
    return first > 0 ? first : 1;
}
static void show_dialog(DlgReq *r) {
    /* text: the static and edit items as the program set them; buttons: its button items */
    static wchar_t text[16384]; text[0] = 0; int nb = 0;
    for (int i = 0; i < r->n; i++) {
        const DlgDesc *d = &r->d[i];
        if (!strcmp(d->kind, "button")) { nb++; continue; }
        if (!strcmp(d->kind, "control")) continue;
        char u8[8000]; macroman_to_utf8(d->text, d->ntext, u8, sizeof u8);
        wchar_t w[8000]; to_w(u8, w, 8000);
        for (wchar_t *p = w; *p; p++) if (*p == L'\r') *p = L'\n';
        size_t L = wcslen(text);
        if (L && L < 16000) wcsncat(text, L"\n\n", 16383 - L);
        wcsncat(text, w, 16383 - wcslen(text));
    }
    /* convert \n to \r\n for the static control */
    static wchar_t t2[33000]; int k = 0; for (wchar_t *p = text; *p && k < 32990; p++) { if (*p == L'\n') t2[k++] = L'\r'; t2[k++] = *p; } t2[k] = 0;
    { char u8[33000]; to_utf8(t2, u8, sizeof u8); char line[34000]; snprintf(line, sizeof line, "Dialog: %s", u8); for (char *p = line; *p; p++) if (*p == '\r' || *p == '\n') *p = ' '; log8d(line); }
    /* 0.2.0: the original's start-up notice when its preferences are new (a fresh folder: "The Scratch Folder will be
       created in the folder 'Macintosh HD' on the disk 'Macintosh HD'.", alert 6015 with Cancel / OK - every first start of
       a new version on the scanner, s32 / s35) is answered OK without showing it: OK is what was always answered, and the
       folder is the program's own work folder */
    if (wcsstr(text, L"B01018")) log_w(L"The scanner software stopped: the scanner's CCD did not answer (CCD timeout). Switch the scanner off, wait 10 seconds, switch it on, then start this program again. (On 12 Oct this followed a Stop in the middle of a picture; since 0.4.0 Stop waits for the picture.)");
    if (wcsstr(text, L"The Scratch Folder will be created")) {
        for (int i = 0; i < r->n; i++) { const DlgDesc *dd = &r->d[i];
            if (!strcmp(dd->kind, "button") && dd->ntext == 2 && !memcmp(dd->text, "OK", 2)) {
                r->result = dd->k; wchar_t lm[160]; swprintf(lm, 160, L"  answered OK (item %d) automatically: the work folder notice at a first start.", dd->k); log_d(lm); return; } }
    }
    /* 0.2.0: once the scanner no longer answers, the scanner software's alerts are its reactions to that ("Motion or
       door Problem ! Emergency stop.", "Motion Problem (motor 0)" ... - s36); the window has already said what
       happened, so they are answered without showing them (OK, or the first button) */
    /* 0.3.0: the scanner-number alert (7012) - this program offers the copy itself when the software has stopped */
    if (serial_mm && (wcsstr(text, L"Incompatibility between scanner number") || wcsstr(text, L"B2F_InitScanParameters"))) { r->result = auto_answer(r); wchar_t lm[160]; swprintf(lm, 160, L"  answered item %d automatically (the program offers the parameter copy next).", r->result); log_d(lm); return; }
    if (SCANNER_DEAD) { r->result = auto_answer(r); wchar_t lm[160]; swprintf(lm, 160, L"  answered item %d automatically (not shown: the scanner no longer answers).", r->result); log_d(lm); return; }
    /* RAW: every pass is a new Max Detail of the same crop, and the original asks each time whether the previous
       one may be replaced ("A Max detail for crop ... already exists ... delete the previous one from the Preview
       Browser?" - seen on the scanner, 2026-10-08: asked before every pass and answered OK by hand). While a RAW
       scan runs, OK (= make the new one) is the only answer that lets it go on, so it is given here. */
    if (job_ui == 'c' && rawj_running && wcsstr(text, L"A Max detail for crop")) {
        for (int i = 0; i < r->n; i++) {
            const DlgDesc *dd = &r->d[i]; if (strcmp(dd->kind, "button")) continue;
            if (dd->ntext == 2 && !memcmp(dd->text, "OK", 2)) {
                r->result = dd->k; wchar_t lm[160]; swprintf(lm, 160, L"  answered OK (item %d) automatically: RAW scans replace the previous Max Detail.", dd->k); log_d(lm); return; }
        }
    }
    int W = S(460), bh = S(28), H = 0;
    HDC dc = GetDC(hwnd); SelectObject(dc, font); RECT rc = { 0, 0, W - S(40), 0 }; DrawTextW(dc, t2, -1, &rc, DT_CALCRECT | DT_WORDBREAK); ReleaseDC(hwnd, dc);
    int th = rc.bottom + S(10); if (th > S(500)) th = S(500);
    H = S(20) + th + S(20) + bh + S(50);
    RECT pr; GetWindowRect(hwnd, &pr);
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"S342Dialog", APP_TITLE, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                             pr.left + S(80), pr.top + S(80), W, H, hwnd, NULL, GetModuleHandleW(NULL), NULL);
    HWND st = CreateWindowExW(0, L"STATIC", t2, WS_CHILD | WS_VISIBLE, S(20), S(20), W - S(40), th, d, NULL, NULL, NULL); SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);
    int x = W - S(20), firstbtn = 0;
    for (int i = r->n - 1; i >= 0; i--) {
        const DlgDesc *dd = &r->d[i]; if (strcmp(dd->kind, "button")) continue;
        char u8[600]; macroman_to_utf8(dd->text, dd->ntext, u8, sizeof u8); wchar_t w[600]; to_w(u8, w, 600);
        int bw = S(90); x -= bw;
        HWND b = CreateWindowExW(0, L"BUTTON", w, WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, S(20) + th + S(20), bw, bh, d, (HMENU)(INT_PTR)(1000 + dd->k), NULL, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE); x -= S(10); firstbtn = dd->k;
    }
    if (!nb) {
        HWND b = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE, W - S(110), S(20) + th + S(20), S(90), bh, d, (HMENU)(INT_PTR)(1000 + 1), NULL, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
    }
    (void)firstbtn;
    EnableWindow(hwnd, FALSE);
    dlg_result = -1; MSG m;
    if (autotest) { MSG mm; DWORD t0 = GetTickCount(); while (GetTickCount() - t0 < 1500) { while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&mm); DispatchMessageW(&mm); } Sleep(20); }
                    dlg_result = auto_answer(r); DestroyWindow(d); }
    while (dlg_result < 0 && GetMessageW(&m, NULL, 0, 0) > 0) { if (!IsDialogMessageW(d, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    EnableWindow(hwnd, TRUE); SetForegroundWindow(hwnd);
    r->result = dlg_result < 0 ? 1 : dlg_result;
    wchar_t lm[100]; swprintf(lm, 100, L"  answered item %d", r->result); log_d(lm);
}

static void ini_path(wchar_t *p);
static double wz_get(int lens, const wchar_t *key, double dflt);
/* 0.3.0: a question of this program with its own buttons (one per line when vertical), optionally an input field
   (its text in dlg_edit); returns the button's index. In autotest: auto_pick (and auto_text for the field). */
static int ask_box(const wchar_t *text, const wchar_t *const *btn, int nb, int vertical, int with_edit, int auto_pick, const wchar_t *auto_text) {
    wchar_t t2[8000]; int k = 0; for (const wchar_t *q = text; *q && k < 7990; q++) { if (*q == L'\n') t2[k++] = L'\r'; t2[k++] = *q; } t2[k] = 0;
    { char u8[8000]; to_utf8(text, u8, sizeof u8); char line[8200]; snprintf(line, sizeof line, "Question: %s", u8); for (char *c = line; *c; c++) if (*c == '\r' || *c == '\n') *c = ' '; log8d(line); }
    int W = S(vertical ? 620 : 520), bh = S(28);
    HDC dc = GetDC(hwnd); SelectObject(dc, font); RECT rc = { 0, 0, W - S(40), 0 }; DrawTextW(dc, t2, -1, &rc, DT_CALCRECT | DT_WORDBREAK); ReleaseDC(hwnd, dc);
    int th = rc.bottom + S(10); if (th > S(560)) th = S(560);
    int eh = with_edit ? S(26) + S(12) : 0;
    int H = S(20) + th + eh + S(16) + (vertical ? nb * (bh + S(8)) : bh) + S(50);
    RECT pr; GetWindowRect(hwnd, &pr);
    HWND d = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"S342Dialog", APP_TITLE, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                             pr.left + S(60), pr.top + S(60), W, H, hwnd, NULL, GetModuleHandleW(NULL), NULL);
    HWND st = CreateWindowExW(0, L"STATIC", t2, WS_CHILD | WS_VISIBLE, S(20), S(20), W - S(40), th, d, NULL, NULL, NULL); SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);
    int y = S(20) + th + S(6);
    if (with_edit) { HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, S(20), y, S(120), S(26), d, (HMENU)(INT_PTR)999, NULL, NULL);
                     SendMessageW(e, WM_SETFONT, (WPARAM)font, TRUE); SetFocus(e); y += eh; }
    y += S(10);
    if (vertical) for (int i = 0; i < nb; i++) {
        HWND b = CreateWindowExW(0, L"BUTTON", btn[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_LEFT, S(20), y, W - S(40), bh, d, (HMENU)(INT_PTR)(1000 + i), NULL, NULL);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE); y += bh + S(8); }
    else { int x = W - S(20);
        for (int i = nb - 1; i >= 0; i--) { HDC c2 = GetDC(d); SelectObject(c2, font); SIZE sz; GetTextExtentPoint32W(c2, btn[i], (int)wcslen(btn[i]), &sz); ReleaseDC(d, c2);
            int bw = max(S(90), sz.cx + S(24)); x -= bw;
            HWND b = CreateWindowExW(0, L"BUTTON", btn[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, bw, bh, d, (HMENU)(INT_PTR)(1000 + i), NULL, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE); x -= S(10); } }
    BOOL was = IsWindowEnabled(hwnd); EnableWindow(hwnd, FALSE);
    dlg_result = -1; dlg_edit[0] = 0; MSG m;
    if (autotest) { DWORD t0 = GetTickCount(); while (GetTickCount() - t0 < 1200) { while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(20); }
                    if (with_edit && auto_text) SetWindowTextW(GetDlgItem(d, 999), auto_text);
                    SendMessageW(d, WM_COMMAND, MAKEWPARAM(1000 + (auto_pick >= 0 && auto_pick < nb ? auto_pick : 0), BN_CLICKED), 0); }
    while (dlg_result < 0 && GetMessageW(&m, NULL, 0, 0) > 0) { if (!IsDialogMessageW(d, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    EnableWindow(hwnd, was); SetForegroundWindow(hwnd);
    { wchar_t lm[300]; swprintf(lm, 300, L"  answer: %ls%ls%ls", dlg_result >= 0 && dlg_result < nb ? btn[dlg_result] : L"?", with_edit ? L" / typed: " : L"", with_edit ? dlg_edit : L""); log_d(lm); }
    return dlg_result;
}

/* 0.3.0: start this program again with the same command line plus extra (after the parameter copy, or to switch to
   the simulator); this process then ends */
static int relaunch_strip_tests;   /* 0.3.1: Fix streaks restarts without the test options that led to it (no loop in tests) */
static int relaunch(const wchar_t *extra) {
    static wchar_t cl[8192]; int argc = 0, k = 0; wchar_t **av = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!av) return 0;
    for (int i = 0; i < argc && k < 7000; i++) {   /* review: an earlier copy's arguments are not carried on */
        if (relaunch_strip_tests && (!wcscmp(av[i], L"--fix-streaks") || !wcscmp(av[i], L"--setup-chart"))) continue;
        if (relaunch_strip_tests && !wcscmp(av[i], L"--setup-test") && i + 1 < argc) { i++; continue; }
        if ((!wcscmp(av[i], L"--prefs-from-flash") || !wcscmp(av[i], L"--lw-butting") || !wcscmp(av[i], L"--clean-gain")) && i + 1 < argc) { i++; continue; }
        k += swprintf(cl + k, 8192 - k, L"%ls\"%ls\"", k ? L" " : L"", av[i]); }
    LocalFree(av);
    swprintf(cl + k, 8192 - k, L" %ls", extra ? extra : L"");
    STARTUPINFOW si = { 0 }; si.cb = sizeof si; PROCESS_INFORMATION pi;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!CreateProcessW(exe, cl, NULL, NULL, FALSE, 0, NULL, exedir, &si, &pi)) return 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 1;
}

/* ---- 0.3.0: finding the scanner on the SCSI buses ----
   1. the place saved in smart342.ini [scanner] (scsi_port / scsi_bus / scsi_id) when the scanner still answers there;
   2. Windows' own list of SCSI devices (no command is sent to them); if no Scitex scanner is in it,
   3. an INQUIRY at every other ID of every bus (a scanner switched on after Windows started is not in the list).
   One Scitex scanner: used. Several: the user chooses. None: the devices found are shown, with "Search again",
   "Type the SCSI ID", "Use the simulator" and "Quit". The place used is saved. */
static int scsi_choose_opt = -1; static wchar_t scsi_type_opt[16];   /* developer options (autotest): --scsi-choose K, --scsi-type-id N */
static int scsi_none_opt = -1;                                        /* developer option (autotest): the answer in the "not found" question */
static int scsi_port = -1, scsi_bus, scsi_id; static char scsi_name[64];
static void scsi_save(int port, int bus, int id) {
    wchar_t p[MAX_PATH * 2], v[16]; ini_path(p);
    swprintf(v, 16, L"%d", port); WritePrivateProfileStringW(L"scanner", L"scsi_port", v, p);
    swprintf(v, 16, L"%d", bus); WritePrivateProfileStringW(L"scanner", L"scsi_bus", v, p);
    swprintf(v, 16, L"%d", id); WritePrivateProfileStringW(L"scanner", L"scsi_id", v, p);
}
static void dev_line(const SptiDev *d, wchar_t *w, int n) {
    wchar_t v[16], pr[24], r[8]; to_w(d->vendor, v, 16); to_w(d->product, pr, 24); to_w(d->rev, r, 8);
    swprintf(w, n, L"port %d, bus %d, ID %d: %ls %ls %ls%ls", d->port, d->path, d->target, v[0] ? v : L"(no name)", pr, r, d->listed ? L"" : L" (answered the search)");
}
/* returns the scanner, or NULL with *use_sim = 1 (simulator wanted / test list), -1 (Quit chosen) or 0 (none) */
static ScsiTarget *find_scanner(int *use_sim) {
    char msg[400]; wchar_t p[MAX_PATH * 2], w[600]; ini_path(p); *use_sim = 0;
    int sp = GetPrivateProfileIntW(L"scanner", L"scsi_port", -1, p), sb = GetPrivateProfileIntW(L"scanner", L"scsi_bus", 0, p), si = GetPrivateProfileIntW(L"scanner", L"scsi_id", -1, p);
    if (sp >= 0 && si >= 0 && !spti_fake) {
        status(L"Looking for the scanner where it was last time...");
        ScsiTarget *t = spti_open_at(sp, sb, si, NULL, btrace8, msg, sizeof msg); log8(msg);
        if (t) { scsi_port = sp; scsi_bus = sb; scsi_id = si; return t; }
        log_w(L"The scanner did not answer at the SCSI place used last time - searching every SCSI ID.");
    }
    static wchar_t notans[600]; notans[0] = 0;   /* review: a listed scanner that did not answer - said in the "not found" question */
    for (int round = 0; ; round++) {
        static SptiDev dv[128]; int n, ns = 0, sc[16];
        status(L"Looking for the scanner in Windows' list of SCSI devices...");
        n = spti_scan(dv, 128, 0);
        for (int i = 0; i < n; i++) if (dv[i].scitex && ns < 16) sc[ns++] = i;
        if (!ns) {
            status(L"Asking every SCSI ID (a few seconds)...");
            n = spti_scan(dv, 128, 1); ns = 0;
            for (int i = 0; i < n; i++) if (dv[i].scitex && ns < 16) sc[ns++] = i;
        }
        { swprintf(w, 600, L"SCSI search: %d device%ls found, %d Scitex scanner%ls.", n, n == 1 ? L"" : L"s", ns, ns == 1 ? L"" : L"s"); log_d(w); }
        for (int i = 0; i < n; i++) { wchar_t l[200]; dev_line(&dv[i], l, 200); swprintf(w, 600, L"  %ls%ls", l, dv[i].claimed ? L" (in use by a Windows driver)" : L""); log_d(w); }
        int pick = -1;
        if (ns == 1 && !notans[0]) pick = sc[0];
        else if (ns > 1 && !notans[0]) {
            static wchar_t lab[17][200]; const wchar_t *b[17];
            for (int i = 0; i < ns; i++) { wchar_t l[180]; dev_line(&dv[sc[i]], l, 180); swprintf(lab[i], 200, L"Use the scanner at %ls", l); b[i] = lab[i]; }
            b[ns] = L"Quit";
            int a = ask_box(L"More than one Scitex scanner is connected. Which one should this program use? (The choice is saved; Tools > SCSI connection searches again.)", b, ns + 1, 1, 0, scsi_choose_opt >= 0 ? scsi_choose_opt : 0, NULL);
            if (a < 0 || a >= ns) { *use_sim = -1; return NULL; }
            pick = sc[a];
        } else {
            static wchar_t txt[6000]; int k = 0;
            if (notans[0]) k += swprintf(txt + k, 6000 - k, L"%ls\n\n", notans);
            else k += swprintf(txt + k, 6000 - k, L"No Scitex scanner was found on the SCSI buses.\n\n");
            k += swprintf(txt + k, 6000 - k, L"Check: the scanner is switched on and its light is steady green; the SCSI cable and terminator are in place; this program was started as Administrator (right-click > Run as administrator).\n\n");
            if (n) { k += swprintf(txt + k, 6000 - k, L"SCSI devices found:\n"); for (int i = 0; i < n && k < 5600; i++) { wchar_t l[200]; dev_line(&dv[i], l, 200); k += swprintf(txt + k, 6000 - k, L"  %ls\n", l); } }
            else k += swprintf(txt + k, 6000 - k, L"No SCSI device at all was found - is the SCSI card installed, and is the program running as Administrator?\n");
            k += swprintf(txt + k, 6000 - k, L"\nThe scanner's SCSI ID is set on its front panel. You can also type it.");
            const wchar_t *b[4] = { L"Search again", L"Type the SCSI ID...", L"Use the simulator", L"Quit" };
            int a = ask_box(txt, b, 4, 0, 0, scsi_none_opt >= 0 ? scsi_none_opt : 3, NULL);
            if (a == 0) { notans[0] = 0; continue; }
            if (a == 2) { *use_sim = 1; return NULL; }
            if (a != 1) { *use_sim = -1; return NULL; }
            const wchar_t *b2[2] = { L"Try this ID", L"Cancel" };
            int a2 = ask_box(L"The scanner's SCSI ID (0 - 15; shown on its front panel):", b2, 2, 0, 1, 0, scsi_type_opt[0] ? scsi_type_opt : L"4");
            if (a2 != 0) continue;
            int id = _wtoi(dlg_edit); if (id < 0 || id > 15 || !dlg_edit[0]) { MessageBoxW(hwnd, L"Type a number from 0 to 15.", APP_TITLE, MB_ICONINFORMATION); continue; }
            if (spti_fake) { swprintf(w, 600, L"SCSI ID %d typed (test list: the simulator stands in for the scanner).", id); log_w(w); *use_sim = 1; return NULL; }
            ScsiTarget *t = spti_open(id, NULL, btrace8, msg, sizeof msg); log8(msg);
            if (t) {
                int pp = -1, pb = 0; sscanf(msg, "Scanner found on \\\\.\\Scsi%d: bus %d", &pp, &pb);
                if (pp >= 0) { scsi_save(pp, pb, id); scsi_port = pp; scsi_bus = pb; scsi_id = id; }
                return t; }
            { wchar_t mw[500]; to_w(msg, mw, 500); MessageBoxW(hwnd, mw, APP_TITLE, MB_ICONWARNING); }
            continue;
        }
        const SptiDev *d = &dv[pick];
        dev_line(d, w, 600); { wchar_t m2[700]; swprintf(m2, 700, L"Scanner: %ls.", w); log_w(m2); }
        snprintf(scsi_name, sizeof scsi_name, "%s %s %s", d->vendor, d->product, d->rev);
        if (spti_fake) { scsi_save(d->port, d->path, d->target); scsi_port = d->port; scsi_bus = d->path; scsi_id = d->target;
                         log_w(L"(test list of SCSI devices: the place is saved; the simulator stands in for the scanner)"); *use_sim = 1; return NULL; }
        ScsiTarget *t = spti_open_at(d->port, d->path, d->target, NULL, btrace8, msg, sizeof msg); log8(msg);
        if (t) { scsi_save(d->port, d->path, d->target); scsi_port = d->port; scsi_bus = d->path; scsi_id = d->target; return t; }
        /* review: listed but not answering (switched off, or still starting up): the "not found" question, saying so */
        { wchar_t mw[500]; to_w(msg, mw, 500); swprintf(notans, 600, L"The scanner Windows lists at %ls does not answer: %ls", w, mw); }
        if (round > 50) return NULL;
    }
}
static void scsi_menu(void) {
    wchar_t m[600];
    if (simulate && scsi_port < 0) swprintf(m, 600, L"The simulated scanner is in use (no SCSI).\n\nAt the next start without the simulator the program uses the SCSI place saved before, or searches every SCSI ID.");
    else { wchar_t nm[80]; to_w(scsi_name, nm, 80); swprintf(m, 600, L"The scanner is used at SCSI port %d, bus %d, ID %d%ls%ls%ls.\n\nSearch every SCSI ID again at the next start (for example after changing the scanner's SCSI ID or the SCSI card)?", scsi_port, scsi_bus, scsi_id, nm[0] ? L" (" : L"", nm, nm[0] ? L")" : L""); }
    const wchar_t *b[2] = { L"Search again at the next start", L"Keep it" };
    if (ask_box(m, b, 2, 0, 0, 1, NULL) == 0) {
        wchar_t p[MAX_PATH * 2]; ini_path(p);
        WritePrivateProfileStringW(L"scanner", L"scsi_port", NULL, p); WritePrivateProfileStringW(L"scanner", L"scsi_bus", NULL, p); WritePrivateProfileStringW(L"scanner", L"scsi_id", NULL, p);
        log_w(L"SCSI place forgotten: the next start searches every SCSI ID.");
    }
}

/* ---- 0.3.0: the scanner's own parameters into Smart342.Prefs (Scanner(Flash Disk) -> Mac, read-only) ----
   prefs_offer: the scanner software stopped because Smart342.Prefs belongs to another scanner; the parameter file it
   read from the scanner (scsi_flash_cap) is checked, saved in the session's Logs folder, and - when the user agrees -
   the program starts again with --prefs-from-flash, which makes the copy (prefs_apply) before the scanner software
   starts. The copy is the original Calibration program's (prefscopy.c); the previous file is kept as Smart342.Old. */
static wchar_t prefs_from[MAX_PATH * 2]; static int prefs_lw_opt = -1, prefs_copied_now;
static int prefs_auto = -1;   /* developer option (autotest): --prefs-answer 0 copy / 1 quit */
static void vol_path(wchar_t *o, const wchar_t *rel) { swprintf(o, MAX_PATH * 2, L"%ls\\%ls", voldir, rel); }
static uint8_t *slurp_w(const wchar_t *p, size_t *n) {
    FILE *f = _wfopen(p, L"rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l > 0 ? l + 1 : 1); if (l > 0 && fread(d, 1, l, f) != (size_t)l) { free(d); fclose(f); return NULL; } fclose(f); *n = (size_t)(l > 0 ? l : 0); d[*n] = 0; return d;
}
static int write_w(const wchar_t *p, const void *d, size_t n) {   /* via a temporary file, then replaced */
    wchar_t t[MAX_PATH * 2 + 8]; swprintf(t, MAX_PATH * 2 + 8, L"%ls.new", p);
    FILE *f = _wfopen(t, L"wb"); if (!f) return 0; int ok = fwrite(d, 1, n, f) == n; ok = fclose(f) == 0 && ok;
    if (!ok) { DeleteFileW(t); return 0; }
    return MoveFileExW(t, p, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static void prefs_offer(void) {
    wchar_t m[1600]; char fs[32] = "", ps[32] = "", why[300] = "";
    int ok = scsi_flash_cap && flash_file_check(scsi_flash_cap, scsi_nflash_cap, why, sizeof why);
    if (ok) prefs_serial(scsi_flash_cap + 16, scsi_nflash_cap - 16, fs, 32);
    { wchar_t pp[MAX_PATH * 2]; vol_path(pp, L"Smart342(L) V2.2.0\\Smart342.Prefs.rsrc"); size_t n; uint8_t *d = slurp_w(pp, &n); if (d) { prefs_serial(d, n, ps, 32); free(d); } }
    if (!fs[0]) snprintf(fs, 32, "%u", (unsigned)serial_flash_no);
    if (!ps[0]) snprintf(ps, 32, "%u", (unsigned)serial_prefs_no);
    wchar_t wf[32], wp_[32]; to_w(fs, wf, 32); to_w(ps, wp_, 32);
    set_busy(1);
    if (!ok || prefs_copied_now || prefs_from[0] || serial_lw < 0) {   /* review: a start that already tried the copy does not offer it again */
        wchar_t ww[300]; to_w(why[0] ? why : (prefs_copied_now || prefs_from[0]) ? "the copy was tried at this start and the numbers still differ (see the log)" : serial_lw < 0 ? "the scanner's line-work configuration was not read" : "the scanner's parameter file was not read", ww, 300);
        swprintf(m, 1600, L"This program's scanner settings (Smart342.Prefs) belong to scanner %ls, but the scanner connected is number %ls.\n\nThe settings cannot be copied from the scanner now: %ls.\n\nSwitch the scanner off and on, start the program again, and if this message comes again send the Logs folder.", wp_, wf, ww);
        log_w(m); status(L"Stopped: the scanner settings belong to another scanner - see the log.");
        if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); else MessageBoxW(hwnd, m, APP_TITLE, MB_ICONWARNING);
        return;
    }
    swprintf(m, 1600, L"This program's scanner settings (Smart342.Prefs) belong to scanner %ls, but the scanner connected is number %ls. The scanner software does not run with another scanner's settings.\n\n"
             L"The program can copy THIS scanner's own parameters from its flash disk into the settings - the same copy as \"Scanner(Flash Disk) --> Mac\" in the original Calibration program. "
             L"The parameters have just been read from the scanner by the scanner software itself; nothing is written to the scanner. The current settings are kept as Smart342.Old.\n\n"
             L"After the copy the program starts again.", wp_, wf);
    log_w(L"The scanner software stopped: Smart342.Prefs belongs to another scanner.");
    const wchar_t *b[2] = { L"Copy this scanner's parameters and start again", L"Quit" };
    int a = ask_box(m, b, 2, 1, 0, prefs_auto >= 0 ? prefs_auto : 0, NULL);
    if (a != 0) { log_w(L"Parameter copy declined."); status(L"Ended. Close the window."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    wchar_t fp[MAX_PATH * 2], wn[40]; swprintf(wn, 40, L"%ls", wf); for (wchar_t *c = wn; *c; c++) if (*c == L'/' || *c == L'\\' || *c == L':') *c = L'-';
    swprintf(fp, MAX_PATH * 2, L"%ls\\flash14-scanner-%ls.bin", logdir, wn);
    if (!write_w(fp, scsi_flash_cap, scsi_nflash_cap)) { MessageBoxW(hwnd, L"Could not save the scanner's parameter file in the Logs folder.", APP_TITLE, MB_ICONERROR); return; }
    { wchar_t lm[MAX_PATH * 2 + 100]; swprintf(lm, MAX_PATH * 2 + 100, L"The scanner's parameter file (%u bytes, checked) saved: %ls", (unsigned)scsi_nflash_cap, fp); log_w(lm); }
    wchar_t extra[MAX_PATH * 2 + 80]; swprintf(extra, MAX_PATH * 2 + 80, L"--prefs-from-flash \"%ls\" --lw-butting %d", fp, serial_lw ? 1 : 0);
    if (!relaunch(extra)) { MessageBoxW(hwnd, L"Could not start the program again - start it by hand; the copy is then offered again.", APP_TITLE, MB_ICONERROR); return; }
    log_w(L"Starting again with the copy..."); closing = 1; DestroyWindow(hwnd);
}
/* before the scanner software starts: the copy itself */
static int prefs_apply(void) {
    wchar_t m[1200]; size_t nf = 0, np = 0;
    uint8_t *fl = slurp_w(prefs_from, &nf);
    wchar_t pa[MAX_PATH * 2], pp[MAX_PATH * 2], oa[MAX_PATH * 2], oar[MAX_PATH * 2];
    vol_path(pa, L"Smart342(L) V2.2.0\\Smart342.Prefs.rsrc"); vol_path(pp, L"System Folder\\Preferences\\Smart342.Prefs.rsrc");
    vol_path(oa, L"Smart342(L) V2.2.0\\Smart342.Old"); vol_path(oar, L"Smart342(L) V2.2.0\\Smart342.Old.rsrc");
    uint8_t *pr = slurp_w(pa, &np);
    if (!fl || !pr) { swprintf(m, 1200, L"Parameter copy: could not read %ls.", !fl ? prefs_from : pa); log_w(m); free(fl); free(pr); return 0; }
    Bytes out; char fs[32], ps[32], msg[300];
    int n = prefs_copy_from_flash(fl, nf, pr, np, prefs_lw_opt == 1, &out, fs, ps, 32, msg, sizeof msg);
    if (!n) { wchar_t w[300]; to_w(msg, w, 300); swprintf(m, 1200, L"Parameter copy NOT made: %ls. Smart342.Prefs is unchanged.", w); log_w(m); MessageBoxW(hwnd, m, APP_TITLE, MB_ICONWARNING); free(fl); free(pr); return 0; }
    int ok = write_w(oa, "", 0) && write_w(oar, pr, np);                 /* the backup first (create_PrefsfileBackup) */
    ok = ok && write_w(pa, out.b, out.n) && write_w(pp, out.b, out.n);
    {   /* the backup's Finder info (type SCPR, creator S42I), as the original makes it */
        wchar_t fi[MAX_PATH * 2]; vol_path(fi, L".finfo.json"); size_t n2; uint8_t *j = slurp_w(fi, &n2);
        const char *key = "\"Smart342(L) V2.2.0/Smart342.Old\"";
        if (j && !strstr((char *)j, key) && n2 >= 4 && !memcmp(j + n2 - 4, "\n]\n}", 4)) {
            const char *add = ",\n\"Smart342(L) V2.2.0/Smart342.Old\": [\n\"SCPR\",\n\"S42I\",\n0\n]\n}";
            size_t L = strlen(add); uint8_t *o = malloc(n2 + L); memcpy(o, j, n2 - 2); memcpy(o + n2 - 2, add, L);
            if (!write_w(fi, o, n2 - 2 + L)) log_d(L"Note: the Finder information of Smart342.Old was not written.");
            free(o);
        }
        free(j);
    }
    wchar_t w1[32], w2[32], w3[300]; to_w(fs, w1, 32); to_w(ps, w2, 32); to_w(msg, w3, 300);
    if (ok) swprintf(m, 1200, L"Scanner parameters copied from the scanner's flash disk into Smart342.Prefs: scanner %ls (the settings were for %ls); %ls. The previous settings are kept as Smart342.Old. Nothing was written to the scanner.", w1, w2, w3);
    else swprintf(m, 1200, L"Parameter copy: writing the files failed - Smart342.Prefs may be incomplete; the previous settings are in Smart342.Old.rsrc. Send the Logs folder.");
    log_w(m); bytes_free(&out); free(fl); free(pr); prefs_copied_now = ok;
    return ok;
}

/* ---- 0.3.1: Tools > Fix streaks - this scanner's clean gain vectors into Smart342.Prefs (cleangain.c) ----
   From the running averages of this scanner's white lines (2 or more per lens and mount), the GAIN resources the
   scanner software uses for its "clean gain" correction are made and stored - by starting the program again with
   --clean-gain <file> (the Prefs are only changed while the scanner software is not running). Only the Mac-side
   settings change; nothing is written to the scanner. The previous Smart342.Prefs is kept in the Logs folder. */
static void settings_save(void);
static int cg_noted, cg_relaunched;   /* cg_relaunched: the new start may already read smart342.ini - not written again here */
/* the GAIN resource the scanner software uses now for lens / mount (from the app-folder Smart342.Prefs - the copy the
   driver opens: FSpOpenResFile 'Smart342(L) V2.2.0/Smart342.Prefs' in every session trace): 1 = this program made it
   (its mark in mark), 0 = another one (the vendor's), -1 = none / not readable */
static int cg_current(int lens, int refl, char *mark, int markmax) {
    wchar_t pa[MAX_PATH * 2]; vol_path(pa, L"Smart342(L) V2.2.0\\Smart342.Prefs.rsrc"); size_t np = 0; uint8_t *pr = slurp_w(pa, &np);
    static uint8_t r[200 + CG_N * 2 + 16]; long n = prefs_get_resource(pr, np, "GAIN", 601 + 2 * lens + (refl ? 1 : 0), r, sizeof r); free(pr);
    if (mark && markmax > 0) mark[0] = 0;
    return n < 200 ? -1 : cg_resource_ours(r, (size_t)n, mark, markmax);
}
/* 0.4.0: the vector itself (CG_N values) of that GAIN resource: 0, or -1 none / not readable */
static int cg_vector_now(int lens, int refl, uint16_t *v) {
    wchar_t pa[MAX_PATH * 2]; vol_path(pa, L"Smart342(L) V2.2.0\\Smart342.Prefs.rsrc"); size_t np = 0; uint8_t *pr = slurp_w(pa, &np);
    static uint8_t r[200 + CG_N * 2 + 16]; long n = prefs_get_resource(pr, np, "GAIN", 601 + 2 * lens + (refl ? 1 : 0), r, sizeof r); free(pr);
    if (n != 200 + CG_N * 2) return -1;
    for (int i = 0; i < CG_N; i++) v[i] = (uint16_t)(r[200 + 2 * i] << 8 | r[201 + 2 * i]);
    return 0;
}
static void cg_note(void) {
    if (!cg_note_due || cg_noted) return;
    cg_noted = 1; wchar_t m[600]; char mk[100]; int own = cg_last_lens >= 0 ? cg_current(cg_last_lens, cg_last_refl, mk, sizeof mk) : -1;
    if (own == 1) { wchar_t w[100]; to_w(mk, w, 100);
        swprintf(m, 600, L"Note: this calibration leaves fine streaks along the scan (%.1f %% - largest in bright areas) although the clean gain vector stored is this program's (%ls): the scanner's light path has changed (lamp, CCD, dust on the KEEP CLEAN strip, or another scanner). Tools > \"Fix streaks\" measures a new one.", cg_last_res * 100, w); }
    else swprintf(m, 600, L"Note: this scanner's calibration leaves fine streaks along the scan (%.1f %% - largest in bright areas): the scanner software's stored clean gain vector is not this scanner's. Tools > \"Fix streaks\" measures this scanner's own (with an empty frame of the holder).", cg_last_res * 100);
    log_w(m);
}
static wchar_t cg_pending[MAX_PATH * 2], cg_restart[MAX_PATH * 2 + 40]; static int cg_store_auto = -1;   /* developer option (autotest): --clean-gain-answer 0 store / 1 not */
/* stores the GAIN resources (n of them): written to the Logs folder, then the scanner software ends (it saves its
   settings and parks the scanner, as at Quit) and the program starts again from WM_APP_EXITED with --clean-gain <file>;
   the new start writes the Prefs before the scanner software runs (cg_apply). Returns 1 when the restart is under way. */
static int cg_store_res(int n, const int *ids, char (*names)[64], uint8_t (*data)[200 + CG_N * 2]) {
    if (closing || !emu_running || busy_now) { log_w(L"Fix streaks: not stored - the scanner software ended or became busy. Choose Tools > Fix streaks again."); return 0; }
    wchar_t fp[MAX_PATH * 2]; swprintf(fp, MAX_PATH * 2, L"%ls\\clean-gain-vectors.bin", logdir);
    FILE *f = _wfopen(fp, L"wb"); int ok = f != NULL; size_t nd = 200 + CG_N * 2;
    if (f) { ok = fwrite("CGP1", 4, 1, f) == 1 && fwrite(&n, sizeof n, 1, f) == 1;
        for (int i = 0; i < n && ok; i++) ok = fwrite(&ids[i], sizeof ids[i], 1, f) == 1 && fwrite(names[i], 64, 1, f) == 1 && fwrite(data[i], nd, 1, f) == 1;
        ok = fclose(f) == 0 && ok; }
    if (!ok) { MessageBoxW(hwnd, L"Could not save the clean gain vector in the Logs folder.", APP_TITLE, MB_ICONERROR); return 0; }
    swprintf(cg_restart, MAX_PATH * 2 + 40, L"--clean-gain \"%ls\"", fp);
    log_w(L"Fix streaks: the scanner software ends, then the program starts again to store the clean gain vector...");
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    if (!closing) { cg_restart[0] = 0; log_w(L"Fix streaks: not stored - the program could not end the scanner software."); return 0; }
    return 1;
}
/* WM_APP_EXITED while closing: the scanner software has ended */
static void cg_restart_now(int how) {
    if (!cg_restart[0]) return;
    KillTimer(hwnd, 1);   /* review: the quit timer must not fire inside a message box below */
    static wchar_t extra[MAX_PATH * 2 + 40]; wcscpy(extra, cg_restart); cg_restart[0] = 0;
    if (how != 0) { log_w(L"Fix streaks: the scanner software did not end normally - the clean gain vectors were NOT stored. Start the program again and choose Tools > Fix streaks."); return; }
    settings_save();   /* review: the new start reads smart342.ini - this session's settings first */
    relaunch_strip_tests = 1;
    if (!relaunch(extra)) MessageBoxW(hwnd, L"Could not start the program again - start it by hand and choose Tools > Fix streaks again.", APP_TITLE, MB_ICONERROR);
    else { log_w(L"Starting again to store the clean gain vectors..."); cg_relaunched = 1; }
}
static void cg_apply(void) {
    wchar_t m[800]; size_t nf = 0, np = 0, npp = 0;
    uint8_t *f = slurp_w(cg_pending, &nf);
    wchar_t pa[MAX_PATH * 2], pp[MAX_PATH * 2], bk[MAX_PATH * 2], bkp[MAX_PATH * 2];
    /* the app-folder copy is the one the driver opens and keeps up to date (session traces: FSpOpenResFile
       'Smart342(L) V2.2.0/Smart342.Prefs'); the Preferences copy is written the same, as the parameter copy does */
    vol_path(pa, L"Smart342(L) V2.2.0\\Smart342.Prefs.rsrc"); vol_path(pp, L"System Folder\\Preferences\\Smart342.Prefs.rsrc");
    uint8_t *pr = slurp_w(pa, &np), *prp = slurp_w(pp, &npp);
    int n = 0; if (f && nf >= 8 && !memcmp(f, "CGP1", 4)) memcpy(&n, f + 4, sizeof n);
    size_t rec = sizeof(int) + 64 + 200 + CG_N * 2;
    if (!f || !pr || n < 1 || n > 10 || nf != 8 + (size_t)n * rec) { log_w(L"Fix streaks: the stored vectors could not be read - Smart342.Prefs is unchanged."); free(f); free(pr); free(prp); return; }
    int ids[10]; const char *names[10]; const uint8_t *data[10]; size_t nd[10];
    for (int i = 0; i < n; i++) { const uint8_t *r = f + 8 + i * rec; memcpy(&ids[i], r, sizeof(int)); names[i] = (const char *)r + sizeof(int); data[i] = r + sizeof(int) + 64; nd[i] = 200 + CG_N * 2;
        if (ids[i] < 601 || ids[i] > 610 || memchr(names[i], 0, 64) == NULL || !cg_resource_ours(data[i], nd[i], NULL, 0)) { log_w(L"Fix streaks: unexpected resource - Smart342.Prefs is unchanged."); free(f); free(pr); free(prp); return; } }
    Bytes out; char msg[200];
    if (!prefs_put_resources(pr, np, "GAIN", ids, names, data, nd, n, &out, msg, sizeof msg)) { wchar_t w[200]; to_w(msg, w, 200); swprintf(m, 800, L"Fix streaks NOT stored: %ls.", w); log_w(m); free(f); free(pr); free(prp); return; }
    swprintf(bk, MAX_PATH * 2, L"%ls\\Smart342.Prefs before the clean gain vectors.rsrc", logdir);
    swprintf(bkp, MAX_PATH * 2, L"%ls\\Preferences-Smart342.Prefs before the clean gain vectors.rsrc", logdir);
    int ok = write_w(bk, pr, np) && (!prp || write_w(bkp, prp, npp));   /* both backups before anything is changed */
    ok = ok && write_w(pa, out.b, out.n) && write_w(pp, out.b, out.n);
    if (ok) { int k = swprintf(m, 800, L"Fix streaks: this scanner's clean gain vectors stored in Smart342.Prefs (GAIN"); for (int i = 0; i < n; i++) k += swprintf(m + k, 800 - k, L" %d", ids[i]);
              swprintf(m + k, 800 - k, L"). The previous files are kept in this session's Logs folder. Nothing was written to the scanner."); }
    else swprintf(m, 800, L"Fix streaks: writing Smart342.Prefs failed - the previous files are in this session's Logs folder. Send the Logs folder.");
    log_w(m); bytes_free(&out); free(f); free(pr); free(prp);
}

/* ------------------------------------------------------------------ actions */
static double rng_min, rng_max;   /* allowed resolution in px/mm for the current preview/crop/scale; 0 = unknown */
static int probe_pending;
static double cur_dpi(void) { wchar_t w[64]; GetWindowTextW(GetDlgItem(hwnd, IDC_RES), w, 64); for (wchar_t *c = w; *c; c++) if (*c == L',') *c = L'.'; return _wtof(w); }
/* RAW blocks (0.1.19): does a W x H RAW fit in one Max Detail, and the plan of blocks for a larger one - a
   rectangle from the developer option --raw-limit, else the area rule (raw_area pixels): of the block shapes
   (width in steps of 16) the one that needs the fewest blocks */
static int raw_fits(long long W, long long H) {
    if (raw_max_w <= 0 && raw_area <= 0) return 1;      /* developer option --raw-limit 0x0 */
    if (raw_max_w > 0) return (W <= raw_max_w && H <= raw_max_h) || (raw_max_w2 > 0 && W <= raw_max_w2 && H <= raw_max_h2);
    return W * H <= raw_area;
}
static int raw_plan(int W, int H, int ovl, int (*rects)[4], int maxn, int *bw, int *bh) {
    static int tmp[256][4]; int best = 0; *bw = *bh = 0;
    if (maxn > 256) maxn = 256;
    if (raw_max_w > 0) {
        int n1 = rt_plan(W, H, raw_max_w, raw_max_h, ovl, tmp, 256);
        if (n1 > 0 && n1 <= maxn) { best = n1; memcpy(rects, tmp, sizeof(int[4]) * n1); *bw = raw_max_w; *bh = raw_max_h; }
        if (raw_max_w2 > 0) { int n2 = rt_plan(W, H, raw_max_w2, raw_max_h2, ovl, tmp, 256);
            if (n2 > 0 && n2 <= maxn && (!best || n2 < best)) { best = n2; memcpy(rects, tmp, sizeof(int[4]) * n2); *bw = raw_max_w2; *bh = raw_max_h2; } }
        return best;
    }
    for (int mw = W < 256 ? W : 256; ; mw += 16) {
        if (mw > W) mw = W;
        long mh = raw_area / mw; if (mh > H) mh = H;
        if (mh >= 128 || mh == H) {
            int n = rt_plan(W, H, mw, (int)mh, ovl, tmp, 256);
            if (n > 0 && n <= maxn && (!best || n < best)) { best = n; memcpy(rects, tmp, sizeof(int[4]) * n); *bw = mw; *bh = (int)mh; }
        }
        if (mw >= W) break;
    }
    return best;
}
static void update_res_info(void) {
    wchar_t m[300];
    if (!pbits) swprintf(m, 300, L"Allowed range: make a preview");
    else if (probe_pending) swprintf(m, 300, L"Allowed range: checking...");
    else if (rng_max <= 0) swprintf(m, 300, L"Allowed range: no valid resolution for this crop");
    else {
        double d = cur_dpi(); int lo = (int)ceil(rng_min * 25.4), hi = (int)floor(rng_max * 25.4);
        double w = edit_num(IDC_CW), hh = edit_num(IDC_CH), sc = edit_num(IDC_SCALE); if (sc <= 0) sc = 100;
        int crop = SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED;
        wchar_t opt[48] = L""; if (pv_opt_dpi > 0) swprintf(opt, 48, L", optical %.1f", OPT_SHOW(pv_opt_dpi));
        if (crop && d > 0) {
            double px = w * sc / 100 * d / 25.4, py = hh * sc / 100 * d / 25.4, mb = px * py * (sel(IDC_FORMAT) == FMT_RAW ? 6 : sel(IDC_MODE) == 0 ? 4 : sel(IDC_MODE) == 1 ? 3 : 1) / 1048576.0;
            int rawbig = sel(IDC_FORMAT) == FMT_RAW && !raw_fits((long long)(px + 0.5), (long long)(py + 0.5));
            int nblk = 0, bw_, bh_; if (rawbig && !raw_stream_on) { static int rr_[256][4]; nblk = raw_plan((int)(px + 0.5) + 16, (int)(py + 0.5) + 16, 64, rr_, 256, &bw_, &bh_); }
            wchar_t rb[48] = L""; if (rawbig) { if (raw_stream_on) swprintf(rb, 48, L" - RAW streamed in one request"); else swprintf(rb, 48, nblk > 0 && nblk <= 256 ? L" - RAW in %d blocks" : L" - too large for RAW!", nblk); }
            swprintf(m, 300, mb < 10 ? L"Allowed: %d - %d dpi%ls%ls\nOutput %.0f x %.0f px, about %.1f MB%ls" : L"Allowed: %d - %d dpi%ls%ls\nOutput %.0f x %.0f px, about %.0f MB%ls", lo, hi, opt,
                     (d < lo || d > hi) ? L"  (outside!)" : L"", px, py, mb, rb);
        } else swprintf(m, 300, L"Allowed: %d - %d dpi%ls%ls", lo, hi, opt, (d > 0 && (d < lo || d > hi)) ? L"  (outside!)" : L"");
    }
    SetWindowTextW(GetDlgItem(hwnd, IDC_RESINFO), m);
}
/* ask the original front end for the allowed range with the current crop and scale (after a preview) */
static int start_probe(void) {             /* 1 = the range check was started */
    if (!pbits || !emu_running) return 0;
    { char fv[16]; snprintf(fv, sizeof fv, "%d", frame_sel); push('F', NULL, NULL, fv); }   /* the frame the crop belongs to */
    char w[64], h[64], x[64], y[64], scale[64];
    num8(IDC_SCALE, scale, sizeof scale);
    if (scale[0]) push('s', "pscl", "sing", scale);
    if (SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        num8(IDC_CW, w, sizeof w); num8(IDC_CH, h, sizeof h); num8(IDC_CX, x, sizeof x); num8(IDC_CY, y, sizeof y);
        push('s', "psiz.pwth", "sing", w); push('s', "psiz.phgt", "sing", h);
        push('s', "pofs.pwth", "sing", x); push('s', "pofs.phgt", "sing", y);
    }
    char v[64]; snprintf(v, sizeof v, "%.6f", cur_dpi() / 25.4); push('r', NULL, NULL, v);
    probe_pending = 1; update_res_info();
    return 1;
}
/* table choices: names exactly as the original's popups hold them; "" = not chosen yet (the original's own default) */
static char sel_tab[POP_N][96]; static int tab_touched[POP_N];
static int tab_ctl(int k) { return k == POP_SHARP ? IDC_SHARP : k == POP_COLOR ? IDC_COLOR : k == POP_FILM ? IDC_FILM : 0; }
static void tables_fill(void) {               /* WM_APP_TABLES: refill the combos, keep a choice the new list still has */
    PopList L[POP_N]; EnterCriticalSection(&cs); memcpy(L, ui_lists, sizeof L); LeaveCriticalSection(&cs);
    for (int k = 0; k < POP_N; k++) {
        int id = tab_ctl(k); if (!id) continue;
        HWND c = GetDlgItem(hwnd, id); SendMessageW(c, CB_RESETCONTENT, 0, 0);
        int pick = -1;
        for (int i = 0; i < L[k].n; i++) {
            char disp[128]; size_t n = strlen(L[k].item[i]);
            if (n && L[k].item[i][n - 1] == '@') snprintf(disp, sizeof disp, "%.*s (std)", (int)n - 1, L[k].item[i]); else snprintf(disp, sizeof disp, "%s", L[k].item[i]);
            wchar_t w[128]; to_w(disp, w, 128); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)w);
            if (sel_tab[k][0] && !strcmp(sel_tab[k], L[k].item[i])) pick = i;
        }
        if (pick < 0) {                       /* not chosen, or not offered any more: the original's current one */
            if (sel_tab[k][0] && L[k].n) { wchar_t m[300], w[96]; to_w(sel_tab[k], w, 96); swprintf(m, 300, L"%ls table \"%ls\" is not offered for this mode/media - back to the standard one.", k == POP_SHARP ? L"Sharpness" : L"Colour", w); log_w(m); }
            pick = L[k].cur >= 1 && L[k].cur <= L[k].n ? L[k].cur - 1 : 0;
            sel_tab[k][0] = 0;
        }
        SendMessageW(c, CB_SETCURSEL, pick, 0); EnableWindow(c, L[k].n > 1 && (k != POP_FILM || sel(IDC_MEDIA) == 1));
    }
}
static void table_choice(int k) {             /* the user picked an entry */
    int i = (int)SendMessageW(GetDlgItem(hwnd, tab_ctl(k)), CB_GETCURSEL, 0, 0);
    EnterCriticalSection(&cs); if (i >= 0 && i < ui_lists[k].n) snprintf(sel_tab[k], sizeof sel_tab[k], "%s", ui_lists[k].item[i]); LeaveCriticalSection(&cs);
    tab_touched[k] = 1;
}
static void push_film(void) {                /* negatives: the film type, chosen the way a user does (popups.c) */
    if (sel(IDC_MEDIA) == 1 && tab_touched[POP_FILM] && sel_tab[POP_FILM][0]) push('f', NULL, NULL, sel_tab[POP_FILM]);
}
static void push_tables(void) {               /* before a scan: only what was chosen (untouched = the original's own default) */
    static const char *prop[POP_N] = { "pgrd", "psrp", "pclr", NULL, NULL };
    for (int k = 0; k < POP_N; k++)
        if (prop[k] && tab_touched[k] && sel_tab[k][0]) push('g', prop[k], "TEXT", sel_tab[k]);
}

/* ---- the window's settings are kept between starts (smart342.ini next to the program; not in autotest) ---- */
static void ini_path(wchar_t *p) { swprintf(p, MAX_PATH * 2, L"%ls\\smart342.ini", exedir); }
static const int INI_EDITS[] = { IDC_RES, IDC_SCALE, IDC_CW, IDC_CH, IDC_CX, IDC_CY, IDC_NAME };
static const wchar_t *INI_EDIT_KEYS[] = { L"resolution", L"scale", L"crop_w", L"crop_h", L"crop_left", L"crop_top", L"file_name" };
static const int INI_COMBOS[] = { IDC_MOUNT, IDC_MODE, IDC_MEDIA, IDC_FORMAT, IDC_PASSES, IDC_GAMMA };   /* not the experimental HDR choice: it starts as "normal" every time (review) */
static const wchar_t *INI_COMBO_KEYS[] = { L"original", L"mode", L"media", L"format", L"raw_passes", L"raw_gamma" };
static const wchar_t *INI_TAB_KEYS[POP_N] = { L"gradation", L"sharpness", L"colour", L"filmtype", NULL, NULL };
static int gamma_default;   /* developer option --gamma 0|1 */
static int ini_in_autotest, auto_list, auto_list_mix, auto_frame, format_default, keep_raw, passes_default, keep_passes_default, hdr_default;
static HMENU tools_menu; static int raw_stream_opt;
static wchar_t raw_limit_opt[32], focus_opt[32]; static int raw_test_auto, focus_test_auto, lens_test_auto, stop_after_ms, stop_at_capture, mirror_opt;
static wchar_t auto_name[64];   /* developer option --name: output file name in autotest */
static double hdr_test_gain;   /* developer option --hdr-test-gain X (simulator only) */   /* developer option --ini: test the settings file in autotest */
static void settings_save(void) {
    if (autotest && !ini_in_autotest) return;
    wchar_t p[MAX_PATH * 2], v[400]; ini_path(p);
    for (int i = 0; i < (int)(sizeof INI_EDITS / sizeof *INI_EDITS); i++) { GetWindowTextW(GetDlgItem(hwnd, INI_EDITS[i]), v, 400); WritePrivateProfileStringW(L"window", INI_EDIT_KEYS[i], v, p); }
    for (int i = 0; i < (int)(sizeof INI_COMBOS / sizeof *INI_COMBOS); i++) { swprintf(v, 400, L"%d", sel(INI_COMBOS[i])); WritePrivateProfileStringW(L"window", INI_COMBO_KEYS[i], v, p); }
    { swprintf(v, 400, L"%d", cass_type()); WritePrivateProfileStringW(L"window", L"holder", v, p); }   /* cassette-directory type */
    WritePrivateProfileStringW(L"window", L"keep_each_pass", SendMessageW(GetDlgItem(hwnd, IDC_KEEPPASS), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"window", L"mirror", SendMessageW(GetDlgItem(hwnd, IDC_MIRROR), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"window", L"use_crop", SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", p);
    if (rsh_opt < 0) { swprintf(v, 400, L"%d", rsh_level); WritePrivateProfileStringW(L"window", L"raw_sharpen", v, p); }   /* not a developer override (review) */
    if (colreg_opt < 0) WritePrivateProfileStringW(L"window", L"raw_colour_registration", colreg_on ? L"1" : L"0", p);
    if (!psn_opt) WritePrivateProfileStringW(L"window", L"pixel_shift_passes", ps_npass == 8 ? L"8" : L"5", p);
    if (!psslow_opt) WritePrivateProfileStringW(L"window", L"pixel_shift_slow_moves", ps_slow ? L"1" : L"0", p);
    if (!stephold_opt) WritePrivateProfileStringW(L"window", L"raw_step_and_hold", stephold_on ? L"1" : L"0", p);
    if (!raw_stream_opt) WritePrivateProfileStringW(L"window", L"raw_streamed", raw_stream_on ? L"1" : L"0", p);
    if (!log_detail_opt) WritePrivateProfileStringW(L"window", L"detailed_log", log_detail ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"negative", L"positive", neg_pos_on ? L"1" : L"0", p);
    if (neg_c_opt < 0) { swprintf(v, 400, L"%d", neg_contrast); WritePrivateProfileStringW(L"negative", L"contrast", v, p); }
    if (neg_rot_opt < 0) { swprintf(v, 400, L"%d", neg_rot); WritePrivateProfileStringW(L"negative", L"rotate", v, p); }
    WritePrivateProfileStringW(L"negative", L"levels_kept", neg_lock ? L"1" : L"0", p);
    if (neg_lock_lv.ok) { swprintf(v, 400, L"%d %.5f %.5f %.5f %.5f %.5f %.5f", neg_lock_lv.ch, neg_lock_lv.lo[0], neg_lock_lv.hi[0], neg_lock_lv.lo[1], neg_lock_lv.hi[1], neg_lock_lv.lo[2], neg_lock_lv.hi[2]);
                          WritePrivateProfileStringW(L"negative", L"kept_levels", v, p); }
    for (int k = 0; k < POP_N; k++) if (INI_TAB_KEYS[k]) { to_w(tab_touched[k] ? sel_tab[k] : "", v, 400); WritePrivateProfileStringW(L"tables", INI_TAB_KEYS[k], v, p); }
}
static void settings_load(void) {
    if (autotest && !ini_in_autotest) return;
    wchar_t p[MAX_PATH * 2], v[400]; ini_path(p);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return;
    for (int i = 0; i < (int)(sizeof INI_EDITS / sizeof *INI_EDITS); i++)
        if (GetPrivateProfileStringW(L"window", INI_EDIT_KEYS[i], L"\x01", v, 400, p) && v[0] != 1) SetWindowTextW(GetDlgItem(hwnd, INI_EDITS[i]), v);
    for (int i = 0; i < (int)(sizeof INI_COMBOS / sizeof *INI_COMBOS); i++) {
        int n = (int)SendMessageW(GetDlgItem(hwnd, INI_COMBOS[i]), CB_GETCOUNT, 0, 0), x = GetPrivateProfileIntW(L"window", INI_COMBO_KEYS[i], -1, p);
        if (x >= 0 && x < n) SendMessageW(GetDlgItem(hwnd, INI_COMBOS[i]), CB_SETCURSEL, x, 0);
    }
    { int t = GetPrivateProfileIntW(L"window", L"holder", -1, p);
      if (t < 0 && GetPrivateProfileStringW(L"window", L"cassette", L"", v, 400, p)) {    /* 0.1.x: a 'pcas' code -> the holder it really selected */
          char c[16], m[300]; to_utf8(v, c, 16); t = cas_legacy_type(c);
          if (t >= 0 && cas_holder(t)) { snprintf(m, sizeof m, "Holder: the settings file's cassette \"%s\" made the scanner software use \"%s\" - that is what is selected now.", c, cas_holder(t)->name); log8(m); } }
      cass_fill(sel(IDC_MOUNT), t >= 0 ? t : cass_default); }   /* always the list of the loaded mount (review) */
    SendMessageW(GetDlgItem(hwnd, IDC_MIRROR), BM_SETCHECK, GetPrivateProfileIntW(L"window", L"mirror", 0, p) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, IDC_KEEPPASS), BM_SETCHECK, GetPrivateProfileIntW(L"window", L"keep_each_pass", 0, p) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, GetPrivateProfileIntW(L"window", L"use_crop", 0, p) ? BST_CHECKED : BST_UNCHECKED, 0);
    { int r = GetPrivateProfileIntW(L"window", L"raw_sharpen", 0, p); rsh_level = r >= 0 && r <= 3 ? r : 0; }
    colreg_on = GetPrivateProfileIntW(L"window", L"raw_colour_registration", 1, p) != 0;
    ps_npass = GetPrivateProfileIntW(L"window", L"pixel_shift_passes", 5, p) == 8 ? 8 : 5;
    ps_slow = GetPrivateProfileIntW(L"window", L"pixel_shift_slow_moves", 1, p) != 0;
    stephold_on = GetPrivateProfileIntW(L"window", L"raw_step_and_hold", 1, p) != 0;
    raw_stream_on = GetPrivateProfileIntW(L"window", L"raw_streamed", 1, p) != 0;
    log_detail = GetPrivateProfileIntW(L"window", L"detailed_log", 0, p) != 0;
    neg_pos_on = GetPrivateProfileIntW(L"negative", L"positive", 1, p) != 0;
    { int c = GetPrivateProfileIntW(L"negative", L"contrast", 1, p); neg_contrast = c >= 0 && c <= 2 ? c : 1; }
    neg_rot = GetPrivateProfileIntW(L"negative", L"rotate", 0, p) & 3;
    { memset(&neg_lock_lv, 0, sizeof neg_lock_lv); int ch = 0; double l[6];
      if (GetPrivateProfileStringW(L"negative", L"kept_levels", L"", v, 400, p) && swscanf(v, L"%d %lf %lf %lf %lf %lf %lf", &ch, &l[0], &l[1], &l[2], &l[3], &l[4], &l[5]) == 7 && (ch == 1 || ch == 3)) {
          neg_lock_lv.ch = ch; for (int c = 0; c < 3; c++) { neg_lock_lv.lo[c] = l[2 * c]; neg_lock_lv.hi[c] = l[2 * c + 1]; }
          neg_lock_lv.ok = 1; for (int c = 0; c < ch; c++) if (!(neg_lock_lv.hi[c] - neg_lock_lv.lo[c] > 0.05)) neg_lock_lv.ok = 0;
          if (neg_lock_lv.ok) neg_fix_range(&neg_lock_lv); } }
    neg_lock = GetPrivateProfileIntW(L"negative", L"levels_kept", 0, p) != 0 && neg_lock_lv.ok;
    ps_approach = GetPrivateProfileIntW(L"window", L"pixel_shift_approach", PS_APPROACH, p) <= 0 ? 0 : PS_APPROACH;
    for (int k = 0; k < POP_N; k++) if (INI_TAB_KEYS[k] && GetPrivateProfileStringW(L"tables", INI_TAB_KEYS[k], L"", v, 400, p) && v[0]) {
        to_utf8(v, sel_tab[k], sizeof sel_tab[k]); tab_touched[k] = 1; }
}
/* scan001.tif -> scan002.tif after a saved scan (the number keeps its width; names without a number are kept) */
static void next_file_name(void) {
    wchar_t w[300]; GetWindowTextW(GetDlgItem(hwnd, IDC_NAME), w, 300);
    wchar_t *dot = wcsrchr(w, L'.'); int end = dot ? (int)(dot - w) : (int)wcslen(w), b = end;
    while (b > 0 && w[b - 1] >= L'0' && w[b - 1] <= L'9') b--;
    if (b == end || end - b > 6) return;
    long n = wcstol(w + b, NULL, 10) + 1; wchar_t num[16]; swprintf(num, 16, L"%0*ld", end - b, n);
    wchar_t out[320]; swprintf(out, 320, L"%.*ls%ls%ls", b, w, num, dot ? dot : L"");
    if (wcslen(out) <= 31) SetWindowTextW(GetDlgItem(hwnd, IDC_NAME), out);
}
static void bq_show(void) {
    HWND l = GetDlgItem(hwnd, IDC_BLIST); SendMessageW(l, LB_RESETCONTENT, 0, 0);
    if (!nbq) SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)L"Scan list: draw a crop, set resolution and file name, press \"Add crop to list\".");
    for (int i = 0; i < nbq; i++) { wchar_t t[300]; wchar_t fm[60]; if (bq[i].fmt == FMT_RAW && bq[i].hdr && HDR_LONG[bq[i].hdr]) swprintf(fm, 60, L"RAW long x%.0f", HDR_K[bq[i].hdr]); else if (bq[i].fmt == FMT_RAW && bq[i].hdr) swprintf(fm, 60, L"RAW HDR+%d", bq[i].hdr); else if (bq[i].fmt == FMT_RAW && PASSES_V[bq[i].passes] > 1) swprintf(fm, 60, L"RAW x%d", PASSES_V[bq[i].passes]); else swprintf(fm, 60, L"%ls", FMT_L[bq[i].fmt]);
                                       swprintf(t, 300, L"%d.  %ls   %ls   frame %d   %.2f x %.2f mm at %.2f, %.2f   %ls dpi%ls%ls%ls", i + 1, bq[i].name, fm, bq[i].frame, bq[i].w, bq[i].h, bq[i].x, bq[i].y, bq[i].res,
                                       bq[i].scale[0] ? L"   scale " : L"", bq[i].scale, bq[i].scale[0] ? L" %" : L""); SendMessageW(l, LB_ADDSTRING, 0, (LPARAM)t); }
    wchar_t b[40]; swprintf(b, 40, nbq ? L"Scan list (%d)" : L"Scan list", nbq); SetWindowTextW(GetDlgItem(hwnd, IDC_BSCAN), b);
    EnableWindow(GetDlgItem(hwnd, IDC_BSCAN), nbq > 0 && !busy_now); EnableWindow(GetDlgItem(hwnd, IDC_BCLEAR), nbq > 0 && !bq_running);
    InvalidateRect(hprev, NULL, FALSE);
}
static void bq_add(void) {
    if (!pbits || pv_mm_rows <= 0) { MessageBoxW(hwnd, L"Make a preview first.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) != BST_CHECKED) { MessageBoxW(hwnd, L"Draw a crop on the preview (or tick \"Crop (mm)\") first.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (nbq >= 40) { MessageBoxW(hwnd, L"The list holds 40 crops.", APP_TITLE, MB_ICONINFORMATION); return; }
    BItem *b = &bq[nbq]; b->frame = frame_sel; b->fmt = sel(IDC_FORMAT); b->passes = sel(IDC_PASSES); b->hdr = sel(IDC_HDR); b->w = edit_num(IDC_CW); b->h = edit_num(IDC_CH); b->x = edit_num(IDC_CX); b->y = edit_num(IDC_CY);
    GetWindowTextW(GetDlgItem(hwnd, IDC_RES), b->res, 32); GetWindowTextW(GetDlgItem(hwnd, IDC_SCALE), b->scale, 32); GetWindowTextW(GetDlgItem(hwnd, IDC_NAME), b->name, 64);
    char n8[200]; to_utf8(b->name, n8, sizeof n8);
    if (!valid_name(n8)) { MessageBoxW(hwnd, L"The output file name must be 1-31 characters, without : / \\.", APP_TITLE, MB_ICONWARNING); return; }
    for (int i = 0; i < nbq; i++) if (!wcscmp(bq[i].name, b->name)) { MessageBoxW(hwnd, L"This file name is already in the list.", APP_TITLE, MB_ICONWARNING); return; }
    if (b->w <= 0 || b->h <= 0) return;
    /* the allowed range is checked for this crop when the list is scanned (each entry is checked like a single scan) */
    nbq++; next_file_name(); bq_show();
}
static void do_scan(void);
static void select_frame(int k, int probe);
static void bq_next(void) {             /* start the scan of entry bq_idx with its own crop, resolution, scale and name */
    if (bq_idx >= nbq) { bq_running = 0; log_w(L"Scan list finished."); bq_show(); return; }
    BItem *b = &bq[bq_idx];
    if (b->frame < 1 || b->frame > frames_total || !frame_ready[b->frame]) {   /* (found in review: the list waited for ever) */
        wchar_t m[200]; swprintf(m, 200, L"Scan list stopped: entry %d (%ls) is from frame %d, which has no preview now - make a preview first.", bq_idx + 1, b->name, b->frame);
        log_w(m); status(L"Scan list stopped - see the log."); bq_running = 0; bq_show(); return; }
    if (b->frame != frame_sel) select_frame(b->frame, 0);
    set_edit_mm(IDC_CW, b->w); set_edit_mm(IDC_CH, b->h); set_edit_mm(IDC_CX, b->x); set_edit_mm(IDC_CY, b->y);
    SetWindowTextW(GetDlgItem(hwnd, IDC_RES), b->res); SetWindowTextW(GetDlgItem(hwnd, IDC_SCALE), b->scale); SetWindowTextW(GetDlgItem(hwnd, IDC_NAME), b->name);
    SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(GetDlgItem(hwnd, IDC_FORMAT), CB_SETCURSEL, b->fmt, 0); SendMessageW(GetDlgItem(hwnd, IDC_PASSES), CB_SETCURSEL, b->passes, 0); SendMessageW(GetDlgItem(hwnd, IDC_HDR), CB_SETCURSEL, b->hdr, 0); passes_enable();
    KillTimer(hwnd, 3);
    wchar_t m[200]; swprintf(m, 200, L"Scan list: entry %d of %d (%ls).", bq_idx + 1, nbq, b->name); log_w(m);
    swprintf(pg.prefix, 48, L"List %d of %d: ", bq_idx + 1, nbq);
    SendMessageW(GetDlgItem(hwnd, IDC_BLIST), LB_SETCURSEL, bq_idx, 0);
    bq_wait_range = 1;                     /* the allowed range for this entry's crop first; the scan follows (WM_APP_RANGE) */
    if (!start_probe()) { bq_wait_range = 0; bq_running = 0; log_w(L"Scan list stopped: the allowed range could not be checked (no preview?)."); status(L"Scan list stopped - see the log."); bq_show(); }
}
static void frames_fill(void) {
    HWND c = GetDlgItem(hwnd, IDC_FRAME); SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (int i = 1; i <= frames_total; i++) { wchar_t t[32]; swprintf(t, 32, frame_ready[i] ? L"%d" : L"%d (no preview)", i); SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageW(c, CB_SETCURSEL, frame_sel - 1, 0); EnableWindow(c, frames_total > 1);
    wchar_t m[100]; swprintf(m, 100, frames_total > 1 ? L"of %d (this cassette holds %d frames)" : L"", frames_total, frames_total);
    SetWindowTextW(GetDlgItem(hwnd, IDC_FRAMEINFO), m);
}
/* show frame k and make it the one crops and scans refer to (the front end switches with start_preview, fghooks.c) */
static void select_frame(int k, int probe) {
    if (k < 1 || k > frames_total) return;
    frame_sel = k; frames_fill();
    if (frame_ready[k]) load_preview_frame(k); else { free(pbits); pbits = NULL; InvalidateRect(hprev, NULL, FALSE); }
    { char fv[16]; snprintf(fv, sizeof fv, "%d", k); push('F', NULL, NULL, fv); }
    bq_show();
    if (probe && frame_ready[k]) start_probe(); else update_res_info();
}
static void do_preview(void) {
    user_stopped = 0;
    pv_mount = sel(IDC_MOUNT); pv_negative = sel(IDC_MEDIA) == 1;
    push_original_settings();
    push('n', NULL, NULL, "1");           /* how many frames; the answer (WM_APP_FRAMES) previews each of them */
    memset(frame_ready, 0, sizeof frame_ready); frames_done = 0; frames_auto_checked = 0;
    job_ui = 'p'; set_busy(1); status(L"Preview running...");
    if (autotest && stop_at_capture == 1) SetTimer(hwnd, 10, 20, NULL);   /* developer option --stop-at-capture preview */
    pg_begin(L"Preview", L"frame", 1);
    log_w(L"Preview requested.");
}
static int move_to_scans(const char *name, const wchar_t *note, wchar_t *dst_out);
/* ---- RAW 16-bit: the original's Max Detail (a prescan at the scan resolution, 12 bits per channel, before gradation,
   colour tables and sharpening) of exactly the crop, converted here into a 16-bit TIFF (rawscan.c, RE notes) ---- */
static struct {
    char name[300], scale[64]; double dpi; int flip, mirror, mount, negative, passes, pass, keep, failed;
    int cass_i, mode_i, media_i, frame, usecrop; char film[96], cw[64], ch_[64], cx[64], cy[64];
    double hdr_k; uint16_t *hdr_n; double long_k, long_r;   /* 0.5.0: long exposure (one pass, k x the integration time; long_r = the ratio applied) */   /* experimental HDR: exposure factor of pass 2 (0 = off), pass 1 kept here */   /* settings at the start (found in review: the window could change them between passes) */
    DWORD t0, tp; RawStack st; int st_on, w, h, ch, lin; double sh_dx[RS_MAXPASS], sh_dy[RS_MAXPASS];
    /* tiles (a RAW larger than one block): rects in picture pixels incl. the margin (x0, y0, w, h, origin -margin),
       the crop in mm, mm per pixel, the picture size */
    int tiled, ntiles, tile, tW, tH, tmargin, trect[256][4]; double tleft, ttop, tmmpp; RtMosaic tm; int tm_on;
    double tdx_max, tgain_dev; int tunreg, tedge;
    int stream; long stream_sent0;   /* streamed RAW (each pass); guard.c's count of streamed blocks before it */
    int cnt_seq0;                    /* guard.c's count of wait-counter reads before it */
    /* pixel shift (0.1.21): F x finer across from passes with the lens carriage at different steps (rawsr.c) */
    int ps_F, ps_n; double ps_rw, ps_xdpi; uint16_t *ps_img[PS_MAX]; int ps_w, ps_h, ps_ch; SrInfo ps_info;
    int ps_step[PS_MAX];             /* lens carriage step of each ACCEPTED pass (0.1.22: 0, +1, then chosen from the measured offsets) */
    double ps_ax[PS_MAX], ps_ay[PS_MAX];   /* 0.1.24: the accepted offsets of each accepted pass against pass 1 (checked: sr_accept_pass) */
    int ps_no[PS_MAX];               /* 0.1.24: the pass number (as made) of each accepted pass */
    int ps_cmd, ps_rej, ps_rejstep;  /* 0.1.24: the step of the pass being made; passes left out as implausible; the last one's step */
    int ps_nslow, ps_nmade;          /* 0.1.26 (review): passes whose lens moves really went out slow (rawscan.c lens_shift_slow_used); passes made */
    int noalign, align_seq0, align_fired0;
    int gamma;                       /* 0.2.0: RAW gamma of this job's files: 0 linear, 1 = 2.2 (write_tiff16xy) */
    int stephold, sh_fired0, mm_seq0, optsnap, sc100, sh_mode, sh_expose, hdr_still1;   /* 0.4.1 hdr_still1: pass 1 of an HDR RAW made step-and-hold (1), continuous (0), not yet (-1) */   /* 0.1.28: step-and-hold for this RAW (rawscan.c stephold_bp); the picture motion seen in the last pass (sh_mode 0 = not seen); the resolution sent a hair below the CCD dpi; scale 100 */   /* 0.1.22 research: the scanner's colour alignment off for this RAW (rawscan.c) */
} rawj;
/* tile k: its crop in mm (kept inside the preview, shifted inward if the margin would leave it) and its picture origin */
static void tile_crop(int k, int *ox, int *oy) {
    double l = rawj.tleft + rawj.trect[k][0] * rawj.tmmpp, t = rawj.ttop + rawj.trect[k][1] * rawj.tmmpp;
    double w = rawj.trect[k][2] * rawj.tmmpp, h = rawj.trect[k][3] * rawj.tmmpp;
    if (pv_mm_cols > 0) { if (l + w > pv_mm_cols) l = pv_mm_cols - w; if (l < 0) l = 0; }
    if (pv_mm_rows > 0) { if (t + h > pv_mm_rows) t = pv_mm_rows - h; if (t < 0) t = 0; }
    snprintf(rawj.cw, sizeof rawj.cw, "%.5f", w); snprintf(rawj.ch_, sizeof rawj.ch_, "%.5f", h);
    snprintf(rawj.cx, sizeof rawj.cx, "%.5f", l); snprintf(rawj.cy, sizeof rawj.cy, "%.5f", t);
    *ox = (int)floor((l - rawj.tleft) / rawj.tmmpp + 0.5); *oy = (int)floor((t - rawj.ttop) / rawj.tmmpp + 0.5);
}
static void ps_free(void) {            /* pixel shift: the passes kept for the combine */
    for (int k = 0; k < PS_MAX; k++) { free(rawj.ps_img[k]); rawj.ps_img[k] = NULL; }
    rawj.ps_n = 0;
}
static void ps_end(void) {             /* the end of a pixel-shift RAW (done, failed or stopped): lens back to the driver's
                                          position for the next RAW (not touched otherwise: the lens shift test sets it
                                          before its own RAW) */
    if (rawj.ps_F) { lens_shift_pulses = 0; lens_shift_approach = 0; lens_shift_slow = 0; rawj.ps_F = 0; }
    ps_free();
}
static void raw_push(void) {           /* one Max Detail request with the settings taken at the start (each pass the same) */
    char v[64];
    push_mount(rawj.mount); push_holder(rawj.cass_i);
    push('s', "pmod", "enum", MODE_C[rawj.mode_i]); push('s', "pmed", "enum", MEDIA_C[rawj.media_i]);
    if (rawj.film[0]) push('f', NULL, NULL, rawj.film);
    { char fv[16]; snprintf(fv, sizeof fv, "%d", rawj.frame); push('F', NULL, NULL, fv); }   /* also leaves an earlier Max Detail */
    snprintf(v, sizeof v, "%.6f", rawj.dpi / 25.4); push('s', "pres", "sing", v);
    if (rawj.scale[0]) push('s', "pscl", "sing", rawj.scale);
    if (rawj.usecrop) {
        push('s', "psiz.pwth", "sing", rawj.cw); push('s', "psiz.phgt", "sing", rawj.ch_);
        push('s', "pofs.pwth", "sing", rawj.cx); push('s', "pofs.phgt", "sing", rawj.cy);
        if (rawj.pass == 1) { char m[300]; snprintf(m, sizeof m, "Crop sent: Left %s Top %s Width %s Height %s mm", rawj.cx, rawj.cy, rawj.cw, rawj.ch_); wchar_t wm[300]; to_w(m, wm, 300); log_d(wm); }
    }
    { double sc = atof(rawj.scale); char hv[80]; snprintf(hv, sizeof hv, "%.6f %.6f", rawj.dpi / 25.4, sc > 0 ? sc : 0.0); push('h', NULL, NULL, hv); }
    if (rawj.ps_F) { lens_shift_pulses = rawj.ps_cmd; lens_shift_approach = ps_approach; lens_shift_slow = ps_slow; }   /* pixel shift: this pass's lens carriage step, approached from above (0.1.23) */
    raw_noalign = rawj.noalign; rawj.align_seq0 = raw_lutx_seq; rawj.align_fired0 = raw_noalign_fired;
    raw_stephold = rawj.stephold; rawj.sh_fired0 = raw_stephold_fired; rawj.mm_seq0 = raw_motor_seq;
    { char kv[64]; snprintf(kv, sizeof kv, "%g %d %.6f", rawj.hdr_k > 1 && rawj.pass == 2 ? rawj.hdr_k : rawj.long_k > 1 ? rawj.long_k : 0.0, rawj.stream ? 1 : 0, rawj.ps_F ? rawj.ps_rw : 0.0); push('m', NULL, NULL, kv); }
    rawj.stream_sent0 = raw_stream_sent; rawj.cnt_seq0 = raw_stream_cnt_seq;
    rawj.tp = GetTickCount();
}
static struct { int on, stream; char cw[32], ch[32], cx[32], cy[32]; } rt_ovr;   /* a test's RAW step: its own area (and streamed or not) */
/* streamed RAW: the controller keeps the last preview / normal Max Detail in its memory (fw_reserve_now, guard.c) and
   needs at least 3 blocks of the streamed one (10 lines each, as the driver asks: every Max Detail on the hardware)
   in the rest. Checked here with a margin, before anything is queued; the guard checks the exact rule again. */
static int raw_stream_mem_fits(int whole_preview, long long *need_o) {
    double sc = atof(rawj.scale); if (!(sc > 0)) sc = 100;
    double res = rawj.ps_F ? (float)rawj.ps_rw : (float)(rawj.dpi / 25.4);   /* across */
    double wmm = whole_preview ? pv_mm_cols : atof(rawj.cw);
    long long W = (long long)(wmm * res * sc / 100 + 0.5), need = 3LL * 10 * W * 6, fr = 7077888LL - (long long)fw_reserve_now;
    if (need_o) *need_o = need;
    return need + 65536 <= fr;
}
static int raw_stream_mem_ok(int whole_preview) {
    long long need; if (raw_stream_mem_fits(whole_preview, &need)) return 1;
    wchar_t m[300]; swprintf(m, 300, L"Streamed RAW: the scanner's memory still holds the last preview or RAW (%lu KB); %lld KB are needed for this width. Make a new preview, then start the RAW again.", fw_reserve_now / 1024, need / 1024);
    log_w(m); if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); return 0;
}
static void do_raw(const char *name, double dpi, const char *scale) {
    user_stopped = 0;
    if (!frame_ready[frame_sel]) { MessageBoxW(hwnd, L"RAW scans are made from a preview: make a preview of this frame first.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!raw_patch_ok) { MessageBoxW(hwnd, L"RAW scans are not available with this scanner driver (see the log).", APP_TITLE, MB_ICONWARNING); return; }
    if (!rt_ovr.on && !ps_on && HDR_K[sel(IDC_HDR)] > 1 && !raw_hdr_ok) { MessageBoxW(hwnd, L"RAW HDR is not available with this scanner driver.", APP_TITLE, MB_ICONWARNING); return; }
    if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; }
    snprintf(rawj.name, sizeof rawj.name, "%s", name); snprintf(rawj.scale, sizeof rawj.scale, "%s", scale);
    rawj.dpi = dpi; rawj.mount = sel(IDC_MOUNT); rawj.negative = sel(IDC_MEDIA) == 1;
    rawj.mirror = SendMessageW(GetDlgItem(hwnd, IDC_MIRROR), BM_GETCHECK, 0, 0) == BST_CHECKED;
    rawj.flip = (rawj.mount == 0) ^ rawj.mirror;     /* transparent: as the preview is shown (display_is_mirror_to_PM) */
    rawj.cass_i = cass_type(); rawj.mode_i = sel(IDC_MODE); rawj.media_i = sel(IDC_MEDIA); rawj.frame = frame_sel;
    rawj.film[0] = 0; if (sel(IDC_MEDIA) == 1 && tab_touched[POP_FILM] && sel_tab[POP_FILM][0]) snprintf(rawj.film, sizeof rawj.film, "%s", sel_tab[POP_FILM]);
    rawj.usecrop = SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    num8(IDC_CW, rawj.cw, sizeof rawj.cw); num8(IDC_CH, rawj.ch_, sizeof rawj.ch_); num8(IDC_CX, rawj.cx, sizeof rawj.cx); num8(IDC_CY, rawj.cy, sizeof rawj.cy);
    rawj.passes = PASSES_V[sel(IDC_PASSES)]; rawj.pass = 1; rawj.failed = 0;
    free(rawj.hdr_n); rawj.hdr_n = NULL; rawj.hdr_k = HDR_K[sel(IDC_HDR)]; rawj.long_k = HDR_LONG[sel(IDC_HDR)] ? rawj.hdr_k : 0; rawj.long_r = 0; if (rawj.long_k > 1) rawj.hdr_k = 0;
    /* 0.1.28: a resolution within 0.02 % of the CCD's own sampling is sent a hair BELOW it. The driver's column vector
       is G[k] = trunc(8 k x CCD dpi / requested dpi) (dtsinpol_vec): at 2662.14 with the L110's 2662.1376 the ratio is
       just under 1 and every green column but the first was interpolated at 1/8 pixel (s32 hardware trace: 0, 7, 15, ..;
       ~9 % contrast lost at 42 lp/mm); at or below the CCD dpi it is 8k, the CCD's own samples. 1e-5 below keeps 8k for
       12,500 columns even if the driver's own float value is a little lower than this program's. */
    rawj.optsnap = 0;
    { double sc = atof(rawj.scale); rawj.sc100 = !(sc > 0) || fabs(sc - 100) < 1e-9; }   /* the across sampling is dpi x scale / 100 (review) */
    if (!ps_on && !rt_ovr.on && !ft_on && !lt_on && rawj.sc100 && OPT_NEAR(rawj.dpi, pv_opt_dpi)) {
        double want = OPT_REQ(pv_opt_dpi);
        if (rawj.dpi != want) { wchar_t m[300]; swprintf(m, 300, L"RAW: %.2f dpi is the CCD's own sampling (%.4f dpi) - sent as %.4f dpi, a hair below it, so every column is the CCD's own sample (just above it the scanner would shift green by 1/8 pixel).", rawj.dpi, pv_opt_dpi, want); log_d(m); }
        rawj.dpi = want; rawj.optsnap = 1;
    }
    rawj.noalign = noalign_on && noalign_ok && !rt_ovr.on && !ft_on && !lt_on;
    /* 0.4.1: HDR RAWs too. In STEP mode the driver skips the spiral speed calculation and its range check entirely
       (calculate_prescan_mode 0x04b25c -> "STEP MODE" -> return; read 13 Oct) - the carriage stands still while each
       line is exposed, so a longer exposure needs no speed limit. The HDR merge then checks STEP for the long pass
       instead of the spiral range (raw_hdr_step). */
    rawj.stephold = stephold_on && stephold_ok && !rt_ovr.on && !ft_on && !lt_on;
    rawj.sh_mode = rawj.sh_expose = 0; rawj.hdr_still1 = -1;
    if (stephold_on && !stephold_ok) log_d(L"Step-and-hold: not available (the driver code is not the expected 2.2.0 code) - this RAW is scanned with the normal continuous motion.");
    if (rawj.noalign && !ps_on && pv_opt_dpi > 0 && fabs(rawj.dpi / pv_opt_dpi - 1) < 0.05 && !rawj.optsnap) {   /* the driver then snaps the step to 1.0 (0x03d8b0): the RAW would be the CCD samples at a dpi it does not say (review) */
        /* 0.1.27: stop instead of scanning with the alignment on under a research name, and give the dpi to 2 decimals -
           the CCD's L110 sampling is 2662.1376 dpi (0.733660 / 0.007 mm as floats), so the 1-decimal "2662.1" was refused too */
        wchar_t m[400]; swprintf(m, 400, L"Scanner colour alignment off: no RAW made - %.2f dpi is within 5 %% of the CCD's %.2f dpi, where the scanner would deliver its own samples at a different scale. Type %.1f dpi as the resolution (Scale 100 %%; or use pixel shift), or untick Tools > RAW: the scanner's own colour alignment off.", rawj.dpi, pv_opt_dpi, OPT_SHOW(pv_opt_dpi)); log_w(m);
        rawj.noalign = 0; if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); else MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); return; }
    if (noalign_on && !noalign_ok) log_w(L"Scanner colour alignment off: not available (the driver code is not the expected 2.2.0 code) - this RAW uses the scanner's own alignment.");
    if (rt_ovr.on) {   /* RAW size test step: its own area, colour (3 channels, the largest block), one plain pass, 100 % */
        rawj.usecrop = 1; snprintf(rawj.cw, sizeof rawj.cw, "%s", rt_ovr.cw); snprintf(rawj.ch_, sizeof rawj.ch_, "%s", rt_ovr.ch);
        snprintf(rawj.cx, sizeof rawj.cx, "%s", rt_ovr.cx); snprintf(rawj.cy, sizeof rawj.cy, "%s", rt_ovr.cy);
        rawj.mode_i = 1; rawj.passes = 1; rawj.hdr_k = 0; rawj.long_k = 0; snprintf(rawj.scale, sizeof rawj.scale, "100");
    }
    if (rawj.hdr_k > 1) { if (rawj.passes != 1) log_w(L"RAW HDR: made of 2 passes (normal + longer exposure); the RAW passes setting is not used.");
                          rawj.passes = 2; }
    if (rawj.long_k > 1 && rawj.passes != 1) { log_w(L"RAW long exposure: one pass; the RAW passes setting is not used."); rawj.passes = 1; }
    /* pixel shift (Tools menu): across at the lens's CCD sampling, F passes or more with the lens carriage at different
       steps, combined F x finer across (rawsr.c); along, the scanner steps F x finer itself (up to the highest
       resolution the scanner software allows for this crop) */
    ps_free(); rawj.ps_F = 0; memset(&rawj.ps_info, 0, sizeof rawj.ps_info);
    if (ps_on && !rt_ovr.on) {
        const wchar_t *why = !focus_ok ? L"Pixel shift needs the lens control, which is not available with this scanner driver."
                           : !(pv_opt_dpi > 0) ? L"Pixel shift: make a preview first (the lens of the preview sets the sampling)."
                           : !rawj.usecrop ? L"Pixel shift: draw a crop first (tick Crop) - it is made for a part of the frame." : NULL;
        if (why) { log_w(why); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); else MessageBoxW(hwnd, why, APP_TITLE, MB_ICONINFORMATION); return; }
        int F = 2; double along = F * pv_opt_dpi;
        if (rng_max > 0 && along > floor(rng_max * 25.4)) along = floor(rng_max * 25.4);
        if (along < pv_opt_dpi) along = pv_opt_dpi;
        if (rawj.passes != 1 || rawj.hdr_k > 1) log_w(L"Pixel shift: the RAW passes and RAW exposure settings are not used (pixel shift makes its own passes).");
        rawj.ps_F = F; rawj.ps_rw = OPT_REQ(pv_opt_dpi) / 25.4; rawj.ps_xdpi = F * pv_opt_dpi;   /* across a hair below the CCD dpi (0.1.28: s33 L89 passes were at 1/8 pixel) */
        for (int i = 0; i < PS_MAX; i++) rawj.ps_step[i] = PS_STEPS[i];   /* passes 3.. are replaced by the measured choice */
        {   /* 0.3.0: the step chooser's expected step - measured for this lens by Set up this scanner, else general values
               (35 mm lens 1.54, 6x6 lens 1.25 px per step: the mean of the slow from-above runs s30 - s36 / s33; other
               lenses the thin-lens estimate) - and its scatter (sr_pred, rawsr.c) */
            int L = pv_lens, sg = rawj.flip ? -1 : 1; double cal = (L >= 0 && L <= 3) ? wz_get(L, L"px_step", 0) : 0;
            int cok = cal != 0 && wz_get(L, L"ok", 0) != 0; double f = lens_shift_factor > 0 ? lens_shift_factor : 157.48;
            double thin = lens_pitch > 0 ? (1 + lens_mag) / (f * lens_pitch) : 0;
            memset(&sr_pred, 0, sizeof sr_pred); sr_pred.on = 1; sr_pred.sigma_step = 0.1; sr_pred.sigma0 = 0.3;
            wchar_t m[500];
            if (cok) { double sc = wz_get(L, L"scatter", 0.3); sr_pred.prior = sg * fabs(cal); sr_pred.sigma0 = sc < 0.15 ? 0.15 : sc > 0.6 ? 0.6 : sc;
                swprintf(m, 500, L"Pixel shift: %.3f px per lens step expected (measured for this lens by Set up this scanner), scatter %.2f px.", fabs(cal), sr_pred.sigma0); }
            else { double g = L == 2 ? 1.54 : L == 1 ? 1.25 : thin; sr_pred.prior = g > 0 ? sg * g : 0;
                swprintf(m, 500, L"Pixel shift: %.2f px per lens step expected (general value - %ls; Tools > Set up this scanner measures it).", g, cal != 0 ? L"this lens's setup found the steps too uneven to use" : L"this lens is not measured on this scanner"); }
            log_d(m);
        }
        rawj.ps_cmd = PS_STEPS[0]; rawj.ps_rej = 0; rawj.ps_rejstep = -999; rawj.ps_nslow = rawj.ps_nmade = 0;
        rawj.dpi = along; snprintf(rawj.scale, sizeof rawj.scale, "100"); rawj.passes = ps_npass; rawj.hdr_k = 0; rawj.long_k = 0;
        wchar_t m[800]; swprintf(m, 800, L"Pixel shift %dx across: %ls - across the CCD's own %.1f dpi, combined %d x finer = %.1f dpi; along %.1f dpi%ls. %ls, lens carriage at %+d, %+d, then the steps chosen from the measured offsets; each pass approaches its step %ls%ls; your resolution setting is not used.",
                 F, pv_lens >= 0 && pv_lens <= 4 ? LENS_NAME[pv_lens] : L"lens unknown", pv_opt_dpi, F, rawj.ps_xdpi, along,
                 along < F * pv_opt_dpi - 0.5 ? L" (the highest the scanner software allows for this crop)" : L"", ps_npass == 8 ? L"8 passes" : L"3 to 5 passes",
                 PS_STEPS[0], PS_STEPS[1], ps_approach ? L"from above (the driver's own correction brings it up from 1 mm below)" : L"straight (pixel_shift_approach=0)",
                 ps_slow ? L", with SLOW lens moves (the motion board's constant-rate move, no fast ramp - Tools menu)" : L"");
        log_d(m);
    }
    /* 0.2.0: step-and-hold (a saved setting, on by default) only where it removes the smear - along at the lens's CCD
       resolution or finer. Coarser, the driver's STEP mode still creeps part of a row during each line (0.1.28
       motor_mode_bp: "expose" pulses > 0) for about 1.4 x the time, so those RAWs keep the continuous motion. Pixel
       shift (along 2 x the CCD's) and the CCD's own resolution (sent a hair below it, OPT_REQ) qualify. */
    if (rawj.stephold) {
        double sc = atof(rawj.scale); if (!(sc > 0)) sc = 100;
        double along = rawj.dpi * sc / 100;      /* the scanner samples at resolution x scale (as across, rawj.sc100) */
        if (!(pv_opt_dpi > 0) || along < OPT_REQ(pv_opt_dpi) * (1 - 1e-9)) {
            rawj.stephold = 0; wchar_t m[300];
            if (pv_opt_dpi > 0) swprintf(m, 300, L"Step-and-hold: not used for this RAW - %.1f dpi along is coarser than the lens's CCD resolution (%.1f dpi), where it would not remove the smear; continuous motion.", along, OPT_SHOW(pv_opt_dpi));
            else swprintf(m, 300, L"Step-and-hold: not used for this RAW - the lens's CCD resolution is not known; continuous motion.");
            log_d(m); }
        else { wchar_t m[200]; swprintf(m, 200, L"Step-and-hold: on for this RAW (%.1f dpi along, the lens's CCD resolution %.1f dpi).", along, OPT_SHOW(pv_opt_dpi)); log_d(m); }
    }
    /* 0.2.0: the RAW gamma of this job's files (the tests' RAWs are always linear: they are measured) */
    rawj.gamma = (rt_ovr.on || ft_on || lt_on) ? 0 : sel(IDC_GAMMA) == 1;
    /* tiles: a RAW larger than one block is made of several blocks (rawtile.c), or streamed in one request */
    rawj.tiled = 0; rawj.stream = 0; if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; }
    if (rt_ovr.on) rawj.stream = rt_ovr.stream;
    else {
        double sc = atof(rawj.scale); if (!(sc > 0)) sc = 100;
        double res = (float)(rawj.dpi / 25.4), resw = rawj.ps_F ? (float)rawj.ps_rw : res, cwm = atof(rawj.cw), chm = atof(rawj.ch_);
        int W = rawj.usecrop ? (int)((float)cwm * resw * (float)sc / 100 + 0.5) : 0, H = rawj.usecrop ? (int)((float)chm * res * (float)sc / 100 + 0.5) : 0;
        int fits = raw_fits(W, H);
        if (rawj.ps_F && (W < 32 || (long long)W * rawj.ps_F * H * 6 > 3900000000LL)) {
            wchar_t m[300]; swprintf(m, 300, W < 32 ? L"Pixel shift: the crop is only %d pixels across at the CCD sampling - draw a larger crop (32 or more)." : L"Pixel shift: the result (%d pixels across x %d) would be larger than a TIFF file can hold (4 GB) - draw a smaller crop.", W < 32 ? W : W * rawj.ps_F, H);
            log_w(m); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); else MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); ps_free(); rawj.ps_F = 0; return; }
        if (rawj.ps_F && !fits && !raw_stream_on) {
            wchar_t m[400]; swprintf(m, 400, L"Pixel shift: %d x %d pixels per pass is more than one RAW block (%ld pixels), and pixel shift is not combined with blocks. Make the crop smaller, or turn on Tools > Streamed RAW for large crops (checked on your scanner with the RAW check).", W, H, raw_area);
            log_w(m); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); else MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); ps_free(); rawj.ps_F = 0; return; }
        if (rawj.usecrop && W > 0 && H > 0 && !fits && (long long)W * H * 6 > 3900000000LL) { MessageBoxW(hwnd, L"This RAW would be larger than a TIFF file can hold (4 GB) - choose a lower resolution or a smaller crop.", APP_TITLE, MB_ICONWARNING); return; }
        /* 0.4.0: streamed is the default; when the scanner's memory still holds a large RAW block (a streamed one needs 3
           blocks of 10 lines beside it) a plain RAW is made of blocks this time instead of asking for a new preview */
        int stream_ok = raw_stream_on && (rawj.ps_F || !rawj.usecrop || raw_stream_mem_fits(0, NULL));
        if (raw_stream_on && !stream_ok && rawj.usecrop && W > 0 && H > 0 && !fits)
            log_w(L"RAW: the scanner's memory still holds the last RAW block - this RAW is made of blocks (a new preview frees the memory for a streamed one).");
        if (rawj.usecrop && W > 0 && H > 0 && !fits && stream_ok) {
            rawj.stream = 1;
            wchar_t m[300]; swprintf(m, 300, L"RAW: %d x %d pixels is larger than one RAW block (%ld pixels) - STREAMED in one request: the scanner passes it through as many blocks as fit in its memory.", W, H, raw_area); log_w(m);
        } else if (rawj.usecrop && W > 0 && H > 0 && !fits) {
            int M = 8, ovl = 64, bw = 0, bh = 0;
            int n = raw_plan(W + 2 * M, H + 2 * M, ovl, rawj.trect, 256, &bw, &bh);
            if (n <= 0 || n > 256) { MessageBoxW(hwnd, L"This RAW would need too many blocks - choose a lower resolution or a smaller crop.", APP_TITLE, MB_ICONWARNING); return; }
            for (int k = 0; k < n; k++) { rawj.trect[k][0] -= M; rawj.trect[k][1] -= M; }
            if (rtm_init(&rawj.tm, W, H, 3)) { MessageBoxW(hwnd, L"Not enough memory for this RAW - choose a lower resolution or a smaller crop.", APP_TITLE, MB_ICONWARNING); return; }
            rawj.tm_on = 1; rawj.tiled = 1; rawj.ntiles = n; rawj.tile = 0; rawj.tW = W; rawj.tH = H; rawj.tmargin = M;
            rawj.tleft = atof(rawj.cx); rawj.ttop = atof(rawj.cy); rawj.tmmpp = 25.4 / dpi * 100 / sc; rawj.tdx_max = 0; rawj.tgain_dev = 0; rawj.tunreg = 0; rawj.tedge = 0;
            if (rawj.passes != 1 || rawj.hdr_k > 1) log_w(L"RAW: passes and HDR are not combined with blocks yet - one plain pass.");
            rawj.passes = 1; rawj.hdr_k = 0; rawj.long_k = 0;
            wchar_t m[300];
            if (raw_max_w > 0) swprintf(m, 300, L"RAW: %d x %d pixels is larger than one RAW block on this scanner (up to %d x %d) - made of %d blocks of %d x %d with 64 pixels overlap, put together here.", W, H, bw, bh, n, rawj.trect[0][2], rawj.trect[0][3]);
            else swprintf(m, 300, L"RAW: %d x %d pixels is larger than one RAW block (%ld pixels) - made of %d blocks of %d x %d with 64 pixels overlap, put together here.", W, H, raw_area, n, rawj.trect[0][2], rawj.trect[0][3]);
            log_w(m);
            int ox, oy; tile_crop(0, &ox, &oy);
        }
    }
    if (rawj.stream && !raw_stream_mem_ok(rawj.usecrop ? 0 : 1)) { ps_free(); rawj.ps_F = 0; if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    rawj.keep = !rt_ovr.on && ((rawj.passes > 1 && SendMessageW(GetDlgItem(hwnd, IDC_KEEPPASS), BM_GETCHECK, 0, 0) == BST_CHECKED) || rawj.hdr_k > 1);   /* experimental HDR: both passes are always saved (the evidence of a first test) */
    rawj.t0 = GetTickCount();
    if (autotest && stop_at_capture == 3) SetTimer(hwnd, 10, 20, NULL);   /* developer option --stop-at-capture raw */
    /* the progress bar: one unit per Max Detail this RAW makes (0.2.0) */
    if (rawj.tiled) pg_begin(rt_ovr.on ? L"RAW check" : L"RAW scan", L"block", rawj.ntiles);
    else if (rawj.hdr_k > 1) { pg_begin(L"RAW HDR scan", L"pass", 2); pg_weight(2, rawj.hdr_k); }   /* pass 2 integrates k x longer */
    else if (rawj.ps_F) pg_begin(L"Pixel shift RAW", L"pass", rawj.passes == 8 ? 8 : PS_MIN), pg_set_total(rawj.passes == 8 ? 8 : PS_MIN, rawj.passes == 8 ? 8 : 5);
    else pg_begin(rt_ovr.on ? L"RAW check" : L"RAW scan", L"pass", rawj.passes);
    raw_push();
    rawj_running = 1;
    job_ui = 'c'; set_busy(1); status(rawj.ps_F ? L"Pixel shift RAW running (pass 1)..." : rawj.passes > 1 ? L"RAW scan running (pass 1)..." : L"RAW scan running...");
    { wchar_t m[200]; swprintf(m, 200, L"RAW scan requested: %.1f dpi%ls%ls.", rawj.dpi, rawj.mirror ? L", mirrored" : L"", rawj.hdr_k > 1 ? L", EXPERIMENTAL HDR" : rawj.long_k > 1 ? (rawj.long_k > 2 ? L", LONG EXPOSURE x4" : L", LONG EXPOSURE x2") : rawj.ps_F ? L" along, pixel shift" : rawj.passes > 1 ? L", averaging passes" : L"");
      log_w(m);
      if (rawj.hdr_k > 1) { swprintf(m, 200, L"RAW HDR: pass 1 normal, pass 2 with %.0f x the integration time (+%.0f stop%ls), merged in the shadows%ls.", rawj.hdr_k, log2(rawj.hdr_k), rawj.hdr_k > 2 ? L"s" : L"", rawj.keep ? L"; both passes are also saved" : L""); log_w(m); }
      else if (rawj.ps_F) { if (rawj.keep) log_w(L"Pixel shift: each pass is also saved (Keep each pass)."); }
      else if (rawj.passes > 1) { swprintf(m, 200, L"RAW: %d passes of the same crop, aligned to pass 1 and averaged%ls.", rawj.passes, rawj.keep ? L"; each pass is also saved" : L""); log_w(m); } }
}
/* ---- RAW check (Tools menu, 0.1.19): the controller's memory rule and the streamed RAW, on this scanner ----
   Found in the controller's firmware (fwmem.h): a Max Detail is kept whole in its memory (at most 1,179,648 colour
   pixels; 2618 x 3783 in session s9 was 8.4 times that), while a final-scan request (cmd 4) streams through a ring of
   as many blocks as fit. The check, in colour at 2000 dpi (or the allowed maximum), centred on the crop (or preview):
     N  a normal RAW of 600 x 400 pixels - the reference (every run);
     Q  a normal RAW of 1000 x 1000 pixels - our RAW block size (every run: it also leaves the controller with only
        about 1 MB free, so the streamed steps must go round their ring of buffers several times - review);
     S  the area of N again, STREAMED (1.44 MB through about 1 MB of ring) - must be the same picture as N: same size,
        match >= 0.995, shift <= 2.5 px (the scanner's repeatability, s10) and the same in every band of 50 rows
        (within 0.4 px: a lost or repeated line makes a 1 px step - rawcmp.c, 0.1.20);
     L  a streamed RAW of 1200 x 1100 pixels (7.9 MB through about 1 MB of ring) - complete, with N's and Q's pictures
        found inside it at their places by the same rule. After S passed or its picture differed (D).
   Q, S and L are recorded once each (smart342.ini [rawcheck]); a step still pending at the next start is the one that
   stopped the scanner. Nothing is written to the scanner (the interlock stays on and checks every block against the
   memory rule as well). */
typedef struct { char key[4]; int w, h, stream; } RtStep;
static const RtStep RT_STEPS[] = { { "N", 600, 400, 0 }, { "Q", 1000, 1000, 0 }, { "S", 600, 400, 1 }, { "L", 1200, 1100, 1 } };
#define RT_N ((int)(sizeof RT_STEPS / sizeof *RT_STEPS))
static int rt_on, rt_cur = -1, rt_cw, rt_ch, rt_meas_ok;   /* running; step index; its size; the step's own check */
static double rt_dpi; static uint16_t *rt_ref, *rt_ref2; static int rt_ref_w, rt_ref_h, rt_ref2_w, rt_ref2_h, rt_did_n, rt_did_q, rt_hold0;
static double rt_area_l[RT_N], rt_area_t[RT_N];          /* each step's area (left, top in mm) */
static void rt_ini(wchar_t *p) { swprintf(p, MAX_PATH * 2, L"%ls\\smart342.ini", exedir); }
static char rt_get(const char *key) { wchar_t p[MAX_PATH * 2], k[32], v[8]; rt_ini(p); to_w(key, k, 32);
    GetPrivateProfileStringW(L"rawcheck", k, L"", v, 8, p); return (char)v[0]; }
static void rt_put(const char *key, const wchar_t *val) { wchar_t p[MAX_PATH * 2], k[32]; rt_ini(p); to_w(key, k, 32); WritePrivateProfileStringW(L"rawcheck", k, val, p); }
static void rt_show_limits(void) {
    wchar_t m[400];
    if (raw_max_w > 0) swprintf(m, 400, L"RAW block size: up to %d x %d pixels (developer option --raw-limit).", raw_max_w, raw_max_h);
    else swprintf(m, 400, L"RAW block size: up to %ld pixels (width x lines) in one RAW request. The scanner keeps a whole RAW request in its memory: at most 1,179,648 colour pixels (found in its firmware; 2618 x 3783 stopped it on 8 Oct). Larger crops: %ls.",
                  raw_area, raw_stream_on ? L"streamed in one request (Tools > Diagnostics)" : L"made of blocks");
    log_w(m);
}
/* a reference picture (N or Q) inside img (w x h) at its expected place (x0, y0) (rawcmp.c): registered as a whole
   (|shift| <= max_off - two scans of the same place differ by up to about a pixel on this scanner, s10), then in
   bands of 50 rows: the shift must not change along the picture (a lost or repeated line would make a step of about
   1 px; limit 0.4). The 0.1.19 limit of 0.5 px on the whole shift failed S on the scanner (s10: +0.76 px along, the
   same in every band - the scanner's repeatability, not streaming). */
static int rc_find_ref(const uint16_t *ref, int rw, int rh, const uint16_t *img, int w, int h, int ch, int x0, int y0, double max_off, wchar_t *m, int mmax) {
    RcCmp r;
    int e = ch == 3 ? rc_compare(ref, rw, rh, img, w, h, x0, y0, max_off, &r) : -1;
    if (e == -1) { swprintf(m, mmax, L"the reference area (%d, %d) is outside the picture", x0, y0); return 0; }
    if (e == -2) { swprintf(m, mmax, L"out of memory"); return 0; }
    wchar_t step[80] = L"";
    if (r.jump_row >= 0 && r.bdy_max - r.bdy_min > RC_SPREAD_MAX) swprintf(step, 80, L"; largest step %+.2f px at row %d", r.jump, r.jump_row);
    swprintf(m, mmax, L"shift %+.3f px across, %+.3f px along (limit %.1f), match %.4f, brightness %.3f x; in %d bands of %d rows (%d measured) the shift varies by %.3f px along, %.3f across (limit %.1f; a lost or repeated line: about 1)%ls",
             r.dx, r.dy, max_off, r.ncc, r.bright, r.bands, RC_BAND, r.measured, r.bdy_max - r.bdy_min, r.bdx_max - r.bdx_min, RC_SPREAD_MAX, step);
    return r.ok;
}
/* the controller's timing counters after a streamed RAW (guard.c reads them with the driver's own command once all of
   the picture was read; the driver's names for them, from its final_scan_report): a09c "host - controler timing
   violation" = the scanner's buffers were full and it had to wait for the computer; a09a "controler - bip timing
   violation" = the controller was late for the image processor (a line may have been lost - the RAW check's
   picture comparison tells). The motion board's own count of spiral stops is logged by the driver itself
   ("Num stop_spiral"). Returns the a09c count, -1 if it was not read for this RAW. */
static long rt_waits = -1;      /* RAW check: the largest count of S and L in this run (-1 none read) */
static long rc_wait_log(void) {
    wchar_t m[300];
    if (raw_stream_cnt_seq == rawj.cnt_seq0) { log_d(L"RAW: the scanner's wait counter was not read for this RAW (see the trace, STREAMCNT)."); return -1; }
    long n = raw_stream_wait;
    if (raw_stream_cnt1 > 0) { swprintf(m, 300, L"RAW note: the scanner's controller counted %ld \"controler - bip timing violation%ls\" (its own name; in the RAW check of 9 Oct, 38 and 218 of them in streamed RAWs left no line lost, repeated or changed).", (long)raw_stream_cnt1, raw_stream_cnt1 == 1 ? L"" : L"s"); log_w(m); }
    if (n > 0) swprintf(m, 300, L"RAW: the scanner's buffers were full %ld time%ls - it had to wait for the computer to read (\"host - controler timing violation\"; the driver's \"Num stop_spiral\" shows whether the motion stopped).", n, n == 1 ? L"" : L"s");
    else swprintf(m, 300, L"RAW: the scanner's wait counter is 0 - it counted no wait for the computer (\"host - controler timing violation\" 0, \"controler - bip\" %ld).", (long)raw_stream_cnt1);
    log_d(m); return n;
}
/* the RAW check's measurement of the picture of the running step (called before it is saved) */
static void rc_measure(const uint16_t *img, int w, int h, int ch) {
    if (rt_cur < 0 || rt_cur >= RT_N) return;
    const RtStep *st = &RT_STEPS[rt_cur]; wchar_t m[700], r[400];
    rt_meas_ok = (w == st->w && h == st->h);
    if (!rt_meas_ok) { swprintf(m, 400, L"RAW check %lc: the picture is %d x %d pixels, %d x %d were asked for.", (wchar_t)st->key[0], w, h, st->w, st->h); log_w(m); return; }
    if (st->key[0] == 'N') {
        free(rt_ref); rt_ref = malloc((size_t)w * h * ch * 2); if (rt_ref) memcpy(rt_ref, img, (size_t)w * h * ch * 2); rt_ref_w = w; rt_ref_h = h; rt_did_n = 1; }
    else if (st->key[0] == 'Q') {   /* kept as the second reference for L (it covers rows 50..1050 of L - review) */
        free(rt_ref2); rt_ref2 = malloc((size_t)w * h * ch * 2); if (rt_ref2) memcpy(rt_ref2, img, (size_t)w * h * ch * 2); rt_ref2_w = w; rt_ref2_h = h; rt_did_q = 1; }
    else if (st->stream) {
        /* the ring the controller used for this request (the interlock's record of the last block) */
        long ring = fw_last_need; double laps = ring > 0 ? (double)w * h * 6 / ring : 0;
        swprintf(m, 400, L"RAW check %lc: the scanner streamed %.0f KB through %ld KB of buffers (%.1f times round).", (wchar_t)st->key[0], (double)w * h * 6 / 1024, ring / 1024, laps); log_w(m);
        { long n = raw_stream_cnt_seq != rawj.cnt_seq0 ? raw_stream_wait : -1; if (n > rt_waits) rt_waits = n; }
        if (laps <= 1.0) { rt_meas_ok = 0; log_w(L"RAW check: the buffers were not used more than once - this step proves nothing about streaming."); return; }
        if (st->key[0] == 'S') {
            rt_meas_ok = rc_find_ref(rt_ref, rt_ref_w, rt_ref_h, img, w, h, ch, 0, 0, 2.5, r, 400);
            swprintf(m, 700, L"RAW check S: streamed against normal (N): %ls - %ls.", r, rt_meas_ok ? L"the same picture" : L"NOT the same picture"); log_w(m);
        } else {      /* L: N and Q at their places inside it (crop coordinates are display coordinates; Mirror mirrors across);
                         a lost or repeated line inside them makes a step between bands of rows (rawcmp.c); one
                         before them only moves them by a line - within the scanner's repeatability, not detectable */
            double mmpp = 25.4 / rt_dpi; rt_meas_ok = 1;
            for (int k = 0; k < 2; k++) {
                const uint16_t *ref = k ? rt_ref2 : rt_ref; int rw = k ? rt_ref2_w : rt_ref_w, rh = k ? rt_ref2_h : rt_ref_h;
                int ox = (int)floor((rt_area_l[k] - rt_area_l[rt_cur]) / mmpp + 0.5), oy = (int)floor((rt_area_t[k] - rt_area_t[rt_cur]) / mmpp + 0.5);
                int x0 = rawj.mirror ? w - (ox + rw) : ox;
                int ok = rc_find_ref(ref, rw, rh, img, w, h, ch, x0, oy, 2.5, r, 400);
                swprintf(m, 700, L"RAW check L: streamed against normal (%lc inside it, rows %d..%d): %ls - %ls.", k ? L'Q' : L'N', oy, oy + rh, r, ok ? L"the same picture" : L"NOT the same picture"); log_w(m);
                rt_meas_ok &= ok;
            }
        }
    }
}
static void rt_finish(const wchar_t *why) {
    rt_on = 0; rt_cur = -1; rt_ovr.on = 0; rt_ovr.stream = 0; rt_bypass = 0; rt_put("pending", NULL);
    free(rt_ref); rt_ref = NULL; free(rt_ref2); rt_ref2 = NULL;
    log_w(why);
    wchar_t m[300]; char r[RT_N];
    for (int i = 0; i < RT_N; i++) r[i] = rt_get(RT_STEPS[i].key);
    swprintf(m, 300, L"RAW check results: N %lc, Q %lc, S %lc, L %lc (P passed, D picture differs, F stopped the scanner, E did not finish, - not run) - smart342.ini [rawcheck].",
             r[0] ? r[0] : '-', r[1] ? r[1] : '-', r[2] ? r[2] : '-', r[3] ? r[3] : '-'); log_w(m);
    if (r[2] == 'P' && r[3] == 'P') {
        log_w(L"RAW check: STREAMED RAW WORKS on this scanner - Tools > Streamed RAW for large crops makes a large RAW in one request instead of blocks.");
        if (rt_waits > 0) log_w(L"RAW check: the scanner's buffers also ran full in this check and it waited for the computer - and the picture was still complete: waiting is handled.");
        else if (rt_waits == 0) log_w(L"RAW check: the scanner's wait counter stayed 0 in this check - a full buffer was probably not tried (see the log of large streamed RAWs: \"buffers were full\").");
    }
    rt_waits = -1;
    status(L"RAW check finished - see the log."); set_busy(0);
    if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
/* the next step: N and Q in every run, then the first of S, L without a result (none after one that did not pass;
   L only after S passed) */
static int rt_next_index(void) {
    if (!rt_did_n) return 0;
    if (!rt_did_q) { char q = rt_get("Q"); return q && q != 'P' ? -1 : 1; }
    for (int i = 2; i < RT_N; i++) {
        char r = rt_get(RT_STEPS[i].key);
        if (!r) { char sr = rt_get("S"); if (RT_STEPS[i].key[0] == 'L' && sr != 'P' && sr != 'D') return -1; return i; }
        if (r != 'P' && r != 'D') return -1;       /* D: the picture differed, the scanner kept answering - go on */
    }
    return -1;
}
static void rt_step(void) {
    int i = rt_next_index();
    if (i < 0) { rt_finish(L"RAW check: done."); return; }
    const RtStep *st = &RT_STEPS[i];
    double dpi = 2000; if (rng_max > 0 && floor(rng_max * 25.4) < dpi) dpi = floor(rng_max * 25.4);
    if (rng_min > 0 && ceil(rng_min * 25.4) > dpi) { log_w(L"RAW check: 2000 dpi is not allowed here - choose another holder."); rt_finish(L"RAW check stopped."); return; }
    double wmm = st->w * 25.4 / dpi, hmm = st->h * 25.4 / dpi;
    int crop = SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    double cx = crop ? edit_num(IDC_CX) + edit_num(IDC_CW) / 2 : pv_mm_cols / 2, cy = crop ? edit_num(IDC_CY) + edit_num(IDC_CH) / 2 : pv_mm_rows / 2;
    if (pv_mm_cols <= 0 || pv_mm_rows <= 0 || wmm > pv_mm_cols || hmm > pv_mm_rows) {
        wchar_t m[200]; swprintf(m, 200, L"RAW check: %d x %d pixels at %.0f dpi (%.1f x %.1f mm) does not fit in this preview - use a larger holder or frame.", st->w, st->h, dpi, wmm, hmm); log_w(m);
        rt_finish(L"RAW check stopped."); return; }
    double l = cx - wmm / 2, t = cy - hmm / 2;
    if (st->key[0] == 'S') { l = rt_area_l[0]; t = rt_area_t[0]; }      /* exactly the area of N */
    if (l < 0) l = 0; if (t < 0) t = 0; if (l + wmm > pv_mm_cols) l = pv_mm_cols - wmm; if (t + hmm > pv_mm_rows) t = pv_mm_rows - hmm;
    rt_area_l[i] = l; rt_area_t[i] = t;
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", wmm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", hmm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", l); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", t);
    rt_cur = i; rt_cw = st->w; rt_ch = st->h; rt_dpi = dpi; rt_meas_ok = 0; rt_hold0 = fw_refused;
    { wchar_t kw[8]; to_w(st->key, kw, 8); rt_put("pending", kw); }     /* written before anything is sent */
    wchar_t m[300]; swprintf(m, 300, L"RAW check: step %lc - %ls RAW of %d x %d pixels (%.2f million) at %.0f dpi, colour.", (wchar_t)st->key[0], st->stream ? L"STREAMED" : L"normal", st->w, st->h, st->w * (double)st->h / 1e6, dpi); log_w(m);
    char nm[64]; snprintf(nm, sizeof nm, "rawcheck_%s_%dx%d", st->key, st->w, st->h);
    rt_ovr.on = 1; rt_ovr.stream = st->stream; rt_bypass = 1; raw_start_result = 0;
    do_raw(nm, dpi, "100");
    if (!rawj_running) { rt_put("pending", NULL); rt_finish(L"RAW check: the step could not start - see the lines above."); return; }
    { wchar_t sw[120]; swprintf(sw, 120, L"RAW check %lc: %d x %d pixels...", (wchar_t)st->key[0], st->w, st->h); status(sw); }
}
/* the end of a step: ok = 1 saved, 0 = not finished */
static void rt_step_end(int ok) {
    const RtStep *st = &RT_STEPS[rt_cur];
    rt_ovr.on = 0; rt_ovr.stream = 0; rt_bypass = 0;
    wchar_t m[300];
    if (fw_refused != rt_hold0) {   /* the interlock held this step's block back: nothing reached the scanner (review) */
        rt_put("pending", NULL); user_stopped = 0; if (st->key[0] != 'N') rt_put(st->key, L"E");
        rt_finish(L"RAW check: the safety interlock held this step back (see SAFETY above) - nothing reached the scanner; recorded as E. Please send the Logs folder."); return; }
    if (!ok && user_stopped && !SCANNER_DEAD) { rt_put("pending", NULL); user_stopped = 0; rt_finish(L"RAW check stopped by you - this step has no result and runs again next time."); return; }
    if (SCANNER_DEAD) { rt_put(st->key, L"F"); rt_put("pending", NULL);
        swprintf(m, 300, L"RAW check: step %lc (%ls, %d x %d pixels) STOPPED THE SCANNER. Switch it off and on and restart this program; please send the Logs folder.", (wchar_t)st->key[0], st->stream ? L"streamed" : L"normal", st->w, st->h); log_w(m);
        rt_on = 0; rt_cur = -1; free(rt_ref); rt_ref = NULL; free(rt_ref2); rt_ref2 = NULL; status(L"RAW check: the scanner stopped - switch it off and on, restart."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    rt_put("pending", NULL);
    if (ok && rt_meas_ok) {
        if (st->key[0] != 'N') rt_put(st->key, L"P");
        swprintf(m, 300, L"RAW check: step %lc (%d x %d pixels, %ls) PASSED.", (wchar_t)st->key[0], st->w, st->h, st->stream ? L"streamed" : L"normal"); log_w(m);
        rt_step(); return; }
    if (ok && st->key[0] != 'N') {   /* the RAW came, its picture differs (D); the scanner still answers: the other steps
                                        still tell something (s10: S failed on a limit that was too tight and L never ran) */
        rt_put(st->key, L"D");
        swprintf(m, 300, L"RAW check: step %lc did not pass (the picture is not as expected) - the scanner still answers; going on with the next step.", (wchar_t)st->key[0]); log_w(m);
        rt_step(); return; }
    if (strcmp(st->key, "N")) rt_put(st->key, L"E");
    swprintf(m, 300, L"RAW check: step %lc did not pass (%ls) - the scanner still answers. Please send the Logs folder.", (wchar_t)st->key[0], ok ? L"the picture is not as expected" : raw_start_result != 1 ? L"the scanner software did not accept it" : L"it did not finish"); log_w(m);
    rt_finish(L"RAW check stopped: see the lines above.");
}
static void rt_start(int ask) {
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (simulate == 0 && !raw_patch_ok) { MessageBoxW(hwnd, L"RAW scans are not available with this scanner driver.", APP_TITLE, MB_ICONWARNING); return; }
    if (!frame_ready[frame_sel] || pv_mm_cols <= 0) { MessageBoxW(hwnd, L"Make a preview first (a slide in the holder; the check is centred on the crop, or on the preview without a crop).", APP_TITLE, MB_ICONINFORMATION); return; }
    int left = 0, bad = 0; for (int i = 1; i < RT_N; i++) { char r = rt_get(RT_STEPS[i].key); if (!r) left++; else if (r != 'P') bad++; }
    if (ask && (!left || bad)) {   /* also after a step that did not pass (E/F): otherwise later runs stop at it (review) */
        if (MessageBoxW(hwnd, bad ? L"RAW check: a step did not pass last time (see smart342.ini [rawcheck] and the log). Run all steps again?" : L"RAW check: every step already has a result (see the log). Run all steps again?", APP_TITLE, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) { rt_did_n = 0; rt_finish(L"RAW check: earlier results kept."); return; }
        for (int i = 0; i < RT_N; i++) rt_put(RT_STEPS[i].key, NULL);
    }
    if (ask && MessageBoxW(hwnd,
        L"RAW check\n\n"
        L"The scanner keeps a whole RAW request in its memory: at most 1,179,648 colour pixels (found in its firmware; the "
        L"RAW of 8 Oct was 8 times that and stopped the scanner). A normal scan instead streams through its memory - and a "
        L"RAW can be sent the same way (EXPERIMENTAL).\n\nThis check, at 2000 dpi in colour, centred on the crop:\n"
        L"  N  normal RAW, 600 x 400 pixels (the reference)\n  Q  normal RAW, 1000 x 1000 pixels (the RAW block size)\n"
        L"  S  the area of N again, STREAMED - must be the same picture, no line lost or repeated\n  L  STREAMED RAW, 1200 x 1100 pixels (more than the scanner could keep whole) - N and Q must be found inside it, no line lost or repeated\n\n"
        L"If a step stops the scanner (it stops answering, as on 8 Oct; nothing is written to it), switch it off and on, "
        L"restart this program and choose this check again: it continues after that step.\n\nStart?", APP_TITLE, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return;
    rt_on = 1; rt_did_n = 0; rt_did_q = 0; rt_waits = -1; log_w(L"RAW check started."); rt_show_limits(); set_busy(1);
    rt_step();
}
/* at start: [raw] area; a RAW check step still pending stopped the scanner; the 0.1.15-0.1.18 RAW size test is gone */
static void rt_load(void) {
    wchar_t p[MAX_PATH * 2], v[32]; rt_ini(p);
    long a = GetPrivateProfileIntW(L"raw", L"area", 0, p);
    if (a > 0) { if (a > RAW_AREA_MAX) a = RAW_AREA_MAX; if (a < 65536) a = 65536; raw_area = a; }
    if (GetPrivateProfileIntW(L"raw", L"max_w", 0, p) > 0 || GetPrivateProfileStringW(L"rawtest", L"pending", L"", v, 32, p)) {
        log_w(L"RAW: the limits of the earlier RAW size test are no longer used - the scanner's own memory rule (found in its firmware) sets the RAW block size now.");
        WritePrivateProfileStringW(L"rawtest", L"pending", NULL, p); }
    int held = GetPrivateProfileIntW(L"rawcheck", L"held", 0, p); WritePrivateProfileStringW(L"rawcheck", L"held", NULL, p);
    if (GetPrivateProfileStringW(L"rawcheck", L"pending", L"", v, 32, p) && v[0]) {
        char k[8]; to_utf8(v, k, 8); if (strcmp(k, "N")) rt_put(k, held ? L"E" : L"F"); rt_put("pending", NULL);
        wchar_t m[300];
        if (held) swprintf(m, 300, L"RAW check: step %ls was held back by the safety interlock last time (nothing reached the scanner) - recorded as E.", v);
        else swprintf(m, 300, L"RAW check: step %ls did not finish last time - recorded as the step that stopped the scanner.", v);
        log_w(m);
    }
}
/* ---- focus (experimental): offset of the focus motor from the scanner's own focus position ----
   rawscan.c focus_bp moves motor 1 (FOCUS) by focus_off mm before every scan / preview / RAW while it is not 0
   (limited to -0.5 .. +0.5 mm: the range the motor passes at every start-up and the original's own "defocus" range).
   The setting is kept in smart342.ini [focus] offset_mm. */
static HMENU focus_menu;
static double focus_cur(void) { return pv_lens >= 0 && pv_lens <= 4 ? focus_off_l[pv_lens] : 0; }
static void focus_menu_check(void) {
    if (!focus_menu) return;
    for (int k = 0; k <= 20; k++) CheckMenuItem(focus_menu, IDM_FOCUS0 + k, MF_BYCOMMAND | (fabs((k - 10) * 0.05 - focus_cur()) < 0.001 ? MF_CHECKED : MF_UNCHECKED));
}
/* set the focus offset of lens l (0..4; -1 = all lenses, developer option) */
static void focus_set_l(int l, double v, int save, const wchar_t *why) {
    if (!(v >= -0.5)) v = isnan(v) ? 0 : -0.5; if (v > 0.5) v = 0.5;
    for (int k = 0; k < 5; k++) if (l < 0 || k == l) focus_off_l[k] = v;
    focus_menu_check();
    wchar_t m[300], ln[64]; if (l < 0) swprintf(ln, 64, L"all lenses"); else swprintf(ln, 64, L"%ls", LENS_NAME[l]);
    if (v == 0) swprintf(m, 300, L"Focus (%ls): the scanner's own focus position%ls.", ln, why ? why : L"");
    else swprintf(m, 300, L"Focus (%ls): %+.2f mm from the scanner's own focus position (EXPERIMENTAL)%ls.", ln, v, why ? why : L"");
    log_w(m);
    if (save && l >= 0 && (!autotest || ini_in_autotest)) { wchar_t p[MAX_PATH * 2], t[32], key[16]; ini_path(p); swprintf(t, 32, L"%.2f", v); swprintf(key, 16, L"lens%d", l); WritePrivateProfileStringW(L"focus", key, t, p); }
}
static void focus_set(double v, int save, const wchar_t *why) {
    if (pv_lens < 0 || pv_lens > 4) { log_w(L"Focus: make a preview first - the focus offset belongs to the lens the scanner uses."); return; }
    focus_set_l(pv_lens, v, save, why);
}
/* Focus test: the same small area as RAW at the lens's optical resolution, focus offsets 0, -0.5, -0.25, +0.25,
   +0.5, 0 mm; sharpness = mean squared difference of neighbouring green samples / mean^2 (central 80 %); shift
   against step 1 (rs_register on green). The repeat of 0 at the end gives the repeatability; a change in
   sharpness clearly larger than that means the focus motor focuses this lens. */
static const double FT_OFF[6] = { 0, -0.5, -0.25, 0.25, 0.5, 0 };
#define FT_N 6
static double ft_dpi_used;
static int ft_on, ft_cur, ft_lens; static double ft_sharp[FT_N], ft_mean[FT_N], ft_dx[FT_N], ft_dy[FT_N], ft_ncc[FT_N], ft_keep_off;
static uint16_t *ft_ref; static int ft_w, ft_h;
static void ft_measure(const uint16_t *img, int w, int h, int ch) {
    int g = ch == 3 ? 1 : 0, x0 = w / 10, x1 = w - w / 10, y0 = h / 10, y1 = h - h / 10;
    double se = 0, sm = 0; long long n = 0, nm = 0;
    for (int y = y0; y < y1 - 1; y++) for (int x = x0; x < x1 - 1; x++) {
        double a = img[((size_t)y * w + x) * ch + g], b = img[((size_t)y * w + x + 1) * ch + g], c = img[((size_t)(y + 1) * w + x) * ch + g];
        se += (b - a) * (b - a) + (c - a) * (c - a); n++; sm += a; nm++; }
    double mean = nm ? sm / nm : 0;
    ft_mean[ft_cur] = mean; ft_sharp[ft_cur] = (n && mean > 0) ? se / n / (mean * mean) : 0;
    ft_dx[ft_cur] = ft_dy[ft_cur] = 0; ft_ncc[ft_cur] = 1;
    if (ft_cur == 0) { free(ft_ref); ft_ref = malloc((size_t)w * h * ch * 2); if (ft_ref) memcpy(ft_ref, img, (size_t)w * h * ch * 2); ft_w = w; ft_h = h; }
    else if (ft_ref && w == ft_w && h == ft_h) {
        RsImg a = { ft_ref, w, h, ch, g, NULL }, b = { img, w, h, ch, g, NULL }; double dx = 0, dy = 0;
        ft_ncc[ft_cur] = rs_register(a, b, NULL, 0, &dx, &dy); ft_dx[ft_cur] = dx; ft_dy[ft_cur] = dy; }
}
static void ft_finish(int ok) {
    ft_on = 0; rt_ovr.on = 0; focus_set_l(ft_lens, ft_keep_off, 0, L" (back to the setting before the test)");
    pg_ok = ok; pg_end(); set_busy(0); free(ft_ref); ft_ref = NULL;
    if (!ok) { status(L"Focus test stopped - see the log."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    wchar_t m[300];
    log_w(L"Focus test results (sharpness relative to the first step; shift against the first step in pixels):");
    double s0 = ft_sharp[0] > 0 ? ft_sharp[0] : 1;
    for (int i = 0; i < FT_N; i++) { swprintf(m, 300, L"  focus %+.2f mm: sharpness %.3f, brightness %.0f, shift %+.2f / %+.2f px (match %.3f)", FT_OFF[i], ft_sharp[i] / s0, ft_mean[i], ft_dx[i], ft_dy[i], ft_ncc[i]); log_w(m); }
    double rep = fabs(ft_sharp[5] - ft_sharp[0]) / s0, z = (ft_sharp[0] + ft_sharp[5]) / 2 / s0, spread = 0;
    for (int i = 1; i <= 4; i++) { double v = ft_sharp[i] / s0; if (fabs(v - z) > spread) spread = fabs(v - z); }
    double lim = 3 * rep > 0.03 ? 3 * rep : 0.03;
    if (spread <= lim) {
        swprintf(m, 300, L"Focus test: the sharpness hardly changes with the focus motor (at most %.1f %%, repeatability %.1f %%) - for this lens the focus motor makes no measurable difference; the setting stays as it was.", spread * 100, rep * 100); log_w(m);
        status(L"Focus test finished: no measurable effect.");
    } else {
        /* parabola through the best point and its neighbours (offsets -0.5 -0.25 0 +0.25 +0.5) */
        double xs[5] = { -0.5, -0.25, 0, 0.25, 0.5 }, ys[5] = { ft_sharp[1] / s0, ft_sharp[2] / s0, z, ft_sharp[3] / s0, ft_sharp[4] / s0 };
        int k = 0; for (int i = 1; i < 5; i++) if (ys[i] > ys[k]) k = i;
        double bx = xs[k];
        if (k > 0 && k < 4) { double a = ys[k - 1], b = ys[k], c = ys[k + 1], den = a - 2 * b + c; if (den < 0) bx = xs[k] + 0.25 * 0.5 * (a - c) / den; }
        bx = floor(bx / 0.05 + 0.5) * 0.05;
        swprintf(m, 300, L"Focus test: the focus motor DOES change the sharpness of this lens (by up to %.1f %%, repeatability %.1f %%); sharpest at about %+.2f mm%ls.", spread * 100, rep * 100, bx, (k == 0 || k == 4) ? L" (at the end of the tested range - the best may lie beyond it)" : L"");
        log_w(m);
        ft_keep_off = bx; focus_set_l(ft_lens, bx, 1, L" (from the focus test)");
        status(L"Focus test finished: focus set - see the log.");
    }
    if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
static void ft_step(void) {
    if (ft_cur >= FT_N) { ft_finish(1); return; }
    focus_off_l[ft_lens] = FT_OFF[ft_cur];
    wchar_t m[200]; swprintf(m, 200, L"Focus test %d of %d: focus %+.2f mm...", ft_cur + 1, FT_N, FT_OFF[ft_cur]); log_w(m); status(m);
    char nm[64]; snprintf(nm, sizeof nm, "focustest_%+.2f", FT_OFF[ft_cur]);
    rt_ovr.on = 1; rawj_running = 0; do_raw(nm, ft_dpi_used, "100");
    if (!rawj_running) { log_w(L"Focus test: the RAW could not be started (see above) - test stopped."); ft_finish(0); }   /* as the lens shift test (review) */
}
static void ft_step_end(int ok) {
    if (!ok) { log_w(user_stopped ? L"Focus test stopped by you." : L"Focus test: a step did not finish - test stopped."); user_stopped = 0; ft_finish(0); return; }
    ft_cur++; ft_step();
}
static void ft_start(int ask) {
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!frame_ready[frame_sel] || pv_mm_cols <= 0 || !(pv_opt_dpi > 0)) { MessageBoxW(hwnd, L"Make a preview first and draw a small crop on sharp detail (film grain, a sharp edge).", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!focus_ok) { MessageBoxW(hwnd, L"The focus control is not available with this scanner driver.", APP_TITLE, MB_ICONWARNING); return; }
    double dpi = floor(pv_opt_dpi);
    if (rng_max > 0 && dpi > floor(rng_max * 25.4)) dpi = floor(rng_max * 25.4);
    if (rng_min > 0 && dpi < ceil(rng_min * 25.4)) dpi = ceil(rng_min * 25.4);
    int side = 400; if (raw_max_w > 0 && raw_max_w < side) side = raw_max_w; if (raw_max_h > 0 && raw_max_h < side) side = raw_max_h;
    double mm = side * 25.4 / dpi;
    int crop = SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    double cx = crop ? edit_num(IDC_CX) + edit_num(IDC_CW) / 2 : pv_mm_cols / 2, cy = crop ? edit_num(IDC_CY) + edit_num(IDC_CH) / 2 : pv_mm_rows / 2;
    double wmm = mm, hmm = mm; if (crop) { if (edit_num(IDC_CW) < wmm) wmm = edit_num(IDC_CW); if (edit_num(IDC_CH) < hmm) hmm = edit_num(IDC_CH); }
    if (wmm > pv_mm_cols) wmm = pv_mm_cols; if (hmm > pv_mm_rows) hmm = pv_mm_rows;
    double l = cx - wmm / 2, t = cy - hmm / 2; if (l < 0) l = 0; if (t < 0) t = 0; if (l + wmm > pv_mm_cols) l = pv_mm_cols - wmm; if (t + hmm > pv_mm_rows) t = pv_mm_rows - hmm;
    if (ask) {
        wchar_t q[900]; swprintf(q, 900, L"Focus test (EXPERIMENTAL)\n\nScans the same small area (%.1f x %.1f mm, at the lens's optical %.0f dpi) six times as RAW, with the focus motor "
            L"at 0, -0.5, -0.25, +0.25, +0.5 and again 0 mm from the scanner's own focus position, and measures the sharpness.\n\n"
            L"Put the crop on sharp detail (film grain, a sharp edge). The focus motor stays within the range it moves through at every start-up and the original's own defocus range; "
            L"nothing is written to the scanner. If the sharpness changes, the sharpest offset becomes the focus setting (Tools > Focus offset); if not, this lens does not use the focus motor.\n\n"
            L"About 3 minutes. Start?", wmm, hmm, dpi);
        if (MessageBoxW(hwnd, q, APP_TITLE, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return; }
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", wmm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", hmm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", l); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", t);
    pg_end(); pg_begin(L"Focus test", L"step", FT_N); pg.locked = 1;
    ft_dpi_used = dpi; ft_lens = pv_lens; ft_keep_off = focus_off_l[pv_lens]; ft_on = 1; ft_cur = 0; memset(ft_sharp, 0, sizeof ft_sharp);
    { wchar_t m[200]; swprintf(m, 200, L"Focus test started: %ls, %.0f dpi, %.2f x %.2f mm at %.2f, %.2f.", pv_lens >= 0 && pv_lens <= 4 ? LENS_NAME[pv_lens] : L"lens unknown", dpi, wmm, hmm, l, t); log_w(m); }
    set_busy(1); ft_step();
}
/* ---- lens shift test (EXPERIMENTAL, research): does moving the lens carriage (motor 2) shift the image ACROSS? ----
   The same small area as RAW at the lens's optical resolution (1 RAW pixel = 1 CCD cell across), with the lens
   carriage moved a few motor steps from where the driver puts it, for the scan only (rawscan.c lens_shift_do; the
   steps of 0.1.23 below). The shift of each picture against the first is measured (rs_register, green). If the
   carriage moves the lens sideways along the CCD, the picture moves ACROSS by (1 + m) / (steps per mm x cell size)
   pixels per step (thin lens; L89: 1.24, L110: 1.57) - then pixel-shift sampling across is possible. If it moves
   ALONG the scan, it is not (the carriage already steps there). The repeats show how repeatable the positions are. */
/* 0.1.23: two series of six RAWs, 0, +1, +2, +3, 0, +2 steps - first as before (the carriage goes straight up from the
   driver's lens position to the step), then from above (rawscan.c lens_shift_approach: every picture, step 0 included,
   first B steps above, then down - the driver's own backlash correction then brings it up from 1 mm below). The repeats
   of 0 and +2 show how repeatable the positions are each way, the step sizes how even. */
/* 0.1.26: a series from above with SLOW lens moves (rawscan.c lens_shift_slow: the motion board's constant-rate move 0e
   instead of its ramped 0f; same targets). 0.1.27: the normal and the slow from-above pictures ALTERNATE (hardware s30:
   the picture drifted ~0.3 px/min in the first minutes after the program start - the normal series ran during it, the
   slow one after, so the comparison was confounded). Each mode still visits 0, +1, +2, +3, 0, +2. */
#define LT_N 18
#define LT_APPROACH 8
static const int LT_K[LT_N] = { 0, 1, 2, 3, 0, 2, 0, 0, 1, 1, 2, 2, 3, 3, 0, 0, 2, 2 };
static const int LT_B[LT_N] = { 0, 0, 0, 0, 0, 0, LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH,
                                LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH, LT_APPROACH };
static const int LT_S[LT_N] = { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 };
static const int LT_SERIES[3][6] = { { 0, 1, 2, 3, 4, 5 }, { 6, 8, 10, 12, 14, 16 }, { 7, 9, 11, 13, 15, 17 } };
static const wchar_t *LT_SNAME[3] = { L"straight (as before)", L"from above", L"from above, slow moves" };
#define LT_MAX 24
static int lt_on, lt_cur, lt_slow_used[LT_MAX]; static double lt_dx[LT_MAX], lt_dy[LT_MAX], lt_ncc[LT_MAX], lt_mean[LT_MAX], lt_dpi;
/* 0.3.0: the series run (the lens shift test's, or Set up this scanner's own - lt_wz) */
static const int *lt_k = LT_K, *lt_b = LT_B, *lt_s = LT_S; static int lt_n = LT_N, lt_wz;
static void wz_series_done(int ok);
static uint16_t *lt_ref; static int lt_w, lt_h;
static void lt_measure(const uint16_t *img, int w, int h, int ch) {
    int g = ch == 3 ? 1 : 0; double sm = 0; long long n = 0;
    for (int y = h / 10; y < h - h / 10; y++) for (int x = w / 10; x < w - w / 10; x++) { sm += img[((size_t)y * w + x) * ch + g]; n++; }
    lt_mean[lt_cur] = n ? sm / n : 0; lt_dx[lt_cur] = lt_dy[lt_cur] = 0; lt_ncc[lt_cur] = 1; lt_slow_used[lt_cur] = lens_shift_slow_used;
    if (lt_cur == 0) { free(lt_ref); lt_ref = malloc((size_t)w * h * ch * 2); if (lt_ref) memcpy(lt_ref, img, (size_t)w * h * ch * 2); lt_w = w; lt_h = h; }
    else if (lt_ref && w == lt_w && h == lt_h) {
        RsImg a = { lt_ref, w, h, ch, g, NULL }, b = { img, w, h, ch, g, NULL }; double dx = 0, dy = 0;
        lt_ncc[lt_cur] = rs_register(a, b, NULL, 0, &dx, &dy);
        /* 0.1.25: along, every picture should start where the others did (within ~0.65 line, s27); repeating bars can lock
           a whole bar period off (s27 picture 12: +12.013 lines, period 12.58) - then measured again from the others'
           median along (the across value is kept as the start) */
        if (lt_cur >= 2) {
            double v[LT_MAX]; int nv = 0; for (int i = 0; i < lt_cur; i++) v[nv++] = lt_dy[i];
            for (int i = 1; i < nv; i++) for (int j = i; j > 0 && v[j] < v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
            double med = nv % 2 ? v[nv / 2] : 0.5 * (v[nv / 2 - 1] + v[nv / 2]), tol = fmax(3.0, 0.04 * lt_dpi / 25.4);
            if (fabs(dy - med) > tol) {
                double rx_ = dx, ry_ = med, sc = rs_register(a, b, NULL, 1, &rx_, &ry_);
                wchar_t m[300];
                if (sc >= 0.5 && fabs(ry_ - med) <= tol) {
                    swprintf(m, 300, L"Lens shift test picture %d: measured %+.3f lines along - far from the others (%+.2f; repeating detail can match a whole period off); measured again from there: %+.3f px across, %+.3f along (match %.3f) - used.", lt_cur + 1, dy, med, rx_, ry_, sc);
                    dx = rx_; dy = ry_; lt_ncc[lt_cur] = sc;
                } else swprintf(m, 300, L"WARNING: lens shift test picture %d: measured %+.3f lines along, far from the others (%+.2f), and no match near them - its result is doubtful.", lt_cur + 1, dy, med);
                log_w(m);
            }
        }
        lt_dx[lt_cur] = dx; lt_dy[lt_cur] = dy; }
}
/* one series (base 0 or 6): positions against its own first picture; step sizes, straight-line fit, repeat errors */
static void lt_series(int si, double *slope, double *slope_y, double *dev, double *rep, double *jump_out) {
    const wchar_t *nm = LT_SNAME[si]; const int *ix = LT_SERIES[si];
    double rx[6], ry[6]; int kk[6]; for (int i = 0; i < 6; i++) { rx[i] = lt_dx[ix[i]] - lt_dx[ix[0]]; ry[i] = lt_dy[ix[i]] - lt_dy[ix[0]]; kk[i] = LT_K[ix[i]]; }
    /* 0.1.24: a least-squares line WITH an offset through all six pictures (the first step from rest can be short);
       0.1.25: in rawsr.c sr_fit_series (unit-tested on s27), plus the best single jump - the carriage landing about a
       motor step off and staying there (hardware s21 / s25 / s27) */
    double rms, slj, rmsj, jump; int jp;
    sr_fit_series(rx, kk, 6, slope, &rms, dev, &jp, &jump, &slj, &rmsj);
    { double sy_, rmsy, devy, jy, sjy, rjy; int jpy; sr_fit_series(ry, kk, 6, &sy_, &rmsy, &devy, &jpy, &jy, &sjy, &rjy); *slope_y = sy_; }
    /* 0.1.25: the repeat error ACROSS only - the scan start alternates about +-0.65 line along every other scan (s18,
       s21, s27), which is not the lens; the 0.1.24 verdicts mixed it in */
    *rep = fmax(fabs(rx[4]), fabs(rx[5] - rx[2]));
    wchar_t m[400];
    swprintf(m, 400, L"  %ls: positions across %+.3f / %+.3f / %+.3f / %+.3f px at 0 / +1 / +2 / +3 steps; step sizes %.3f, %.3f, %.3f px; back at 0: %+.3f px, back at +2: %+.3f px (difference to the first +2: %.3f px).",
             nm, rx[0], rx[1], rx[2], rx[3], rx[1], rx[2] - rx[1], rx[3] - rx[2], rx[4], rx[5], fabs(rx[5] - rx[2]));
    log_w(m);
    swprintf(m, 400, L"  %ls: %.3f px per step across (least-squares line through all six pictures; scatter about it %.3f px rms, largest %.3f px), %.3f along; repeat error across %.3f px.", nm, *slope, rms, *dev, *slope_y, *rep);
    log_w(m);
    *jump_out = 0;
    if (jp && fabs(jump) >= 0.5 && rmsj < 0.5 * rms) {
        *jump_out = jump;
        double st = fabs(slj) > 0.05 ? fabs(jump / slj) : 0;
        swprintf(m, 400, L"  %ls: one JUMP of %+.3f px before picture %d (about %.1f motor steps): the carriage landed off and stayed there; allowing for it %.3f px per step, scatter %.3f px rms.", nm, jump, ix[jp] + 1, st, slj, rmsj);
        log_w(m); *slope = slj;
    } else { swprintf(m, 400, L"  %ls: no jump found.", nm); log_w(m); }
}
static void lt_finish(int ok) {
    lt_on = 0; rt_ovr.on = 0; lens_shift_pulses = 0; lens_shift_approach = 0; lens_shift_slow = 0; pg_ok = ok; pg_end(); set_busy(0); free(lt_ref); lt_ref = NULL;
    if (lt_wz) { lt_wz = 0; wz_series_done(ok); return; }
    if (!ok) { status(L"Lens shift test stopped - see the log."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return; }
    wchar_t m[500];
    log_w(L"Lens shift test results (shift of each picture against the first, in pixels = CCD cells across):");
    log_w(L"  (pictures 7 - 18 alternate: normal moves 7, 9, ..., 17; slow moves 8, 10, ..., 18)");
    for (int i = 0; i < LT_N; i++) { swprintf(m, 500, L"  picture %d, lens carriage %+d steps%ls: across %+.3f px, along %+.3f px (match %.3f), brightness %.0f", i + 1, LT_K[i], LT_B[i] ? (LT_S[i] ? (lt_slow_used[i] ? L" (from above, slow moves)" : L" (from above; slow moves NOT used - see the trace)") : L" (from above)") : L"", lt_dx[i], lt_dy[i], lt_ncc[i], lt_mean[i]); log_w(m); }
    double s0, y0, d0, r0, s1, y1, d1, r1, s2, y2, d2, r2, j0, j1, j2;
    lt_series(0, &s0, &y0, &d0, &r0, &j0); lt_series(1, &s1, &y1, &d1, &r1, &j1); lt_series(2, &s2, &y2, &d2, &r2, &j2);
    (void)s2; (void)y2; (void)j0;
    double pred = (lens_shift_factor > 0 && lens_pitch > 0) ? (1 + lens_mag) / (lens_shift_factor * lens_pitch) : 0;
    swprintf(m, 500, L"Lens shift test: a sideways lens motion would give %.3f px per step (thin lens, magnification %.4f).", pred, lens_mag); log_w(m);
    double px = s1, py = y1, ax = fabs(px), ay = fabs(py), lim = fmax(0.05, r1 / 3);
    if (ax > lim && ay < 0.15 * ax && pred > 0 && fabs(ax - pred) < 0.35 * pred)
        swprintf(m, 500, L"Lens shift test: the lens carriage moves the picture ACROSS, as a sideways lens would (%.0f %% of the expected amount) - pixel shift across works with this lens.", 100 * ax / pred);
    else if (ay > lim && ax < 0.15 * ay)
        swprintf(m, 500, L"Lens shift test: the lens carriage moves the picture ALONG the scan (%.3f px per step), not across - pixel shift across is not possible this way.", ay);
    else if (ax <= lim && ay <= lim)
        swprintf(m, 500, L"Lens shift test: no measurable movement of the picture (at most %.3f px per step) - the steps may be taken up by play in the mechanism; try again or send the logs.", fmax(ax, ay));
    else swprintf(m, 500, L"Lens shift test: unclear result (across %.3f, along %.3f px per step, expected %.3f across) - please send the Logs folder.", px, py, pred);
    log_w(m);
    /* a difference counts only beyond 0.05 px and 30 % (wintest: the simulator's noise alone gave 0.003 against 0.006 px) */
    if (r1 < r0 - 0.05 && r1 < 0.7 * r0) swprintf(m, 500, L"Lens shift test: from above the positions are more repeatable (repeat error %.3f px against %.3f px straight) - pixel shift approaches from above (the setting it uses).", r1, r0);
    else if (r0 < r1 - 0.05 && r0 < 0.7 * r1) swprintf(m, 500, L"Lens shift test: straight the positions are more repeatable (repeat error %.3f px against %.3f px from above) - pixel shift approaches from above unless pixel_shift_approach=0 is set in smart342.ini [window]; please send the Logs folder.", r0, r1);
    else if (r1 < 0.15 && r0 < 0.15) swprintf(m, 500, L"Lens shift test: positions repeatable both ways (repeat error %.3f px from above, %.3f px straight).", r1, r0);
    else swprintf(m, 500, L"Lens shift test: positions are NOT repeatable either way (repeat error %.3f px from above, %.3f px straight) - pixel shift then relies on measuring every pass (it does). Please send the Logs folder.", r1, r0);
    log_w(m);
    swprintf(m, 500, L"Lens shift test: largest deviation of a picture from its series's fitted line (uneven steps, repeats and slips) %.3f px from above, %.3f px straight (pixel shift measures every pass, so this costs only extra passes).", d1, d0);
    log_w(m);
    /* 0.1.26: normal against slow moves, both from above */
    { int ns = 0, nl = 0; for (int i = 0; i < LT_N; i++) { ns += LT_S[i] && lt_slow_used[i]; nl += !LT_S[i] && lt_slow_used[i]; }
      if (ns < 6 || nl) swprintf(m, 500, L"Lens shift test: slow moves were used for %d of the 6 slow pictures and %d of the others (should be 6 and 0) - no comparison (please send the Logs folder).", ns, nl);
      else if (j1 != 0 && j2 == 0) swprintf(m, 500, L"Lens shift test: SLOW moves: no jump, normal moves: a jump of %+.3f px (from above both). Repeat error %.3f px slow, %.3f px normal; largest deviation %.3f / %.3f px. One run - repeat it before relying on it.", j1, r2, r1, d2, d1);
      else if (j1 == 0 && j2 != 0) swprintf(m, 500, L"Lens shift test: slow moves jumped (%+.3f px), normal moves did not (from above both) - slow moves do not prevent the slips. Repeat error %.3f px slow, %.3f px normal.", j2, r2, r1);
      else if (j1 != 0 && j2 != 0) swprintf(m, 500, L"Lens shift test: both jumped (normal %+.3f px, slow %+.3f px) - slow moves do not prevent the slips. Repeat error %.3f px slow, %.3f px normal.", j1, j2, r2, r1);
      else swprintf(m, 500, L"Lens shift test: no jump either way this time (from above both). Repeat error %.3f px slow, %.3f px normal; largest deviation %.3f / %.3f px.", r2, r1, d2, d1);
      log_w(m); }
    status(L"Lens shift test finished - see the log.");
    double b0 = lt_mean[0] > 0 ? lt_mean[0] : 1; swprintf(m, 500, L"Lens shift test: brightness at +3 steps %.2f %% of the first (light fall-off check).", 100 * lt_mean[3] / b0); log_w(m);
    if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
static void lt_step(void) {
    if (lt_cur >= lt_n) { lt_finish(1); return; }
    lens_shift_pulses = lt_k[lt_cur]; lens_shift_approach = lt_b[lt_cur]; lens_shift_slow = lt_s[lt_cur]; lens_shift_slow_used = 0;
    const wchar_t *what = lt_wz ? L"Setting up: lens step" : L"Lens shift test";
    wchar_t m[200]; swprintf(m, 200, L"%ls %d of %d: lens carriage %+d steps%ls...", what, lt_cur + 1, lt_n, lt_k[lt_cur], lt_b[lt_cur] ? (lt_s[lt_cur] ? L" (from above, slow moves)" : L" (from above)") : L""); if (lt_wz) log_d(m); else log_w(m); status(m);
    char nm[64]; snprintf(nm, sizeof nm, "%s_%02d_%+d%s", lt_wz ? "setup" : "lensshift", lt_cur + 1, lt_k[lt_cur], lt_b[lt_cur] ? (lt_s[lt_cur] ? "_above_slow" : "_above") : "");
    rt_ovr.on = 1; rawj_running = 0; do_raw(nm, lt_dpi, "100");
    if (!rawj_running) { log_w(L"Lens step series: the RAW could not be started (see above) - stopped."); lt_finish(0); }   /* review: do_raw refused - the test must not stay busy */
}
static void lt_step_end(int ok) {
    if (!ok) { log_w(user_stopped ? L"Lens step series stopped by you." : L"Lens step series: a step did not finish - stopped."); user_stopped = 0; lt_finish(0); return; }
    lt_cur++; lt_step();
}
static void lt_start(int ask) {
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!frame_ready[frame_sel] || pv_mm_cols <= 0 || !(pv_opt_dpi > 0)) { MessageBoxW(hwnd, L"Make a preview first and draw a small crop on sharp detail (film grain, a sharp edge).", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!focus_ok) { MessageBoxW(hwnd, L"The lens control is not available with this scanner driver.", APP_TITLE, MB_ICONWARNING); return; }
    double dpi = floor(pv_opt_dpi);
    if (rng_max > 0 && dpi > floor(rng_max * 25.4)) dpi = floor(rng_max * 25.4);
    if (rng_min > 0 && dpi < ceil(rng_min * 25.4)) dpi = ceil(rng_min * 25.4);
    int side = 400; if (raw_max_w > 0 && raw_max_w < side) side = raw_max_w; if (raw_max_h > 0 && raw_max_h < side) side = raw_max_h;
    double mm = side * 25.4 / dpi;
    int crop = SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED;
    double cx = crop ? edit_num(IDC_CX) + edit_num(IDC_CW) / 2 : pv_mm_cols / 2, cy = crop ? edit_num(IDC_CY) + edit_num(IDC_CH) / 2 : pv_mm_rows / 2;
    double wmm = mm, hmm = mm; if (crop) { if (edit_num(IDC_CW) < wmm) wmm = edit_num(IDC_CW); if (edit_num(IDC_CH) < hmm) hmm = edit_num(IDC_CH); }
    if (wmm > pv_mm_cols) wmm = pv_mm_cols; if (hmm > pv_mm_rows) hmm = pv_mm_rows;
    double l = cx - wmm / 2, t = cy - hmm / 2; if (l < 0) l = 0; if (t < 0) t = 0; if (l + wmm > pv_mm_cols) l = pv_mm_cols - wmm; if (t + hmm > pv_mm_rows) t = pv_mm_rows - hmm;
    if (ask) {
        wchar_t q[1400]; swprintf(q, 1400, L"Lens shift test (EXPERIMENTAL, research)\n\nScans the same small area (%.1f x %.1f mm at the lens's optical %.0f dpi) as RAW with the lens carriage "
            L"at 0, +1, +2, +3, 0, +2 motor steps (1 step = 6.35 micrometres) - first straight to each step; then from above (8 steps above first, then down - the scanner driver then brings the carriage up from 1 mm below, as it does for the lens position itself), each step twice in a row: once with normal moves and once with SLOW moves (the motion board's constant-rate move, same positions), so any drift affects both alike - eighteen RAWs in all - and measures where the picture lands each time.\n\n"
            L"It shows how repeatable the lens positions are each way and whether slow moves avoid the carriage's slips (for pixel shift). Put the crop on sharp detail. Only the driver's own motor commands are used; nothing is written to the scanner. About 8 minutes. Start?", wmm, hmm, dpi);
        if (MessageBoxW(hwnd, q, APP_TITLE, MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return; }
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", wmm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", hmm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", l); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", t);
    pg_end(); pg_begin(L"Lens shift test", L"picture", LT_N); pg.locked = 1;
    lt_k = LT_K; lt_b = LT_B; lt_s = LT_S; lt_n = LT_N; lt_wz = 0;
    lt_dpi = dpi; lt_on = 1; lt_cur = 0; memset(lt_dx, 0, sizeof lt_dx); memset(lt_dy, 0, sizeof lt_dy); memset(lt_slow_used, 0, sizeof lt_slow_used);
    { wchar_t m[200]; swprintf(m, 200, L"Lens shift test started: %ls, %.0f dpi, %.2f x %.2f mm at %.2f, %.2f.", pv_lens >= 0 && pv_lens <= 4 ? LENS_NAME[pv_lens] : L"lens unknown", dpi, wmm, hmm, l, t); log_w(m); }
    set_busy(1); lt_step();
}
/* ---- 0.3.0: Set up this scanner ----
   Measures, for each lens the owner has a holder for, how the lens carriage moves the picture (what pixel shift relies
   on): the holder is chosen from a list, the owner puts film in it, the program makes a preview, picks a small area with
   the most detail itself, and scans it as RAW at the lens's own resolution with the lens carriage at 0, +1, +2, +3, +4,
   +6, +8, 0, +2, +5 steps - approached as pixel shift approaches them (from above, slow moves if that setting is on). The
   shift of each picture is measured (as the lens shift test does) and fitted (rawsr.c sr_calib_fit): pixels per step,
   drift per picture, scatter, the first step, repeatability. The results are saved in smart342.ini under this scanner's
   number ([scanner 95171130] lens2_px_step ...) and used by pixel shift for that lens. Only the scanner software's own
   motor commands are used; nothing is written to the scanner. Offered once at the first start with a scanner; Tools >
   Set up this scanner runs it again. */
#define WZ_N 10
static const int WZ_K[WZ_N] = { 0, 1, 2, 3, 4, 6, 8, 0, 2, 5 };
static int wz_b[WZ_N], wz_s[WZ_N];
static const struct { int type, lens; const wchar_t *what; } WZ_H[] = {
    { 7, 2, L"35 mm lens - holder 4 T35mm H" }, { 1, 2, L"35 mm lens - holder T 35mm" },
    { 2, 1, L"6x6 / 6x7 lens - holder T 6 x 6" }, { 3, 1, L"6x6 / 6x7 lens - holder T 6 x 7" }, { 9, 1, L"6x6 / 6x7 lens - holder 3 T6x7 H" },
    { 4, 0, L"4x5 lens - holder T 4 x 5" }, { 5, 3, L"8x10 lens - holder T 8 x 10 H" } };
#define WZ_NH ((int)(sizeof WZ_H / sizeof *WZ_H))
static int wz_on, wz_state, wz_type = -1, wz_lens = -1, wz_nmeasured; static int wz_test_type = -1;   /* developer option (autotest): --setup-test HOLDERTYPE */
enum { WZ_IDLE, WZ_PREVIEW, WZ_SERIES, WZ_CHART_PREVIEW, WZ_CHART_CROP, WZ_CHART_RAW };
static int ch_on, wz_chart_opt;   /* the chart RAW is being made; developer option (autotest) --setup-chart: measure the (simulated) chart after the lens */
static HWND wz_panel;
/* review: the simulator (Simulator\flash14.bin is a real scanner's file) keeps its own section, so a setup made in it never
   counts for the real scanner of that number */
static void wz_sec(wchar_t *sec) { swprintf(sec, 64, L"%lsscanner %u", simulate ? L"simulator " : L"", (unsigned)serial_flash_no); }
static double wz_get(int lens, const wchar_t *key, double dflt) {
    wchar_t p[MAX_PATH * 2], sec[64], k[64], v[64]; ini_path(p); wz_sec(sec); swprintf(k, 64, L"lens%d_%ls", lens, key);
    GetPrivateProfileStringW(sec, k, L"", v, 64, p); if (!v[0]) return dflt; for (wchar_t *c = v; *c; c++) if (*c == L',') *c = L'.'; return _wtof(v);
}
static void wz_put(int lens, const wchar_t *key, double v, const wchar_t *fmt) {
    wchar_t p[MAX_PATH * 2], sec[64], k[64], t[64]; ini_path(p); wz_sec(sec); swprintf(k, 64, L"lens%d_%ls", lens, key); swprintf(t, 64, fmt, v);
    WritePrivateProfileStringW(sec, k, t, p);
}
static int wz_measured(int lens) { return wz_get(lens, L"px_step", 0) != 0; }
static void wz_end(const wchar_t *why) {
    wz_on = 0; wz_state = WZ_IDLE; rt_ovr.on = 0; if (wz_panel) { DestroyWindow(wz_panel); wz_panel = NULL; }
    if (why) log_w(why);
    status(L"Ready."); set_busy(0);
    if (autotest && wz_test_type >= 0) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
static void wz_menu(void);
/* the area with the most detail in BOTH directions (min of the mean gradients across and along), in the preview as
   shown (crop coordinates, as a drag), away from the edges; windows with clipped pixels count less */
static int wz_pick_crop(double side_mm, double *l, double *t, double *score) {
    if (!pbits || pw < 16 || ph < 16 || !(pv_mm_cols > 0) || !(pv_mm_rows > 0)) return 0;
    double mmx = pv_mm_cols / pw, mmy = pv_mm_rows / ph;
    int ww = (int)(side_mm / mmx + 0.5), wh = (int)(side_mm / mmy + 0.5);
    if (ww < 4 || wh < 4 || ww > pw - 4 || wh > ph - 4) return 0;
    int stride = (pw * 3 + 3) & ~3, W1 = pw + 1;
    double *ix = calloc((size_t)W1 * (ph + 1), sizeof(double)), *iy = calloc((size_t)W1 * (ph + 1), sizeof(double)), *ic = calloc((size_t)W1 * (ph + 1), sizeof(double));
    if (!ix || !iy || !ic) { free(ix); free(iy); free(ic); return 0; }
    for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
        const uint8_t *q = pbits + (size_t)y * stride + x * 3; int g = q[1];
        double gx = x + 1 < pw ? fabs((double)q[4] - g) : 0, gy = y + 1 < ph ? fabs((double)q[stride + 1] - g) : 0, cl = (g >= 254 || g <= 1) ? 1 : 0;
        size_t o = (size_t)(y + 1) * W1 + (x + 1), a = (size_t)y * W1 + (x + 1), b = (size_t)(y + 1) * W1 + x, c = (size_t)y * W1 + x;
        ix[o] = gx + ix[a] + ix[b] - ix[c]; iy[o] = gy + iy[a] + iy[b] - iy[c]; ic[o] = cl + ic[a] + ic[b] - ic[c];
    }
    #define BOX(I, x0, y0) (I[(size_t)((y0) + wh) * W1 + (x0) + ww] - I[(size_t)(y0) * W1 + (x0) + ww] - I[(size_t)((y0) + wh) * W1 + (x0)] + I[(size_t)(y0) * W1 + (x0)])
    int mx = pw / 25 + 1, my = ph / 25 + 1, sx = ww / 4 > 0 ? ww / 4 : 1, sy = wh / 4 > 0 ? wh / 4 : 1;
    double best = -1; int bx = -1, by = -1; double area = (double)ww * wh;
    for (int y0 = my; y0 + wh <= ph - my; y0 += sy) for (int x0 = mx; x0 + ww <= pw - mx; x0 += sx) {
        double a = BOX(ix, x0, y0) / area, b = BOX(iy, x0, y0) / area, clip = BOX(ic, x0, y0) / area;
        double sc = (a < b ? a : b) * (1 - 2 * clip);
        if (sc > best) { best = sc; bx = x0; by = y0; }
    }
    #undef BOX
    free(ix); free(iy); free(ic);
    if (bx < 0) return 0;
    *l = bx * mmx; *t = by * mmy; *score = best; return 1;
}
static void wz_preview_done(void) {
    wz_state = WZ_IDLE;
    if (!frame_ready[frame_sel] || !(pv_opt_dpi > 0) || pv_lens < 0 || pv_lens > 3) { wz_end(L"Set up this scanner: the preview or its lens is not known - stopped. Please send the Logs folder."); return; }
    { int want = -1; for (int i = 0; i < WZ_NH; i++) if (WZ_H[i].type == wz_type) want = WZ_H[i].lens;
        if (want >= 0 && want != pv_lens) { wchar_t m[300]; swprintf(m, 300, L"Set up this scanner: this holder used the %ls, not the expected one - measured as the %ls.", LENS_NAME[pv_lens], LENS_NAME[pv_lens]); log_w(m); } }
    wz_lens = pv_lens;
    double dpi = floor(pv_opt_dpi);   /* review: the lens's own resolution, scale 100 - not the allowed range of an earlier crop / scale */
    int side = 400; if (raw_max_w > 0 && raw_max_w < side) side = raw_max_w; if (raw_max_h > 0 && raw_max_h < side) side = raw_max_h;
    double mm = side * 25.4 / dpi, l = 0, t = 0, sc = 0;
    if (mm > pv_mm_cols * 0.5) mm = pv_mm_cols * 0.5; if (mm > pv_mm_rows * 0.5) mm = pv_mm_rows * 0.5;
    if (!wz_pick_crop(mm, &l, &t, &sc)) { wz_end(L"Set up this scanner: no area could be chosen on the preview - stopped."); return; }
    { wchar_t m[300]; swprintf(m, 300, L"Set up this scanner: area with the most detail: %.2f x %.2f mm at %.2f, %.2f (detail %.1f grey levels per pixel).", mm, mm, l, t, sc); log_w(m); }
    if (sc < 1.5) {
        wchar_t m[600]; swprintf(m, 600, L"The preview shows very little fine detail (%.1f grey levels per pixel at best). The measurement needs a picture with fine detail in both directions - an ordinary photograph, text, or a strip with sprocket holes.\n\nPut another piece of film in, or measure anyway?", sc);
        const wchar_t *b[3] = { L"Measure anyway", L"Choose again", L"Stop the setup" };
        int a = ask_box(m, b, 3, 0, 0, 0, NULL);
        if (a == 1) { wz_menu(); return; } if (a != 0) { wz_end(L"Set up this scanner stopped."); return; }
    }
    /* the crop on the preview, as the user would draw it */
    set_edit_mm(IDC_CX, l); set_edit_mm(IDC_CY, t); set_edit_mm(IDC_CW, mm); set_edit_mm(IDC_CH, mm);
    SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0); InvalidateRect(hprev, NULL, FALSE);
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", mm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", mm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", l); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", t);
    for (int i = 0; i < WZ_N; i++) { wz_b[i] = ps_approach; wz_s[i] = ps_slow; }
    lt_k = WZ_K; lt_b = wz_b; lt_s = wz_s; lt_n = WZ_N; lt_wz = 1;
    pg_end(); pg_begin(L"Setting up the lens", L"picture", WZ_N); pg.locked = 1;
    lt_dpi = dpi; lt_on = 1; lt_cur = 0; memset(lt_dx, 0, sizeof lt_dx); memset(lt_dy, 0, sizeof lt_dy); memset(lt_slow_used, 0, sizeof lt_slow_used);
    { wchar_t m[300]; swprintf(m, 300, L"Set up this scanner: measuring the %ls - %d RAWs at %.0f dpi (about %d minutes).", LENS_NAME[wz_lens], WZ_N, dpi, 1 + WZ_N * 40 / 60); log_w(m); }
    wz_state = WZ_SERIES; set_busy(1); lt_step();
}
static void wz_series_done(int ok) {
    wz_state = WZ_IDLE;
    if (!ok) { wz_end(L"Set up this scanner stopped: the lens step series did not finish (nothing was saved for this lens)."); return; }
    SrCalib c; double dx[WZ_N], dy[WZ_N];
    for (int i = 0; i < WZ_N; i++) { dx[i] = lt_dx[i]; dy[i] = lt_dy[i]; }
    sr_calib_fit(dx, dy, WZ_K, WZ_N, &c);
    double pred = (lens_shift_factor > 0 && lens_pitch > 0) ? (1 + lens_mag) / (lens_shift_factor * lens_pitch) : 0;
    wchar_t m[900];
    for (int i = 0; i < WZ_N; i++) { swprintf(m, 900, L"  picture %d, lens carriage %+d steps: across %+.3f px, along %+.3f px (match %.3f)%ls", i + 1, WZ_K[i], lt_dx[i], lt_dy[i], lt_ncc[i], lt_s[i] && !lt_slow_used[i] ? L" (slow moves NOT used)" : L""); log_d(m); }
    swprintf(m, 900, L"Set up this scanner - %ls: %.3f px per step across (thin-lens estimate %.3f), drift %+.3f px per picture, scatter %.3f px rms (largest %.3f), first step %.2f of a step, repeat error %.3f px, along varies by %.3f px. %ls",
             LENS_NAME[wz_lens], c.slope, pred, c.drift, c.rms, c.maxdev, c.first, c.repeat, c.along,
             c.ok ? L"Pixel shift uses these values with this lens; saved." : L"These steps were too uneven to rely on - saved as measured; pixel shift keeps using general values for this lens (it measures every pass anyway).");
    log_w(m);
    { wchar_t p[MAX_PATH * 2], sec[64], v[64]; ini_path(p); wz_sec(sec);
      SYSTEMTIME st; GetLocalTime(&st);
      wz_put(wz_lens, L"px_step", c.slope, L"%.4f"); wz_put(wz_lens, L"drift", c.drift, L"%.4f"); wz_put(wz_lens, L"scatter", c.rms, L"%.4f");
      wz_put(wz_lens, L"largest", c.maxdev, L"%.4f"); wz_put(wz_lens, L"first", c.first, L"%.3f"); wz_put(wz_lens, L"repeat", c.repeat, L"%.4f");
      wz_put(wz_lens, L"along", c.along, L"%.4f"); wz_put(wz_lens, L"ok", c.ok, L"%.0f"); wz_put(wz_lens, L"approach", ps_approach, L"%.0f"); wz_put(wz_lens, L"slow", ps_slow, L"%.0f");
      wz_put(wz_lens, L"holder", wz_type, L"%.0f");
      swprintf(v, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute); { wchar_t k[64]; swprintf(k, 64, L"lens%d_date", wz_lens); WritePrivateProfileStringW(sec, k, v, p); }
      WritePrivateProfileStringW(sec, L"setup", L"done", p); }
    wz_nmeasured++;
    if (autotest && wz_test_type >= 0 && !wz_chart_opt) { wz_end(L"Set up this scanner: test run finished."); return; }
    {   /* optional: the sharpness with the Scitex focus chart (RAW sharpening's model) */
        wchar_t q[900], w[64]; to_w(cas_holder(wz_type) ? cas_holder(wz_type)->name : "", w, 64);
        swprintf(q, 900, L"%ls: lens steps measured (%.2f px per step).\n\nOPTIONAL - if you have the Scitex focus chart: the program can also measure how sharp this lens and the CCD are, for RAW sharpening (the SHARP file). Put the chart in the %ls holder instead of the film. After the preview, draw a crop around the chart's two blocks of fine bars (bars running up and down next to bars running across, as on the chart) and press Measure.\n\nWithout the chart, RAW sharpening uses a model measured on another Smart 342L.", LENS_NAME[wz_lens], fabs(c.slope), w);
        const wchar_t *b[2] = { L"Measure with the focus chart", L"Skip - no chart" };
        if (ask_box(q, b, 2, 0, 0, wz_chart_opt ? 0 : 1, NULL) == 0) {
            if (!(autotest && wz_test_type >= 0)) { const wchar_t *b2[2] = { L"Start", L"Skip" };
                if (ask_box(L"Put the Scitex focus chart in the holder, close the lid, then press Start.", b2, 2, 0, 0, 0, NULL) != 0) goto chart_skipped; }
            if (simulate && autotest) _putenv("SIM_CHART_NOW=1");   /* tests: the simulated chart is "put in" now (sim.c) */
            wz_state = WZ_CHART_PREVIEW; log_w(L"Set up this scanner: focus chart - preview."); do_preview(); return;
        }
    }
    chart_skipped:
    if (autotest && wz_test_type >= 0) { wz_end(L"Set up this scanner: test run finished."); return; }
    { wchar_t q[900]; swprintf(q, 900, L"%ls measured:\n\n  %.2f pixels per lens step (%ls)\n  scatter %.2f px, repeat error %.2f px, drift %+.2f px per picture\n\n%ls\n\nMeasure another lens?", LENS_NAME[wz_lens], fabs(c.slope), c.slope < 0 ? L"the picture moves to the left in the RAW" : L"the picture moves to the right in the RAW", c.rms, c.repeat, c.drift,
               c.ok ? L"Pixel shift will use these values with this lens." : L"The steps were too uneven to rely on - pixel shift keeps using general values for this lens. A picture with more detail, or a new run after the scanner has warmed up, may help.");
      const wchar_t *b[2] = { L"Measure another lens", L"Finish" };
      if (ask_box(q, b, 2, 0, 0, 1, NULL) == 0) { wz_menu(); return; } }
    wz_end(L"Set up this scanner finished. Tools > Set up this scanner runs it again at any time; Tools > Fix streaks measures this scanner's clean gain vectors (with an empty frame).");
}
/* the chart step: a small window beside the main one (the preview stays usable for drawing the crop) */
static LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && HIWORD(wp) == BN_CLICKED) { PostMessageW(hwnd, WM_COMMAND, LOWORD(wp), 0); DestroyWindow(h); wz_panel = NULL; return 0; }
    if (msg == WM_CLOSE) { PostMessageW(hwnd, WM_COMMAND, IDM_CHART_SKIP, 0); DestroyWindow(h); wz_panel = NULL; return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}
static void wz_chart_panel(void) {
    RECT pr; GetWindowRect(hwnd, &pr); int W = S(330), H = S(170);
    wz_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"S342Panel", L"Set up this scanner - focus chart", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                               pr.right - W - S(30), pr.top + S(60), W, H, hwnd, NULL, GetModuleHandleW(NULL), NULL);
    HWND st = CreateWindowExW(0, L"STATIC", L"Draw a crop around the chart's two blocks of fine bars on the preview (bars running up and down next to bars running across), then press Measure.", WS_CHILD | WS_VISIBLE, S(12), S(10), W - S(30), S(70), wz_panel, NULL, NULL, NULL);
    SendMessageW(st, WM_SETFONT, (WPARAM)font, TRUE);
    HWND b1 = CreateWindowExW(0, L"BUTTON", L"Measure", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(12), S(88), S(140), S(28), wz_panel, (HMENU)(INT_PTR)IDM_CHART_MEASURE, NULL, NULL);
    HWND b2 = CreateWindowExW(0, L"BUTTON", L"Skip the chart", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(162), S(88), S(140), S(28), wz_panel, (HMENU)(INT_PTR)IDM_CHART_SKIP, NULL, NULL);
    SendMessageW(b1, WM_SETFONT, (WPARAM)font, TRUE); SendMessageW(b2, WM_SETFONT, (WPARAM)font, TRUE);
}
static void wz_chart_preview_done(void) {
    wz_state = WZ_CHART_CROP; status(L"Set up this scanner: draw a crop around the chart's bar blocks, then press Measure.");
    if (autotest && wz_test_type >= 0) {   /* the simulated chart: its blocks lie in the middle of the frame */
        double w = 6, l = pv_mm_cols / 2 - 3, t = pv_mm_rows / 2 - 3;
        set_edit_mm(IDC_CX, l); set_edit_mm(IDC_CY, t); set_edit_mm(IDC_CW, w); set_edit_mm(IDC_CH, w);
        SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
        PostMessageW(hwnd, WM_COMMAND, IDM_CHART_MEASURE, 0); return; }
    wz_chart_panel();
}
static void wz_chart_skip(void) {
    if (wz_state != WZ_CHART_CROP) return;
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); wz_chart_panel(); return; }
    wz_state = WZ_IDLE; log_w(L"Set up this scanner: focus chart skipped.");
    if (autotest && wz_test_type >= 0) { wz_end(L"Set up this scanner: test run finished."); return; }
    wz_menu();
}
static void wz_chart_measure(void) {
    if (wz_state != WZ_CHART_CROP) return;
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); wz_chart_panel(); return; }
    if (pv_lens != wz_lens || !frame_ready[frame_sel] || !(pv_opt_dpi > 0)) {   /* review: the chart must be measured with the lens just set up */
        wchar_t m[300]; swprintf(m, 300, L"The preview is not one made with the %ls (the lens just measured). Choose that holder again and make a preview of the chart, then press Measure.", LENS_NAME[wz_lens]);
        MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION); wz_chart_panel(); return; }
    if (SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) != BST_CHECKED || edit_num(IDC_CW) <= 0 || edit_num(IDC_CH) <= 0) {
        MessageBoxW(hwnd, L"Draw a crop around the two bar blocks first.", APP_TITLE, MB_ICONINFORMATION); wz_chart_panel(); return; }
    double dpi = floor(pv_opt_dpi), wmm = edit_num(IDC_CW), hmm = edit_num(IDC_CH);
    double px = wmm * dpi / 25.4, py = hmm * dpi / 25.4;
    if (!raw_fits((long long)(px + 1), (long long)(py + 1)) || px * py > 1000000) { MessageBoxW(hwnd, L"The crop is too large for one RAW at the lens's resolution - draw it just around the two bar blocks (a few millimetres).", APP_TITLE, MB_ICONINFORMATION); wz_chart_panel(); return; }
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", wmm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", hmm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", edit_num(IDC_CX)); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", edit_num(IDC_CY));
    { wchar_t m[300]; swprintf(m, 300, L"Set up this scanner: focus chart RAW, %.2f x %.2f mm at %.0f dpi.", wmm, hmm, dpi); log_w(m); }
    wz_state = WZ_CHART_RAW; ch_on = 1; rt_ovr.on = 1; rawj_running = 0; set_busy(1); do_raw("setup_chart", dpi, "100");
    if (!rawj_running) { ch_on = 0; rt_ovr.on = 0; set_busy(0); wz_end(L"Set up this scanner: the chart RAW could not be started (see above)."); }
}
static ChFit ch_fit; static int ch_ok; static ChBars ch_a, ch_b;
static void ch_measure(const uint16_t *img, int w, int h, int ch) {
    wchar_t m[900]; ch_ok = 0;
    ChBox bx, by; int nf = chart_find(img, w, h, ch, &bx, &by);
    if (!bx.ok || !by.ok) { swprintf(m, 900, L"Focus chart: %ls not found in the crop - draw the crop around both bar blocks (bars running up and down, and bars running across).", !bx.ok && !by.ok ? L"no bar block" : !bx.ok ? L"the block of bars running up and down was" : L"the block of bars running across was"); log_w(m); return; }
    (void)nf;
    double px = 25.4 / rawj.dpi;
    if (chart_bars(img, w, h, ch, &bx, 0, px, &ch_a) || chart_bars(img, w, h, ch, &by, 1, px, &ch_b)) { log_w(L"Focus chart: the bars could not be measured."); return; }
    swprintf(m, 900, L"Focus chart: bars %.2f lp/mm across (block %d-%d x %d-%d), %.2f lp/mm along (block %d-%d x %d-%d); duty %.2f / %.2f.", ch_a.f0, bx.x0, bx.x1, bx.y0, bx.y1, ch_b.f0, by.x0, by.x1, by.y0, by.y1, ch_a.duty, ch_b.duty); log_d(m);
    { int k = swprintf(m, 900, L"  across:"); for (int i = 0; i < ch_a.nh; i++) k += swprintf(m + k, 900 - k, L" %.1f:%.3f", ch_a.f[i], ch_a.v[i]); log_d(m);
      k = swprintf(m, 900, L"  along:"); for (int i = 0; i < ch_b.nh; i++) k += swprintf(m + k, 900 - k, L" %.1f:%.3f", ch_b.f[i], ch_b.v[i]); log_d(m); }
    double mag = 0.007 * pv_opt_dpi / 25.4, fnum = rsh_lens_fnum(pv_lens), mot = 25.4 / rawj.dpi * 1000;   /* continuous motion (a test RAW) */
    if (chart_fit(&ch_a, &ch_b, mag, fnum, mot, 0.54, &ch_fit)) { log_w(L"Focus chart: too few bar harmonics to fit (is it the Scitex focus chart, sharp and well lit?)."); return; }
    ch_ok = ch_fit.rms <= 0.05;
    if (fabs(ch_a.f0 - 8.33) > 0.6 || fabs(ch_b.f0 - 8.33) > 0.6) log_w(L"Focus chart: note - the bars are not the 8.3 lp/mm of the Scitex focus chart; the fit is used anyway.");
}
static void wz_chart_done(int ok) {
    wz_state = WZ_IDLE; wchar_t m[900];
    if (!ok) { wz_end(L"Set up this scanner: the chart RAW did not finish - nothing saved for the sharpness."); return; }
    if (ch_ok) {
        swprintf(m, 900, L"Set up this scanner - %ls, sharpness from the focus chart: residual blur %.2f um across / %.2f um along at the CCD, halo %.2f / %.2f of %.1f um (fit %.3f rms over %d harmonics). RAW sharpening uses it for this lens; saved.", LENS_NAME[wz_lens], ch_fit.sx, ch_fit.sy, ch_fit.ax, ch_fit.ay, ch_fit.sh, ch_fit.rms, ch_fit.npts);
        log_w(m);
        wz_put(wz_lens, L"sharp_sx", ch_fit.sx, L"%.4f"); wz_put(wz_lens, L"sharp_sy", ch_fit.sy, L"%.4f"); wz_put(wz_lens, L"sharp_ax", ch_fit.ax, L"%.4f");
        wz_put(wz_lens, L"sharp_ay", ch_fit.ay, L"%.4f"); wz_put(wz_lens, L"sharp_sh", ch_fit.sh, L"%.3f"); wz_put(wz_lens, L"sharp_rms", ch_fit.rms, L"%.4f");
        { wchar_t p[MAX_PATH * 2], sec[64], k[64], v[64]; ini_path(p); wz_sec(sec); SYSTEMTIME st; GetLocalTime(&st);
          swprintf(v, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute); swprintf(k, 64, L"lens%d_sharp_date", wz_lens); WritePrivateProfileStringW(sec, k, v, p); }
    } else log_w(L"Set up this scanner: the focus chart could not be measured (see above) - RAW sharpening keeps its general model for this lens.");
    if (autotest && wz_test_type >= 0) { wz_end(L"Set up this scanner: test run finished."); return; }
    wz_menu();
}
static void wz_holder(int t) {
    const CasHolder *hh = cas_holder(t); if (!hh) return;
    SendMessageW(GetDlgItem(hwnd, IDC_MOUNT), CB_SETCURSEL, 0, 0); cass_fill(0, t);
    SendMessageW(GetDlgItem(hwnd, IDC_MODE), CB_SETCURSEL, 1, 0);       /* Color RGB */
    frame_sel = 1;
    wz_type = t; wz_state = WZ_PREVIEW;
    wchar_t m[200], w[64]; to_w(hh->name, w, 64); swprintf(m, 200, L"Set up this scanner: holder %ls - preview.", w); log_w(m);
    do_preview();
}
static void wz_menu(void) {
    static wchar_t lab[WZ_NH + 1][160]; const wchar_t *b[WZ_NH + 1];
    for (int i = 0; i < WZ_NH; i++) { swprintf(lab[i], 160, L"%ls%ls", WZ_H[i].what, wz_measured(WZ_H[i].lens) ? L"   (this lens is measured - again)" : L""); b[i] = lab[i]; }
    b[WZ_NH] = L"Finish";
    wchar_t q[1400]; swprintf(q, 1400, L"Set up this scanner (scanner %u)\n\nPixel shift moves the lens a few motor steps between passes. How far the picture moves per step differs between scanners and lenses, so it is measured here for each lens you have a holder for. "
        L"Choose a holder, put a piece of film with fine detail in it (an ordinary picture, text, or a strip with sprocket holes - not clear or black film) and close the lid. "
        L"The program makes a preview, chooses a small area with detail itself and scans it ten times with the lens at different steps (about 7 minutes per lens). Only the scanner software's own motor commands are used; nothing is written to the scanner.\n\nWhich holder?", (unsigned)serial_flash_no);
    int pick = wz_test_type >= 0 ? -1 : 0, a;
    if (wz_test_type >= 0) { for (int i = 0; i < WZ_NH; i++) if (WZ_H[i].type == wz_test_type) pick = i; if (pick < 0) pick = WZ_NH; }
    a = ask_box(q, b, WZ_NH + 1, 1, 0, pick, NULL);
    if (a < 0 || a >= WZ_NH) { int any = wz_nmeasured; wz_end(any ? L"Set up this scanner finished." : L"Set up this scanner: nothing measured."); return; }
    if (!(autotest && wz_test_type >= 0)) {
        wchar_t q2[400], w[64]; to_w(cas_holder(WZ_H[a].type)->name, w, 64);
        swprintf(q2, 400, L"Put the %ls holder with the film into the scanner and close the lid, then press Start.", w);
        const wchar_t *b2[2] = { L"Start", L"Back" };
        if (ask_box(q2, b2, 2, 0, 0, 0, NULL) != 0) { wz_menu(); return; }
    }
    wz_holder(WZ_H[a].type);
}
static void wz_start(void) {
    if (wz_on) { MessageBoxW(hwnd, L"Set up this scanner is already running.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (busy_now || !emu_running) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); return; }
    if (!serial_flash_no) { MessageBoxW(hwnd, L"The scanner's number is not known (this scanner software version does not report it), so the measurements could not be kept for this scanner - Set up this scanner is not available.", APP_TITLE, MB_ICONWARNING); return; }
    if (!focus_ok || !lens_ok) { MessageBoxW(hwnd, L"The lens control is not available with this scanner software version - the setup cannot measure the lenses.", APP_TITLE, MB_ICONWARNING); return; }
    wz_on = 1; wz_nmeasured = 0; log_w(L"Set up this scanner started.");
    { wchar_t p[MAX_PATH * 2], sec[64]; ini_path(p); wz_sec(sec); WritePrivateProfileStringW(sec, L"setup_offered", L"1", p); }
    wz_menu();
}
/* at the first start with a scanner (its number has no [scanner N] setup yet): offered once */
static void wz_offer(void) {
    if (autotest || !serial_flash_no) return;
    wchar_t p[MAX_PATH * 2], sec[64], v[16]; ini_path(p); wz_sec(sec);
    GetPrivateProfileStringW(sec, L"setup_offered", L"", v, 16, p); if (v[0]) return;
    WritePrivateProfileStringW(sec, L"setup_offered", L"1", p);
    wchar_t q[800]; swprintf(q, 800, L"This is the first start with scanner %u.\n\nSet up this scanner now? The program measures, for each lens you have a holder for, how its lens steps move the picture - pixel shift needs that. About 7 minutes per lens; nothing is written to the scanner.\n\nWithout it, pixel shift uses general values. Tools > Set up this scanner runs it at any time.", (unsigned)serial_flash_no);
    const wchar_t *b[2] = { L"Set up now", L"Later" };
    if (ask_box(q, b, 2, 0, 0, 1, NULL) == 0) wz_start();
}

/* ---- 0.4.0: Tools > Fix streaks, guided (streakcal.c; RE notes s37 / s37b and the Scitex procedure) ----
   Scitex's own procedure (Install&Utils create_transpar_clean_gain_vec): clean the glass, make a few gain calibrations,
   their mean is the "clean gain vector" of the lens. Here: the user cleans the KEEP CLEAN strip and the glass and leaves
   the selected frame of a transparent holder EMPTY. The program checks on the preview that the frame is empty and
   chooses a strip of it (as wide as one RAW allows), makes FS_K small RAWs of it at the lens's CCD resolution - each with
   its own 256-line gain calibration (the RAWs are not saved) - and makes the vector as the per-photosite median of their
   white lines. Before anything is stored it is checked on those pictures: what they would carry with the new vector is
   picture x vector in use / new vector (streakcal.c). Bright places only the white line had (dust on the KEEP CLEAN
   strip) are repaired from the pictures; a vector that is not better than the one in use is not offered. */
#define FS_K 5
enum { FS_IDLE, FS_PREVIEW, FS_RAW };
static int fs_on, fs_state, fs_k, fs_lens = -1, fs_wseq0, fs_dseq0, fs_test, fs_test_pending, fs_fresh, fs_meas_ok;   /* developer option (autotest) --fix-streaks: Fix streaks after the first scan (pending), then running as a test */
static ScProf fs_prof[FS_K]; static uint16_t *fs_line[FS_K]; static double fs_dpi; static wchar_t fs_why[400];
static void fs_free(void) { for (int i = 0; i < FS_K; i++) { sc_profile_free(&fs_prof[i]); free(fs_line[i]); fs_line[i] = NULL; } }
/* ends the procedure; ok = finished with a result (the bar shows it), close = autotest ends the program */
static void fs_end(const wchar_t *why, int ok, int close) {
    fs_on = 0; fs_state = FS_IDLE; rt_ovr.on = 0; fs_free();
    if (why) log_w(why);
    status(ok ? L"Fix streaks finished." : L"Ready."); pg_ok = ok; set_busy(0); pg_end();
    if (close && autotest && fs_test && !closing) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
/* the preview's samples as 14-bit values (the driver's 14 -> 12 bit table undone, as for RAWs; x 4 when the scanner
   did not use its 14-bit mode) */
static double fs_v14(int v) { return pv_v16(v) / 4; }
static int fs_run(const double *f, int n, double thr, int *a, int *b) {   /* the longest run of f >= thr */
    int best = 0, s = -1; *a = 0; *b = -1;
    for (int i = 0; i <= n; i++) {
        if (i < n && f[i] >= thr) { if (s < 0) s = i; continue; }
        if (s >= 0 && i - s > best) { best = i - s; *a = s; *b = i - 1; } s = -1; }
    return best;
}
/* the empty-frame check on the preview of the selected frame and the strip for the RAWs (crop in mm, as drawn); 0 and
   fs_why when the frame does not look empty */
static int fs_pick(double *l, double *t, double *wmm, double *hmm, int *wpx, int *hpx) {
    int w = pv_r12_w, h = pv_r12_h; const uint16_t *im = pv_r12; fs_why[0] = 0;
    if (!im || pv_r12_frame != frame_sel || w < 32 || h < 16 || !(pv_mm_cols > 0) || !(pv_mm_rows > 0)) { swprintf(fs_why, 400, L"the preview of this frame or its size in mm is not available"); return 0; }
    static double lut[4096]; for (int v = 0; v < 4096; v++) lut[v] = fs_v14(v);
    static uint32_t hist[4096]; memset(hist, 0, sizeof hist); size_t np = (size_t)w * h;
    for (size_t i = 0; i < np; i++) hist[im[3 * i + 1] & 4095]++;
    int p99 = 0; { size_t acc = 0; for (int v = 0; v < 4096; v++) { acc += hist[v]; if (acc >= np - np / 100) { p99 = v; break; } } }
    double thr = 0.6 * lut[p99];
    double *fr = calloc((size_t)(w > h ? w : h), sizeof(double)); if (!fr) { swprintf(fs_why, 400, L"not enough memory"); return 0; }
    #define BR(x, y) (lut[im[3 * ((size_t)(y) * w + (x)) + 1] & 4095] >= thr)
    int r0 = 0, r1 = h - 1, c0 = 0, c1 = w - 1;
    for (int y = 0; y < h; y++) { int k = 0; for (int x = 0; x < w; x++) k += BR(x, y); fr[y] = (double)k / w; }
    fs_run(fr, h, 0.3, &r0, &r1);
    for (int pass = 0; pass < 2 && r1 >= r0; pass++) {
        double need = pass ? 0.98 : 0.95;
        for (int x = 0; x < w; x++) { int k = 0; for (int y = r0; y <= r1; y++) k += BR(x, y); fr[x] = (double)k / (r1 - r0 + 1); }
        fs_run(fr, w, need, &c0, &c1); if (c1 < c0) break;
        for (int y = 0; y < h; y++) { int k = 0; for (int x = c0; x <= c1; x++) k += BR(x, y); fr[y] = (double)k / (c1 - c0 + 1); }
        fs_run(fr, h, need, &r0, &r1);
    }
    #undef BR
    free(fr);
    double mmx = pv_mm_cols / w, mmy = pv_mm_rows / h;
    double mgx = fmax(0.7, 0.03 * (c1 - c0 + 1) * mmx), mgy = fmax(0.7, 0.03 * (r1 - r0 + 1) * mmy);
    int a0 = c0 + (int)ceil(mgx / mmx), a1 = c1 - (int)ceil(mgx / mmx), b0 = r0 + (int)ceil(mgy / mmy), b1 = r1 - (int)ceil(mgy / mmy);
    if (c1 < c0 || r1 < r0 || a1 - a0 + 1 < 24 || b1 - b0 + 1 < 8) { swprintf(fs_why, 400, L"no evenly lit open area was found in the frame"); return 0; }
    /* empty: lit like an empty frame, neutral, even (no picture, no film base) */
    double sum[3] = { 0, 0, 0 }; size_t nw = (size_t)(a1 - a0 + 1) * (b1 - b0 + 1);
    static uint32_t hg[4096]; memset(hg, 0, sizeof hg);
    for (int y = b0; y <= b1; y++) for (int x = a0; x <= a1; x++) { const uint16_t *q = im + 3 * ((size_t)y * w + x);
        for (int c = 0; c < 3; c++) sum[c] += lut[q[c] & 4095]; hg[q[1] & 4095]++; }
    double gm = 0; { size_t acc = 0; for (int v = 0; v < 4096; v++) { acc += hg[v]; if (acc >= nw / 2) { gm = lut[v]; break; } } }
    double rg = sum[1] > 0 ? sum[0] / sum[1] : 0, bg = sum[1] > 0 ? sum[2] / sum[1] : 0;
    /* evenness: blocks of 1/4 of the area (lamp fall-off allowed up to 1.35 : 1), and pixels against their 31 x 31 neighbourhood */
    double bmin = 1e30, bmax = 0;
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
        int x0 = a0 + (a1 - a0 + 1) * i / 4, x1 = a0 + (a1 - a0 + 1) * (i + 1) / 4 - 1, y0 = b0 + (b1 - b0 + 1) * j / 4, y1 = b0 + (b1 - b0 + 1) * (j + 1) / 4 - 1;
        double s = 0; int k = 0; for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) { s += lut[im[3 * ((size_t)y * w + x) + 1] & 4095]; k++; }
        if (k) { s /= k; if (s < bmin) bmin = s; if (s > bmax) bmax = s; } }
    long nodd = 0; { int R = 15, W1 = a1 - a0 + 2, H1 = b1 - b0 + 2; double *ii = calloc((size_t)W1 * H1, sizeof(double));
        if (ii) { for (int y = 0; y < H1 - 1; y++) for (int x = 0; x < W1 - 1; x++)
                      ii[(size_t)(y + 1) * W1 + x + 1] = lut[im[3 * ((size_t)(y + b0) * w + x + a0) + 1] & 4095] + ii[(size_t)y * W1 + x + 1] + ii[(size_t)(y + 1) * W1 + x] - ii[(size_t)y * W1 + x];
                  for (int y = 0; y < H1 - 1; y++) for (int x = 0; x < W1 - 1; x++) {
                      int xa = x - R < 0 ? 0 : x - R, xb = x + R > W1 - 2 ? W1 - 2 : x + R, ya = y - R < 0 ? 0 : y - R, yb = y + R > H1 - 2 ? H1 - 2 : y + R;
                      double m = (ii[(size_t)(yb + 1) * W1 + xb + 1] - ii[(size_t)ya * W1 + xb + 1] - ii[(size_t)(yb + 1) * W1 + xa] + ii[(size_t)ya * W1 + xa]) / ((double)(xb - xa + 1) * (yb - ya + 1));
                      double v = lut[im[3 * ((size_t)(y + b0) * w + x + a0) + 1] & 4095]; if (m > 0 && fabs(v / m - 1) > 0.06) nodd++; }
                  free(ii); } }
    { wchar_t m[400]; swprintf(m, 400, L"Fix streaks: open area of the preview %d - %d x %d - %d (PM pixels): green %.0f (14-bit), red / green %.2f, blue / green %.2f, blocks %.2f : 1, %.2f %% uneven pixels.",
        a0, a1, b0, b1, gm, rg, bg, bmin > 0 ? bmax / bmin : 0, 100.0 * nodd / nw); log_d(m); }
    if (gm < 6000) { swprintf(fs_why, 400, L"it is darker than an empty frame (%.0f %% of the light expected) - film in it, or the wrong frame", gm / 12500 * 100); return 0; }
    if (rg < 0.7 || rg > 1.4 || bg < 0.7 || bg > 1.4) { swprintf(fs_why, 400, L"it is coloured (red / green %.2f, blue / green %.2f) - film in it", rg, bg); return 0; }
    if (bmin <= 0 || bmax / bmin > 1.35 || nodd > (long)(nw / 100)) { swprintf(fs_why, 400, L"it is not evenly lit (%.2f %% of its pixels differ from their surroundings) - film or dust in it", 100.0 * nodd / nw); return 0; }
    /* the strip: as wide as the open area (as many CCD photosites as possible), up to 320 lines, one RAW block */
    double dpi = fs_dpi, avw = (a1 - a0 + 1) * mmx, avh = (b1 - b0 + 1) * mmy;
    int W = (int)floor(avw * dpi / 25.4), H = (int)floor(avh * dpi / 25.4); if (H > 320) H = 320;
    if (raw_max_w > 0 && W > raw_max_w) W = raw_max_w;
    while (H > 64 && !raw_fits(W, H)) H -= 16;
    while (W > 256 && !raw_fits(W, H)) W -= 64;
    if (W < 256 || H < 64 || !raw_fits(W, H)) { swprintf(fs_why, 400, L"the open area is too small for the measurement (%.1f x %.1f mm)", avw, avh); return 0; }
    *wpx = W; *hpx = H; *wmm = W * 25.4 / dpi * 0.99995; *hmm = H * 25.4 / dpi * 0.99995;
    double cxm = (a0 + a1 + 1) / 2.0 * mmx, cym = (b0 + b1 + 1) / 2.0 * mmy;   /* PM coordinates; the crop is in the preview as shown */
    if (pv_mirror) cxm = pv_mm_cols - cxm;
    *l = cxm - *wmm / 2; *t = cym - *hmm / 2;
    if (*l < 0) *l = 0; if (*t < 0) *t = 0;
    return 1;
}
static void fs_raw_next(void) {
    fs_wseq0 = fs_wseq; fs_dseq0 = dsp_seq; fs_meas_ok = 0; fs_why[0] = 0;
    rt_ovr.on = 1; rt_ovr.stream = 0; rawj_running = 0; set_busy(1);
    char nm[40]; snprintf(nm, sizeof nm, "fix_streaks_%d", fs_k + 1);
    { wchar_t m[200]; swprintf(m, 200, L"Fix streaks: picture %d of %d.", fs_k + 1, FS_K); status(m); }
    do_raw(nm, fs_dpi, "100");
    if (!rawj_running) fs_end(L"Fix streaks: the RAW could not be started (see above) - nothing stored.", 0, 1);
}
static void fs_preview_done(void) {
    fs_state = FS_IDLE;
    if (!frame_ready[frame_sel] || pv_lens < 0 || pv_lens > 4 || !(pv_opt_dpi > 0) || pv_mount != 0) { fs_end(L"Fix streaks: the preview, its lens or its mount is not known - stopped. Please send the Logs folder.", 0, 1); return; }
    fs_lens = pv_lens; fs_dpi = OPT_REQ(pv_opt_dpi);
    double l, t, wmm, hmm; int W, H;
    if (!fs_pick(&l, &t, &wmm, &hmm, &W, &H)) {
        wchar_t m[700]; swprintf(m, 700, L"Fix streaks: frame %d does not look empty: %ls.\n\nThe measurement needs the selected frame of the holder EMPTY (no film, no slide).%ls", frame_sel, fs_why,
                                 fs_fresh ? L"" : L" This check used the preview shown - if you have just taken the film out, make a new preview.");
        log_w(m);
        if (!fs_fresh) { const wchar_t *b[2] = { L"Make a new preview", L"Stop" };
            if (ask_box(m, b, 2, 0, 0, 1, NULL) == 0) { fs_fresh = 1; fs_state = FS_PREVIEW; log_w(L"Fix streaks: new preview."); do_preview(); return; } }
        else if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION);
        fs_end(L"Fix streaks stopped: the frame is not empty - nothing stored.", 0, 1); return; }
    /* the crop on the preview, as the user would draw it */
    set_edit_mm(IDC_CX, l); set_edit_mm(IDC_CY, t); set_edit_mm(IDC_CW, wmm); set_edit_mm(IDC_CH, hmm);
    SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0); InvalidateRect(hprev, NULL, FALSE);
    snprintf(rt_ovr.cw, sizeof rt_ovr.cw, "%.5f", wmm); snprintf(rt_ovr.ch, sizeof rt_ovr.ch, "%.5f", hmm);
    snprintf(rt_ovr.cx, sizeof rt_ovr.cx, "%.5f", l); snprintf(rt_ovr.cy, sizeof rt_ovr.cy, "%.5f", t);
    { wchar_t m[400]; swprintf(m, 400, L"Fix streaks: frame %d is empty; %d RAWs of %.2f x %.2f mm at %.2f, %.2f (%d x %d pixels at the %ls's %.1f dpi) - about %d minutes.", frame_sel, FS_K, wmm, hmm, l, t, W, H, LENS_NAME[fs_lens], OPT_SHOW(pv_opt_dpi), 1 + FS_K * 45 / 60); log_w(m); }
    pg_end(); pg_begin(L"Fix streaks", L"picture", FS_K); pg.locked = 1;
    fs_k = 0; fs_state = FS_RAW; fs_raw_next();
}
/* one RAW (raw_done): its picture as column means in CCD order, and the white line of its own gain calibration */
static void fs_measure(const uint16_t *img, int w, int h, int ch) {
    fs_meas_ok = 0;
    if (fs_k < 0 || fs_k >= FS_K) return;
    if (ch != 3) { swprintf(fs_why, 400, L"the RAW is not in colour"); return; }
    if (dsp_seq == fs_dseq0 || dsp_rows != h || dsp_cols != w || dsp_drop < 0 || dsp_cut < 0) { swprintf(fs_why, 400, L"the scanner software's picture geometry (DROP_PIXELS) was not seen for this RAW"); return; }
    if (fabs(dsp_inpol - 1) > 2e-4 || dsp_wcomp != 1) { swprintf(fs_why, 400, L"the RAW is not at the CCD's own sampling (INPOL_WIDTH_STEP %.6f, width compression %d)", dsp_inpol, (int)dsp_wcomp); return; }
    if (fs_wseq == fs_wseq0 || fs_wn < 256 || fs_wlens != fs_lens || fs_wrefl) { swprintf(fs_why, 400, L"no 256-line white measurement of the %ls was seen for this RAW", LENS_NAME[fs_lens]); return; }
    double s[3] = { 0, 0, 0 }; for (size_t i = 0; i < (size_t)w * h; i++) for (int c = 0; c < 3; c++) s[c] += img[3 * i + c];
    for (int c = 0; c < 3; c++) s[c] /= (double)w * h;
    if (s[1] < 4 * 6000.0 || s[0] < 0.7 * s[1] || s[0] > 1.4 * s[1] || s[2] < 0.7 * s[1] || s[2] > 1.4 * s[1]) {
        swprintf(fs_why, 400, L"the RAW does not look like an empty frame (red %.0f, green %.0f, blue %.0f of 65535) - film in the frame", s[0], s[1], s[2]); return; }
    int base = dsp_drop + dsp_cut - 1, x0 = rawj.flip ? base + w - 1 : base, dir = rawj.flip ? -1 : 1;
    if (base < 0 || base + w > CG_LIGHT) { swprintf(fs_why, 400, L"the picture lies outside the CCD's photosites (%d - %d)", base, base + w - 1); return; }
    if (sc_profile(img, w, h, 3, x0, dir, &fs_prof[fs_k])) { swprintf(fs_why, 400, L"not enough memory"); return; }
    free(fs_line[fs_k]); fs_line[fs_k] = malloc(sizeof(uint16_t) * CG_N); if (!fs_line[fs_k]) { swprintf(fs_why, 400, L"not enough memory"); return; }
    memcpy(fs_line[fs_k], fs_wline, sizeof(uint16_t) * CG_N);
    { wchar_t m[300]; swprintf(m, 300, L"Fix streaks: picture %d: CCD photosites %d - %d (DROP_PIXELS %d, CUT_PIX %d, INPOL %.6f%ls), mean red %.0f, green %.0f, blue %.0f; white line of %d lines.", fs_k + 1, base, base + w - 1, (int)dsp_drop, (int)dsp_cut, dsp_inpol, rawj.flip ? L", RAW columns in reverse CCD order" : L"", s[0], s[1], s[2], (int)fs_wn); log_d(m); }
    fs_meas_ok = 1;
}
static void fs_evaluate(void);
static void fs_step_end(int ok) {
    if (!ok || !fs_meas_ok) { wchar_t m[600]; swprintf(m, 600, L"Fix streaks stopped at picture %d: %ls - nothing stored.", fs_k + 1, fs_why[0] ? fs_why : L"the RAW did not finish (see above)"); fs_end(m, 0, 1); return; }
    fs_k++;
    if (fs_k < FS_K) { fs_raw_next(); return; }
    fs_evaluate();
}
static void fs_evaluate(void) {
    static uint16_t vnew[CG_N], vold[CG_N], vfix[CG_N];
    const uint16_t *lp[FS_K]; for (int k = 0; k < FS_K; k++) lp[k] = fs_line[k];
    const wchar_t *LNs = LENS_NAME[fs_lens]; wchar_t m[3000];
    if (sc_median_vector(lp, FS_K, vnew)) { fs_end(L"Fix streaks: the white lines could not be combined - nothing stored.", 0, 1); return; }
    { int lo = 0, hi = -1; double g = 0; for (int p = 0; p < CG_LIGHT; p++) g += vnew[p * 3 + 1]; g /= CG_LIGHT;
      double pat = cg_pattern(vnew); int nl = cg_lit(vnew, &lo, &hi), bad = g < 1000 || pat > 0.05 || nl < 1000;
      for (int c = 0; c < 3 && !bad && hi > lo; c++) { double mn = 1e30, mx = 0; for (int p = lo; p <= hi; p++) { double v = vnew[p * 3 + c]; if (v < mn) mn = v; if (v > mx) mx = v; } if (mn < 0.5 * mx) bad = 1; }
      swprintf(m, 3000, L"Fix streaks: new vector from %d white lines (median per photosite): level %.0f, pixel pattern %.2f %%, lit photosites %d - %d.", FS_K, g, pat * 100, lo, hi); log_d(m);
      if (bad) { swprintf(m, 3000, L"Fix streaks: the measured white line is not plausible (level %.0f, pixel pattern %.2f %%; or a deep dip - dust on the KEEP CLEAN strip?) - nothing stored. Clean the strip and run Fix streaks again.", g, pat * 100); fs_end(m, 0, 1); if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONWARNING); return; } }
    char mk[100]; int own = cg_current(fs_lens, 0, mk, sizeof mk);
    if (cg_vector_now(fs_lens, 0, vold)) { fs_end(L"Fix streaks: Smart342.Prefs has no clean gain vector for this lens that could be read - nothing stored. Please send the Logs folder.", 0, 1); return; }
    /* the mapping: as the driver's geometry says, or reversed (a picture mirrored by the front end) if that fits much better */
    ScEval e, er; int rev = 0;
    if (sc_eval(fs_prof, lp, FS_K, vold, vnew, &e)) { fs_end(L"Fix streaks: the pictures could not be evaluated - nothing stored. Please send the Logs folder.", 0, 1); return; }
    { ScProf rp[FS_K]; int okr = 1; memset(rp, 0, sizeof rp);
      for (int k = 0; k < FS_K && okr; k++) { rp[k].c0 = fs_prof[k].c0; rp[k].n = fs_prof[k].n; int n = rp[k].n;
          for (int c = 0; c < 3 && okr; c++) { rp[k].p[c] = malloc(n * sizeof(double)); rp[k].h1[c] = malloc(n * sizeof(double)); rp[k].h2[c] = malloc(n * sizeof(double));
              if (!rp[k].p[c] || !rp[k].h1[c] || !rp[k].h2[c]) { okr = 0; break; }
              for (int i = 0; i < n; i++) { rp[k].p[c][i] = fs_prof[k].p[c][n - 1 - i]; rp[k].h1[c][i] = fs_prof[k].h1[c][n - 1 - i]; rp[k].h2[c][i] = fs_prof[k].h2[c][n - 1 - i]; } } }
      if (okr && sc_eval(rp, lp, FS_K, vold, vnew, &er) == 0 && er.corr0 > 0.3 && er.corr0 > e.corr0 + 0.2) {
          rev = 1; e = er; for (int k = 0; k < FS_K; k++) { ScProf t = fs_prof[k]; fs_prof[k] = rp[k]; rp[k] = t; } }
      for (int k = 0; k < FS_K; k++) sc_profile_free(&rp[k]); }
    ScEval ef = e; int nfix = 0; const uint16_t *vuse = vnew;
    if (e.nflag > 0) { nfix = sc_repair(fs_prof, FS_K, vold, vnew, &e, vfix);
        if (nfix > 0 && sc_eval(fs_prof, lp, FS_K, vold, vfix, &ef) == 0) vuse = vfix; else { ef = e; nfix = 0; } }
    int dec = sc_decide(&ef), c0 = fs_prof[0].c0, c1 = fs_prof[0].c0 + fs_prof[0].n - 1;
    swprintf(m, 3000, L"Fix streaks: pictures CCD %d - %d; mapping %ls, correlation %.2f at the driver's position%ls; fine streaks R/G/B (rms, high-pass): in use %.3f / %.3f / %.3f %%, new %.3f / %.3f / %.3f %%, noise %.3f / %.3f / %.3f %%; above the noise: in use %.3f / %.3f / %.3f %%, new %.3f / %.3f / %.3f %%%ls; %d bright place(s) only the white line had.",
             c0, c1, rev ? L"REVERSED against the driver's geometry" : L"as the driver's geometry", e.corr0, e.shift ? L" (shifted by the check)" : L"",
             e.rms_old[0] * 100, e.rms_old[1] * 100, e.rms_old[2] * 100, e.rms_new[0] * 100, e.rms_new[1] * 100, e.rms_new[2] * 100, e.noise[0] * 100, e.noise[1] * 100, e.noise[2] * 100,
             e.ex_old[0] * 100, e.ex_old[1] * 100, e.ex_old[2] * 100, e.ex_new[0] * 100, e.ex_new[1] * 100, e.ex_new[2] * 100, L" (0.5.0: the result uses green only - the scanner moves red and blue by part of a photosite, so their prediction per photosite does not hold)", e.nflag);
    log_d(m);
    if (e.shift) { swprintf(m, 3000, L"Fix streaks: the pictures fit the white line %+d photosites from the driver's geometry (correlation %.2f instead of %.2f) - used.", e.shift, e.corr, e.corr0); log_d(m); }
    wchar_t pl[1200] = L""; int k = 0;
    for (int r = 0; r < e.nfr && k < 1000; r++) k += swprintf(pl + k, 1200 - k, L"%ls%d - %d (+%.2f %%)", r ? L", " : L"", e.flag_c0[r], e.flag_c1[r], e.flag_d[r] * 100);
    if (e.nflag > e.nfr) swprintf(pl + k, 1200 - k, L" and %d more", e.nflag - e.nfr);
    if (nfix) { swprintf(m, 3000, L"Fix streaks: %d place(s) only the white measurement saw (dust on the KEEP CLEAN strip) at CCD photosites %ls: repaired from the green pictures in all three colours (%d photosites); green above the noise now %.3f %%.", e.nflag, pl, nfix, ef.ex_new[1] * 100); log_d(m); }
    double eo = e.ex_old[1] * 100, en = ef.ex_new[1] * 100;   /* 0.5.0: green - the channel whose RAW columns are the CCD's own photosites (streakcal.h) */
    wchar_t wo[100] = L""; if (own == 1) to_w(mk, wo, 100);
    if (dec == 2) {
        swprintf(m, 3000, L"Fix streaks - %ls: the new vector would not make the pictures better than the one in use (fine streaks above the noise: %.2f %% now, %.2f %% with the new one) - nothing stored.\n\nClean the KEEP CLEAN strip and the glass again, check that the frame is empty, and run Fix streaks again.", LNs, eo, en);
        if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONINFORMATION);
        fs_end(m, 0, 1); return; }
    int q = swprintf(m, 3000, L"Fix streaks - %ls, transparent: %d white measurements of 256 lines; checked on %d pictures of the empty frame (CCD photosites %d - %d of 0 - %d).\n\n"
                              L"Fine streaks in the pictures, above their noise (green - the colour the scanner does not shift; the vector corrects red and blue the same way):\n    with the vector in use (%ls): %.2f %%\n    with the new vector: %.2f %%\n",
                     LNs, FS_K, FS_K, c0, c1, CG_LIGHT - 1, own == 1 ? wo : own == 0 ? L"another scanner's" : L"unknown", eo, en);
    if (nfix) q += swprintf(m + q, 3000 - q, L"\n%d place(s) only the white measurement saw - dust on the KEEP CLEAN strip - at CCD photosites %ls: repaired from the pictures. Cleaning the strip and running Fix streaks again gives a cleaner measurement.\n", e.nflag, pl);
    if (rev) q += swprintf(m + q, 3000 - q, L"\nNote: the pictures fit the white line only mirrored - please send the Logs folder.\n");
    swprintf(m + q, 3000 - q, L"\nStore it? The scanner software then ends (it parks the scanner) and the program starts again. Only the program's settings on this PC change (Smart342.Prefs, the previous one is kept in the Logs folder); nothing is written to the scanner.");
    log_w(L"Fix streaks: the new clean gain vector is ready to store.");
    const wchar_t *b[2] = { L"Store it and start again", L"Not now" };
    if (ask_box(m, b, 2, 0, 0, cg_store_auto >= 0 ? cg_store_auto : 0, NULL) != 0) { fs_end(L"Fix streaks: not stored.", 0, 1); return; }
    SYSTEMTIME st; GetLocalTime(&st); int when[6] = { st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond };
    char mark[100]; snprintf(mark, sizeof mark, "scanner %u, Fix streaks %d x 256 lines%s, %02d.%02d.%04d", (unsigned)serial_flash_no, FS_K, nfix ? ", repaired" : "", when[2], when[1], when[0]);
    static char names[1][64]; static uint8_t data[1][200 + CG_N * 2]; int ids[1];
    if (cg_resource(vuse, fs_lens, 0, when, mark, data[0], sizeof data[0], &ids[0], names[0], 64) <= 0) { fs_end(L"Fix streaks: the resource could not be made - nothing stored.", 0, 1); return; }
    fs_end(NULL, 1, 0);
    if (!cg_store_res(1, ids, names, data) && autotest && fs_test && !closing) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}
static int fs_start(void) {   /* 1 = started (it then ends through fs_end) */
    if (fs_on || wz_on || lt_on || ft_on || rt_on) { MessageBoxW(hwnd, L"Another measurement is running - wait until it has finished.", APP_TITLE, MB_ICONINFORMATION); return 0; }
    if (busy_now || !emu_running || closing) { MessageBoxW(hwnd, L"Wait until the scanner is ready.", APP_TITLE, MB_ICONINFORMATION); return 0; }
    if (!serial_flash_no) { MessageBoxW(hwnd, L"The scanner's number is not known - the calibration could not be kept for this scanner.", APP_TITLE, MB_ICONWARNING); return 0; }
    if (!cg_hooks_ok || !cg_lines_ok || !raw_patch_ok) { MessageBoxW(hwnd, L"Fix streaks is not available with this scanner software version (see the log).", APP_TITLE, MB_ICONWARNING); return 0; }
    if (sel(IDC_MOUNT) != 0) { MessageBoxW(hwnd, L"Fix streaks measures with a transparent holder: choose Mount: Transparent and the holder you use (the measurement is made for its lens), then choose Tools > Fix streaks again.", APP_TITLE, MB_ICONINFORMATION); return 0; }
    /* 0.5.0 (hardware 18 Oct: the 35 mm holder was in while T 6 x 6 was selected): the selected holder named plainly */
    wchar_t holder[80] = L"?"; { HWND cb = GetDlgItem(hwnd, IDC_CASS); int s = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0); if (s >= 0 && SendMessageW(cb, CB_GETLBTEXTLEN, s, 0) < 79) SendMessageW(cb, CB_GETLBTEXT, s, (LPARAM)holder); }
    wchar_t q[2000]; swprintf(q, 2000, L"Fix streaks: the scanner software corrects every CCD pixel with a stored \"clean gain vector\" per lens. One from another scanner, or dust on the scanner's KEEP CLEAN strip, leaves fine streaks along the scan. This measures this scanner's own vector for the lens of the holder in the window, and checks it before it is stored.\n\n"
        L"HOLDER: %ls, frame %d. The holder in the scanner must be this one - the lens is chosen from the holder in the window. If another holder is in, choose Not now and set Cassette first.\n\n"
        L"Before you start:\n    1. Clean the KEEP CLEAN strip (the white strip the scanner calibrates on) and the glass with a soft, lint-free cloth.\n    2. Put the holder in with frame %d EMPTY - no film, no slide (the program scans the empty frame to check the new vector). Use a frame whose glass is clean and not scratched.\n    3. Close the lid.\n\n"
        L"The program makes a preview if needed, checks that the frame is empty, then makes %d small RAW scans of it (about %d minutes; they are not saved). Nothing is stored until you have seen the result.", holder, frame_sel, frame_sel, FS_K, 1 + FS_K * 45 / 60);
    const wchar_t *b[2] = { L"Start", L"Not now" };
    if (ask_box(q, b, 2, 0, 0, 0, NULL) != 0) { log_w(L"Fix streaks: not started."); return 0; }
    if (busy_now || !emu_running || closing) { log_w(L"Fix streaks: not started - the scanner became busy."); return 0; }
    fs_on = 1; fs_k = 0; fs_lens = -1; memset(fs_prof, 0, sizeof fs_prof); log_w(L"Fix streaks started.");
    if (frame_ready[frame_sel] && !pv_invalid && pv_r12 && pv_r12_frame == frame_sel && pv_mount == 0) { fs_fresh = 0; fs_preview_done(); }
    else { fs_fresh = 1; fs_state = FS_PREVIEW; log_w(L"Fix streaks: preview."); do_preview(); }
    return 1;
}

/* lens of the last prescan (rawscan.c lens_bp) -> the optical sampling across the scan */
static void update_res_info(void);
static void lens_report(void) {
    if (!lens_ok || lens_last < 0 || lens_last > 4 || lens_seq == lens_seq_seen || !(lens_pitch > 0)) return;
    lens_seq_seen = lens_seq;
    double d = lens_mag / lens_pitch * 25.4;
    if (lens_last == pv_lens && fabs(d - pv_opt_dpi) < 1) return;
    pv_lens = lens_last; pv_opt_dpi = d;
    wchar_t m[300]; swprintf(m, 300, L"Lens: %ls - the CCD samples about %.0f dpi across the scan (%.1f px/mm). Higher resolutions are interpolated across; along the scan the carriage steps at the chosen resolution.", LENS_NAME[pv_lens], d, d / 25.4);
    log_w(m); update_res_info(); focus_menu_check();
    if (focus_off_l[pv_lens] != 0) { swprintf(m, 300, L"Focus for this lens: %+.2f mm (EXPERIMENTAL, Tools > Focus offset).", focus_off_l[pv_lens]); log_w(m); }
}
static void tif_tag(FILE *f, int tag, int type, uint32_t count, uint32_t value) {
    uint8_t e[12] = { (uint8_t)tag, (uint8_t)(tag >> 8), (uint8_t)type, (uint8_t)(type >> 8),
                      (uint8_t)count, (uint8_t)(count >> 8), (uint8_t)(count >> 16), (uint8_t)(count >> 24) };
    if (type == 3 && count == 1) { e[8] = (uint8_t)value; e[9] = (uint8_t)(value >> 8); }
    else { e[8] = (uint8_t)value; e[9] = (uint8_t)(value >> 8); e[10] = (uint8_t)(value >> 16); e[11] = (uint8_t)(value >> 24); }
    fwrite(e, 1, 12, f);
}
static void le32w(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
static void le16w(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }
/* uncompressed 16-bit TIFF (Intel byte order), strips of about 1 MB; img = w*h*ch samples (host order). 1 = written */
/* 0.2.0: RAW gamma (window, saved): every RAW file of a RAW job - the result, the SHARP copy, kept passes - is written
   with the job's choice (rawj.gamma, taken at the start; the tests' RAWs always linear): linear = the scanner's values
   (proportional to the light), or a pure power 1/2.2 on the 16-bit range (65535 x (v / 65535)^(1/2.2)) - an encoding
   only: every measurement, registration, merge and the sharpening are done on the linear values first. The file's
   description says which. */
static uint16_t *gamma_lut(void) {
    static uint16_t *lut; if (lut) return lut;
    uint16_t *l = malloc(65536 * sizeof *l); if (!l) return NULL;
    for (int v = 0; v < 65536; v++) { double o = 65535.0 * pow(v / 65535.0, 1 / 2.2) + 0.5; l[v] = (uint16_t)(o > 65535 ? 65535 : o); }
    return lut = l;
}
static int tiff_pos;   /* 0.4.0: writing a positive - its values are encoded already (no RAW gamma; its own description) */
static int write_tiff16xy(const wchar_t *dst, const uint16_t *img, int pix, int lines, int ch, double xdpi, double ydpi, const char *desc0) {
    const uint16_t *lut = rawj.gamma && !tiff_pos ? gamma_lut() : NULL;
    if (rawj.gamma && !tiff_pos && !lut) return 0;
    size_t dn0 = strlen(desc0); char *desc = malloc(dn0 + 200); if (!desc) return 0;
    snprintf(desc, dn0 + 200, "%s %s", desc0, tiff_pos ? "" : lut ? "Written with gamma 2.2: file value = 65535 x (linear value / 65535)^(1/2.2)." : "Linear (gamma 1.0): the values are proportional to the light.");
    long long out_bytes = (long long)lines * pix * ch * 2;
    FILE *o = _wfopen(dst, L"wb"); if (!o) { free(desc); return 0; }
    int rps = (int)(1048576 / ((long long)pix * ch * 2)); if (rps < 1) rps = 1; if (rps > lines) rps = lines;
    int nstrips = (lines + rps - 1) / rps;
    size_t rowb = (size_t)pix * ch * 2;
    fwrite("II*\0", 1, 4, o); le32w(o, 0);
    int ok = 1; uint8_t *row = malloc(rowb); if (!row) ok = 0;
    for (int y = 0; ok && y < lines; y++) {
        const uint16_t *s = img + (size_t)y * pix * ch;
        for (size_t i = 0; i < (size_t)pix * ch; i++) { uint16_t v = lut ? lut[s[i]] : s[i]; row[2 * i] = (uint8_t)v; row[2 * i + 1] = (uint8_t)(v >> 8); }
        if (fwrite(row, 1, rowb, o) != rowb) ok = 0;
    }
    free(row);
    uint32_t data_end = (uint32_t)(8 + out_bytes);
    const char *sw = "Smart342 for Windows " APP_VERSION8;
    uint32_t ntags = 15, ifd = data_end + (data_end & 1), after = ifd + 2 + ntags * 12 + 4;
    uint32_t bps_at = after, xres_at = bps_at + 6, yres_at = xres_at + 8, desc_at = yres_at + 8, sw_at = desc_at + (uint32_t)strlen(desc) + 1;
    sw_at += sw_at & 1;
    uint32_t so_at = sw_at + (uint32_t)strlen(sw) + 1; so_at += so_at & 1; uint32_t sc_at = so_at + 4 * nstrips;
    if (ok) {
        if (data_end & 1) fputc(0, o);
        le16w(o, (uint16_t)ntags);
        tif_tag(o, 256, 4, 1, (uint32_t)pix); tif_tag(o, 257, 4, 1, (uint32_t)lines);
        if (ch == 3) tif_tag(o, 258, 3, 3, bps_at); else tif_tag(o, 258, 3, 1, 16);
        tif_tag(o, 259, 3, 1, 1); tif_tag(o, 262, 3, 1, ch == 3 ? 2 : 1);
        tif_tag(o, 270, 2, (uint32_t)strlen(desc) + 1, desc_at);
        tif_tag(o, 273, 4, (uint32_t)nstrips, nstrips == 1 ? 8 : so_at);
        tif_tag(o, 277, 3, 1, (uint32_t)ch); tif_tag(o, 278, 4, 1, (uint32_t)rps);
        tif_tag(o, 279, 4, (uint32_t)nstrips, nstrips == 1 ? (uint32_t)out_bytes : sc_at);
        tif_tag(o, 282, 5, 1, xres_at); tif_tag(o, 283, 5, 1, yres_at); tif_tag(o, 284, 3, 1, 1); tif_tag(o, 296, 3, 1, 2);
        tif_tag(o, 305, 2, (uint32_t)strlen(sw) + 1, sw_at);
        le32w(o, 0);
        le16w(o, 16); le16w(o, 16); le16w(o, 16);
        uint32_t dn = 1000, dvx = (uint32_t)(xdpi * dn + 0.5), dvy = (uint32_t)(ydpi * dn + 0.5);
        le32w(o, dvx); le32w(o, dn); le32w(o, dvy); le32w(o, dn);
        fwrite(desc, 1, strlen(desc) + 1, o); if ((desc_at + strlen(desc) + 1) & 1) fputc(0, o);
        fwrite(sw, 1, strlen(sw) + 1, o); if ((sw_at + strlen(sw) + 1) & 1) fputc(0, o);
        for (int i = 0; i < nstrips; i++) le32w(o, (uint32_t)(8 + (long long)i * rps * rowb));
        for (int i = 0; i < nstrips; i++) { int r = lines - i * rps; if (r > rps) r = rps; le32w(o, (uint32_t)((long long)r * rowb)); }
        fseek(o, 4, SEEK_SET); le32w(o, ifd);
        if (ferror(o)) ok = 0;
    }
    if (fclose(o)) ok = 0;
    if (!ok) DeleteFileW(dst);
    free(desc);
    return ok;
}
static int write_tiff16(const wchar_t *dst, const uint16_t *img, int pix, int lines, int ch, double dpi, const char *desc) {
    return write_tiff16xy(dst, img, pix, lines, ch, dpi, dpi, desc);
}
/* the newest ScitexScanMaxDetPM_<n> in the data folder */
static int raw_find(wchar_t *path) {
    wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%ls\\Smart342(L) V2.2.0\\Smart342(L)_DataFolder!!!\\ScitexScanMaxDetPM_*", voldir);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME best = { 0, 0 }; int got = 0;
    do { if (wcsstr(fd.cFileName, L".rsrc")) continue;
         if (!got || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; got = 1;
             swprintf(path, MAX_PATH * 2, L"%ls\\Smart342(L) V2.2.0\\Smart342(L)_DataFolder!!!\\%ls", voldir, fd.cFileName); }
    } while (FindNextFileW(h, &fd));
    FindClose(h); return got;
}
/* Read the Max Detail just made and convert it (12-bit -> 16-bit, mirror). Returns the picture (malloc) or NULL. */
static double sim_bed_sigma = -1;   /* developer option --sim-bed-texture */
static int sim_drop_line = -1;      /* developer option --sim-drop-line N (with --sim-bed-texture): streamed RAWs lose line N (rows below move up one line) - checks that the RAW check finds it */
static double bed_hash(int64_t x, int64_t y, int c) {
    uint64_t h = (uint64_t)x * 0x9E3779B97F4A7C15ULL ^ (uint64_t)y * 0xC2B2AE3D27D4EB4FULL ^ (uint64_t)(c + 1) * 0x165667B19E3779F9ULL;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ULL; h ^= h >> 32; return (double)(h >> 11) * (2.0 / 9007199254740992.0) - 1; }
static double bed_vn(double x, double y, double cell, int c) {
    x /= cell; y /= cell; double fx = floor(x), fy = floor(y), tx = x - fx, ty = y - fy;
    tx = tx * tx * (3 - 2 * tx); ty = ty * ty * (3 - 2 * ty); int64_t ix = (int64_t)fx, iy = (int64_t)fy;
    double a = bed_hash(ix, iy, c), b = bed_hash(ix + 1, iy, c), d = bed_hash(ix, iy + 1, c), e = bed_hash(ix + 1, iy + 1, c);
    return (a + (b - a) * tx) * (1 - ty) + (d + (e - d) * tx) * ty; }
static double bed_tex(double u, double v, int c) { return 4 * (8000 + 2000 * bed_vn(u, v, 37, c) + 1000 * bed_vn(u, v, 9, c + 7) + 480 * bed_vn(u, v, 4, c + 13)); }
static uint16_t *raw_read(int *pix_o, int *lines_o, int *ch_o, int *lin_o) {
    wchar_t src[MAX_PATH * 2], rpath[MAX_PATH * 2 + 8];
    if (!raw_find(src)) { log_w(L"RAW: the scanner software reported the Max Detail as done, but its file was not found."); return NULL; }
    swprintf(rpath, MAX_PATH * 2 + 8, L"%ls.rsrc", src);
    if (keep_raw) { wchar_t k[MAX_PATH * 2], ps[16] = L""; if (rawj.pass > 1) swprintf(ps, 16, L"_p%d", rawj.pass);
                    swprintf(k, MAX_PATH * 2, L"%ls\\MaxDetPM%ls.bin", logdir, ps); CopyFileW(src, k, FALSE);
                    if (rawj.pass == 1) { swprintf(k, MAX_PATH * 2, L"%ls\\MaxDetPM.rsrc", logdir); CopyFileW(rpath, k, FALSE); } }   /* developer option --keep-raw */
    int lines = 0, pix = 0;
    FILE *f = _wfopen(src, L"rb"); if (!f) { log_w(L"RAW: cannot open the Max Detail file."); return NULL; }
    _fseeki64(f, 0, SEEK_END); long long n = _ftelli64(f); _fseeki64(f, 0, SEEK_SET);
    int ch = 0;
    if (!pm_size(rpath, &lines, &pix) && lines > 0 && pix > 0) {
        if (n == (long long)lines * pix * 6) ch = 3; else if (n == (long long)lines * pix * 2) ch = 1; }
    { wchar_t *b = wcsrchr(src, L'\\'); wchar_t m[300]; swprintf(m, 300, L"RAW: %ls = %d pixels x %d lines, %d channel(s), %lld bytes (requested %d x %d).", b ? b + 1 : src, pix, lines, ch, n, raw_req_w, raw_req_h); log_d(m); }
    if (!ch) { fclose(f); log_w(L"RAW: the file size does not match the size the driver recorded - not converted."); return NULL; }
    if (pix != raw_req_w || lines != raw_req_h) { fclose(f); log_w(L"RAW: the file is not the size that was requested - not converted (an older Max Detail?)."); return NULL; }
    if ((long long)lines * pix * ch * 2 > 3900000000LL) { fclose(f); log_w(L"RAW: the picture is larger than a TIFF file can hold (4 GB) - choose a lower resolution or a smaller crop."); return NULL; }
    /* 12-bit sample -> 16-bit: in the driver's 14-bit mode the 14 -> 12 bit table it applied is undone (each 12-bit
       value becomes the middle of the 14-bit values the table mapped onto it) and the 14-bit value is written x 4;
       otherwise the 12-bit value is written x 16 */
    static uint16_t lut[4096]; int inpol = raw_inpol14; double sl = raw_tab_slope, of = raw_tab_off;
    int lin = inpol == 1 && sl > 0.01 && sl < 1 && of >= 0 && of < 4096;
    for (int v = 0; v < 4096; v++) {
        double o;
        if (lin) { double v14 = v < (int)of ? v : of + (v - of + 0.5) / sl; if (v14 > 16383) v14 = 16383; o = v14 * 4; }
        else o = v * 16.0;
        lut[v] = (uint16_t)(o > 65535 ? 65535 : o + 0.5);
    }
    if (rawj.pass == 1) { wchar_t m[300]; if (lin) swprintf(m, 300, L"RAW: the scanner worked in its 14-bit mode; the driver's 14 -> 12 bit table (slope %.7f above %.0f) is undone: 16-bit value = 14-bit value x 4.", sl, of);
      else swprintf(m, 300, L"RAW: %ls - 16-bit value = 12-bit value x 16.", inpol == 0 ? L"the scanner did not use its 14-bit mode" : L"14-bit mode unknown");
      log_d(m); }
    size_t rowb = (size_t)pix * ch * 2;
    uint16_t *img = malloc((size_t)lines * pix * ch * sizeof(uint16_t)); uint8_t *in = malloc(rowb);
    if (!img || !in) { free(img); free(in); fclose(f); log_w(L"RAW: not enough memory for this picture."); return NULL; }
    /* developer option --sim-bed-texture SIGMA (simulator only): the samples are replaced by a texture fixed to the
       bed (mm), seen through this block's crop as sent (left/top/mm per pixel), with a small per-block position error
       and gain and Gaussian noise - so the putting together of RAW blocks can be checked end to end against the
       picture the whole crop must give (scripts/tilecheck.py) */
    double bl = atof(rawj.cx), bt = atof(rawj.cy), mmpp = 25.4 / rawj.dpi * 100 / (atof(rawj.scale) > 0 ? atof(rawj.scale) : 100);
    int kb = rawj.pass; double edx = 0.3 * sin(kb * 1.3), edy = 0.2 * cos(kb * 0.7), eg = 1 + 0.01 * sin(kb * 1.7);
    uint64_t nz = 0x9E3779B97F4A7C15ULL ^ (uint64_t)kb * 0x2545F4914F6CDD1DULL;
    for (int y = 0; y < lines; y++) {
        if (fread(in, 1, rowb, f) != rowb) { free(img); free(in); fclose(f); log_w(L"RAW: the Max Detail file is short."); return NULL; }
        uint16_t *d = img + (size_t)y * pix * ch;
        for (int x = 0; x < pix; x++) {
            int sx = rawj.flip ? pix - 1 - x : x;
            if (simulate && sim_bed_sigma >= 0) {
                int xd = rawj.mount == 0 ? pix - 1 - sx : sx;   /* display order: the preview is shown mirrored for transparent originals */
                double u = bl / mmpp + xd + edx, v = bt / mmpp + y + (rawj.stream && sim_drop_line >= 0 && y >= sim_drop_line) + edy;
                for (int c = 0; c < ch; c++) { nz ^= nz << 13; nz ^= nz >> 7; nz ^= nz << 17; double g1 = (nz >> 11) * (1.0 / 9007199254740992.0) + 1e-12;
                    nz ^= nz << 13; nz ^= nz >> 7; nz ^= nz << 17; double g2 = (nz >> 11) * (1.0 / 9007199254740992.0);
                    double val = eg * bed_tex(u, v, c) + sim_bed_sigma * sqrt(-2 * log(g1)) * cos(6.283185307 * g2);
                    d[x * ch + c] = (uint16_t)(val < 0 ? 0 : val > 65535 ? 65535 : val + 0.5); }
                continue; }
            for (int c = 0; c < ch; c++) d[x * ch + c] = lut[(in[(sx * ch + c) * 2] << 8 | in[(sx * ch + c) * 2 + 1]) & 4095];
        }
    }
    free(in); fclose(f);
    *pix_o = pix; *lines_o = lines; *ch_o = ch; *lin_o = lin;
    return img;
}
static void raw_outname(wchar_t *dst, const wchar_t *suffix) {
    wchar_t nm[300]; to_w(rawj.name, nm, 300); for (wchar_t *q = nm; *q; q++) if (wcschr(L"<>:\"\\|?*", *q)) *q = L'_';
    wchar_t *dot = wcsrchr(nm, L'.'); if (dot && (!_wcsicmp(dot, L".tif") || !_wcsicmp(dot, L".tiff"))) *dot = 0;
    CreateDirectoryW(scandir_, NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    swprintf(dst, MAX_PATH * 2, L"%ls\\%04d%02d%02d-%02d%02d%02d %ls RAW16%ls.tif", scandir_, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, nm, suffix);
}
static void raw_desc(char *desc, size_t n, int npass) {
    int k = snprintf(desc, n, "Scitex Smart342 RAW: Max Detail prescan at %.1f dpi, 12 bits per channel from the driver, %s; "
        "no gradation, colour table or sharpening; %s%s%s", rawj.dpi, rawj.lin ? "14-bit mode, the driver's 14->12 table undone, value = 14-bit x 4" : "value = 12-bit x 16",
        rawj.mount == 0 ? "transparent" : "reflective", rawj.negative ? ", negative (not inverted)" : "", rawj.mirror ? ", mirrored" : "");
    if (rawj.noalign) k += snprintf(desc + k, n - k, "; the scanner's colour alignment OFF (red, green and blue are the CCD's own samples of their rows)");
    if (rawj.stephold && rawj.sh_mode == 6) k += snprintf(desc + k, n - k, rawj.sh_expose == 0 ? "; carriage STEP-AND-HOLD for each line (the driver's STEP mode, motion board 1012) instead of continuous motion" : "; carriage in STEPS for each line (the driver's STEP mode, motion board 1012) with %d motor pulses of creep during each line", rawj.sh_expose);
    if (rawj.ps_F && rawj.ps_info.n > 1) {
        const SrInfo *in = &rawj.ps_info;
        k += snprintf(desc + k, n - k, "; PIXEL SHIFT %dx across: %d passes, lens carriage at", rawj.ps_F, in->n);
        for (int i = 0; i < in->n && k < (int)n - 200; i++) k += snprintf(desc + k, n - k, " %+d", rawj.ps_step[i]);
        { char sl[96] = ""; if (ps_slow) snprintf(sl, sizeof sl, "; slow lens moves (320e) in %d of %d passes made", rawj.ps_nslow, rawj.ps_nmade);
          k += snprintf(desc + k, n - k, " motor steps (%s%s); offsets against pass 1 (across/along, px):", ps_approach ? "each approached from above: 8 steps above, then down - the scanner driver's own correction brings it up from 1 mm below" : "straight from the driver's lens position", sl); }
        for (int i = 1; i < in->n && k < (int)n - 200; i++) k += snprintf(desc + k, n - k, " %+.3f/%+.3f", in->dx[i], in->dy[i]);
        { int nu = 0; for (int i = 0; i < in->n; i++) nu += in->used[i] != 0;
          if (nu > 0 && nu < in->n) { k += snprintf(desc + k, n - k, "; left out (farther from their columns than another pass):");
              for (int i = 0; i < in->n && k < (int)n - 200; i++) if (!in->used[i]) k += snprintf(desc + k, n - k, " pass %d", i + 1); } }
        k += snprintf(desc + k, n - k, "; largest phase gap %.2f cell, farthest fine column %.2f column from a sample; each sample placed at its measured position on the %dx grid (nearest column, no sharpening), brightness matched per pass; across %.1f dpi (CCD sampling %.1f dpi x %d), along %.1f dpi",
                      in->maxgap, in->griddist, rawj.ps_F, rawj.ps_xdpi, rawj.ps_xdpi / rawj.ps_F, rawj.ps_F, rawj.dpi);
    }
    if (npass > 1) {
        k += snprintf(desc + k, n - k, "; mean of %d passes aligned to pass 1 (Lanczos-3), shifts in pixels:", npass);
        for (int i = 1; i < npass && k < (int)n - 40; i++) k += snprintf(desc + k, n - k, " %+.3f/%+.3f", rawj.sh_dx[i], rawj.sh_dy[i]);
    }
    snprintf(desc + k, n - k, ".");
}
/* 0.1.27: Mode "Greyscale (B/W)" + RAW: one 16-bit channel from the GREEN row only. Measured on the Scitex focus chart
   (hardware s31, 10 Oct, pixel shift): green holds the finest detail (contrast across at 58 / 67 lp/mm G .152 / .103, R
   .134 / .082, B about .04 - blue is clearly softer, red a little); along R = G. The scanner's own B/W TIFF mixes about
   20 % red, 70 % green and 5 - 10 % blue near greys (its 3D table for B/W read from the original driver in the
   simulator) - a standard luminance mix would give about 9 % less contrast at the finest detail than green alone, but
   less sensor noise (green alone: about 1.33 x the noise of that mix). Returns the channel count to write. Called before
   the colour registration, which then has nothing to do (one channel). Not for the RAW check / focus / lens tests. */
static int raw_bw(uint16_t **pimg, int w, int h, int ch, char *desc, size_t dn) {
    if (rawj.mode_i != 2 || ch != 3 || rt_ovr.on || ft_on || lt_on) return ch;
    size_t n = (size_t)w * h; uint16_t *g = malloc(n * sizeof(uint16_t));
    if (!g) { log_w(L"B/W RAW: not enough memory for the one-channel copy - saved with three channels."); return ch; }
    for (size_t i = 0; i < n; i++) g[i] = (*pimg)[i * 3 + 1];
    free(*pimg); *pimg = g;
    size_t k = strlen(desc);
    snprintf(desc + k, dn - k, " B/W (Mode Greyscale): ONE channel - the green row's samples only (the sharpest of the three rows; red and blue left out).");
    log_w(L"B/W RAW: saved as one 16-bit channel from the GREEN row only - the sharpest of the CCD's three colour rows (red a little softer, blue clearly). Mode Color RGB keeps all three channels.");
    return 1;
}
/* RAW colour registration (rawreg.c, 0.1.22): R and B measured against G in the picture and resampled onto it (G
   untouched). *pimg is replaced by the registered picture (the old buffer freed); a note is added to desc. Not for the
   RAW check / focus / lens tests, nor single-channel RAWs. */
static void raw_colreg(uint16_t **pimg, int w, int h, int ch, char *desc, size_t dn) {
    if ((!colreg_on && !rawj.noalign) || ch != 3 || rt_ovr.on || ft_on || lt_on) return;
    wchar_t m[700]; RrInfo in;
    if (rr_measure(*pimg, w, h, &in)) { log_w(L"RAW colour registration: picture too small or out of memory - not done."); return; }
    if (!in.ok[0] && !in.ok[1]) {
        int dis = 0; for (int c = 0; c < 2; c++) if (in.madx[c] > RR_MAX_MAD || in.mady[c] > RR_MAX_MAD) dis = 1;
        log_d(dis ? L"RAW colour registration: red and blue against green differ between the parts of the picture (colour film: the colours' pictures differ) - the RAW is left as delivered."
                  : L"RAW colour registration: no detail to measure red and blue against green - the RAW is left as delivered."); return; }
    if (!rr_moves(&in, 0, w) && !rr_moves(&in, 1, w)) { swprintf(m, 700, L"RAW colour registration: red and blue already in register with green (within %.2f px) - the RAW is left as delivered.", RR_MIN_MOVE); log_d(m); return; }
    uint16_t *o = malloc((size_t)w * h * 3 * sizeof(uint16_t));
    if (!o || rr_apply(*pimg, w, h, &in, o)) { free(o); log_w(L"RAW colour registration: out of memory - not done."); return; }
    free(*pimg); *pimg = o;
    int k = (int)strlen(desc);
    k += snprintf(desc + k, dn - k, " COLOUR REGISTRATION by this program: red and blue measured against green in the picture (%d bands) and resampled onto it (Lanczos-3); green as delivered;", in.nb);
    for (int c = 0; c < 2 && k < (int)dn - 160; c++) {
        if (in.ok[c] && !rr_moves(&in, c, w)) k += snprintf(desc + k, dn - k, " %s in register (below %.2f px), as delivered;", c ? "blue" : "red", RR_MIN_MOVE);
        else if (in.ok[c]) {   /* only the directions that were measured are moved (review: no "dx +0.000" for one left) */
            char ax[80], ay[80];
            if (in.corrx[c]) snprintf(ax, sizeof ax, "across dx %+.3f %+.5f/px", in.a_dx[c], in.b_dx[c]); else snprintf(ax, sizeof ax, "across left as delivered (bands disagree)");
            if (in.corry[c]) snprintf(ay, sizeof ay, "along dy %+.3f %+.5f/px", in.a_dy[c], in.b_dy[c]); else snprintf(ay, sizeof ay, "along left as delivered (bands disagree)");
            k += snprintf(desc + k, dn - k, " %s moved: %s, %s (x from the centre);", c ? "blue" : "red", ax, ay); }
        else k += snprintf(desc + k, dn - k, " %s not measurable, as delivered;", c ? "blue" : "red");
    }
    { wchar_t p[2][200];
      for (int c = 0; c < 2; c++) {
          wchar_t ax[60], ay[60];
          if (in.corrx[c]) swprintf(ax, 60, L"across %+.2f px", in.a_dx[c]); else swprintf(ax, 60, L"across left (bands disagree)");
          if (in.corry[c]) swprintf(ay, 60, L"along %+.2f px", in.a_dy[c]); else swprintf(ay, 60, L"along left (bands disagree)");
          if (!in.ok[c]) swprintf(p[c], 200, L"%ls not measurable, as delivered", c ? L"blue" : L"red");
          else if (!rr_moves(&in, c, w)) swprintf(p[c], 200, L"%ls already in register (below %.2f px), as delivered", c ? L"blue" : L"red", RR_MIN_MOVE);
          else swprintf(p[c], 200, L"%ls %ls, %ls%ls", c ? L"blue" : L"red", ax, ay, in.whole[c] ? L" (narrow picture: one shift for the whole width)" : L""); }
      swprintf(m, 700, L"RAW colour registration (moved onto green, at the centre of the picture): %ls; %ls.", p[0], p[1]); }
    log_d(m);
}
/* RAW sharpening (rawsharp.c, 0.1.22): an extra file "<name> SHARP-<level>.tif" next to the RAW, which stays as it is.
   xdpi / ydpi: the picture's sampling; ps: pixel-shift result (its across samples are CCD samples at their positions). */
static void raw_sharpen(const wchar_t *dst, const uint16_t *img, int w, int h, int ch, double xdpi, double ydpi, int ps, const char *desc0) {
    if (rsh_level <= 0 || rt_ovr.on || ft_on || lt_on) return;
    wchar_t m[900];
    double fnum = rsh_lens_fnum(pv_lens);
    if (fnum <= 0 || pv_opt_dpi <= 0) { swprintf(m, 900, L"RAW sharpening: not made - the lens of this scan is not known%ls.", pv_lens == 4 ? L" (line-work lens)" : L""); log_w(m); return; }
    RshModel md; memset(&md, 0, sizeof md);
    md.fnum = fnum; md.cell_um = 7.0; md.mag = 0.007 * pv_opt_dpi / 25.4; md.pitch_x_um = 25400.0 / xdpi; md.pitch_y_um = 25400.0 / ydpi;
    md.motion_um = md.pitch_y_um; md.sigma_ccd_um = 1.83; md.K = RSH_K[rsh_level];
    /* 0.3.0: with step-and-hold (on by default since 0.2.0) the carriage stands still during each line - no motion blur
       to restore along (s34: 70 - 100 % of the one-row smear gone); the model kept it, so the SHARP file was sharpened
       along for a blur that was not there */
    int still = rawj.stephold && rawj.sh_mode == 6 && rawj.sh_expose == 0;
    if (still) md.motion_um = 0;
    /* 0.1.24: fitted to the Scitex focus chart's bars (s21 / s23 / s24, 35 mm lens; scripts/sharpness/fit_chart.py, rms
       0.012): residual Gaussian 1.693 um across / 3.573 um along at the CCD, halo 0.458 across / 0.198 along of 7.66 um */
    md.sigma_x_ccd_um = 1.693; md.sigma_y_ccd_um = 3.573; md.halo_x = 0.458; md.halo_y = 0.198; md.halo_sigma_ccd_um = 7.66;
    /* 0.3.0: this scanner's own fit for this lens (Set up this scanner, focus chart), when measured */
    int own = pv_lens >= 0 && pv_lens <= 3 && wz_get(pv_lens, L"sharp_rms", -1) >= 0;
    if (own) { md.sigma_x_ccd_um = wz_get(pv_lens, L"sharp_sx", md.sigma_x_ccd_um); md.sigma_y_ccd_um = wz_get(pv_lens, L"sharp_sy", md.sigma_y_ccd_um);
               md.halo_x = wz_get(pv_lens, L"sharp_ax", md.halo_x); md.halo_y = wz_get(pv_lens, L"sharp_ay", md.halo_y); md.halo_sigma_ccd_um = wz_get(pv_lens, L"sharp_sh", md.halo_sigma_ccd_um); }
    md.interp_x = (!ps && fabs(xdpi - pv_opt_dpi) > 0.001 * pv_opt_dpi) ? pv_opt_dpi / xdpi : 0;
    RshInfo in; if (rsh_design(&md, &in)) { log_w(L"RAW sharpening: the filter could not be made - not saved."); return; }
    uint16_t *out = malloc((size_t)w * h * ch * sizeof(uint16_t));
    if (!out) { log_w(L"RAW sharpening: not enough memory - not saved."); return; }
    status(L"RAW sharpening..."); DWORD t0 = GetTickCount();
    rsh_apply(&in, img, w, h, ch, out);
    wchar_t sd[MAX_PATH * 2]; wcscpy(sd, dst); { wchar_t *dot = wcsrchr(sd, L'.'); if (dot) *dot = 0; }
    { wchar_t suf[32]; swprintf(suf, 32, L" SHARP-%lc.tif", L"OLMS"[rsh_level]); wcscat(sd, suf); }
    char desc[4000]; snprintf(desc, sizeof desc, "SHARPENED COPY of a RAW (the RAW file next to it is unchanged). %s RAW SHARPENING (%ls): Wiener restoration (K %.4f) of a model of the optics and sensor - "
        "diffraction f/%.1f at magnification %.6f, CCD cell 7 um (%.2f um on the film), line movement %.4f um%s, residual Gaussian %.3f um across / %.3f um along at the CCD, halo %.3f across / %.3f along of %.2f um, scanner interpolation step across %.6f (0: CCD samples); "
        "%s; 15 x 15 taps; white-noise gain %.2f (G). Response along (G) at %.0f/%.0f/%.0f/%.0f lp/mm %.2f/%.2f/%.2f/%.2f -> %.2f/%.2f/%.2f/%.2f.",
        desc0, RSH_NAME[rsh_level], md.K, fnum, md.mag, md.cell_um / md.mag, md.motion_um, still ? " (step-and-hold: the carriage stood still)" : "", md.sigma_x_ccd_um, md.sigma_y_ccd_um, md.halo_x, md.halo_y, md.halo_sigma_ccd_um, md.interp_x,
        own ? "fitted to the Scitex focus chart scanned with this lens on this scanner (Set up this scanner)" : "fitted to the Scitex focus chart scanned with the 35 mm lens of another Smart 342L on 10 Oct 2026 (this scanner's own: Set up this scanner with the chart)", in.noise_gain[1],
        in.f_rep[0], in.f_rep[1], in.f_rep[2], in.f_rep[3], in.rep_model_y[0], in.rep_model_y[1], in.rep_model_y[2], in.rep_model_y[3], in.rep_resp_y[0], in.rep_resp_y[1], in.rep_resp_y[2], in.rep_resp_y[3]);
    int ok = write_tiff16xy(sd, out, w, h, ch, xdpi, ydpi, desc); free(out);
    if (!ok) { log_w(L"RAW sharpening: writing the TIFF failed (disk full?)."); return; }
    swprintf(m, 900, L"RAW sharpening (%ls, %.1f s): contrast of fine detail along at %.0f / %.0f / %.0f lp/mm on the film %.2f / %.2f / %.2f -> %.2f / %.2f / %.2f, across %.2f / %.2f / %.2f -> %.2f / %.2f / %.2f (model%ls); grain and noise x%.1f. Saved as: %ls",
        RSH_NAME[rsh_level], (GetTickCount() - t0) / 1000.0, in.f_rep[1], in.f_rep[2], in.f_rep[3], in.rep_model_y[1], in.rep_model_y[2], in.rep_model_y[3], in.rep_resp_y[1], in.rep_resp_y[2], in.rep_resp_y[3],
        in.rep_model_x[1], in.rep_model_x[2], in.rep_model_x[3], in.rep_resp_x[1], in.rep_resp_x[2], in.rep_resp_x[3],
        own ? L", fitted to the focus chart with this lens on this scanner" : pv_lens == 2 ? L", fitted to the focus chart with this lens on another Smart 342L" : L" - measured only for the 35 mm lens", in.noise_gain[1], sd);
    log_w(m);
}
/* 0.5.0: a long-exposure RAW (one pass with k x the integration time, the HDR hook): did the longer time happen, did the
   driver accept the motion, is anything clipped. 0 = save it (rawj.long_r = the ratio applied, or 0 when it did not
   happen - then it is a normal RAW and says so), -1 = nothing saved */
static int raw_long_check(const uint16_t *img, int pix, int lines, int ch, double secs) {
    wchar_t m[700]; rawj.long_r = 0;
    if (!raw_hdr_fired) { log_w(L"RAW long exposure: the longer exposure was NOT applied (the driver did not reach the point where it is set) - this RAW is a NORMAL exposure and is saved as one."); return 0; }
    int still = rawj.stephold && rawj.sh_mode == 6 && rawj.sh_expose == 0;
    swprintf(m, 700, L"RAW long exposure (%.0f s): integration time %.2f -> %.2f (scanner maximum %.1f); %ls.", secs, raw_hdr_t0, raw_hdr_t1, raw_hdr_tmax,
             still ? L"carriage step-and-hold (no speed limit)" : raw_last_range == 0 ? L"the driver's planned speed in range" : L"the driver did NOT accept the motion");
    log_d(m);
    if (!still && rawj.stephold && rawj.sh_mode == 6) { log_w(L"RAW long exposure: the carriage crept during each line (the rows are coarser than the CCD's sampling) - nothing saved. Scan at or above the lens's CCD resolution, or switch step-and-hold off."); return -1; }
    if (!still && raw_last_range != 0) { log_w(L"RAW long exposure: the driver did not accept the motion for the longer exposure - nothing saved. Try Long x2, step-and-hold at the CCD's resolution, or a lower resolution."); return -1; }
    rawj.long_r = raw_hdr_t0 > 0 ? raw_hdr_t1 / raw_hdr_t0 : rawj.long_k;
    if (raw_hdr_dark != 1) log_w(L"RAW long exposure: the dark calibration at the longer time did not run - the black level may be slightly high (noted in the file).");
    size_t n = (size_t)pix * lines * ch, nc = 0; for (size_t i = 0; i < n; i++) nc += img[i] >= 65000;
    if (nc > n / 1000) { swprintf(m, 700, L"RAW long exposure: %.2f %% of the values are clipped (too bright for x %.0f) - for this film use Long x2 or Normal.", 100.0 * nc / n, rawj.long_r); log_w(m); }
    else { uint16_t mx = 0; for (size_t i = 0; i < n; i++) if (img[i] > mx) mx = img[i];
           swprintf(m, 700, L"RAW long exposure: x %.3f applied, nothing clipped (brightest value %.0f %% of the range).", rawj.long_r, mx * 100.0 / 65532); log_d(m); }
    return 0;
}
/* 0.4.0: the positive of a negative RAW (negconv.c), next to it: " POS16.tif" */
static double med16(const uint16_t *img, size_t n, int ch, int c, double sc) {   /* median of one channel, film pixels only (not holder, not clear); sc: the picture is sc x a normal exposure (0.5.0 long exposure) */
    static uint32_t hh[65536]; memset(hh, 0, sizeof hh); size_t k = 0; int key = ch == 3 ? 1 : 0;
    for (size_t i = 0; i < n; i++) { const uint16_t *q = img + i * ch; if (q[key] < NC_OPAQUE * sc || q[key] > NC_WHITE / 2 * sc) continue; hh[q[c]]++; k++; }
    if (k < 100) return -1;
    size_t acc = 0; for (int v = 0; v < 65536; v++) { acc += hh[v]; if (acc >= k / 2) return v; }
    return -1;
}
static void raw_positive(const wchar_t *dst, const uint16_t *img, int w, int h, int ch, double xdpi, double ydpi) {
    if (!rawj.negative || !neg_pos_on || rt_ovr.on || ft_on || lt_on || (ch != 1 && ch != 3)) return;
    NcParams np; neg_params(&np); NcLevels lv; memset(&lv, 0, sizeof lv); wchar_t src[200] = L"", m[900];
    double lk = rawj.long_r > 1 ? rawj.long_r : 1;   /* 0.5.0: a long-exposure RAW is lk x a normal one */
    if (neg_lock && neg_lock_lv.ok) { lv = neg_lock_lv; swprintf(src, 200, L"the kept levels (a roll)");
        if (lk > 1) for (int c = 0; c < lv.ch; c++) { lv.lo[c] -= log10(lk); lv.hi[c] -= log10(lk); } }   /* 0.5.0: long exposure (the integration ratio) */
    else if (pv_r12 && pv_r12_frame == rawj.frame && pv_mount == rawj.mount && pv_mm_cols > 0 && pv_mm_rows > 0) {
        /* the preview's frame levels, when the preview's brightness at the RAW's place matches the RAW's (the same scale) */
        uint16_t *p16 = pv_to16(); NcLevels pl;
        if (p16 && pv_neg_levels(p16, &pl) == 0) {
            int W = pv_r12_w, H = pv_r12_h, a0 = 0, a1 = W - 1, b0 = 0, b1 = H - 1;
            if (rawj.usecrop) { double l = atof(rawj.cx), t = atof(rawj.cy), cw = atof(rawj.cw), chh = atof(rawj.ch_);
                int d0 = (int)floor(l / pv_mm_cols * W), d1 = (int)ceil((l + cw) / pv_mm_cols * W) - 1;   /* display columns */
                a0 = pv_mirror ? W - 1 - d1 : d0; a1 = pv_mirror ? W - 1 - d0 : d1;
                b0 = (int)floor(t / pv_mm_rows * H); b1 = (int)ceil((t + chh) / pv_mm_rows * H) - 1;
                if (a0 < 0) a0 = 0; if (b0 < 0) b0 = 0; if (a1 > W - 1) a1 = W - 1; if (b1 > H - 1) b1 = H - 1; }
            double rr[3] = { 0, 0, 0 }; int match = a1 - a0 >= 4 && b1 - b0 >= 4;
            if (match) { size_t n = (size_t)(a1 - a0 + 1) * (b1 - b0 + 1); uint16_t *sub = malloc(n * 6);
                if (!sub) match = 0;
                else { size_t k = 0; for (int y = b0; y <= b1; y++) for (int x = a0; x <= a1; x++) { memcpy(sub + 3 * k, p16 + 3 * ((size_t)y * W + x), 6); k++; }
                    for (int c = 0; c < ch; c++) { double mp = med16(sub, n, 3, ch == 3 ? c : 1, 1), mr = med16(img, (size_t)w * h, ch, c, lk);
                        rr[c] = mp > 0 && mr > 0 ? mr / mp : 0; if (!(rr[c] > 0.9 * lk && rr[c] < 1.1 * lk)) match = 0; }
                    free(sub); } }
            if (match) { lv = pl; swprintf(src, 200, L"the preview of frame %d (its brightness matches this RAW's: %.3f / %.3f / %.3f)", rawj.frame, rr[0], rr[1], rr[2]);
                /* a long-exposure RAW is rr x brighter = log10(rr) less dense: the preview's levels moved by that, per channel,
                   so the RAW's own finer values are used in full (0.5.0) */
                if (lk > 1) for (int c = 0; c < lv.ch; c++) { double r = rr[lv.ch == 3 ? c : 0]; if (r > 0) { lv.lo[c] -= log10(r); lv.hi[c] -= log10(r); } } }
            else { swprintf(m, 900, L"Negative: the preview's brightness does not match this RAW's (%.3f / %.3f / %.3f) - the positive uses this RAW's own levels.", rr[0], rr[1], rr[2]); log_d(m); }
        }
        free(p16);
    }
    if (!lv.ok) { int x0, y0, x1, y1;
        /* the levels from the RAW itself; a long-exposure RAW is measured on a copy brought to the normal scale (the holder /
           clear-film limits are for that scale), then the levels are moved back (0.5.0) */
        const uint16_t *li = img; uint16_t *sc = NULL;
        if (lk > 1 && (sc = malloc((size_t)w * h * ch * sizeof(uint16_t))) != NULL) { for (size_t i = 0; i < (size_t)w * h * ch; i++) sc[i] = (uint16_t)(img[i] / lk + 0.5); li = sc; }
        int bad = nc_area(li, w, h, ch, &x0, &y0, &x1, &y1) || nc_levels(li, w, h, ch, x0, y0, x1, y1, &np, &lv);
        free(sc);
        if (bad) { log_w(L"Negative: no positive made - this RAW has no density range to convert (is it a negative?)."); return; }
        if (lk > 1) for (int c = 0; c < lv.ch; c++) { lv.lo[c] -= log10(lk); lv.hi[c] -= log10(lk); }
        swprintf(src, 200, L"this RAW (%d - %d x %d - %d)", x0, x1, y0, y1); }
    if (lv.ch == 3 && ch == 1) { lv.ch = 1; lv.lo[0] = lv.lo[1]; lv.hi[0] = lv.hi[1]; }   /* a greyscale RAW: the green levels */
    if (lv.ch != ch) { log_w(L"Negative: the kept levels are for colour - no positive made for this greyscale RAW (Tools > Negatives > each frame its own)."); return; }
    neg_fix_range(&lv);
    uint16_t *out = malloc((size_t)w * h * ch * sizeof(uint16_t));
    if (!out || nc_convert(img, (size_t)w * h, ch, &lv, &np, out)) { free(out); log_w(L"Negative: not enough memory for the positive - not made."); return; }
    int ow = w, oh = h; double ox = xdpi, oy = ydpi;
    if (neg_rot) { uint16_t *r = nc_rotate(out, w, h, ch, neg_rot, &ow, &oh); if (r) { free(out); out = r; if (neg_rot & 1) { ox = ydpi; oy = xdpi; } } else log_w(L"Negative: not enough memory to rotate the positive - saved unrotated."), ow = w, oh = h; }
    wchar_t pd[MAX_PATH * 2]; wcscpy(pd, dst); { wchar_t *dot = wcsrchr(pd, L'.'); if (dot) *dot = 0; } wcscat(pd, L" POS16.tif");
    char desc[1500], s8[300]; to_utf8(src, s8, sizeof s8);
    snprintf(desc, sizeof desc, "POSITIVE made from a NEGATIVE RAW (the RAW file next to it is unchanged) by Smart342 for Windows: per channel the density D = log10(65535 / value) between the levels "
             "lo / hi (red %.4f / %.4f, green %.4f / %.4f, blue %.4f / %.4f; from %s), scene range %.3f (film gamma %.2f), linear light 10^((t - 1) x range) with black removed, "
             "encoded with gamma 2.2 into %.0f %% - %.0f %% of the 16-bit range%s.", lv.lo[0], lv.hi[0], lv.lo[ch == 3 ? 1 : 0], lv.hi[ch == 3 ? 1 : 0], lv.lo[ch == 3 ? 2 : 0], lv.hi[ch == 3 ? 2 : 0], s8,
             lv.range, np.gamma, np.out_lo * 100, np.out_hi * 100, neg_rot == 1 ? "; rotated 90 degrees clockwise" : neg_rot == 2 ? "; rotated 180 degrees" : neg_rot == 3 ? "; rotated 90 degrees counter-clockwise" : "");
    tiff_pos = 1; int ok = write_tiff16xy(pd, out, ow, oh, ch, ox, oy, desc); tiff_pos = 0; free(out);
    if (!ok) { log_w(L"Negative: writing the positive failed (disk full?)."); return; }
    swprintf(m, 900, L"Positive saved (levels from %ls; contrast %ls%ls): %ls", src, NEG_CNAME[neg_contrast], neg_rot == 1 ? L", rotated right" : neg_rot == 2 ? L", rotated 180" : neg_rot == 3 ? L", rotated left" : L"", pd);
    log_w(m);
}
static void raw_extras(const wchar_t *dst, const uint16_t *img, int w, int h, int ch, double xdpi, double ydpi, int ps, const char *desc0) {
    raw_sharpen(dst, img, w, h, ch, xdpi, ydpi, ps, desc0);
    raw_positive(dst, img, w, h, ch, xdpi, ydpi);
}
/* experimental HDR (rawhdr.c): pass 1 normal, pass 2 with k x the integration time (rawscan.c hdr_bp); returns as
   raw_done. img is taken over (freed here). */
static int raw_hdr_step(uint16_t *img, int pix, int lines, int ch, double secs) {
    wchar_t m[700];
    if (rawj.pass == 1) {
        rawj.hdr_n = img; rawj.w = pix; rawj.h = lines; rawj.ch = ch;
        if (rawj.keep) { wchar_t dst[MAX_PATH * 2]; raw_outname(dst, L" normal"); char desc[1500]; raw_desc(desc, sizeof desc, 1);
            if (!write_tiff16(dst, img, pix, lines, ch, rawj.dpi, desc)) log_w(L"RAW: saving this pass failed (disk full?)."); }
        rawj.hdr_still1 = rawj.stephold && rawj.sh_mode == 6 && rawj.sh_expose == 0;
        swprintf(m, 700, L"RAW HDR pass 1 (normal exposure) done (%.0f s, %ls). Pass 2: %.0f x the integration time.", secs, rawj.hdr_still1 ? L"step-and-hold" : L"continuous motion", rawj.hdr_k); log_d(m);
        rawj.pass = 2; status(L"RAW HDR: pass 2 (longer exposure) running..."); raw_push(); return 1;
    }
    uint16_t *n = rawj.hdr_n; rawj.hdr_n = NULL;
    if (!n || pix != rawj.w || lines != rawj.h || ch != rawj.ch) { free(n); free(img); log_w(L"RAW HDR: pass 2 is not the size of pass 1 - stopped."); return -1; }
    /* did the longer exposure happen, and did the driver accept the motion for it? (never assumed) */
    if (!raw_hdr_fired) { free(n); free(img); log_w(L"RAW HDR: the longer exposure was NOT applied (the driver did not reach the point where it is set) - nothing merged."); return -1; }
    int still2 = rawj.stephold && rawj.sh_mode == 6 && rawj.sh_expose == 0;
    if (still2) swprintf(m, 700, L"RAW HDR pass 2 (%.0f s): integration time %.2f -> %.2f (scanner maximum %.1f); carriage step-and-hold (the driver's STEP mode: no speed limit - the carriage stands still while each line is exposed).",
                         secs, raw_hdr_t0, raw_hdr_t1, raw_hdr_tmax);
    else swprintf(m, 700, L"RAW HDR pass 2 (%.0f s): integration time %.2f -> %.2f (scanner maximum %.1f); the driver planned speed %.1f, range check %ls.",
             secs, raw_hdr_t0, raw_hdr_t1, raw_hdr_tmax, raw_last_speed, raw_last_range == 0 ? L"in range" : raw_last_range < 0 ? L"TOO SLOW" : raw_last_range > 0 ? L"TOO FAST" : L"not seen");
    log_d(m);
    if (!still2 && rawj.stephold && rawj.sh_mode == 6) { free(n); free(img); log_w(L"RAW HDR: pass 2 was made in steps but the carriage crept during each line (the rows are coarser than the CCD's sampling) - nothing merged. Scan at or above the lens's CCD resolution for step-and-hold, or switch step-and-hold off."); return -1; }
    if (!still2 && raw_last_range != 0) { free(n); free(img); log_w(L"RAW HDR: the driver did not accept the motion for the longer exposure - nothing merged. Try +1 stop or a lower resolution."); return -1; }
    if (rawj.hdr_still1 >= 0 && rawj.hdr_still1 != still2) { swprintf(m, 700, L"RAW HDR: note - pass 1 was made %ls, pass 2 %ls; the two passes are not equally sharp along the scan. Please send the Logs folder.",
        rawj.hdr_still1 ? L"step-and-hold" : L"with continuous motion", still2 ? L"step-and-hold" : L"with continuous motion"); log_w(m); }
    if (raw_hdr_dark != 1) { free(n); free(img); swprintf(m, 700, L"RAW HDR: the dark calibration at the longer time did not run or failed (%d) - nothing merged.", raw_hdr_dark); log_w(m); return -1; }
    log_d(L"RAW HDR: dark calibration redone at the longer time (as the driver does for final scans).");
    if (hdr_test_gain > 0 && simulate) {   /* developer option (simulator only): the simulator does not model exposure */
        for (size_t i = 0; i < (size_t)pix * lines * ch; i++) { double v = img[i] * hdr_test_gain + 30; img[i] = v > 65532 ? 65532 : (uint16_t)(v + 0.5); }
        swprintf(m, 700, L"TEST: pass 2 multiplied by %.2f (+30) to stand in for the longer exposure the simulator does not model.", hdr_test_gain); log_d(m); }
    if (rawj.keep) { wchar_t dst[MAX_PATH * 2], suf[40]; swprintf(suf, 40, L" bright x%.0f", rawj.hdr_k); raw_outname(dst, suf); char desc[1500]; raw_desc(desc, sizeof desc, 1);
        snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " Longer exposure: integration time %.2f instead of %.2f (white calibration of the normal time), as delivered.", raw_hdr_t1, raw_hdr_t0);
        if (!write_tiff16(dst, img, pix, lines, ch, rawj.dpi, desc)) log_w(L"RAW: saving this pass failed (disk full?)."); }
    size_t ns = (size_t)pix * lines * ch;
    float *al = malloc(ns * sizeof(float)); uint16_t *out = malloc(ns * sizeof(uint16_t));
    if (!al || !out) { free(al); free(out); free(n); free(img); log_w(L"RAW HDR: not enough memory to merge - choose a smaller crop."); return -1; }
    RsInfo in; double g0 = 0; DWORD a0 = GetTickCount(); HdrAlignInfo la;
    int ra = hdr_align2(n, img, pix, lines, ch, al, &in, &g0, &la);
    if (ra) { free(al); free(out); free(n); free(img);
        swprintf(m, 700, L"RAW HDR: the passes could not be lined up (%ls, rough brightness ratio %.2f) - nothing merged.", ra == -3 ? L"too few usable pixels" : ra == -4 ? L"they do not match" : L"memory", g0);
        log_w(m); return -1; }
    double txa = 1e9, txb = -1e9, tya = 1e9, tyb = -1e9;
    for (int t = 0; t < in.ntiles; t++) if (in.tscore[t] > 0.5) {
        if (in.tdx[t] < txa) txa = in.tdx[t]; if (in.tdx[t] > txb) txb = in.tdx[t];
        if (in.tdy[t] < tya) tya = in.tdy[t]; if (in.tdy[t] > tyb) tyb = in.tdy[t]; }
    swprintf(m, 700, L"RAW HDR: rough brightness ratio %.3f (integration ratio %.3f); pass 2 shifted %+.3f px across, %+.3f px along (match %.4f); 3 x 3 tiles: across %+.3f .. %+.3f, along %+.3f .. %+.3f.",
             g0, raw_hdr_t0 > 0 ? raw_hdr_t1 / raw_hdr_t0 : 0, in.dx, in.dy, in.score, txa < 1e8 ? txa : 0, txb > -1e8 ? txb : 0, tya < 1e8 ? tya : 0, tyb > -1e8 ? tyb : 0);
    log_d(m);
    if (txb - txa > 2 || tyb - tya > 2) {   /* not one plain shift: e.g. the picture is stretched along the scan in pass 2 */
        free(al); free(out); free(n); free(img);
        log_w(L"RAW HDR: the two passes differ by more than a plain shift (tiles disagree by over 2 pixels: the longer exposure may have changed the geometry) - nothing merged. Please send the Logs folder."); return -1; }
    /* 0.4.1: the shift per band along the scan and per channel (rawhdr.c local_align) */
    if (la.local) {
        if (ch == 3) swprintf(m, 700, L"RAW HDR: alignment per band along the scan (%d bands; measured R %d, G %d, B %d%ls): along R %+.3f .. %+.3f, G %+.3f .. %+.3f, B %+.3f .. %+.3f px; across R %+.3f .. %+.3f, G %+.3f .. %+.3f, B %+.3f .. %+.3f px.",
                              la.nbands, la.nvalid[0], la.nvalid[1], la.nvalid[2], la.borrowed[0] || la.borrowed[1] || la.borrowed[2] ? L"; a channel with too few took the best channel's profile" : L"",
                              la.dymin[0], la.dymax[0], la.dymin[1], la.dymax[1], la.dymin[2], la.dymax[2], la.dxmin[0], la.dxmax[0], la.dxmin[1], la.dxmax[1], la.dxmin[2], la.dxmax[2]);
        else swprintf(m, 700, L"RAW HDR: alignment per band along the scan (%d bands, %d measured): along %+.3f .. %+.3f px, across %+.3f .. %+.3f px.", la.nbands, la.nvalid[0], la.dymin[0], la.dymax[0], la.dxmin[0], la.dxmax[0]);
        log_d(m);
    } else { swprintf(m, 700, L"RAW HDR: one shift for the whole picture (%ls).", la.why == 1 ? L"too few rows to split into bands" : la.why == 2 ? L"too few bands with dark detail to measure" : L"memory"); log_d(m); }
    HdrInfo hi; int rm = hdr_merge(n, al, pix, lines, ch, out, &hi);
    free(al); free(img);
    if (rm == 0 && hi.spill_px < 0) log_w(L"RAW HDR: the spill-over check could not run (memory) - light spilling from overexposed areas may show in the shadows.");
    if (rm == 0 && hi.spill_px >= 0) { swprintf(m, 700, L"RAW HDR: spill-over check - %lld pixels (%.3f %% of those that would use the longer pass) were brighter in the longer pass than the normal pass allows (light spilling from overexposed areas); the normal pass is used there.", hi.spill_px, 100 * hi.spill_frac); log_d(m); }
    if (ch == 3) swprintf(m, 700, L"RAW HDR fit (pass 2 = g x pass 1 + o): R g %.3f o %.0f, G g %.3f o %.0f, B g %.3f o %.0f (rms %.0f/%.0f/%.0f, %lld/%lld/%lld blocks of 8 x 8); from pass 2: %.1f %%, blended %.1f %%, normal %.1f %% (%.1f s).",
                          hi.g[0], hi.o[0], hi.g[1], hi.o[1], hi.g[2], hi.o[2], hi.fit_rms[0], hi.fit_rms[1], hi.fit_rms[2], hi.nfit[0], hi.nfit[1], hi.nfit[2],
                          100 * hi.frac_bright, 100 * hi.frac_blend, 100 * hi.frac_normal, (GetTickCount() - a0) / 1000.0);
    else swprintf(m, 700, L"RAW HDR fit: g %.3f o %.0f (rms %.0f, %lld blocks of 8 x 8); from pass 2: %.1f %%, blended %.1f %%, normal %.1f %%.", hi.g[0], hi.o[0], hi.fit_rms[0], hi.nfit[0],
                  100 * hi.frac_bright, 100 * hi.frac_blend, 100 * hi.frac_normal);
    log_d(m);
    if (rm == -2) { free(out); free(n); log_w(L"RAW HDR: pass 2 is not brighter than pass 1 (g below 1.2) - the longer exposure had no effect; nothing merged."); return -1; }
    { double ir = raw_hdr_t0 > 0 ? raw_hdr_t1 / raw_hdr_t0 : 0, gg = hi.g[ch == 3 ? 1 : 0];
      if (ir > 0 && rm == 0 && fabs(gg / ir - 1) > 0.25) { swprintf(m, 700, L"RAW HDR: note - the measured brightness ratio %.3f differs from the integration ratio %.3f by more than 25 %%; the merge uses the measured one. Please send the Logs folder.", gg, ir); log_w(m); } }
    if (rm == -1) { free(out); free(n); log_w(L"RAW HDR: too few pixels inside the range of both passes to compare them - nothing merged."); return -1; }
    wchar_t dst[MAX_PATH * 2], suf[40]; swprintf(suf, 40, L" HDR+%.0f", log2(rawj.hdr_k)); raw_outname(dst, suf);
    char desc[2500]; raw_desc(desc, sizeof desc, 1);
    snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " EXPERIMENTAL HDR: merged with a pass of integration time %.2f instead of %.2f (fitted brightness ratio G %.3f): darker parts from the longer pass, brightest from the normal one; normal-exposure scale. Longer pass aligned %s; spill-over check: %s%lld pixels kept from the normal pass.",
             raw_hdr_t1, raw_hdr_t0, hi.g[ch == 3 ? 1 : 0], la.local ? "per band along the scan and per channel" : "with one shift", hi.spill_px < 0 ? "NOT RUN, " : "", hi.spill_px < 0 ? 0LL : hi.spill_px);
    int och = raw_bw(&out, pix, lines, ch, desc, sizeof desc);
    raw_colreg(&out, pix, lines, och, desc, sizeof desc);
    int ok = write_tiff16(dst, out, pix, lines, och, rawj.dpi, desc); if (ok) raw_extras(dst, out, pix, lines, och, rawj.dpi, rawj.dpi, 0, desc); free(out); free(n);
    if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
    swprintf(m, 700, L"RAW HDR scan saved (%d x %d pixels, %d x 16 bits, %.0f s in all): %ls", pix, lines, och, (GetTickCount() - rawj.t0) / 1000.0, dst);
    log_w(m); status(L"RAW HDR scan finished."); next_file_name(); return 0;
}
/* after each Max Detail: 1 = another pass was started (the job goes on), 0 = saved, -1 = failed (see the log) */
/* one block of a tiled RAW done: put it in place (registered in the overlap, gain matched, blended); the next block,
   or the picture when all are done. 1 = next block running, 0 = saved, -1 = failed */
static int raw_tile_step(uint16_t *img, int pix, int lines, int ch, double secs) {
    int k = rawj.tile;
    if (ch != 3) { free(img); rtm_free(&rawj.tm); rawj.tm_on = 0; log_w(L"RAW blocks: only colour is supported - stopped."); return -1; }
    int ox, oy; tile_crop(k, &ox, &oy);                     /* the origin of this block (the crop it was sent with) */
    /* crop coordinates are those of the preview as displayed (mirrored for transparent originals, as the original
       displays it); a RAW after raw_read shows its crop in display order unless the Mirror tick is set (raw_read flips
       by transparent XOR Mirror; the display is mirrored for transparent): so a block sits at its display column,
       mirrored only with the Mirror tick (found in review: placing by the transparent flip reversed the block order) */
    int x0 = rawj.mirror ? rawj.tW - (ox + pix) : ox;
    RtTileInfo inf; if (rtm_add(&rawj.tm, img, pix, lines, x0, oy, 32, 8, &inf)) { free(img); rtm_free(&rawj.tm); rawj.tm_on = 0; log_w(L"RAW blocks: not enough memory - stopped."); return -1; }
    free(img);
    wchar_t m[400];
    if (inf.registered) {
        swprintf(m, 400, L"RAW block %d of %d (%d x %d at %d, %d, %.0f s): shift %+.2f / %+.2f px%ls, gain R %.4f G %.4f B %.4f%ls, overlap difference %.0f -> %.0f",
                 k + 1, rawj.ntiles, pix, lines, x0, oy, secs, inf.dx, inf.dy, inf.score == -2 ? L" (nothing to measure - nominal place)" : L"",
                 inf.gain[0], inf.gain[1], inf.gain[2], inf.gain_used ? L"" : L" (not applied)", inf.rms_before, inf.rms_after);
        double d = fmax(fabs(inf.dx), fabs(inf.dy)); if (d > rawj.tdx_max) rawj.tdx_max = d;
        for (int c = 0; c < 3; c++) if (fabs(inf.gain[c] - 1) > rawj.tgain_dev) rawj.tgain_dev = fabs(inf.gain[c] - 1);
        if (inf.score == -2) rawj.tunreg++;
    } else swprintf(m, 400, L"RAW block %d of %d (%d x %d at %d, %d, %.0f s): placed (first block)", k + 1, rawj.ntiles, pix, lines, x0, oy, secs);
    log_d(m);
    if (inf.registered && inf.ov_mean > 0 && inf.rms_after > 0.08 * inf.ov_mean) { swprintf(m, 400, L"RAW block %d: WARNING - its overlap does not match what is already placed (difference %.0f at level %.0f); please check the picture there and send the Logs folder.", k + 1, inf.rms_after, inf.ov_mean); log_w(m); rawj.tedge++; }
    if (inf.registered && inf.at_edge) { swprintf(m, 400, L"RAW block %d: WARNING - the best match lay at the edge of the 8-pixel search; this block may be misplaced. Please send the Logs folder.", k + 1); log_w(m); rawj.tedge++; }
    if (k + 1 < rawj.ntiles) {
        rawj.tile = k + 1; rawj.pass = k + 2; tile_crop(rawj.tile, &ox, &oy);
        wchar_t st[100]; swprintf(st, 100, L"RAW block %d of %d running...", rawj.tile + 1, rawj.ntiles); status(st);
        raw_push(); return 1;
    }
    size_t n = (size_t)rawj.tW * rawj.tH * 3; uint16_t *out = malloc(n * sizeof(uint16_t));
    if (!out) { rtm_free(&rawj.tm); rawj.tm_on = 0; log_w(L"RAW blocks: not enough memory for the picture."); return -1; }
    long long miss = rtm_result(&rawj.tm, out); rtm_free(&rawj.tm); rawj.tm_on = 0;
    if (miss) { swprintf(m, 400, L"RAW blocks: %lld border samples were not covered (the margin could not be scanned there) - filled from their neighbours.", miss); log_w(m); }
    swprintf(m, 400, L"RAW blocks put together: largest shift %.2f px, largest gain difference %.2f %%%ls.", rawj.tdx_max, rawj.tgain_dev * 100, rawj.tunreg ? L"; some overlaps had no detail to measure (nominal place used)" : L""); log_d(m);
    if (rawj.tedge) { swprintf(m, 400, L"RAW blocks: %d block(s) could not be placed with certainty (see the warnings above).", rawj.tedge); log_w(m); }
    wchar_t dst[MAX_PATH * 2]; raw_outname(dst, L""); char desc[2500]; raw_desc(desc, sizeof desc, 1);
    snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " Made of %d RAW blocks (registered in 64-pixel overlaps, gain matched, blended).", rawj.ntiles);
    int och = raw_bw(&out, rawj.tW, rawj.tH, 3, desc, sizeof desc);
    raw_colreg(&out, rawj.tW, rawj.tH, och, desc, sizeof desc);
    int ok = write_tiff16(dst, out, rawj.tW, rawj.tH, och, rawj.dpi, desc); if (ok) raw_extras(dst, out, rawj.tW, rawj.tH, och, rawj.dpi, rawj.dpi, 0, desc); free(out);
    if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
    swprintf(m, 400, L"RAW scan saved (%d x %d pixels, %d x 16 bits, %.1f MB, %d blocks, %.0f s): %ls", rawj.tW, rawj.tH, och, (double)rawj.tW * rawj.tH * och * 2 / 1048576.0, rawj.ntiles, (GetTickCount() - rawj.t0) / 1000.0, dst);
    log_w(m); pg_done(); status(L"RAW scan finished."); next_file_name(); return 0;
}
/* pixel shift (0.1.21): one pass done (img taken over). Returns as raw_done: 1 = the next pass is running, 0 = saved,
   -1 = failed. After each pass from the second on, the passes are registered against pass 1 (rawsr.c sr_measure, the
   same measurement the combine makes): when their sub-pixel phases across fill the F-times-finer grid, they are combined;
   otherwise the next lens step is taken (up to PS_MAX passes). */
/* pass 1 alone as a plain RAW (the passes did not line up, or no second pass was usable) */
static int ps_save_pass1(int pix, int lines, int ch, const char *why) {
    wchar_t m[700]; int F = rawj.ps_F;
    wchar_t dst[MAX_PATH * 2]; raw_outname(dst, L" PS pass1"); char desc[2500]; rawj.ps_info.n = 0; raw_desc(desc, sizeof desc, 1);
    snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " Pixel shift pass 1 only (%s), %.1f dpi across, %.1f along.", why, rawj.ps_xdpi / F, rawj.dpi);
    uint16_t *p1 = rawj.ps_img[0]; rawj.ps_img[0] = NULL;   /* taken over: colour registration may replace it (review) */
    int och = raw_bw(&p1, pix, lines, ch, desc, sizeof desc);
    raw_colreg(&p1, pix, lines, och, desc, sizeof desc);
    int ok = write_tiff16xy(dst, p1, pix, lines, och, rawj.ps_xdpi / F, rawj.dpi, desc); if (ok) raw_extras(dst, p1, pix, lines, och, rawj.ps_xdpi / F, rawj.dpi, 0, desc); free(p1);
    ps_end(); if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
    swprintf(m, 700, L"RAW scan saved (pass 1 only): %ls", dst); log_w(m); pg_done(); status(L"RAW scan finished (pixel shift not combined - see the log)."); next_file_name(); return 0;
}
static int raw_ps_step(uint16_t *img, int pix, int lines, int ch, double secs) {
    wchar_t m[700]; int k = rawj.ps_n, F = rawj.ps_F, step = rawj.ps_cmd;
    int npass = rawj.passes == 8 ? 8 : 5;   /* the setting at the start of this RAW (review: the menu may change meanwhile) */
    if (rawj.keep) { wchar_t dst[MAX_PATH * 2], suf[48]; swprintf(suf, 48, L" PS pass%d step%+d", rawj.pass, step); raw_outname(dst, suf);
        char desc[1500]; { int sn = rawj.ps_info.n; rawj.ps_info.n = 0; raw_desc(desc, sizeof desc, 1); rawj.ps_info.n = sn; } snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " Pixel shift pass %d, lens carriage %+d steps, as delivered (%.1f dpi across, %.1f along).", rawj.pass, step, rawj.ps_xdpi / F, rawj.dpi);
        if (!write_tiff16xy(dst, img, pix, lines, ch, rawj.ps_xdpi / F, rawj.dpi, desc)) log_w(L"RAW: saving this pass failed (disk full?)."); }
    if (k == 0) { rawj.ps_w = pix; rawj.ps_h = lines; rawj.ps_ch = ch; }
    else if (pix != rawj.ps_w || lines != rawj.ps_h || ch != rawj.ps_ch) { free(img); ps_end(); log_w(L"Pixel shift: this pass is not the size of pass 1 - stopped."); return -1; }
    rawj.ps_nmade++; if (ps_slow && lens_shift_slow_used) rawj.ps_nslow++;
    swprintf(m, 700, L"Pixel shift pass %d (lens carriage %+d steps): %d x %d pixels, %.0f s%ls.", rawj.pass, step, pix, lines, secs,
             !ps_slow ? L"" : lens_shift_slow_used ? L"; slow lens moves" : L"; slow lens moves NOT used (see the trace)"); log_d(m);
    if (k == 0) { rawj.ps_img[0] = img; rawj.ps_ax[0] = rawj.ps_ay[0] = 0; rawj.ps_step[0] = step; rawj.ps_no[0] = rawj.pass; rawj.ps_n = 1; }
    else {   /* 0.1.24: every pass's offset checked before it is used (sr_accept_pass; hardware 10 Oct: periodic bars made
                the registration lock whole bar periods off, and such passes were combined) */
        double along_tol = fmax(3.0, 0.04 * rawj.dpi / 25.4);   /* lines: 0.04 mm (the scan start repeats within ~0.02 mm) */
        /* the direction of the picture's movement per lens step in the RAW as stored (after raw_read's flip): negative in
           every hardware session (s11 - s26: transparent, not mirrored = flipped); the other orientation is inferred
           (the flip reverses x), not measured */
        int sign = rawj.flip ? -1 : 1;
        double dx, dy, sc, fdx, fdy, fsc; int how;
        int r = sr_accept_pass(rawj.ps_img[0], img, pix, lines, ch, rawj.ps_ax, rawj.ps_ay, rawj.ps_step, rawj.ps_n, step, sign, along_tol, &dx, &dy, &sc, &how, &fdx, &fdy, &fsc);
        if (r < 0) { free(img); ps_end(); log_w(L"Pixel shift: not enough memory - stopped."); return -1; }
        if (r == 1) {
            if (how) { swprintf(m, 700, L"Pixel shift pass %d: first measured %+.3f px across, %+.3f px along (match %.4f) - not plausible for lens step %+d, or too weak a match (repeating detail can match a whole period off); measured again from the expected place: %+.3f / %+.3f (match %.4f) - used.", rawj.pass, fdx, fdy, fsc, step, dx, dy, sc); log_d(m); }
            rawj.ps_img[k] = img; rawj.ps_ax[k] = dx; rawj.ps_ay[k] = dy; rawj.ps_step[k] = step; rawj.ps_no[k] = rawj.pass; rawj.ps_n = k + 1;
        } else {
            swprintf(m, 700, L"WARNING: pixel shift pass %d (lens step %+d) LEFT OUT: measured %+.3f px across, %+.3f px along (match %.4f) - not plausible for this step or too weak a match - and no plausible match near the expected place. A crop with only repeating detail (bars) can cause this - include an edge or other non-repeating detail.", rawj.pass, step, fdx, fdy, fsc); log_w(m);
            free(img); rawj.ps_rej++; rawj.ps_rejstep = step;
        }
    }
    int combine = 0, made = rawj.pass;
    if (rawj.ps_n >= 2) {
        SrInfo in; int r = sr_measure2((const uint16_t *const *)rawj.ps_img, rawj.ps_n, pix, lines, ch, F, rawj.ps_ax, rawj.ps_ay, &in);
        if (r == -1) { ps_end(); log_w(L"Pixel shift: not enough memory (or a crop under 16 pixels) - stopped."); return -1; }
        if (r == -2) {   /* a pass that does not line up with pass 1: keep pass 1 as a plain RAW, say so */
            int bad = 1; while (bad < rawj.ps_n - 1 && in.score[bad] >= 0.5) bad++;
            swprintf(m, 700, L"WARNING: pixel shift: pass %d does not line up with pass 1 (match %.3f) - not combined. Pass 1 is saved as a plain RAW (%.1f dpi across, %.1f along). Please send the Logs folder.", rawj.ps_no[bad], in.score[bad], rawj.ps_xdpi / F, rawj.dpi); log_w(m);
            return ps_save_pass1(pix, lines, ch, "the passes did not line up"); }
        for (int i = 1; i < rawj.ps_n; i++) { swprintf(m, 700, L"  pass %d against pass 1: %+.3f px across, %+.3f px along (match %.4f) - samples at %.2f of a CCD cell", rawj.ps_no[i], in.dx[i], in.dy[i], in.score[i], in.phase[i]); log_d(m); }
        swprintf(m, 700, L"Pixel shift: every %dx column has a sample within %.2f of a column (needed: %.2f or less, after at least %d passes); largest gap between the sample positions %.2f of a cell.", F, in.griddist, SR_GRIDDIST_OK, PS_MIN, in.maxgap); log_d(m);
        rawj.ps_info = in;
        /* 0.1.23: the normal setting stops once every finer column class is fed within SR_BIAS_OK of a column (the expected
           position error of the combine, sr_grid_cost2 - the same rule the unit test's curves use); covered alone was not
           enough (the 0, +1, +7 of 10 Oct: covered, but one class 0.21 of a column off) */
        double mb = 0.5; { double pos[PS_MAX]; for (int i = 0; i < rawj.ps_n; i++) pos[i] = -in.dx[i]; sr_grid_cost2(pos, rawj.ps_n, F, &mb); }
        swprintf(m, 700, L"Pixel shift: largest expected position error of a %dx column class %.2f of a column (the normal setting stops at %.2f or less).", F, mb, SR_BIAS_OK); log_d(m);
        if (npass < 8 && rawj.ps_n >= PS_MIN && in.griddist <= SR_GRIDDIST_OK && mb <= SR_BIAS_OK) combine = 1;   /* normal: stop once close */
        else if (made >= npass && in.griddist <= SR_GRIDDIST_OK) combine = 1;              /* all the passes of the setting made */
        else if (made >= npass) { combine = 1;
            swprintf(m, 700, L"WARNING: pixel shift: after %d passes some %dx columns are still %.2f of a column from the nearest sample - combined anyway; those columns are filled in between (less gain than possible). Please send the Logs folder.", made, F, in.griddist); log_w(m); }
    } else if (made >= npass) {   /* 0.1.24: no second pass was usable */
        swprintf(m, 700, L"WARNING: pixel shift: none of the %d further passes could be placed - pass 1 is saved as a plain RAW. Please send the Logs folder.", made - 1); log_w(m);
        return ps_save_pass1(pix, lines, ch, "no further pass could be placed");
    }
    if (!combine) {   /* the next lens step from the measured offsets (rawsr.c sr_next_step) */
        int nk;
        if (rawj.ps_n >= 2) {
            /* candidates predicted beyond a quarter of the crop's smaller side are not taken: the registration searches
               about a third of it (rawstack.c coarse) - review 0.1.23 */
            double max_off = (pix < lines ? pix : lines) / 4.0;
            double pgd = 0, pph = 0; nk = sr_next_step3(rawj.ps_info.dx, rawj.ps_step, rawj.ps_n, F, npass == 8, max_off, &pgd, &pph);
            if (nk < 0) { nk = rawj.ps_step[rawj.ps_n - 1] + 1; swprintf(m, 700, L"Pixel shift: the passes so far do not show how far a step moves the picture - next step %+d.", nk); }
            else swprintf(m, 700, L"Pixel shift: next lens step %+d (predicted from the measured passes: samples at %.2f of a CCD cell, every %dx column within %.2f of a sample).", nk, pph, F, pgd);
            log_d(m);
        } else nk = PS_STEPS[1];   /* only pass 1 so far: the second step again */
        if (rawj.ps_rej && nk == rawj.ps_rejstep && nk + 1 <= SR_STEP_MAX) {   /* review 0.1.24: not the step that was just left out again */
            nk++; swprintf(m, 700, L"Pixel shift: lens step %+d was just left out - next step %+d instead.", nk - 1, nk); log_d(m); }
        if (nk > SR_STEP_MAX) {
            if (rawj.ps_n < 2) return ps_save_pass1(pix, lines, ch, "no further pass could be placed");
            combine = 1; swprintf(m, 700, L"WARNING: pixel shift: the next step would be beyond %d motor steps - combined with the passes so far. Please send the Logs folder.", SR_STEP_MAX); log_w(m); }
        else rawj.ps_cmd = nk;
    }
    if (!combine) {
        rawj.pass++;
        { wchar_t st[80]; swprintf(st, 80, L"Pixel shift RAW running (pass %d)...", rawj.pass); status(st); }
        if (rawj.pass > pg.total) pg_set_total(rawj.pass, pg.upto);   /* one more pass than planned (up to 5 / 8) */
        raw_push(); return 1;
    }
    lens_shift_pulses = 0;
    int W = pix * F; uint16_t *out = malloc((size_t)W * lines * ch * sizeof(uint16_t));
    if (!out) { ps_end(); log_w(L"Pixel shift: not enough memory for the result."); return -1; }
    SrInfo in; DWORD c0 = GetTickCount(); int nused = 0;
    int r = sr_combine2((const uint16_t *const *)rawj.ps_img, rawj.ps_n, pix, lines, ch, F, rawj.ps_ax, rawj.ps_ay, out, &in);   /* from the checked offsets (0.1.24) */
    if (r) { free(out); ps_end(); swprintf(m, 700, L"Pixel shift: the combine failed (%d) - nothing saved.", r); log_w(m); return -1; }
    rawj.ps_info = in;
    { int q = swprintf(m, 700, L"Pixel shift: %d passes combined in %.1f s; brightness matched to pass 1:", in.n, (GetTickCount() - c0) / 1000.0);
      for (int i = 1; i < in.n && q < 600; i++) q += swprintf(m + q, 700 - q, L" pass %d x%.4f", rawj.ps_no[i], in.gain[i][ch == 3 ? 1 : 0]);
      log_d(m);
      q = swprintf(m, 700, L"Pixel shift: passes placed on the finer grid:"); int nu = 0;
      for (int i = 0; i < in.n && q < 600; i++) { q += swprintf(m + q, 700 - q, L" %d%ls", rawj.ps_no[i], in.used[i] ? L"" : L" (left out: farther from its columns than another pass)"); nu += in.used[i]; }
      if (nu < in.n) log_d(m); nused = nu; }
    wchar_t dst[MAX_PATH * 2], suf[32]; swprintf(suf, 32, L" PS%dx", F); raw_outname(dst, suf);
    char desc[2500]; raw_desc(desc, sizeof desc, 1);
    int och = raw_bw(&out, W, lines, ch, desc, sizeof desc);
    raw_colreg(&out, W, lines, och, desc, sizeof desc);
    int ok = write_tiff16xy(dst, out, W, lines, och, rawj.ps_xdpi, rawj.dpi, desc); if (ok) raw_extras(dst, out, W, lines, och, rawj.ps_xdpi, rawj.dpi, 1, desc); free(out);
    int n = in.n, nrej = rawj.ps_rej; ps_end();
    if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
    wchar_t of[40] = L"", rj[60] = L""; if (nused < n) swprintf(of, 40, L", %d placed", nused);
    if (nrej) swprintf(rj, 60, L"; %d more left out as implausible", nrej);
    swprintf(m, 700, L"RAW scan saved, pixel shift %dx across from %d passes%ls%ls (%d x %d pixels, %.1f dpi across x %.1f dpi along, %d x 16 bits, %.1f MB, %.0f s in all): %ls",
             F, n, of, rj, W, lines, rawj.ps_xdpi, rawj.dpi, och, (double)W * lines * och * 2 / 1048576.0, (GetTickCount() - rawj.t0) / 1000.0, dst);
    log_w(m); pg_done(); status(L"RAW scan finished (pixel shift)."); next_file_name(); return 0;
}
static int raw_done(void) {
    int pix = 0, lines = 0, ch = 0, lin = 0;
    raw_stream_armed = 0;
    if (!rawj.stream && raw_stream_sent != rawj.stream_sent0) { log_w(L"WARNING: this RAW was meant as a normal request but was streamed - please send the Logs folder."); }
    if (rawj.stream) {
        if (raw_stream_sent == rawj.stream_sent0) { log_w(L"WARNING: streamed RAW: the Max Detail block was not seen by the safety layer, so it was sent as a normal request - this RAW is not streamed. Please send the Logs folder."); return -1; }
        log_d(L"RAW: streamed - the scanner passed it through its memory in blocks (one request, cmd 4).");
        rc_wait_log();
    } else if (raw_stream_cnt_seq != rawj.cnt_seq0) {   /* normal RAW: the counters for comparison (0.1.21) */
        wchar_t m[300]; swprintf(m, 300, L"RAW (normal request): the scanner's counters - \"host - controler timing violation\" %ld, \"controler - bip timing violation\" %ld.", (long)raw_stream_wait, (long)raw_stream_cnt1); log_d(m);
    }
    uint16_t *img = raw_read(&pix, &lines, &ch, &lin);
    if (!img) { if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; } if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; } return -1; }
    rawj.lin = lin;
    lens_report();   /* the lens of THIS RAW (lens_bp fires at every Max Detail) - for the sharpening model (review) */
    raw_noalign = 0; raw_stephold = 0;
    if (rawj.stephold) { wchar_t m[300];
        rawj.sh_mode = raw_motor_seq != rawj.mm_seq0 ? raw_motor_mode : 0; rawj.sh_expose = raw_motor_expose;
        if (raw_stephold_fired == rawj.sh_fired0) swprintf(m, 300, L"RAW: step-and-hold was NOT switched on for this pass (the hook did not run) - continuous motion as normal; please send the Logs folder.");
        else if (!rawj.sh_mode) swprintf(m, 300, L"RAW: step-and-hold switched on, but the picture's motion command was not seen - please send the Logs folder.");
        else if (rawj.sh_mode != 6) swprintf(m, 300, L"RAW: step-and-hold switched on, but the driver still moved the carriage CONTINUOUSLY (mode %d) - please send the Logs folder.", rawj.sh_mode);
        else if (raw_motor_expose == 0) swprintf(m, 300, L"RAW: the carriage moved STEP-AND-HOLD for this pass (the driver's STEP mode, motion board 1012, %d rows, still during each line).", raw_motor_rows);
        else swprintf(m, 300, L"RAW: the carriage moved in STEPS for this pass (the driver's STEP mode, motion board 1012, %d rows) but crept %d motor pulses during each line - the rows are coarser than the CCD's sampling; for no smear scan along at or above %.1f dpi.", raw_motor_rows, raw_motor_expose, OPT_SHOW(pv_opt_dpi));
        if (raw_stephold_fired != rawj.sh_fired0 && rawj.sh_mode == 6 && raw_motor_expose == 0) log_d(m); else log_w(m); }
    if (rawj.noalign) { wchar_t m[300];
        swprintf(m, 300, L"RAW: the scanner's colour alignment was %ls for this pass (%ls).", raw_noalign_fired != rawj.align_fired0 ? L"OFF" : L"NOT switched off - the hook did not run",
                 raw_lutx_seq == rawj.align_seq0 ? L"the column vectors were not seen" : raw_lutx_equal ? L"red, green and blue read the same CCD columns - the CCD's own samples" : L"red and blue still interpolated - please send the Logs folder");
        log_w(m); }
    double secs = (GetTickCount() - rawj.tp) / 1000.0;
    if (rawj.tiled) return raw_tile_step(img, pix, lines, ch, secs);
    if (rawj.hdr_k > 1) return raw_hdr_step(img, pix, lines, ch, secs);
    if (rawj.ps_F) return raw_ps_step(img, pix, lines, ch, secs);
    if (rawj.passes == 1) {
        if (rt_on) rc_measure(img, pix, lines, ch);
        if (ft_on) ft_measure(img, pix, lines, ch);
        if (lt_on) lt_measure(img, pix, lines, ch);
        if (ch_on) ch_measure(img, pix, lines, ch);
        if (fs_on && fs_state == FS_RAW) { fs_measure(img, pix, lines, ch); free(img); return 0; }   /* 0.4.0: Fix streaks - measured, not saved */
        if (rawj.long_k > 1 && hdr_test_gain > 0 && simulate && raw_hdr_fired) {   /* developer option (simulator only): the simulator does not model exposure */
            for (size_t i = 0; i < (size_t)pix * lines * ch; i++) { double v = img[i] * hdr_test_gain; img[i] = v > 65532 ? 65532 : (uint16_t)(v + 0.5); }
            wchar_t tm[200]; swprintf(tm, 200, L"TEST: this RAW multiplied by %.2f to stand in for the longer exposure the simulator does not model.", hdr_test_gain); log_d(tm); }
        if (rawj.long_k > 1 && raw_long_check(img, pix, lines, ch, secs)) { free(img); return -1; }
        wchar_t dst[MAX_PATH * 2]; raw_outname(dst, L""); char desc[2500]; raw_desc(desc, sizeof desc, 1);
        if (rawj.long_r > 1) snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " LONG EXPOSURE: integration time %.2f instead of %.2f (x %.3f) with the white calibration of the normal time - the values are about %.2f x those of a Normal RAW of the same film (more levels and less noise for dense film; anything brighter than 1/%.2f of the empty frame clips).%s",
                                      raw_hdr_t1, raw_hdr_t0, rawj.long_r, rawj.long_r, rawj.long_r, raw_hdr_dark == 1 ? " Dark calibration redone at the longer time." : " NOTE: the dark calibration at the longer time did not run - the black level may be slightly high.");
        int och = raw_bw(&img, pix, lines, ch, desc, sizeof desc);
        raw_colreg(&img, pix, lines, och, desc, sizeof desc);
        int ok = write_tiff16(dst, img, pix, lines, och, rawj.dpi, desc); if (ok) raw_extras(dst, img, pix, lines, och, rawj.dpi, rawj.dpi, 0, desc); free(img);
        if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
        wchar_t m[MAX_PATH * 3]; swprintf(m, MAX_PATH * 3, L"RAW scan saved (%d x %d pixels, %d x 16 bits, %.1f MB, %.0f s): %ls", pix, lines, och, (double)lines * pix * och * 2 / 1048576.0, (GetTickCount() - rawj.t0) / 1000.0, dst);
        log_w(m); pg_done(); status(L"RAW scan finished."); if (!rt_ovr.on) next_file_name(); return 0;
    }
    /* several passes */
    if (rawj.keep) { wchar_t dst[MAX_PATH * 2], suf[32]; swprintf(suf, 32, L" pass%d", rawj.pass); raw_outname(dst, suf);
        char desc[1500]; raw_desc(desc, sizeof desc, 1); snprintf(desc + strlen(desc), sizeof desc - strlen(desc), " Pass %d of %d, as delivered (not aligned).", rawj.pass, rawj.passes);
        if (!write_tiff16(dst, img, pix, lines, ch, rawj.dpi, desc)) log_w(L"RAW: saving this pass failed (disk full?)."); }
    if (rawj.pass == 1) {
        if (rs_init(&rawj.st, pix, lines, ch)) { free(img); log_w(L"RAW: not enough memory to average this picture - choose 1 pass or a smaller crop."); return -1; }
        rawj.st_on = 1; rawj.w = pix; rawj.h = lines; rawj.ch = ch;
    } else if (pix != rawj.w || lines != rawj.h || ch != rawj.ch) {
        free(img); rs_free(&rawj.st); rawj.st_on = 0; log_w(L"RAW: this pass is not the size of pass 1 - stopped."); return -1;
    }
    RsInfo in; DWORD a0 = GetTickCount();
    int r = rs_add(&rawj.st, img, &in); free(img);
    if (r) { rs_free(&rawj.st); rawj.st_on = 0; log_w(L"RAW: not enough memory to align this pass - stopped."); return -1; }
    wchar_t m[600];
    if (rawj.pass == 1) { swprintf(m, 600, L"RAW pass 1 of %d done (%.0f s) - the reference for the others.", rawj.passes, secs); log_d(m); }
    else {
        if (in.used) { rawj.sh_dx[rawj.st.n - 1] = in.dx; rawj.sh_dy[rawj.st.n - 1] = in.dy; }
        double txa = 1e9, txb = -1e9, tya = 1e9, tyb = -1e9;
        for (int t = 0; t < in.ntiles; t++) if (in.tscore[t] > 0.5) {
            if (in.tdx[t] < txa) txa = in.tdx[t]; if (in.tdx[t] > txb) txb = in.tdx[t];
            if (in.tdy[t] < tya) tya = in.tdy[t]; if (in.tdy[t] > tyb) tyb = in.tdy[t]; }
        swprintf(m, 600, L"RAW pass %d of %d (%.0f s): shifted %+.3f px across, %+.3f px along against pass 1 (match %.4f); 3 x 3 tiles: across %+.3f .. %+.3f, along %+.3f .. %+.3f; %ls (%.1f s).",
                 rawj.pass, rawj.passes, secs, in.dx, in.dy, in.score, txa < 1e8 ? txa : 0, txb > -1e8 ? txb : 0, tya < 1e8 ? tya : 0, tyb > -1e8 ? tyb : 0,
                 in.resampled ? L"resampled (Lanczos-3)" : L"whole pixels, copied", (GetTickCount() - a0) / 1000.0);
        log_d(m);
        if (!in.used) { swprintf(m, 600, L"RAW: pass %d does not match pass 1 (match %.3f) - left out of the mean.", rawj.pass, in.score); log_w(m); }
        else if (in.score == -2) log_d(L"RAW: note - the centre of the picture has no measurable detail; this pass is added with the whole-pixel shift only.");
        else {
            if (in.weak == 1) log_d(L"RAW: note - in one direction the centre has too little detail to measure the shift; that direction keeps the whole-pixel value.");
            if (in.score < 0.9 || (txb - txa) > 0.5 || (tyb - tya) > 0.5)
                log_w(L"RAW: note - this pass does not line up with pass 1 as one plain shift (low match or tiles disagree); the average may be softer than one pass.");
        }
        /* pass-to-pass difference after alignment (16-bit units), per channel and, for one channel, by level */
        if (in.used) {
        int gc = ch == 3 ? 1 : 0; double tot[3] = { 0 }; long long tn[3] = { 0 };
        for (int c = 0; c < ch; c++) for (int b = 0; b < 4; b++) { tot[c] += in.drms[c][b] * in.drms[c][b] * in.dn[c][b]; tn[c] += in.dn[c][b]; }
        wchar_t lv[200] = L""; int k = 0;
        static const wchar_t *LV[4] = { L"0-25 %", L"25-50 %", L"50-75 %", L"75-100 %" };
        for (int b = 0; b < 4; b++) if (in.dn[gc][b] > 1000) k += swprintf(lv + k, 200 - k, L"%ls%ls: %.1f", k ? L", " : L"", LV[b], in.drms[gc][b]);
        if (ch == 3) swprintf(m, 600, L"RAW pass %d - pass 1 after alignment, rms (16-bit units): R %.1f, G %.1f, B %.1f; G by level of pass 1: %ls.", rawj.pass,
                              tn[0] ? sqrt(tot[0] / tn[0]) : 0, tn[1] ? sqrt(tot[1] / tn[1]) : 0, tn[2] ? sqrt(tot[2] / tn[2]) : 0, lv);
        else swprintf(m, 600, L"RAW pass %d - pass 1 after alignment, rms (16-bit units): %.1f; by level: %ls.", rawj.pass, tn[0] ? sqrt(tot[0] / tn[0]) : 0, lv);
        log_d(m);
        }
    }
    if (rawj.pass < rawj.passes) {
        rawj.pass++;
        { wchar_t s[80]; swprintf(s, 80, L"RAW scan running (pass %d of %d)...", rawj.pass, rawj.passes); status(s); }
        raw_push(); return 1;
    }
    /* all passes: the mean */
    uint16_t *out = malloc((size_t)rawj.w * rawj.h * rawj.ch * sizeof(uint16_t));
    if (!out) { rs_free(&rawj.st); rawj.st_on = 0; log_w(L"RAW: not enough memory for the result."); return -1; }
    rs_result(&rawj.st, out);
    int n = rawj.st.n; rs_free(&rawj.st); rawj.st_on = 0;
    wchar_t dst[MAX_PATH * 2], suf[32]; swprintf(suf, 32, L" x%d", n); raw_outname(dst, suf);
    char desc[2500]; raw_desc(desc, sizeof desc, n);
    int och = raw_bw(&out, rawj.w, rawj.h, rawj.ch, desc, sizeof desc);
    raw_colreg(&out, rawj.w, rawj.h, och, desc, sizeof desc);
    int ok = write_tiff16(dst, out, rawj.w, rawj.h, och, rawj.dpi, desc); if (ok) raw_extras(dst, out, rawj.w, rawj.h, och, rawj.dpi, rawj.dpi, 0, desc); free(out);
    if (!ok) { log_w(L"RAW: writing the TIFF failed (disk full?) - nothing saved."); return -1; }
    swprintf(m, 600, L"RAW scan saved, mean of %d passes (%d x %d pixels, %d x 16 bits, %.1f MB, %.0f s in all): %ls", n, rawj.w, rawj.h, och,
             (double)rawj.w * rawj.h * och * 2 / 1048576.0, (GetTickCount() - rawj.t0) / 1000.0, dst);
    log_w(m); pg_done(); status(L"RAW scan finished."); next_file_name(); return 0;
}
static void do_scan(void) {
    user_stopped = 0;
    if (autotest && stop_after_ms > 0) SetTimer(hwnd, 5, (UINT)stop_after_ms, NULL);   /* developer option --stop-after-ms */
    if (autotest && stop_at_capture == 2) SetTimer(hwnd, 10, 20, NULL);   /* developer option --stop-at-capture scan: Stop as the scan's picture is being sent */
    char name[300], res[64], scale[64], w[64], h[64], x[64], y[64];
    text8(IDC_NAME, name, sizeof name); num8(IDC_RES, res, sizeof res); num8(IDC_SCALE, scale, sizeof scale);
    if (!valid_name(name)) { MessageBoxW(hwnd, L"The output file name must be 1-31 characters, without : / \\.", APP_TITLE, MB_ICONWARNING); return; }
    if (pv_invalid) { MessageBoxW(hwnd, L"The holder was changed after the last preview: make a new preview first.", APP_TITLE, MB_ICONINFORMATION); return; }
    double dpi = cur_dpi(); if (dpi < 10 || dpi > 20000) { MessageBoxW(hwnd, L"Resolution (dpi) is out of range.", APP_TITLE, MB_ICONWARNING); return; }
    int ps_res = sel(IDC_FORMAT) == FMT_RAW && ps_on;   /* 0.3.0: pixel shift chooses its own resolution - the field is not used, so not checked */
    if (rng_max > 0 && !probe_pending && !ps_res && (dpi < rng_min * 25.4 - 0.5 || dpi > rng_max * 25.4 + 0.5)) {
        wchar_t m[300]; swprintf(m, 300, L"%.0f dpi is outside the range the scanner software allows for this crop (%d - %d dpi).", dpi, (int)ceil(rng_min * 25.4), (int)floor(rng_max * 25.4));
        MessageBoxW(hwnd, m, APP_TITLE, MB_ICONWARNING); return; }
    char v[64];
    if (sel(IDC_FORMAT) == FMT_RAW) { do_raw(name, dpi, scale); return; }
    { /* a file of the same name left in the Mac volume (e.g. by an earlier run) is moved out first, marked as such */
        wchar_t dst[MAX_PATH * 2]; int r = move_to_scans(name, L" (left from an earlier run)", dst);
        if (r > 0) { wchar_t m[MAX_PATH * 3]; swprintf(m, MAX_PATH * 3, L"An older file with this name was in the program's work folder; moved to: %ls", dst); log_d(m); }
        else if (r < 0) { MessageBoxW(hwnd, L"An older file with this name is in the program's work folder and could not be moved. Choose another file name.", APP_TITLE, MB_ICONWARNING); return; }
    }
    push_original_settings();
    { char fv[16]; snprintf(fv, sizeof fv, "%d", frame_sel); push('F', NULL, NULL, fv); }   /* scan this frame (as opening its thumbnail) */
    push('s', "pfor", "enum", FMT_C[sel(IDC_FORMAT)]);
    snprintf(v, sizeof v, "%.6f", dpi / 25.4); push('s', "pres", "sing", v);     /* resolution unit: pixels per mm */
    if (scale[0]) push('s', "pscl", "sing", scale);
    push('s', "pmir", "bool", SendMessageW(GetDlgItem(hwnd, IDC_MIRROR), BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
    if (SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_GETCHECK, 0, 0) == BST_CHECKED) {
        num8(IDC_CW, w, sizeof w); num8(IDC_CH, h, sizeof h); num8(IDC_CX, x, sizeof x); num8(IDC_CY, y, sizeof y);
        push('s', "psiz.pwth", "sing", w); push('s', "psiz.phgt", "sing", h);
        push('s', "pofs.pwth", "sing", x); push('s', "pofs.phgt", "sing", y);
        { char m[200]; snprintf(m, sizeof m, "Crop sent: Left %s Top %s Width %s Height %s mm", x, y, w, h); wchar_t wm[200]; to_w(m, wm, 200); log_d(wm); }
    }
    push('s', "pnam", "fss ", name);
    push_tables();
    { /* the values the original reads from its Setup controls at scan time (see fghooks.c) */
        double sc = atof(scale); char hv[80]; snprintf(hv, sizeof hv, "%.6f %.6f", dpi / 25.4, sc > 0 ? sc : 0.0); push('h', NULL, NULL, hv);
        wchar_t m[200]; swprintf(m, 200, L"Resolution sent: %.1f dpi (%.4f pixels/mm)%ls", dpi, dpi / 25.4, sc > 0 ? L", scale as entered" : L""); log_d(m); }
    push('c', NULL, NULL, NULL);
    snprintf(pending_name, sizeof pending_name, "%s", name);
    job_ui = 'c'; set_busy(1); status(L"Scan running...");
    pg_begin(L"Scan", L"", 1);
    log_w(L"Scan requested.");
}
/* Move a file from the Mac volume's root to Scans\, with date and time in front of its name (and an optional note
   before the extension). Returns 1 moved, 0 not there, -1 could not be moved. */
static int move_to_scans(const char *name, const wchar_t *note, wchar_t *dst_out) {
    char esc[300 * 3]; { /* host name of the file at the volume root (Windows-safe escaping, as world.c) */
        size_t k = 0, n = strlen(name);
        for (size_t i = 0; i < n && k + 4 < sizeof esc; i++) { unsigned char c = (unsigned char)name[i];
            if (c < 32 || strchr("<>:\"\\|?*", c) || ((c == ' ' || c == '.') && i == n - 1)) k += (size_t)snprintf(esc + k, sizeof esc - k, "%%%02X", c); else esc[k++] = (char)c; }
        esc[k] = 0; }
    wchar_t escw[300 * 3]; to_w(esc, escw, 900);
    wchar_t src[MAX_PATH * 2], dst[MAX_PATH * 2], rsrc[MAX_PATH * 2];
    swprintf(src, MAX_PATH * 2, L"%ls\\%ls", voldir, escw); swprintf(rsrc, MAX_PATH * 2, L"%ls.rsrc", src);
    if (GetFileAttributesW(src) == INVALID_FILE_ATTRIBUTES) return 0;
    CreateDirectoryW(scandir_, NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t base[300 * 3]; swprintf(base, 900, L"%ls", escw);
    wchar_t ext[64] = L""; wchar_t *dot = wcsrchr(base, L'.'); if (dot && wcslen(dot) < 64) { wcscpy(ext, dot); *dot = 0; }
    /* Windows needs an extension to know the file; the Mac name may have none (found on the scanner, 2026-10-08:
       "scan2000dpiNOsharp" was saved without one and would not open). A name without a known picture extension
       gets one from what the file actually contains (first bytes); anything not recognised is left as it is. */
    { static const wchar_t *KNOWN[] = { L".tif", L".tiff", L".psd", L".eps", L".pct", L".pict", L".ct", NULL };
      int known = 0; for (int i = 0; KNOWN[i]; i++) if (!_wcsicmp(ext, KNOWN[i])) known = 1;
      if (!known) {
          if (ext[0]) { wcscat(base, ext); ext[0] = 0; }          /* "my.scan": the dot part is part of the name */
          uint8_t h[528]; size_t n = 0; FILE *f = _wfopen(src, L"rb"); if (f) { n = fread(h, 1, sizeof h, f); fclose(f); }
          if (n >= 4 && ((h[0] == 'M' && h[1] == 'M' && h[2] == 0 && h[3] == 42) || (h[0] == 'I' && h[1] == 'I' && h[2] == 42 && h[3] == 0))) wcscpy(ext, L".tif");
          else if (n >= 4 && !memcmp(h, "8BPS", 4)) wcscpy(ext, L".psd");
          else if (n >= 4 && (!memcmp(h, "%!PS", 4) || (h[0] == 0xC5 && h[1] == 0xD0 && h[2] == 0xD3 && h[3] == 0xC6))) wcscpy(ext, L".eps");
          else if (n >= 528 && ((h[522] == 0x00 && h[523] == 0x11 && h[524] == 0x02 && h[525] == 0xFF) || (h[522] == 0x11 && h[523] == 0x01))) wcscpy(ext, L".pct");   /* PICT: 512-byte header, size, frame, then the version opcode */
          if (ext[0]) { wchar_t m[200]; swprintf(m, 200, L"The file name had no extension; \"%ls\" added (from the file's contents) so Windows can open it.", ext); log_w(m); }
          else log_w(L"The file name has no extension and the file type was not recognised from its contents - saved without one.");
      } }
    swprintf(dst, MAX_PATH * 2, L"%ls\\%04d%02d%02d-%02d%02d%02d %ls%ls%ls", scandir_, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
             base, note ? note : L"", ext);
    if (!MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED)) return -1;
    DeleteFileW(rsrc);
    if (dst_out) wcscpy(dst_out, dst);
    return 1;
}
static void scan_done(void) {
    wchar_t dst[MAX_PATH * 2];
    int r = move_to_scans(pending_name, NULL, dst);
    if (r == 0) log_w(L"Scan finished, but no output file was written (see the log above).");
    else if (r < 0) log_w(L"Scan finished, but the file could not be moved to the Scans folder.");
    else { wchar_t m[MAX_PATH * 3]; swprintf(m, MAX_PATH * 3, L"Scan saved: %ls", dst); pg_done(); log_w(m); status(L"Scan finished."); next_file_name(); }
}

/* ------------------------------------------------------------------ simulator volume */
static int copy_tree(const wchar_t *src, const wchar_t *dst) {
    if (!CreateDirectoryW(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    wchar_t pat[MAX_PATH * 2]; swprintf(pat, MAX_PATH * 2, L"%ls\\*", src);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2]; swprintf(a, MAX_PATH * 2, L"%ls\\%ls", src, fd.cFileName); swprintf(b, MAX_PATH * 2, L"%ls\\%ls", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(a, b); else ok &= CopyFileW(a, b, FALSE) ? 1 : 0;
    } while (FindNextFileW(h, &fd));
    FindClose(h); return ok;
}

/* ------------------------------------------------------------------ main window */
static void layout(int W, int H) {
    int px = S(310), py = S(10) + S(28), pw2 = W - px - S(10), ph2 = H - S(186) - py - S(74); if (pw2 < 50) pw2 = 50; if (ph2 < 50) ph2 = 50;
    MoveWindow(GetDlgItem(hwnd, IDC_FRAME), px + S(52), S(8), S(110), S(300), TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_FRAMEINFO), px + S(170), S(11), pw2 - S(170), S(20), TRUE);
    MoveWindow(hprev, px, py, pw2, ph2, TRUE);
    { int ly = py + ph2 + S(6), bw = S(110);
      MoveWindow(GetDlgItem(hwnd, IDC_BLIST), px, ly, pw2 - bw - S(6), S(66), TRUE);
      MoveWindow(GetDlgItem(hwnd, IDC_BADD), px + pw2 - bw, ly, bw, S(21), TRUE);
      MoveWindow(GetDlgItem(hwnd, IDC_BSCAN), px + pw2 - bw, ly + S(22), bw, S(21), TRUE);
      MoveWindow(GetDlgItem(hwnd, IDC_BCLEAR), px + pw2 - bw, ly + S(44), bw, S(21), TRUE); }
    MoveWindow(hlog, S(10), H - S(166), W - S(20), S(131), TRUE);
    MoveWindow(hstatus, S(10), H - S(28), W - S(20) - S(256), S(20), TRUE);
    if (hprog_std) MoveWindow(hprog_std, W - S(250), H - S(18) - S(18) / 2, S(240), S(18), TRUE);
    if (hprog_miner) { int bh = bar_h(); MoveWindow(hprog_miner, W - S(250), H - S(18) - bh / 2, S(240), bh, TRUE); }
}
/* every frame of a preview is in (ST_PREVIEW_DONE, or the Automatic holder's frame count after it - WM_APP_FRAMES 2) */
static void preview_all_done(void) {
    pv_invalid = 0;
    pg_done(); status(frames_total > 1 ? L"Preview finished (all frames)." : L"Preview finished."); set_busy(0); lens_report(); push('t', NULL, NULL, NULL); start_probe();
    cg_note();
    if (wz_on && wz_state == WZ_PREVIEW) { wz_preview_done(); return; }   /* 0.3.0: Set up this scanner */
    if (wz_on && wz_state == WZ_CHART_PREVIEW) { wz_chart_preview_done(); return; }
    if (fs_on && fs_state == FS_PREVIEW) { fs_preview_done(); return; }   /* 0.4.0: Fix streaks */
    if (autotest && neg_keep_opt && pv_negative) { neg_keep_opt = 0; neg_keep(); }   /* developer option --neg-keep */
    if (autotest) SetTimer(hwnd, 2, 8000, NULL);
}
static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        hwnd = h; dpi = window_dpi(h);
        NONCLIENTMETRICSW ncm = { sizeof ncm }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        font = CreateFontIndirectW(&ncm.lfMessageFont);
        int y = 12, lx = 10, cx = 118, cw = 180;
        mk(L"STATIC", L"Original", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, IDC_MOUNT); y += 28;
        mk(L"STATIC", L"Cassette", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 300, IDC_CASS); y += 28;
        mk(L"STATIC", L"Mode", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, IDC_MODE); y += 28;
        mk(L"STATIC", L"Media / film", 0, lx, y + 3, 105, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, 84, 200, IDC_MEDIA);
        { HWND f = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx + 88, y, cw - 88, 300, IDC_FILM); SendMessageW(f, CB_SETDROPPEDWIDTH, S(220), 0); EnableWindow(f, FALSE); } y += 30;
        mk(L"BUTTON", L"Preview", BS_PUSHBUTTON | WS_TABSTOP, lx, y, 288, 30, IDC_PREVIEW); y += 40;
        mk(L"STATIC", L"Resolution (dpi)", 0, lx, y + 3, 105, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWN | WS_VSCROLL | WS_TABSTOP, cx, y, 100, 300, IDC_RES);
        mk(L"BUTTON", L"Max", BS_PUSHBUTTON | WS_TABSTOP, cx + 106, y, 60, 24, IDC_RESMAX); y += 28;
        mk(L"STATIC", L"Allowed range: make a preview", 0, lx, y, 290, 34, IDC_RESINFO); y += 34;
        mk(L"STATIC", L"Scale (%)", 0, lx, y + 3, 95, 20, 0); mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, cx, y, 80, 22, IDC_SCALE);
        mk(L"BUTTON", L"Mirror", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 92, y + 1, 80, 20, IDC_MIRROR); y += 30;
        mk(L"STATIC", L"File format", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, IDC_FORMAT); y += 30;
        mk(L"STATIC", L"RAW passes", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, 56, 300, IDC_PASSES);
        mk(L"BUTTON", L"Keep each pass", BS_AUTOCHECKBOX | WS_TABSTOP, cx + 64, y + 1, 120, 20, IDC_KEEPPASS); y += 28;
        mk(L"STATIC", L"RAW exposure", 0, lx, y + 3, 95, 20, 0);
        { HWND hc = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, IDC_HDR); SendMessageW(hc, CB_SETDROPPEDWIDTH, S(220), 0); } y += 28;
        mk(L"STATIC", L"RAW gamma", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 200, IDC_GAMMA); y += 28;
        mk(L"STATIC", L"Sharpness", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 300, IDC_SHARP); y += 28;
        mk(L"STATIC", L"Colour table", 0, lx, y + 3, 95, 20, 0); mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx, y, cw, 400, IDC_COLOR); y += 28;
        mk(L"BUTTON", L"Crop (mm)", BS_AUTOCHECKBOX | WS_TABSTOP, lx, y, 120, 20, IDC_USECROP); y += 24;
        mk(L"STATIC", L"Width", 0, lx, y + 3, 45, 20, 0); mk(L"EDIT", L"24", ES_AUTOHSCROLL | WS_TABSTOP, lx + 50, y, 60, 22, IDC_CW);
        mk(L"STATIC", L"Height", 0, lx + 145, y + 3, 50, 20, 0); mk(L"EDIT", L"36", ES_AUTOHSCROLL | WS_TABSTOP, lx + 200, y, 60, 22, IDC_CH); y += 28;
        mk(L"STATIC", L"Left", 0, lx, y + 3, 45, 20, 0); mk(L"EDIT", L"0", ES_AUTOHSCROLL | WS_TABSTOP, lx + 50, y, 60, 22, IDC_CX);
        mk(L"STATIC", L"Top", 0, lx + 145, y + 3, 50, 20, 0); mk(L"EDIT", L"0", ES_AUTOHSCROLL | WS_TABSTOP, lx + 200, y, 60, 22, IDC_CY); y += 28;
        mk(L"STATIC", L"File name", 0, lx, y + 3, 95, 20, 0); mk(L"EDIT", L"scan001.tif", ES_AUTOHSCROLL | WS_TABSTOP, cx, y, cw, 22, IDC_NAME); y += 30;
        mk(L"BUTTON", L"Scan", BS_PUSHBUTTON | WS_TABSTOP, lx, y, 196, 30, IDC_SCAN);
        mk(L"BUTTON", L"Stop", BS_PUSHBUTTON | WS_TABSTOP, lx + 202, y, 86, 30, IDC_STOP); y += 34;
        mk(L"BUTTON", L"Open Scans folder", BS_PUSHBUTTON | WS_TABSTOP, lx, y, 288, 26, IDC_OPENSCANS);
        { HMENU mb = CreateMenu(), tm = CreatePopupMenu(), dm = CreatePopupMenu(), hm = CreatePopupMenu();
          /* 0.2.0: the everyday RAW settings first; the research and test items under Diagnostics */
          AppendMenuW(tm, MF_STRING | (ps_on ? MF_CHECKED : 0), IDM_PSHIFT, L"Pixel shift RAW: 2 x finer across (for a crop)");
          psn_menu = CreatePopupMenu();
          AppendMenuW(psn_menu, MF_STRING, IDM_PSN5, L"Up to 5 - stop when every finer column has a sample close to it (normal)");
          AppendMenuW(psn_menu, MF_STRING, IDM_PSN8, L"8 - all eight (less noise, takes longer)");
          AppendMenuW(tm, MF_POPUP, (UINT_PTR)psn_menu, L"Pixel shift passes");
          AppendMenuW(tm, MF_STRING, IDM_STEPHOLD, L"RAW step-and-hold: the carriage stands still for each line (sharper along at the CCD's resolution or finer; about 1.4 x the time)");
          AppendMenuW(tm, MF_STRING, IDM_COLREG, L"RAW colour registration: red and blue onto green (fixes the scanner's offset)");
          rsh_menu = CreatePopupMenu();
          AppendMenuW(rsh_menu, MF_STRING, IDM_RSH0 + 0, L"Off");
          AppendMenuW(rsh_menu, MF_STRING, IDM_RSH0 + 1, L"Light");
          AppendMenuW(rsh_menu, MF_STRING, IDM_RSH0 + 2, L"Medium");
          AppendMenuW(rsh_menu, MF_STRING, IDM_RSH0 + 3, L"Strong");
          AppendMenuW(tm, MF_POPUP, (UINT_PTR)rsh_menu, L"RAW sharpening: extra SHARP file (restores the optics' loss)");
          neg_menu = CreatePopupMenu();
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGPOS, L"Make a positive TIFF of every negative RAW (\"POS16\")");
          AppendMenuW(neg_menu, MF_SEPARATOR, 0, NULL);
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGC0 + 0, L"Contrast: softer (film gamma 0.70)");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGC0 + 1, L"Contrast: normal (film gamma 0.60)");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGC0 + 2, L"Contrast: harder (film gamma 0.50)");
          AppendMenuW(neg_menu, MF_SEPARATOR, 0, NULL);
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGAUTO, L"Colour levels: each frame its own (automatic)");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGLOCK, L"Colour levels: keep the shown preview's for the next frames (one roll)");
          AppendMenuW(neg_menu, MF_SEPARATOR, 0, NULL);
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGROT0 + 0, L"Positive not rotated");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGROT0 + 1, L"Positive rotated 90 degrees right");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGROT0 + 2, L"Positive rotated 180 degrees");
          AppendMenuW(neg_menu, MF_STRING, IDM_NEGROT0 + 3, L"Positive rotated 90 degrees left");
          AppendMenuW(tm, MF_POPUP, (UINT_PTR)neg_menu, L"Negatives (RAW -> positive)");
          AppendMenuW(tm, MF_SEPARATOR, 0, NULL);
          AppendMenuW(tm, MF_STRING, IDM_LOGDETAIL, L"Detailed log in the window");
          AppendMenuW(tm, MF_STRING, IDM_SETUP, L"Set up this scanner (measure the lenses for pixel shift)...");
          AppendMenuW(tm, MF_STRING, IDM_FIXSTREAKS, L"Fix streaks: measure this scanner's own clean gain vector (empty frame)...");
          AppendMenuW(tm, MF_STRING, IDM_SCSI, L"SCSI connection...");
          AppendMenuW(dm, MF_STRING | (ps_slow ? MF_CHECKED : 0), IDM_PSSLOW, L"Pixel shift: slow lens moves (constant-rate; avoids carriage slips)");
          AppendMenuW(dm, MF_STRING | (raw_stream_on ? MF_CHECKED : 0), IDM_RAWSTREAM, L"Streamed RAW for large crops (one request; off = made of blocks)");
          AppendMenuW(dm, MF_STRING, IDM_NOALIGN, L"RAW: the scanner's own colour alignment off (research)");
          AppendMenuW(dm, MF_SEPARATOR, 0, NULL);
          AppendMenuW(dm, MF_STRING, IDM_RAWTEST, L"RAW check (block size and streamed RAW)...");
          AppendMenuW(dm, MF_STRING, IDM_RAWLIMITS, L"Show the RAW block size");
          AppendMenuW(dm, MF_STRING, IDM_FOCUSTEST, L"Focus test (experimental: does the focus motor sharpen this lens?)...");
          AppendMenuW(dm, MF_STRING, IDM_LENSTEST, L"Lens shift test (research: can the lens carriage shift the picture across?)...");
          focus_menu = CreatePopupMenu();
          for (int k = 0; k <= 20; k++) { wchar_t t[64]; double v = (k - 10) * 0.05; if (k == 10) swprintf(t, 64, L"0 (the scanner's own focus)"); else swprintf(t, 64, L"%+.2f mm", v); AppendMenuW(focus_menu, MF_STRING, IDM_FOCUS0 + k, t); }
          AppendMenuW(dm, MF_POPUP, (UINT_PTR)focus_menu, L"Focus offset (experimental)");
          AppendMenuW(tm, MF_POPUP, (UINT_PTR)dm, L"Diagnostics");
          tools_menu = tm;
          AppendMenuW(hm, MF_STRING, IDM_ABOUT, L"About Smart342 for Windows");
          AppendMenuW(mb, MF_POPUP, (UINT_PTR)tm, L"&Tools"); AppendMenuW(mb, MF_POPUP, (UINT_PTR)hm, L"&Help"); SetMenu(h, mb); }
        hlog = mk(L"LISTBOX", L"", WS_VSCROLL | WS_HSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 10, 400, 600, 160, IDC_LOG);
        SendMessageW(hlog, LB_SETHORIZONTALEXTENT, 4000, 0);
        hstatus = mk(L"STATIC", L"Starting...", SS_ENDELLIPSIS, 10, 570, 600, 20, IDC_STATUS);
        { INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_PROGRESS_CLASS }; InitCommonControlsEx(&ic);
          /* the standard progress bar; 0.4.0's miner bar (minerbar.c, a drop-in for PROGRESS_CLASS: the same PBM_ messages
             and PBS_MARQUEE) is kept beside it and shown now and then (0.4.1: an easter egg, see pg_begin) */
          hprog_std = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | PBS_SMOOTH, S(620), S(570), S(240), S(18), h, (HMENU)IDC_PROGRESS, GetModuleHandleW(NULL), NULL);
          if (MinerBar_Register(GetModuleHandleW(NULL)))
              hprog_miner = CreateWindowExW(0, MINERBAR_CLASSW, L"", WS_CHILD, S(620), S(570), S(240), bar_h(), h, NULL, GetModuleHandleW(NULL), NULL);
          if (hprog_std) SendMessageW(hprog_std, PBM_SETRANGE32, 0, 1000);
          if (hprog_miner) SendMessageW(hprog_miner, PBM_SETRANGE32, 0, 1000);
          hprog = hprog_std ? hprog_std : hprog_miner; }
        mk(L"STATIC", L"Frame", 0, 310, 11, 48, 20, 0);
        mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 362, 8, 110, 300, IDC_FRAME);
        mk(L"STATIC", L"", 0, 480, 11, 300, 20, IDC_FRAMEINFO);
        mk(L"LISTBOX", L"", WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY, 310, 400, 300, 66, IDC_BLIST);
        mk(L"BUTTON", L"Add crop to list", BS_PUSHBUTTON | WS_TABSTOP, 620, 400, 110, 21, IDC_BADD);
        mk(L"BUTTON", L"Scan list", BS_PUSHBUTTON | WS_TABSTOP, 620, 422, 110, 21, IDC_BSCAN);
        mk(L"BUTTON", L"Clear list", BS_PUSHBUTTON | WS_TABSTOP, 620, 444, 110, 21, IDC_BCLEAR);
        hprev = CreateWindowExW(WS_EX_CLIENTEDGE, L"S342Preview", L"", WS_CHILD | WS_VISIBLE, S(310), S(10), S(400), S(380), h, (HMENU)IDC_PREVWIN, GetModuleHandleW(NULL), NULL);
        { static const wchar_t *R[] = { L"300", L"600", L"1000", L"1200", L"1500", L"2000", L"2400", L"2540", L"3000", L"4000", L"5000", L"6000", NULL };
          for (int i = 0; R[i]; i++) SendMessageW(GetDlgItem(h, IDC_RES), CB_ADDSTRING, 0, (LPARAM)R[i]);
          SetWindowTextW(GetDlgItem(h, IDC_RES), L"300"); }
        combo(IDC_MOUNT, MOUNT_L, 0); cass_fill(0, cass_default); combo(IDC_MODE, MODE_L, mode_default); combo(IDC_MEDIA, MEDIA_L, media_default); combo(IDC_FORMAT, FMT_L, format_default); combo(IDC_HDR, HDR_L, hdr_default); combo(IDC_GAMMA, GAMMA_L, gamma_default);
        { HWND pc = GetDlgItem(h, IDC_PASSES); for (int i = 0; i < (int)(sizeof PASSES_V / sizeof *PASSES_V); i++) { wchar_t t[8]; swprintf(t, 8, L"%d", PASSES_V[i]); SendMessageW(pc, CB_ADDSTRING, 0, (LPARAM)t); }
          SendMessageW(pc, CB_SETCURSEL, passes_default, 0); }
        settings_load();
        if (!autotest || ini_in_autotest || raw_test_auto) rt_load();
        if (!autotest || ini_in_autotest) { wchar_t p[MAX_PATH * 2], v[32], key[16]; ini_path(p);
            for (int k = 0; k < 5; k++) { swprintf(key, 16, L"lens%d", k); GetPrivateProfileStringW(L"focus", key, L"0", v, 32, p); double f = _wtof(v); if (f != 0) focus_set_l(k, f, 0, L" (kept from the last session)"); } }
        if (focus_opt[0]) focus_set_l(-1, _wtof(focus_opt), 0, NULL);
        if (mirror_opt) SendMessageW(GetDlgItem(h, IDC_MIRROR), BM_SETCHECK, BST_CHECKED, 0);
        focus_menu_check();
        if (raw_limit_opt[0]) { int a = 0, b = 0; if (swscanf(raw_limit_opt, L"%dx%d", &a, &b) == 2) {
            if (a > 0 && b > 0) { raw_max_w = a; raw_max_h = b; raw_max_w2 = raw_max_h2 = 0; } else { raw_max_w = raw_max_h = 0; raw_area = 0; }
            log_w(L"Developer option --raw-limit: the window's RAW size rule is replaced (the interlock still checks every block). A RAW above 1,020,000 pixels leaves the scanner with less memory than the interlock assumes at the next start - switch the scanner off and on before using this program normally again."); } }
        if (autotest && !ini_in_autotest) raw_stream_on = 0;   /* the block tests keep their blocks */
        if (raw_stream_opt) { raw_stream_on = 1; log_d(L"Streamed RAW ON (developer option)."); }
        if (tools_menu) CheckMenuItem(tools_menu, IDM_RAWSTREAM, MF_BYCOMMAND | (raw_stream_on ? MF_CHECKED : MF_UNCHECKED));
        if (ps_opt) { ps_on = 1; if (tools_menu) CheckMenuItem(tools_menu, IDM_PSHIFT, MF_BYCOMMAND | MF_CHECKED); log_d(L"Pixel shift RAW ON (developer option)."); }
        if (rsh_opt >= 0 && rsh_opt <= 3) rsh_level = rsh_opt;
        rsh_check();
        if (neg_c_opt >= 0) neg_contrast = neg_c_opt; if (neg_rot_opt >= 0) neg_rot = neg_rot_opt & 3;
        neg_check();
        if (psn_opt) ps_npass = psn_opt == 8 ? 8 : 5;
        psn_check();
        if (psslow_opt) ps_slow = psslow_opt > 0;
        if (tools_menu) CheckMenuItem(tools_menu, IDM_PSSLOW, MF_BYCOMMAND | (ps_slow ? MF_CHECKED : MF_UNCHECKED));
        log_d(ps_slow ? L"Pixel shift: slow lens moves (Tools > Diagnostics)." : L"Pixel shift: fast lens moves (Tools > Diagnostics).");
        if (colreg_opt >= 0) colreg_on = colreg_opt != 0;
        if (stephold_opt) stephold_on = stephold_opt > 0;
        if (log_detail_opt) log_detail = 1;
        if (tools_menu) { CheckMenuItem(tools_menu, IDM_STEPHOLD, MF_BYCOMMAND | (stephold_on ? MF_CHECKED : MF_UNCHECKED)); CheckMenuItem(tools_menu, IDM_LOGDETAIL, MF_BYCOMMAND | (log_detail ? MF_CHECKED : MF_UNCHECKED)); }
        log_d(stephold_on ? L"RAW step-and-hold: on (at the CCD's resolution or finer)." : L"RAW step-and-hold: off.");
        if (noalign_opt) { noalign_on = 1; if (tools_menu) CheckMenuItem(tools_menu, IDM_NOALIGN, MF_BYCOMMAND | MF_CHECKED); log_d(L"Scanner colour alignment OFF for RAWs (developer option)."); }
        if (tools_menu) CheckMenuItem(tools_menu, IDM_COLREG, MF_BYCOMMAND | (colreg_on ? MF_CHECKED : MF_UNCHECKED));
        if (rsh_level) { wchar_t m[200]; swprintf(m, 200, L"RAW sharpening: %ls (Tools menu) - every RAW also gets a sharpened copy.", RSH_NAME[rsh_level]); log_d(m); }
        if (auto_name[0]) SetWindowTextW(GetDlgItem(h, IDC_NAME), auto_name);
        if (keep_passes_default) SendMessageW(GetDlgItem(h, IDC_KEEPPASS), BM_SETCHECK, BST_CHECKED, 0);
        passes_enable();
        set_busy(1);
        return 0; }
    case WM_SIZE: layout(LOWORD(lp), HIWORD(lp)); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = S(780); mm->ptMinTrackSize.y = S(818);   /* 0.2.0: the RAW gamma row fits by tighter spacing - the same height and preview size as 0.1.28 (the tests' crops are dragged on it) */ return 0; }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_STOP) { if (busy_now && started_ui && !stop_req) { user_stopped = 1; stop_req = 1; log_w(L"Stop requested - the scanner software is asked to stop the running job."); status(guard_capture ? L"Stopping when the scanner has sent the picture..." : L"Stopping..."); } }
        else if (LOWORD(wp) == IDM_FOCUSTEST && HIWORD(wp) == 0) ft_start(1);
        else if (LOWORD(wp) == IDM_LENSTEST && HIWORD(wp) == 0) lt_start(1);
        else if (LOWORD(wp) >= IDM_FOCUS0 && LOWORD(wp) <= IDM_FOCUS0 + 20 && HIWORD(wp) == 0) { if (busy_now) MessageBoxW(h, L"Change the focus when no scan is running.", APP_TITLE, MB_ICONINFORMATION); else focus_set((LOWORD(wp) - IDM_FOCUS0 - 10) * 0.05, 1, NULL); }
        else if (LOWORD(wp) == IDM_RAWTEST && HIWORD(wp) == 0) rt_start(1);
        else if (LOWORD(wp) == IDM_RAWLIMITS && HIWORD(wp) == 0) rt_show_limits();
        else if ((LOWORD(wp) == IDM_PSN5 || LOWORD(wp) == IDM_PSN8) && HIWORD(wp) == 0) {
            ps_npass = LOWORD(wp) == IDM_PSN8 ? 8 : 5; psn_check(); settings_save();
            log_w(ps_npass == 8 ? L"Pixel shift passes: 8 - all eight passes are made and combined (each finer column gets several samples: less noise; about 8 x one RAW's time). Saved." : L"Pixel shift passes: up to 5 - stops as soon as every finer column has a sample close to it (3 to 5 passes). Saved."); }
        else if (LOWORD(wp) == IDM_PSSLOW && HIWORD(wp) == 0) {
            if (busy_now) MessageBoxW(h, L"Change this when no scan is running.", APP_TITLE, MB_ICONINFORMATION);
            else { ps_slow = !ps_slow; if (tools_menu) CheckMenuItem(tools_menu, IDM_PSSLOW, MF_BYCOMMAND | (ps_slow ? MF_CHECKED : MF_UNCHECKED)); settings_save();
                log_w(ps_slow ? L"Pixel shift: SLOW lens moves - the lens carriage's moves for each pass use the motion board's constant-rate move (the same targets) instead of its fast ramped move: about 0.4 s instead of 0.2 s per move. The carriage has slipped about one motor step on fast moves. Saved."
                              : L"Pixel shift: normal (fast) lens moves, as the scanner driver makes them. Saved."); } }
        else if (LOWORD(wp) == IDM_LOGDETAIL && HIWORD(wp) == 0) {
            log_detail = !log_detail; if (tools_menu) CheckMenuItem(tools_menu, IDM_LOGDETAIL, MF_BYCOMMAND | (log_detail ? MF_CHECKED : MF_UNCHECKED)); settings_save();
            log_w(log_detail ? L"Detailed log in the window: on (every line of the session log is shown from now on). Saved." : L"Detailed log in the window: off (the full log is still written to the Logs folder). Saved."); }
        else if (LOWORD(wp) == IDM_SCSI && HIWORD(wp) == 0) scsi_menu();
        else if (LOWORD(wp) == IDM_SETUP && HIWORD(wp) == 0) wz_start();
        else if (LOWORD(wp) == IDM_FIXSTREAKS && HIWORD(wp) == 0) (void)fs_start();
        else if (LOWORD(wp) == IDM_CHART_MEASURE && HIWORD(wp) == 0) wz_chart_measure();
        else if (LOWORD(wp) == IDM_CHART_SKIP && HIWORD(wp) == 0) wz_chart_skip();
        else if (LOWORD(wp) == IDM_SETUP_OFFER && HIWORD(wp) == 0) { if (!busy_now) wz_offer(); }
        else if (LOWORD(wp) == IDM_ABOUT && HIWORD(wp) == 0) {
            wchar_t m[600]; swprintf(m, 600, L"%ls\nVersion %ls\n\nRuns a Scitex Smart 342L film scanner on Windows.\n\nThe log of each session is in the Logs folder next to the program - please send it with any question about a scan.", APP_TITLE, APP_VERSION);
            MessageBoxW(h, m, L"About " APP_TITLE, MB_ICONINFORMATION); }
        else if (LOWORD(wp) == IDM_STEPHOLD && HIWORD(wp) == 0) {
            if (!stephold_ok) { log_w(L"Step-and-hold: not available with this scanner software version."); }
            else { stephold_on = !stephold_on; if (tools_menu) CheckMenuItem(tools_menu, IDM_STEPHOLD, MF_BYCOMMAND | (stephold_on ? MF_CHECKED : MF_UNCHECKED)); settings_save();
                log_w(stephold_on ? L"RAW step-and-hold ON: at the lens's CCD resolution or finer (along), the carriage makes one step per line and stands still while the line is exposed - no smear of one line along; about 1.4 x the scan time. Coarser RAWs keep the continuous motion. Saved." : L"RAW step-and-hold OFF: RAWs with the normal continuous motion. Saved."); } }
        else if (LOWORD(wp) == IDM_NOALIGN && HIWORD(wp) == 0) {
            if (!noalign_ok) { log_w(L"Scanner colour alignment off: not available (the driver code is not the expected 2.2.0 code)."); }
            else { noalign_on = !noalign_on; if (tools_menu) CheckMenuItem(tools_menu, IDM_NOALIGN, MF_BYCOMMAND | (noalign_on ? MF_CHECKED : MF_UNCHECKED));
                log_w(noalign_on ? L"Scanner colour alignment OFF for RAWs (research): the scanner does not interpolate red and blue onto green - each channel keeps the CCD's own samples, and this program registers them (colour registration is always done for these RAWs). Not saved - off at the next start." : L"Scanner colour alignment: back to the scanner's own (normal)."); } }
        else if (LOWORD(wp) == IDM_COLREG && HIWORD(wp) == 0) {
            colreg_on = !colreg_on; if (tools_menu) CheckMenuItem(tools_menu, IDM_COLREG, MF_BYCOMMAND | (colreg_on ? MF_CHECKED : MF_UNCHECKED)); settings_save();
            log_w(colreg_on ? L"RAW colour registration ON: red and blue are measured against green in every RAW and moved onto it (green untouched). Saved." : L"RAW colour registration OFF: RAWs are saved with the colours as the scanner delivers them. Saved."); }
        else if (LOWORD(wp) == IDM_NEGPOS && HIWORD(wp) == 0) { neg_pos_on = !neg_pos_on; neg_check(); settings_save();
            log_w(neg_pos_on ? L"Negatives: every RAW of a negative (Media: Negative) also gets a positive TIFF (\"POS16\"). Saved." : L"Negatives: no positive TIFF is made (the RAW only). Saved."); }
        else if (LOWORD(wp) >= IDM_NEGC0 && LOWORD(wp) <= IDM_NEGC0 + 2 && HIWORD(wp) == 0) { neg_contrast = LOWORD(wp) - IDM_NEGC0; neg_check(); settings_save();
            wchar_t m[200]; swprintf(m, 200, L"Negatives: contrast %ls (film gamma %.2f). Saved.", NEG_CNAME[neg_contrast], NEG_GAMMA[neg_contrast]); log_w(m); if (pv_negative && pbits) pv_render(0); }
        else if (LOWORD(wp) == IDM_NEGAUTO && HIWORD(wp) == 0) { neg_lock = 0; neg_check(); settings_save(); log_w(L"Negatives: the colour levels are taken from each frame (automatic). Saved."); if (pv_negative && pbits) pv_render(0); }
        else if (LOWORD(wp) == IDM_NEGLOCK && HIWORD(wp) == 0) neg_keep();
        else if (LOWORD(wp) >= IDM_NEGROT0 && LOWORD(wp) <= IDM_NEGROT0 + 3 && HIWORD(wp) == 0) { neg_rot = LOWORD(wp) - IDM_NEGROT0; neg_check(); settings_save();
            log_w(neg_rot == 0 ? L"Negatives: the positive is saved as scanned (not rotated). Saved." : neg_rot == 1 ? L"Negatives: the positive is rotated 90 degrees right. Saved." : neg_rot == 2 ? L"Negatives: the positive is rotated 180 degrees. Saved." : L"Negatives: the positive is rotated 90 degrees left. Saved."); }
        else if (LOWORD(wp) >= IDM_RSH0 && LOWORD(wp) <= IDM_RSH0 + 3 && HIWORD(wp) == 0) {
            rsh_level = LOWORD(wp) - IDM_RSH0; rsh_check(); settings_save();
            wchar_t m[300]; if (rsh_level) swprintf(m, 300, L"RAW sharpening: %ls - every RAW also gets a sharpened copy (\"SHARP-%lc\" in its name); the RAW itself stays untouched. Saved for the next start.", RSH_NAME[rsh_level], L"OLMS"[rsh_level]);
            else swprintf(m, 300, L"RAW sharpening: off."); log_w(m); }
        else if (LOWORD(wp) == IDM_PSHIFT && HIWORD(wp) == 0) {
            ps_on = !ps_on; if (tools_menu) CheckMenuItem(tools_menu, IDM_PSHIFT, MF_BYCOMMAND | (ps_on ? MF_CHECKED : MF_UNCHECKED));
            log_w(ps_on ? L"Pixel shift RAW ON: RAW scans of a crop are made of several passes with the lens carriage a few motor steps apart, combined 2 x finer across; along, the scanner itself steps finer. The resolution setting is not used. Not saved - off at the next start."
                        : L"Pixel shift RAW off."); }
        else if (LOWORD(wp) == IDM_RAWSTREAM && HIWORD(wp) == 0) {
            raw_stream_on = !raw_stream_on; if (tools_menu) CheckMenuItem(tools_menu, IDM_RAWSTREAM, MF_BYCOMMAND | (raw_stream_on ? MF_CHECKED : MF_UNCHECKED));
            settings_save();
            log_w(raw_stream_on ? L"Streamed RAW on: a RAW larger than one block is made in one request that the scanner streams through its memory, as it does for normal scans. Saved."
                                : L"Streamed RAW off: a RAW larger than one block is made of blocks (each with its own calibration - much slower). Saved.");
            update_res_info(); }
        else if (LOWORD(wp) == IDC_PREVIEW) do_preview();
        else if (LOWORD(wp) == IDC_SCAN) do_scan();
        else if (LOWORD(wp) == IDC_BADD) bq_add();
        else if (LOWORD(wp) == IDC_BSCAN) { if (nbq && !busy_now && !bq_running) { bq_running = 1; bq_idx = 0; bq_next(); } }
        else if (LOWORD(wp) == IDC_BCLEAR) { if (!bq_running) { nbq = 0; bq_show(); } }
        else if (LOWORD(wp) == IDC_OPENSCANS) { CreateDirectoryW(scandir_, NULL); ShellExecuteW(h, L"open", scandir_, NULL, NULL, SW_SHOWNORMAL); }
        else if (((LOWORD(wp) >= IDC_CW && LOWORD(wp) <= IDC_CY) && HIWORD(wp) == EN_CHANGE) || LOWORD(wp) == IDC_USECROP) {
            InvalidateRect(hprev, NULL, FALSE); if (pbits) { SetTimer(h, 3, 700, NULL); } }
        else if (LOWORD(wp) == IDC_SCALE && HIWORD(wp) == EN_CHANGE) { if (pbits) SetTimer(h, 3, 700, NULL); }
        else if (LOWORD(wp) == IDC_RES && (HIWORD(wp) == CBN_EDITCHANGE || HIWORD(wp) == CBN_SELCHANGE)) {
            if (HIWORD(wp) == CBN_SELCHANGE) { int i = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0); wchar_t t[32]; SendMessageW((HWND)lp, CB_GETLBTEXT, i, (LPARAM)t); SetWindowTextW((HWND)lp, t); }
            update_res_info(); }
        else if (LOWORD(wp) == IDC_RESMAX) {
            if (rng_max > 0) { wchar_t t[32]; swprintf(t, 32, L"%d", (int)floor(rng_max * 25.4)); SetWindowTextW(GetDlgItem(h, IDC_RES), t); update_res_info(); }
            else MessageBoxW(h, L"Make a preview first (and draw the crop) - the allowed range is then checked by the scanner software.", APP_TITLE, MB_ICONINFORMATION); }
        else if ((LOWORD(wp) == IDC_MODE || LOWORD(wp) == IDC_MEDIA) && HIWORD(wp) == CBN_SELCHANGE) {
            update_res_info();
            EnableWindow(GetDlgItem(hwnd, IDC_FILM), sel(IDC_MEDIA) == 1 && SendMessageW(GetDlgItem(hwnd, IDC_FILM), CB_GETCOUNT, 0, 0) > 1);
            if (emu_running && !closing) { push('s', "pmod", "enum", MODE_C[sel(IDC_MODE)]); push('s', "pmed", "enum", MEDIA_C[sel(IDC_MEDIA)]); push('t', NULL, NULL, NULL); } }
        else if (LOWORD(wp) == IDC_CASS && HIWORD(wp) == CBN_SELCHANGE) {
            if (emu_running && !closing && !busy_now) { push_mount(sel(IDC_MOUNT)); push_holder(cass_type()); push('n', NULL, NULL, "0"); } }
        else if (LOWORD(wp) == IDC_MOUNT && HIWORD(wp) == CBN_SELCHANGE) {     /* the original offers other holders for the other mount */
            cass_fill(sel(IDC_MOUNT), cass_type());
            if (emu_running && !closing && !busy_now) { push_mount(sel(IDC_MOUNT)); push_holder(cass_type()); push('n', NULL, NULL, "0"); } }
        else if (LOWORD(wp) == IDC_FRAME && HIWORD(wp) == CBN_SELCHANGE) { if (!busy_now) select_frame((int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0) + 1, 1); else frames_fill(); }
        else if (LOWORD(wp) == IDC_FORMAT && HIWORD(wp) == CBN_SELCHANGE) { passes_enable(); update_res_info(); }
        else if (LOWORD(wp) == IDC_HDR && HIWORD(wp) == CBN_SELCHANGE) passes_enable();
        else if (LOWORD(wp) == IDC_SHARP && HIWORD(wp) == CBN_SELCHANGE) table_choice(POP_SHARP);
        else if (LOWORD(wp) == IDC_COLOR && HIWORD(wp) == CBN_SELCHANGE) table_choice(POP_COLOR);
        else if (LOWORD(wp) == IDC_FILM && HIWORD(wp) == CBN_SELCHANGE) { table_choice(POP_FILM); if (emu_running && !closing) push_film(); }
        return 0;
    case WM_APP_LOG: { char *s = (char *)lp; if (wp) log8d(s); else log8(s); free(s); return 0; }
    case WM_APP_STATE:
        if (wp == ST_READY) { started_ui = 1; status(L"Ready."); set_busy(0); log_w(L"Ready."); push_mount(sel(IDC_MOUNT)); push_holder(cass_type()); push('n', NULL, NULL, "0"); push('s', "pmod", "enum", MODE_C[sel(IDC_MODE)]); push('s', "pmed", "enum", MEDIA_C[sel(IDC_MEDIA)]); push('t', NULL, NULL, NULL);
            if (autotest && wz_test_type >= 0) wz_start();               /* developer option --setup-test */
            else if (autotest) do_preview();
            else if (!started_ready_once) { started_ready_once = 1; PostMessageW(h, WM_COMMAND, IDM_SETUP_OFFER, 0); } }
        else if (wp == ST_HOLDER) {                        /* after a holder choice ('k'): the front end's holder; previews discarded? */
            int t = (int)(lp & 0xFFFF);
            if (cas_holder(t) && cas_holder(t)->transparent != (sel(IDC_MOUNT) == 0)) {    /* the mount the front end kept */
                SendMessageW(GetDlgItem(hwnd, IDC_MOUNT), CB_SETCURSEL, cas_holder(t)->transparent ? 0 : 1, 0); cass_fill(sel(IDC_MOUNT), t);
                log_w(cas_holder(t)->transparent ? L"Mount shown again as the scanner software has it: Transparent." : L"Mount shown again as the scanner software has it: Reflective."); }
            if (t != cass_type() && cas_holder(t)) { cass_fill(sel(IDC_MOUNT), t); wchar_t m[160], w[64]; to_w(cas_holder(t)->name, w, 64);
                swprintf(m, 160, L"Holder shown again as the scanner software has it: %ls.", w); log_w(m); }
            if (lp >> 16) { memset(frame_ready, 0, sizeof frame_ready); frames_done = 0; frames_auto_checked = 0; pv_invalid = 1; frames_fill();
                log_w(L"The scanner software was restarted for the new holder (as its Restart button): the previews of the previous holder are gone - make a new preview."); } }
        else if (wp == ST_PREVIEW_DONE) {
            int f = (int)(lp & 0xFFFF); if (f < 1 || f > 63) f = 1;
            if (!frame_ready[f]) { frame_ready[f] = 1; frames_done++; }
            if (f > frames_total) frames_total = f;
            frames_fill();
            if (f == frame_sel) load_preview_frame(f);
            if (user_stopped) { user_stopped = 0; status(L"Stopped (the frames previewed so far are kept)."); set_busy(0); lens_report(); if (wz_on) wz_end(L"Set up this scanner stopped by you."); else if (fs_on) fs_end(L"Fix streaks stopped by you - nothing stored.", 0, 1); else if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (frames_done < frames_total) { wchar_t m[100]; swprintf(m, 100, L"Preview: frame %d of %d done...", frames_done, frames_total); status(m); return 0; }
            if ((cass_type() == 0 || cass_type() == 19) && !frames_auto_checked) {     /* Automatic: the original may have found more masks */
                frames_auto_checked = 1; push('n', NULL, NULL, "2"); return 0; }
            preview_all_done(); }
        else if (wp == ST_SCAN_DONE) { scan_done(); set_busy(0); lens_report();
            if (user_stopped) { user_stopped = 0; log_w(L"Stop: this scan had finished before the Stop took effect - it is saved; nothing further is started."); if (bq_running) { bq_running = 0; log_w(L"Scan list stopped."); bq_show(); } status(L"Stopped."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (bq_running) { bq_idx++; bq_next(); }
            cg_note();
            if (autotest && fs_test_pending && !bq_running) { fs_test_pending = 0; fs_test = 1; if (!fs_start() && !closing) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (autotest && !bq_running) PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        else if (wp == ST_RAW_DONE && user_stopped && (rawj.tiled || rawj.passes > 1 || rawj.hdr_k > 1 || rawj.ps_F || rt_on || ft_on || lt_on || ch_on || fs_on)) {
            /* Stop came while one step of a longer RAW (blocks, passes, HDR, a test) was just finishing: the sequence ends here */
            user_stopped = 0; rawj_running = 0; raw_hdr_armed = 0; raw_stephold = 0; ps_end();
            if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; } free(rawj.hdr_n); rawj.hdr_n = NULL; if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; }
            set_busy(0); log_w(L"Stopped: the RAW was stopped as you asked; nothing was saved from it.");
            if (bq_running) { bq_running = 0; log_w(L"Scan list stopped."); bq_show(); }
            if (rt_on && rt_cur >= 0) { rt_ovr.on = 0; rt_bypass = 0; rt_put("pending", NULL); rt_finish(L"RAW size test stopped by you - this step has no result and runs again next time."); return 0; }
            if (ft_on) { log_w(L"Focus test stopped by you."); ft_finish(0); return 0; }
            if (lt_on) { log_w(L"Lens shift test stopped by you."); lt_finish(0); return 0; }
            if (ch_on) { ch_on = 0; rt_ovr.on = 0; wz_end(L"Set up this scanner stopped by you."); return 0; }
            if (fs_on) { fs_end(L"Fix streaks stopped by you - nothing stored.", 0, 1); return 0; }
            status(L"Stopped."); if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        else if (wp == ST_RAW_DONE) { if (user_stopped) { user_stopped = 0; log_w(L"Stop: the RAW was finished before the scanner could be stopped safely - it is saved; nothing further is started."); }
            int rr = raw_done(); if (rr == 1) return 0;   /* the next pass is running */
            raw_hdr_armed = 0; raw_stephold = 0; free(rawj.hdr_n); rawj.hdr_n = NULL;              /* never left armed after a RAW (review) */
            ps_end();                                                            /* pixel shift: lens back, passes freed */
            rawj_running = 0;
            set_busy(0);
            lens_report();
            if (rt_on && rt_cur >= 0) { rt_step_end(rr == 0); return 0; }
            if (ft_on) { ft_step_end(rr == 0); return 0; }
            if (lt_on) { lt_step_end(rr == 0); return 0; }
            if (ch_on) { ch_on = 0; rt_ovr.on = 0; wz_chart_done(rr == 0); return 0; }
            if (fs_on && fs_state == FS_RAW) { fs_step_end(rr == 0); return 0; }   /* 0.4.0: Fix streaks */
            if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; }
            if (rr < 0) {   /* a failed RAW stops a scan list, as every other failure does (found in review) */
                status(L"RAW scan not finished - see the log.");
                if (bq_running) { bq_running = 0; log_w(L"Scan list stopped."); bq_show(); }
                if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0; }
            if (bq_running) { bq_idx++; bq_next(); }
            cg_note();
            if (autotest && fs_test_pending && !bq_running) { fs_test_pending = 0; fs_test = 1; if (!fs_start() && !closing) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (autotest && !bq_running) PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        else if (wp == ST_JOB_FAILED && ch_on) { ch_on = 0; rt_ovr.on = 0; rawj_running = 0; raw_hdr_armed = 0; raw_stephold = 0; set_busy(0);
            if (user_stopped) { user_stopped = 0; wz_end(L"Set up this scanner stopped by you."); } else wz_chart_done(0); }
        else if (wp == ST_JOB_FAILED && fs_on) { rawj_running = 0; raw_hdr_armed = 0; raw_stephold = 0; set_busy(0);
            if (user_stopped) { user_stopped = 0; fs_end(L"Fix streaks stopped by you - nothing stored.", 0, 1); } else fs_end(L"Fix streaks stopped: the preview or RAW did not finish (see above) - nothing stored.", 0, 1); }
        else if (wp == ST_JOB_FAILED && wz_on && (wz_state == WZ_PREVIEW || wz_state == WZ_CHART_PREVIEW)) { set_busy(0);
            if (user_stopped) { user_stopped = 0; wz_end(L"Set up this scanner stopped by you."); } else wz_end(L"Set up this scanner stopped: the preview failed (see above)."); }
        else if (wp == ST_JOB_FAILED && lt_on) { rawj_running = 0; raw_hdr_armed = 0; raw_stephold = 0; set_busy(0); lt_step_end(0); }
        else if (wp == ST_JOB_FAILED && ft_on) { rawj_running = 0; raw_hdr_armed = 0; raw_stephold = 0; set_busy(0); ft_step_end(0); }
        else if (wp == ST_JOB_FAILED && rt_on && rt_cur >= 0) { rawj_running = 0; if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; } raw_hdr_armed = 0; raw_stephold = 0; set_busy(0); rt_step_end(0); }
        else if (wp == ST_JOB_FAILED && user_stopped) { user_stopped = 0; rawj_running = 0; ps_end(); if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; } if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; } free(rawj.hdr_n); rawj.hdr_n = NULL; raw_hdr_armed = 0; raw_stephold = 0; status(L"Stopped."); set_busy(0); if (bq_running) { bq_running = 0; log_w(L"Scan list stopped."); bq_show(); } if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        else if (wp == ST_JOB_FAILED) { raw_stream_armed = 0; rawj_running = 0; ps_end(); if (rawj.tm_on) { rtm_free(&rawj.tm); rawj.tm_on = 0; } if (rawj.st_on) { rs_free(&rawj.st); rawj.st_on = 0; } free(rawj.hdr_n); rawj.hdr_n = NULL; raw_hdr_armed = 0; raw_stephold = 0; status(L"Not finished - see the log."); set_busy(0); if (bq_running) { bq_running = 0; log_w(L"Scan list stopped."); bq_show(); } if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0); }
        else if (wp == ST_ERROR) { status(serial_mm ? L"The scanner settings belong to another scanner - see the question." : L"Stopped because of an error - see the log. Please send the Logs folder."); set_busy(1); }
        return 0;
    case WM_APP_DIALOG: show_dialog((DlgReq *)lp); return 0;
    case WM_APP_TABLES: tables_fill(); return 0;
    case WM_APP_FRAMES: {
        int n = (int)wp; if (n < 1) n = 1; if (n > 63) n = 63;
        if (lp == 1) {                     /* before a preview: preview frames 1..n, one after the other */
            frames_total = n; if (frame_sel > n) frame_sel = 1; frames_fill();
            if (n > 1) { wchar_t m[120]; swprintf(m, 120, L"This cassette holds %d frames - each is previewed.", n); log_w(m); }
            pg_set_total(n, 0);
            for (int k = 1; k <= n; k++) { char fv[16]; snprintf(fv, sizeof fv, "%d", k); push('P', NULL, NULL, fv); }
        } else if (lp == 0) {              /* cassette chosen: how many frames it holds (no preview yet) */
            if (n != frames_total) { frames_total = n; memset(frame_ready, 0, sizeof frame_ready); frame_sel = 1; frames_fill(); }
        } else if (lp == 2) {              /* Automatic, after the first preview */
            if (n > frames_total) { wchar_t m[160]; swprintf(m, 160, L"Automatic: the scanner software found %d frames and previews the others itself.", n); log_w(m); pg_set_total(n, 0);
                frames_total = n; frames_fill(); status(L"Preview: waiting for the other frames..."); }
            else preview_all_done();   /* 0.4.0 (review): the same end as any preview - Fix streaks / Set up continue from it */
        }
        return 0; }
    case WM_APP_PROGRESS:
        if ((int)wp == -1) { char *t = (char *)lp; if (job_ui && t[0] && !pg.on) { wchar_t w[300], m[400]; to_w(t, w, 300); swprintf(m, 400, L"%ls %ls", job_ui == 'p' ? L"Preview:" : L"Scan:", w); status(m); } free(t); }
        else if ((int)wp == -2) pg_unit_start();               /* the scanner software starts a preview / scan / RAW pass */
        else if ((int)wp >= 0 && (int)wp <= 100) { if (pg.on) pg_stup((int)wp);
            else if (job_ui) { wchar_t m[100]; swprintf(m, 100, L"%ls running... %d %%", job_ui == 'p' ? L"Preview" : L"Scan", (int)wp); status(m); } }
        return 0;
    case WM_APP_RANGE:
        probe_pending = 0; rng_min = (double)wp / 1000; rng_max = (double)lp / 1000;
        { wchar_t m[200]; if (rng_max > 0) swprintf(m, 200, L"Allowed resolution for this crop (checked by the scanner software): %d - %d dpi", (int)ceil(rng_min * 25.4), (int)floor(rng_max * 25.4));
          else swprintf(m, 200, L"The scanner software allows no resolution for this crop (crop too small or outside the scan area?)"); log_w(m); }
        update_res_info();
        if (auto_wait_range) { auto_wait_range = 0; do_scan(); }
        if (bq_wait_range) { bq_wait_range = 0; if (bq_running) { do_scan(); if (!busy_now) { bq_running = 0; log_w(L"Scan list stopped (see the message)."); bq_show(); } } }
        return 0;
    case WM_APP_EXITED:
        emu_running = 0; set_busy(1); pg_end();
        if (wp == 2 && closing) log_w(L"The scanner does not answer any more - closed without parking it.");
        if (closing) { cg_restart_now((int)wp); DestroyWindow(h); return 0; }
        if (wp == 2) {   /* 0.2.0: the scanner no longer answers (guard.c GONE_LIMIT): a clear message instead of an internal error */
            if (rt_on && rt_cur >= 0) { rt_step_end(0); set_busy(1); pg_end(); }   /* a RAW check step: recorded as the step that stopped the scanner; nothing can run now (review) */
            log_w(L"Stopped: the scanner does not answer any more. Switch the scanner off, wait 10 seconds, switch it on, wait for the steady green light, then start this program again. (If you switched it off on purpose, just close the window.)");
            status(L"The scanner does not answer - switch it off and on, then start this program again.");
            if (autotest) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            else MessageBoxW(hwnd, L"The scanner does not answer any more - it may have been switched off, or its cable or adapter came loose.\n\n"
                                   L"Switch the scanner off, wait 10 seconds, switch it on and wait for the steady green light, then start this program again.", APP_TITLE, MB_ICONWARNING);
            return 0; }
        if (serial_mm) { prefs_offer(); return 0; }
        log_w(L"The scanner software has ended.");
        status(wp ? L"Stopped because of an error - see the log. Close the window and send the Logs folder." : L"Ended. Close the window.");
        if (autotest && wp) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        if (emu_running && !closing) {
            closing = 1; status(L"Closing: the scanner software saves its settings and parks the scanner...");
            if (simulate && sim_off_at_quit) { sim_unplug = 1; log_d(L"Developer option --sim-off-at-quit: the simulated scanner is switched off before the quit."); }
            log_w(L"Quit requested."); push('q', NULL, NULL, NULL); SetTimer(h, 1, 30000, NULL); return 0;
        }
        DestroyWindow(h); return 0;
    case WM_TIMER:
        if (wp == 1) { log_w(L"Quit timed out."); if (cg_restart[0]) cg_restart_now(1); DestroyWindow(h); }
        if (wp == 3) { KillTimer(h, 3); if (!busy_now) start_probe(); return 0; }
        if (wp == 5) { KillTimer(h, 5); SendMessageW(h, WM_COMMAND, IDC_STOP, 0); return 0; }
        if (wp == 10) { if (guard_capture) { KillTimer(h, 10); stop_at_capture = 0; log_d(L"Developer option --stop-at-capture: Stop while the picture is being sent."); SendMessageW(h, WM_COMMAND, IDC_STOP, 0); } return 0; }
        if (wp == 7) { pg_tick(); return 0; }
        if (wp == 9) { KillTimer(h, 9); if (!pg.on && hprog) { SendMessageW(hprog, PBM_SETPOS, 0, 0); ShowWindow(hprog, SW_HIDE); } return 0; }
        if (wp == 2) { KillTimer(h, 2);
            if (raw_test_auto) { rt_start(0); if (!rt_on) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (lens_test_auto) {
                SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(GetDlgItem(hwnd, IDC_CX), L"20"); SetWindowTextW(GetDlgItem(hwnd, IDC_CY), L"20"); SetWindowTextW(GetDlgItem(hwnd, IDC_CW), L"4"); SetWindowTextW(GetDlgItem(hwnd, IDC_CH), L"4");
                lt_start(0); if (!lt_on) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (focus_test_auto) {   /* autotest: crop 4 x 4 mm at 20, 20 */
                SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(GetDlgItem(hwnd, IDC_CX), L"20"); SetWindowTextW(GetDlgItem(hwnd, IDC_CY), L"20"); SetWindowTextW(GetDlgItem(hwnd, IDC_CW), L"4"); SetWindowTextW(GetDlgItem(hwnd, IDC_CH), L"4");
                ft_start(0); if (!ft_on) PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
            if (drag_test_on) {   /* the same mouse messages a real drag produces, from mm through the display geometry */
                int ox = 0, oy = 0, dw = 0, dh = 0;
                if (pv_mm_rows > 0 && prev_geom(hprev, &ox, &oy, &dw, &dh)) {
                    int x0 = ox + (int)(drag_test[0] / pv_mm_cols * dw + 0.5), y0 = oy + (int)(drag_test[1] / pv_mm_rows * dh + 0.5);
                    int x1 = ox + (int)((drag_test[0] + drag_test[2]) / pv_mm_cols * dw + 0.5), y1 = oy + (int)((drag_test[1] + drag_test[3]) / pv_mm_rows * dh + 0.5);
                    SendMessageW(hprev, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x0, y0));
                    SendMessageW(hprev, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x1, y1));
                    SendMessageW(hprev, WM_LBUTTONUP, 0, MAKELPARAM(x1, y1));
                }
                wchar_t m[200]; swprintf(m, 200, L"Drag test: crop Left %.2f Top %.2f Width %.2f Height %.2f mm (preview shown %d x %d)",
                    edit_num(IDC_CX), edit_num(IDC_CY), edit_num(IDC_CW), edit_num(IDC_CH), dw, dh); log_w(m);
            } else {
                SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(GetDlgItem(hwnd, IDC_CX), L"2"); SetWindowTextW(GetDlgItem(hwnd, IDC_CY), L"2");
            }
            if (crop_text_on) {   /* --crop-text: the crop typed as text (as an owner would, e.g. with decimal commas) */
                SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0);
                SetWindowTextW(GetDlgItem(hwnd, IDC_CX), crop_text[0]); SetWindowTextW(GetDlgItem(hwnd, IDC_CY), crop_text[1]);
                SetWindowTextW(GetDlgItem(hwnd, IDC_CW), crop_text[2]); SetWindowTextW(GetDlgItem(hwnd, IDC_CH), crop_text[3]);
                wchar_t m[200]; swprintf(m, 200, L"Crop typed: Left %ls Top %ls Width %ls Height %ls", crop_text[0], crop_text[1], crop_text[2], crop_text[3]); log_w(m); }
            if (auto_frame > 0) select_frame(auto_frame, 0);
            if (auto_res[0]) SetWindowTextW(GetDlgItem(hwnd, IDC_RES), auto_res);
            for (int t = 0; t < 3; t++) if (auto_tab[t][0]) {      /* pick by name, as a user would in the combo */
                int k = t == 2 ? POP_FILM : t ? POP_COLOR : POP_SHARP, f = -1;
                for (int i = 0; i < ui_lists[k].n; i++) if (!strcmp(ui_lists[k].item[i], auto_tab[t])) f = i;
                if (f >= 0) { SendMessageW(GetDlgItem(hwnd, tab_ctl(k)), CB_SETCURSEL, f, 0); table_choice(k); if (k == POP_FILM) push_film(); }
                else { wchar_t m[200], w[96]; to_w(auto_tab[t], w, 96); swprintf(m, 200, L"Autotest: table \"%ls\" not in the list", w); log_w(m); } }
            KillTimer(h, 3);
            if (auto_list > 0) {                 /* --list N: N crops side by side into the scan list, then "Scan list" */
                for (int i = 0; i < auto_list; i++) { set_edit_mm(IDC_CX, 2 + i * 12.0); set_edit_mm(IDC_CY, 4 + i * 3.0); set_edit_mm(IDC_CW, 10); set_edit_mm(IDC_CH, 15 + i);
                    SendMessageW(GetDlgItem(hwnd, IDC_USECROP), BM_SETCHECK, BST_CHECKED, 0); KillTimer(h, 3);
                    if (auto_list_mix) SendMessageW(GetDlgItem(hwnd, IDC_FORMAT), CB_SETCURSEL, i % 2 ? 0 : FMT_RAW, 0);   /* RAW, TIFF, RAW ... */
                    bq_add(); }
                SendMessageW(hwnd, WM_COMMAND, IDC_BSCAN, 0);
            } else { auto_wait_range = 1; start_probe(); } }   /* the scan follows when the range is known */
        return 0;
    case WM_DESTROY: if (!cg_relaunched) settings_save();
        if (autotest && logdir[0]) {   /* 0.2.0 (tests): what the window's log showed, in window-log.txt */
            wchar_t p[MAX_PATH * 2]; swprintf(p, MAX_PATH * 2, L"%ls\\window-log.txt", logdir); FILE *f = _wfopen(p, L"wb");
            if (f) { int n = (int)SendMessageW(hlog, LB_GETCOUNT, 0, 0);
                for (int i = 0; i < n; i++) { int len = (int)SendMessageW(hlog, LB_GETTEXTLEN, i, 0); if (len < 0 || len > 8000) continue;
                    wchar_t w[8200]; SendMessageW(hlog, LB_GETTEXT, i, (LPARAM)w); char u[25000]; to_utf8(w, u, sizeof u); fprintf(f, "%s\r\n", u); }
                fclose(f); } }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR cmd, int show) {
    (void)hp; (void)cmd;
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--simulate")) simulate = 1;
        if (!wcscmp(argv[i], L"--autotest")) autotest = 1;
        if (!wcscmp(argv[i], L"--sharp") && i + 1 < argc) to_utf8(argv[++i], auto_tab[0], 96);
        if (!wcscmp(argv[i], L"--color") && i + 1 < argc) to_utf8(argv[++i], auto_tab[1], 96);
        if (!wcscmp(argv[i], L"--ini")) ini_in_autotest = 1;
        if (!wcscmp(argv[i], L"--list") && i + 1 < argc) auto_list = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"--frame") && i + 1 < argc) auto_frame = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"--keep-raw")) keep_raw = 1;
        if (!wcscmp(argv[i], L"--raw-limit") && i + 1 < argc) swprintf(raw_limit_opt, 32, L"%ls", argv[++i]);   /* developer option: WxH, 0x0 = no RAW size guard */
        if (!wcscmp(argv[i], L"--raw-stream")) raw_stream_opt = 1;   /* developer option: Tools > Streamed RAW on */
        if (!wcscmp(argv[i], L"--raw-test")) raw_test_auto = 1;   
        if (!wcscmp(argv[i], L"--focus-test")) focus_test_auto = 1;
        if (!wcscmp(argv[i], L"--lens-test")) lens_test_auto = 1;   /* developer option (autotest): run the lens shift test after the preview */
        if (!wcscmp(argv[i], L"--mirror")) mirror_opt = 1;   /* developer option (autotest): Mirror ticked */
        if (!wcscmp(argv[i], L"--pixel-shift")) ps_opt = 1;
        if (!wcscmp(argv[i], L"--raw-sharpen") && i + 1 < argc) rsh_opt = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"--colreg") && i + 1 < argc) colreg_opt = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"--noalign")) noalign_opt = 1;
        if (!wcscmp(argv[i], L"--miner") && i + 1 < argc) miner_opt = _wtoi(argv[++i]) ? 1 : 0;   /* developer option (tests): the miner progress bar always (1) or never (0); default: now and then */
        if (!wcscmp(argv[i], L"--stephold")) stephold_opt = 1;   /* developer option: Tools > RAW step-and-hold on */
        if (!wcscmp(argv[i], L"--no-stephold")) stephold_opt = -1;   /* developer option: ... off */
        if (!wcscmp(argv[i], L"--no-ps-slow")) psslow_opt = -1;   /* developer option: Pixel shift: slow lens moves off */
        if (!wcscmp(argv[i], L"--detail-log")) log_detail_opt = 1;   /* developer option: Tools > Detailed log in the window */
        if (!wcscmp(argv[i], L"--ps-slow")) psslow_opt = 1;   /* developer option: Tools > Pixel shift: slow lens moves */
        if (!wcscmp(argv[i], L"--ps-passes") && i + 1 < argc) psn_opt = _wtoi(argv[++i]);   /* developer option: pixel shift passes 5 / 8 */   /* developer option: Tools > scanner colour alignment off */   /* developer option: RAW colour registration 0 / 1 */   /* developer option: RAW sharpening level 0..3 */   /* developer option: Tools > Pixel shift RAW on */
        if (!wcscmp(argv[i], L"--sim-drop-line") && i + 1 < argc) sim_drop_line = _wtoi(argv[++i]);   /* developer option (simulator, with --sim-bed-texture) */
        if (!wcscmp(argv[i], L"--sim-bed-texture") && i + 1 < argc) sim_bed_sigma = _wtof(argv[++i]);   /* developer option (simulator): RAW samples from a bed-fixed texture */
        if (!wcscmp(argv[i], L"--sim-off-unit") && i + 1 < argc) sim_off_unit = _wtoi(argv[++i]);   /* developer option (simulator): switch the scanner off as unit K of a job starts (a RAW pass, a frame) */
        if (!wcscmp(argv[i], L"--sim-off-at-quit")) sim_off_at_quit = 1;   /* developer option (simulator): switch the scanner off before the quit */
        if (!wcscmp(argv[i], L"--neg-contrast") && i + 1 < argc) neg_c_opt = _wtoi(argv[++i]) % 3;   /* developer option (autotest): negative contrast 0 / 1 / 2 */
        if (!wcscmp(argv[i], L"--neg-rotate") && i + 1 < argc) neg_rot_opt = _wtoi(argv[++i]) & 3;     /* developer option (autotest): rotate the positive k x 90 right */
        if (!wcscmp(argv[i], L"--neg-keep")) neg_keep_opt = 1;                                        /* developer option (autotest): keep the levels after the preview */
        if (!wcscmp(argv[i], L"--stop-at-capture") && i + 1 < argc) { i++; stop_at_capture = !wcscmp(argv[i], L"preview") ? 1 : !wcscmp(argv[i], L"raw") ? 3 : 2; }   /* developer option (autotest): press Stop as the preview's / scan's first picture is being sent (0.4.0) */
        if (!wcscmp(argv[i], L"--stop-after-ms") && i + 1 < argc) stop_after_ms = _wtoi(argv[++i]);   /* developer option (autotest): press Stop that long after Scan */   /* developer option (autotest): run the focus test after the preview */
        if (!wcscmp(argv[i], L"--focus") && i + 1 < argc) swprintf(focus_opt, 32, L"%ls", argv[++i]);   /* developer option: focus offset mm */   /* developer option (autotest): run the RAW size test after the preview */
        if (!wcscmp(argv[i], L"--name") && i + 1 < argc) swprintf(auto_name, 64, L"%ls", argv[++i]);
        if (!wcscmp(argv[i], L"--list-mix")) auto_list_mix = 1;
        if (!wcscmp(argv[i], L"--passes") && i + 1 < argc) { int v = _wtoi(argv[++i]); for (int k = 0; k < (int)(sizeof PASSES_V / sizeof *PASSES_V); k++) if (PASSES_V[k] == v) passes_default = k; }   /* developer option */
        if (!wcscmp(argv[i], L"--keep-passes")) keep_passes_default = 1;
        if (!wcscmp(argv[i], L"--gamma") && i + 1 < argc) gamma_default = _wtoi(argv[++i]) == 1;   /* developer option: RAW gamma 0 linear, 1 = 2.2 */
        if (!wcscmp(argv[i], L"--hdr") && i + 1 < argc) { int v = _wtoi(argv[++i]); if (v >= 0 && v <= 4) hdr_default = v; }   /* developer option: 0 normal, 1 +1 stop, 2 +2 stops, 3 long x2, 4 long x4 */
        if (!wcscmp(argv[i], L"--hdr-test-gain") && i + 1 < argc) hdr_test_gain = _wtof(argv[++i]);   /* developer option: --list entries alternate RAW / TIFF */
        if (!wcscmp(argv[i], L"--format") && i + 1 < argc) { int k = _wtoi(argv[++i]); if (k >= 0 && k <= FMT_RAW) format_default = k; }   /* developer option: file format index */
        if (!wcscmp(argv[i], L"--film") && i + 1 < argc) to_utf8(argv[++i], auto_tab[2], 96);
        if (!wcscmp(argv[i], L"--media") && i + 1 < argc) media_default = _wtoi(argv[++i]) == 1;
        if (!wcscmp(argv[i], L"--res") && i + 1 < argc) swprintf(auto_res, 32, L"%ls", argv[++i]);   /* developer option (autotest): dpi */
        if (!wcscmp(argv[i], L"--crop-text") && i + 1 < argc) {
            wchar_t *a = argv[++i]; int k = 0;
            for (wchar_t *p = a; k < 4; k++) { wchar_t *e = wcschr(p, L';'); size_t n = e ? (size_t)(e - p) : wcslen(p); if (n > 31) n = 31; wmemcpy(crop_text[k], p, n); crop_text[k][n] = 0; if (!e) { k++; break; } p = e + 1; }
            crop_text_on = (k == 4);
        }
        if (!wcscmp(argv[i], L"--drag-mm") && i + 1 < argc) {
            char c[100]; to_utf8(argv[++i], c, sizeof c);
            drag_test_on = sscanf(c, "%lf,%lf,%lf,%lf", &drag_test[0], &drag_test[1], &drag_test[2], &drag_test[3]) == 4;
        }
        if (!wcscmp(argv[i], L"--cassette") && i + 1 < argc) {      /* developer option: initial holder - a 0.1.x code (the holder it really
                                                                       selected, so recorded sessions replay unchanged) or a cassette-directory type */
            char c[16]; to_utf8(argv[++i], c, sizeof c);
            int t = cas_legacy_type(c); if (t < 0 && c[0] >= '0' && c[0] <= '9') t = atoi(c);
            if (t >= 0 && cas_holder(t)) cass_default = t;
        }
        if (!wcscmp(argv[i], L"--mode") && i + 1 < argc) { int m = _wtoi(argv[++i]); if (m >= 0 && m <= 2) mode_default = m; }   /* developer option: 0 CMYK 1 RGB 2 B/W */
        if (!wcscmp(argv[i], L"--setup-chart")) wz_chart_opt = 1;   /* developer option (autotest): measure the (simulated) chart too */
        if (!wcscmp(argv[i], L"--setup-test") && i + 1 < argc) wz_test_type = _wtoi(argv[++i]);   /* developer option (autotest): Set up this scanner with this holder type */
        if (!wcscmp(argv[i], L"--clean-gain") && i + 1 < argc) swprintf(cg_pending, MAX_PATH * 2, L"%ls", argv[++i]);   /* 0.3.1: store the clean gain vectors (cg_store starts the program with it) */
        if (!wcscmp(argv[i], L"--clean-gain-answer") && i + 1 < argc) cg_store_auto = _wtoi(argv[++i]);   /* developer option (autotest) */
        if (!wcscmp(argv[i], L"--fix-streaks")) fs_test_pending = 1;   /* developer option (autotest): Tools > Fix streaks after the first scan */
        if (!wcscmp(argv[i], L"--prefs-from-flash") && i + 1 < argc) swprintf(prefs_from, MAX_PATH * 2, L"%ls", argv[++i]);   /* 0.3.0: the parameter copy (prefs_offer starts the program with it) */
        if (!wcscmp(argv[i], L"--lw-butting") && i + 1 < argc) prefs_lw_opt = _wtoi(argv[++i]);
        if (!wcscmp(argv[i], L"--prefs-answer") && i + 1 < argc) prefs_auto = _wtoi(argv[++i]);   /* developer option (autotest) */
        if (!wcscmp(argv[i], L"--fake-scsi") && i + 1 < argc) { static char fk[1024]; to_utf8(argv[++i], fk, sizeof fk); spti_fake = fk; simulate = 1; }   /* developer option (tests): a SCSI device list "port:bus:id:VENDOR:PRODUCT;..." - nothing is sent; the simulator stands in */
        if (!wcscmp(argv[i], L"--scsi-choose") && i + 1 < argc) scsi_choose_opt = _wtoi(argv[++i]);   /* developer option (autotest): the answer when several scanners are found */
        if (!wcscmp(argv[i], L"--scsi-none") && i + 1 < argc) scsi_none_opt = _wtoi(argv[++i]);   /* developer option (autotest): the answer when none is found */
        if (!wcscmp(argv[i], L"--scsi-type-id") && i + 1 < argc) swprintf(scsi_type_opt, 16, L"%ls", argv[++i]);   /* developer option (autotest): the ID typed */
        if (!wcscmp(argv[i], L"--replay") && i + 1 < argc) { simulate = 1; world_extclock = 1; to_utf8(argv[++i], replay8, sizeof replay8); }
    }
    GetModuleFileNameW(NULL, exedir, MAX_PATH); wchar_t *bs = wcsrchr(exedir, L'\\'); if (bs) *bs = 0;
    swprintf(voldir, MAX_PATH, L"%ls\\%ls", exedir, replay8[0] ? L"Volume-replay" : simulate ? L"Volume-simulator" : L"Volume");
    swprintf(scandir_, MAX_PATH, L"%ls\\Scans", exedir);
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t logroot[MAX_PATH]; swprintf(logroot, MAX_PATH, L"%ls\\Logs", exedir); CreateDirectoryW(logroot, NULL);
    swprintf(logdir, MAX_PATH, L"%ls\\%04d%02d%02d-%02d%02d%02d%ls", logroot, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, simulate ? L"-sim" : L"");
    CreateDirectoryW(logdir, NULL);
    wchar_t p[MAX_PATH * 2];
    to_utf8(voldir, vol8, sizeof vol8);
    swprintf(p, MAX_PATH * 2, L"%ls\\trace.txt", logdir); to_utf8(p, trace8, sizeof trace8);
    swprintf(p, MAX_PATH * 2, L"%ls\\scsi-trace.bin", logdir); to_utf8(p, btrace8, sizeof btrace8);
    swprintf(p, MAX_PATH * 2, L"%ls\\Simulator\\flash.rf", exedir); to_utf8(p, simflash8, sizeof simflash8);
    swprintf(p, MAX_PATH * 2, L"%ls\\Simulator\\flash14.bin", exedir); to_utf8(p, simbin8, sizeof simbin8);
    swprintf(p, MAX_PATH * 2, L"%ls\\Simulator\\profile.txt", exedir); to_utf8(p, simprof8, sizeof simprof8);
    InitializeCriticalSection(&cs);

    WNDCLASSW wc = { 0 }; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.lpfnWndProc = wnd_proc; wc.lpszClassName = L"S342Main"; RegisterClassW(&wc);
    wc.lpfnWndProc = prev_proc; wc.lpszClassName = L"S342Preview"; wc.hbrBackground = NULL; RegisterClassW(&wc);
    wc.lpfnWndProc = dlg_proc; wc.lpszClassName = L"S342Dialog"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); RegisterClassW(&wc);
    wc.lpfnWndProc = panel_proc; wc.lpszClassName = L"S342Panel"; RegisterClassW(&wc);

    wchar_t title[200]; swprintf(title, 200, L"%ls %ls%ls", APP_TITLE, APP_VERSION, replay8[0] ? L" - REPLAY OF A RECORDED SESSION" : simulate ? L" - SIMULATED SCANNER" : L"");
    CreateWindowExW(0, L"S342Main", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1000, window_dpi(NULL), 96), MulDiv(800, window_dpi(NULL), 96), NULL, NULL, hi, NULL);
    ShowWindow(hwnd, show); UpdateWindow(hwnd);

    if (simulate && GetFileAttributesW(voldir) == INVALID_FILE_ATTRIBUTES) {
        /* the simulator works on its own copy of the volume, so the real one keeps the scanner's settings */
        wchar_t real[MAX_PATH]; swprintf(real, MAX_PATH, L"%ls\\Volume", exedir);
        status(L"Preparing the simulator's work folder...");
        if (!copy_tree(real, voldir)) MessageBoxW(hwnd, L"Could not copy the work folder for the simulator.", APP_TITLE, MB_ICONERROR);
    }
    /* the Mac volume next to the program */
    wchar_t chk[MAX_PATH * 2]; swprintf(chk, MAX_PATH * 2, L"%ls\\Smart342(L) V2.2.0\\Smart342(L)", voldir);
    if (GetFileAttributesW(chk) == INVALID_FILE_ATTRIBUTES) {
        wchar_t m[MAX_PATH * 3]; swprintf(m, MAX_PATH * 3, L"The program's work folder was not found:\n%ls\n\nUnpack the whole package and start the program from its folder.", voldir);
        MessageBoxW(hwnd, m, APP_TITLE, MB_ICONERROR); return 1;
    }
    { wchar_t m[MAX_PATH * 3]; swprintf(m, MAX_PATH * 3, L"Logs for this session: %ls", logdir); log_w(m); }
    if (prefs_from[0]) prefs_apply();
    if (cg_pending[0]) cg_apply();
    {   /* directory IDs are kept from now on (world.c, .dirids.txt). Front-end preferences written before that may hold
           directory IDs of an earlier session (e.g. its tables folder), so they are set aside once and the original
           program writes new ones, as at its very first start. */
        wchar_t ids[MAX_PATH * 2], pf[MAX_PATH * 2], pfr[MAX_PATH * 2], old[MAX_PATH * 2], oldr[MAX_PATH * 2];
        swprintf(ids, MAX_PATH * 2, L"%ls\\.dirids.txt", voldir);
        swprintf(pf, MAX_PATH * 2, L"%ls\\System Folder\\Preferences\\342 application prefs", voldir); swprintf(pfr, MAX_PATH * 2, L"%ls.rsrc", pf);
        swprintf(old, MAX_PATH * 2, L"%ls (before 0.1.9)", pf); swprintf(oldr, MAX_PATH * 2, L"%ls.rsrc", old);
        if (GetFileAttributesW(ids) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(pfr) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(old); DeleteFileW(oldr);
            int ok = MoveFileW(pf, old) | MoveFileW(pfr, oldr);
            if (ok) log_d(L"Front-end preferences ('342 application prefs') from an earlier version set aside as '... (before 0.1.9)'; the program makes new ones.");
            else log_w(L"Note: could not set aside the old settings of the scanner software.");
        }
    }
    world_persist_dirids = 1;
    if (!simulate || spti_fake) {
        int use_sim = 0;
        hw_target = find_scanner(&use_sim);
        if (!hw_target && use_sim && !spti_fake) {   /* "Use the simulator": this program again, with --simulate */
            if (relaunch(L"--simulate")) return 0;
            MessageBoxW(hwnd, L"Could not start the simulator.", APP_TITLE, MB_ICONERROR); return 1;
        }
        if (use_sim < 0) return 0;                           /* Quit chosen */
        if (spti_fake) { if (!use_sim) return 0; log_w(L"SIMULATED SCANNER - no scanner is used (test list of SCSI devices)."); }
        else if (hw_target) {
            hw_target->note = scsi_note;      /* the SCSI layer's own reports (a scanner that stopped answering) */
            /* commands that would change the scanner's stored parameters never reach it (see guard.c) */
            hw_target = guard_new(hw_target); hw_target->note = guard_note;
            log_d(L"Safety interlock on: the scanner's NV-RAM and flash-disk parameters are never written.");
            /* 0.5.0: the SCSI card's limit per command (any card: Windows' port driver reports it) against the Scitex
               driver's largest read (307,200 bytes - final scans; RAW reads are at most 10 lines, smaller) */
            if (spti_caps.known) {
                wchar_t m[900], lim[60];
                if (spti_caps.max_bytes == 0xFFFFFFFFu) swprintf(lim, 60, L"no limit"); else swprintf(lim, 60, L"%lu KB", (unsigned long)(spti_caps.max_bytes / 1024));
                swprintf(m, 900, L"SCSI card limits: %ls per command (maximum transfer %lu bytes, %lu memory pages; alignment mask %lu). Needed: %lu KB.",
                         lim, (unsigned long)spti_caps.max_transfer, (unsigned long)spti_caps.max_pages, (unsigned long)spti_caps.align_mask, (unsigned long)(SPTI_NEED_BYTES / 1024));
                log_d(m);
                if (spti_caps.max_bytes < SPTI_NEED_BYTES) {
                    swprintf(m, 900, L"Your SCSI card's driver allows only %lu KB per command. Previews and smaller scans work, but larger scans need up to %lu KB per command and would fail (Windows error 87).\n\n"
                                     L"Many SCSI card drivers have a setting for this: the registry value MaximumSGList (DWORD) under HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Services\\<the card's driver>\\Parameters\\Device. "
                                     L"Set it to 255 (at least %lu), then restart Windows. See \"SCSI card limits\" in README.txt.",
                             (unsigned long)(spti_caps.max_bytes / 1024), (unsigned long)(SPTI_NEED_BYTES / 1024), (unsigned long)(SPTI_NEED_BYTES / 4096 + 1));
                    log_w(m);
                    if (!autotest) MessageBoxW(hwnd, m, APP_TITLE, MB_ICONWARNING);
                }
            } else log_d(L"SCSI card limits: the card's driver did not report them.");
        }
        if (!hw_target && !spti_fake) {
            status(L"No scanner in use. Close the window, check the scanner and start again as Administrator.");
            if (autotest) return 0;
            MSG m; while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
            return 0;
        }
    } else log_w(L"SIMULATED SCANNER - no scanner is used.");
    status(L"Starting the scanner software (this takes a moment)...");
    emu_running = 1;
    CreateThread(NULL, 64u << 20, emu_thread, NULL, 0, NULL);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { if (!IsDialogMessageW(hwnd, &m)) { TranslateMessage(&m); DispatchMessageW(&m); } }
    return 0;
}
