/* rawscan.c - 12-bit "raw" scans through the original's Max Detail path (research tools, 2026-10-08).
 *
 * Max Detail is a prescan of an area at the final scan resolution (User Guide 7.6); the driver writes it as a 12-bit
 * picture-memory file (ScitexScanMaxDetPM_<n>), before gradation, colour tables and sharpening. Front end:
 *   Handle_Max_Details (0x010914): set_max_details_crop(centre) unless already in max-detail mode, Check_Valid_Param,
 *   start_BG_parameters_file(6) + write_prescan_parameters(USER_PARAM), QuePushOperation(op 3).
 *   set_max_details_crop (0x0bec70): area in output pixels from TOC-0x2510 (w) / TOC-0x250e (h) (only the keyboard
 *   handler set_max_det_size 0x0bef04 clamps them to 180..500); crop mm = pixels / (res(+0x22) * scale(+0x2a) / 100);
 *   centre = exact point (frame data at *(TOC-0x744c) +0x4c/+0x50) * GetScale() (0x09d908).
 *   USER_PARAM = *(TOC-0x764c).
 * Driver: SC_do_full_details_prescan -> scanner_full_details_prescan -> prescan (the routine the preview uses). */
#include "mac.h"
#include <string.h>
#include <stdio.h>

static uint32_t up(Emu *E) { return rd32(E, E->toc - 0x764c); }
static float rf(Emu *E, uint32_t a) { uint32_t b = rd32(E, a); float f; memcpy(&f, &b, 4); return f; }
static void wf(Emu *E, uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); wr32(E, a, b); }
static uint32_t tvec(Emu *E, uint32_t off) { uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu); wr32(E, tv, CODE + off); wr32(E, tv + 4, E->toc); return tv; }

double raw_getscale(Emu *E) { emu_call_own(E, tvec(E, 0x09d908), NULL, 0); return E->nest_f1; }
void raw_dump(Emu *E, const char *tag) {
    uint32_t u = up(E);
    static const int fo[] = { 0x4, 0x8, 0xc, 0x10, 0x14, 0x18, 0x1c, 0x22, 0x26, 0x2a, 0x2e };
    char line[600]; int k = snprintf(line, sizeof line, "%s UP:", tag);
    for (size_t i = 0; i < sizeof fo / sizeof *fo; i++) k += snprintf(line + k, sizeof line - k, " +%02x=%g", fo[i], (double)rf(E, u + fo[i]));
    k += snprintf(line + k, sizeof line - k, " +38=%d +3a=%d  maxdet %d x %d  scale %g", rds16(E, u + 0x38), rds16(E, u + 0x3a),
                  rds16(E, E->toc - 0x2510), rds16(E, E->toc - 0x250e), raw_getscale(E));
    uint32_t fd = rd32(E, E->toc - 0x744c);
    k += snprintf(line + k, sizeof line - k, "  frame centre %g, %g", (double)rf(E, fd + 0x4c), (double)rf(E, fd + 0x50));
    TLOG(E, "   RAW %s\n", line);
}
/* request a Max Detail of wpx x hpx output pixels centred at (cx, cy) in the units of the display frame */
int raw_maxdet(Emu *E, double cx, double cy, int wpx, int hpx) {
    uint32_t fd = rd32(E, E->toc - 0x744c);
    wf(E, fd + 0x4c, (float)cx); wf(E, fd + 0x50, (float)cy);
    wr16(E, E->toc - 0x2510, (uint32_t)wpx); wr16(E, E->toc - 0x250e, (uint32_t)hpx); wr16(E, E->toc - 0x250c, (uint32_t)hpx);
    raw_dump(E, "before");
    int r = (int)(int32_t)emu_call_own(E, tvec(E, 0x010914), NULL, 0);
    raw_dump(E, "after");
    TLOG(E, "   RAW Handle_Max_Details -> %d\n", r);
    return r;
}

/* ---- driver side ----
 * scanner_run_prescan (driver 0x045700) converts each chunk of prescan data in place, in 14-bit mode
 * (use_Inpol_14_bit, not line-art) through a table (loop 0x045a24..0x045a78), for line-art with >> 2 (loop
 * 0x045a94..0x045adc). Both loops count the samples of a chunk (pixels x 3 x rows, r28) with a 16-bit index
 * (extsh r31): above 32767 samples per chunk the index wraps negative and the loop never ends (and writes outside the
 * buffer). The front end never asks for such areas (Max Detail <= 500 x 500 pixels); a whole crop at the final
 * resolution does (e.g. 1400 pixels x 3 x 20 rows = 84000). The six extsh of the index are replaced by mr (the same
 * register, 32 bits); nothing else changes. Each word is checked before it is patched. */
static const uint32_t PATCH_AT[6] = { 0x045a34, 0x045a60, 0x045a70, 0x045aa4, 0x045ac4, 0x045ad4 };
static const uint32_t PATCH_OLD[6] = { 0x7feb0734, 0x7fe60734, 0x7fe70734, 0x7fea0734, 0x7fe30734, 0x7fe40734 };
static const uint32_t PATCH_NEW[6] = { 0x7febfb78, 0x7fe6fb78, 0x7fe7fb78, 0x7feafb78, 0x7fe3fb78, 0x7fe4fb78 };
volatile int raw_inpol14 = -1;
volatile int raw_lim_w, raw_lim_h, raw_lim_w2, raw_lim_h2;   /* RAW size guard in output pixels: inside either rectangle (0 x 0 = no limit) */
volatile long raw_lim_area;    /* and/or at most this many pixels (width x lines; 0 = no limit) - the controller's memory rule (fwmem.h) */
double raw_tab_slope, raw_tab_off;      /* the driver's 14 -> 12 bit table constants (0 = not read) */          /* last prescan: 1 = 14-bit mode (host table), 0 = not, -1 = unknown */
static void inpol_bp(Emu *E, void *ctx) {
    (void)ctx; int v = (int)(E->cpu->gpr[3] & 0xFF) ? 1 : 0;
    if (v != raw_inpol14) TLOG(E, "   RAW prescan 14-bit mode (use_Inpol_14_bit) = %d\n", v);
    raw_inpol14 = v;
}
int raw_patch_driver(Emu *bg) {
    for (int i = 0; i < 6; i++) if (rd32(bg, CODE + PATCH_AT[i]) != PATCH_OLD[i]) {
        TLOG(bg, "   RAW driver patch NOT applied: word at %06x is %08x, expected %08x\n", PATCH_AT[i], rd32(bg, CODE + PATCH_AT[i]), PATCH_OLD[i]);
        return 0;
    }
    for (int i = 0; i < 6; i++) wr32(bg, CODE + PATCH_AT[i], PATCH_NEW[i]);
    emu_add_bp(bg, 0x0459e4, inpol_bp, NULL);     /* right after use_Inpol_14_bit() in the chunk loop: r3 = mode */
    /* the 14 -> 12 bit table scanner_run_prescan builds (0x0458a4..0x045924): v <= 255 -> v, else
       (int)((v - 255) * slope + offset), slope = float G[-0x503c], offset = float G[-0x5040] (read here, not assumed) */
    raw_tab_slope = rf(bg, bg->toc - 0x503c); raw_tab_off = rf(bg, bg->toc - 0x5040);
    TLOG(bg, "   RAW 14->12 table of the driver: slope %.9g, offset %.9g\n", raw_tab_slope, raw_tab_off);
    TLOG(bg, "   RAW driver patch applied (32-bit sample index in scanner_run_prescan's conversion loops)\n");
    return 1;
}

/* Start a raw (Max Detail) prescan of exactly the current crop at the current resolution and scale. The crop, the
 * resolution and the scale are taken from USER_PARAM as the scripted settings left them (+0x0c top, +0x10 left,
 * +0x14 height, +0x18 width in mm, +0x22 resolution px/mm, +0x2a scale %); set_max_details_crop recomputes the same
 * area from the centre and the pixel size (checked: B2F_MaxDetParameters equal the crop). Returns Handle_Max_Details'
 * result (1 = queued) and the pixel size. */
/* Pixel shift (0.1.21): a different resolution ACROSS (px/mm, 0 = the same as along). USER_PARAM has separate fields
 * for along (+0x22 resolution, +0x2a scale) and across (+0x26, +0x2e); the front end's GetScanParams copies the along
 * values into both (0x0881a4..0x088204), the driver uses them separately: along step = 1 / (+0x22 x +0x2a / 100)
 * (0x04b320..0x04b340, 0x0450c4..0x0450e4), across = optical / (+0x26 x +0x2e / 100) (calculate_prescan_image
 * 0x03e010..0x03e0b4); set_max_details_crop takes the width from +0x26/+0x2e (FE 0x0bec70); valid_crop checks the along
 * pair only (0x0be1a4..0x0be314). Handle_Max_Details saves USER_PARAM on entry and restores it on return (0x010934)
 * and sends it to the driver in between (write_prescan_parameters 0x0438e8), so the across values are written just
 * before the call and set back to the along values right after (the next preview or scan rebuilds them anyway).
 * Found 2026-10-09 (agent research, emulator runs: along 100 px/mm with across 53.1125 -> 500 x 266, INPOL 1.000001). */
volatile double raw_res_w;
int raw_start(Emu *E, int *wpx, int *hpx) {
    uint32_t u = up(E);
    double top = rf(E, u + 0x0c), left = rf(E, u + 0x10), h = rf(E, u + 0x14), w = rf(E, u + 0x18), res = rf(E, u + 0x22), sc = rf(E, u + 0x2a);
    double s = raw_getscale(E), resw = raw_res_w > 0 ? raw_res_w : res;
    if (!(s > 0) || !(res > 0) || !(sc > 0) || !(w > 0) || !(h > 0)) { TLOG(E, "   RAW cannot start: scale %g res %g sc %g crop %g x %g\n", s, res, sc, w, h); return 0; }
    int hp = (int)(h * res * sc / 100 + 0.5), wp = (int)(w * (float)resw * sc / 100 + 0.5);
    if (hp < 1 || wp < 1 || hp > 32000 || wp > 32000) return 0;
    *wpx = wp; *hpx = hp;
    /* size guard (2026-10-08, hardware session s9): a Max Detail of 2618 x 3783 pixels stopped the scanner - it no
       longer answered on SCSI right after the image processor's setup command (0x80) for that block. The original
       never asks for more than 500 x 500 (set_max_det_size). Areas beyond the size proven on the scanner are refused
       here, before anything is sent; the window sets the limits (0 = none: only for its RAW size test). */
    if (raw_lim_w > 0 && raw_lim_h > 0) {
        int in1 = wp <= raw_lim_w && hp <= raw_lim_h, in2 = raw_lim_w2 > 0 && raw_lim_h2 > 0 && wp <= raw_lim_w2 && hp <= raw_lim_h2;
        if (!in1 && !in2) { TLOG(E, "   RAW refused: %d x %d px is beyond the proven size %d x %d (or %d x %d)\n", wp, hp, raw_lim_w, raw_lim_h, raw_lim_w2, raw_lim_h2); return -100; } }
    if (raw_lim_area > 0 && (long long)wp * hp > raw_lim_area) { TLOG(E, "   RAW refused: %d x %d px = %lld pixels is more than %ld (the scanner's memory for one Max Detail)\n", wp, hp, (long long)wp * hp, raw_lim_area); return -100; }
    TLOG(E, "   RAW crop %g x %g mm at %g, %g; %g px/mm along, %g px/mm across, %g %% -> %d x %d px\n", w, h, left, top, res, resw, sc, wp, hp);
    float res_w0 = rf(E, u + 0x26), sc_w0 = rf(E, u + 0x2e);
    if (raw_res_w > 0) { wf(E, u + 0x26, (float)resw); wf(E, u + 0x2e, (float)sc);
        TLOG(E, "   RAW across: USER_PARAM +0x26 %g -> %g px/mm, +0x2e %g -> %g %% (along +0x22 %g, +0x2a %g)\n", (double)res_w0, (double)(float)resw, (double)sc_w0, sc, res, sc); }
    int r = raw_maxdet(E, (left + w / 2) / s, (top + h / 2) / s, hp, wp);
    if (raw_res_w > 0) { wf(E, u + 0x26, (float)res); wf(E, u + 0x2e, (float)sc); TLOG(E, "   RAW across: +0x26/+0x2e set back to %g px/mm, %g %%\n", res, sc); }
    return r;
}

/* ---- experimental HDR: one Max Detail with a longer integration time for the picture data only ----
 * Found (simulator traces with the driver's own log, 2026-10-08): per prescan the driver tunes the light and the CCD
 * integration time (set_prescan_light -> calc_prescan_integration_time -> set_integration_time), calibrates dark and
 * white AT that time (calibrate_dark, calibrate_gain), and only then plans the motion for the data
 * (calculate_prescan_mode 0x04b1d8: get_integration_time -> spiral speed = step x 7444 / time, range check against the
 * scanner's 4.0 .. 1380.0) and takes the data. Setting a longer time at the entry of calculate_prescan_mode therefore
 * keeps the white calibration of the normal time (the data comes out k times brighter, highlights clip) while the
 * motion is planned for the longer time by the driver itself (slower carriage, same geometry). The time is set with
 * the driver's own set_integration_time (r3 = 1, r4 = 7 as the driver calls it) and capped at the scanner's own
 * largest scale value (smart_env +0x26; 25 for Simen's scanner). The next prescan starts its light tuning from 1.3
 * again (seen in every trace), so nothing stays changed. Armed only for one RAW pass by the window. */
volatile double raw_hdr_k; volatile int raw_hdr_armed, raw_hdr_fired, raw_hdr_dark = -9;
/* the arguments of this prescan's own dark calibration (scanner_calib_before_prescan, call at 0x04356c:
   calibrate_dark(r27, r30, r28, r29, r31, r23, 1)), recorded at 0x043568 (li r9,1 - r3..r8 are set) while armed */
static uint32_t dark_args[7]; static int dark_rec;
static int op_is_maxdet(Emu *E) { uint32_t p = rd32(E, E->toc - 0x61b8); return p && rds16(E, p) == 3; }   /* current_OperationMode, 3 = MAXDETAILS_ACTION (Handle_Action 0x0972b4) */
int raw_op_is_maxdet(Emu *E) { return E && op_is_maxdet(E); }   /* for guard.c: the driver is doing a Max Detail now */
static void dark_rec_bp(Emu *E, void *ctx) {
    (void)ctx; if (!raw_hdr_armed || !op_is_maxdet(E)) return;
    for (int i = 0; i < 6; i++) dark_args[i] = E->cpu->gpr[3 + i];
    dark_args[6] = 1; dark_rec = 1;
}
volatile double raw_hdr_t0, raw_hdr_t1, raw_hdr_tmax, raw_last_speed; volatile int raw_last_range = -9;
static void hdr_bp(Emu *E, void *ctx) {
    (void)ctx; if (!raw_hdr_armed) return;
    raw_hdr_armed = 0;
    if (!op_is_maxdet(E)) {          /* armed for a RAW only: any other prescan (a preview) is left alone (review) */
        TLOG(E, "   RAW HDR armed, but this prescan is not a Max Detail (operation %d) - not applied, disarmed\n", rds16(E, rd32(E, E->toc - 0x61b8)));
        return; }
    uint64_t f1save = E->cpu->fpr[1];
    emu_call_own(E, tvec(E, 0x032ac0), NULL, 0); double t0 = E->nest_f1;          /* get_integration_time */
    uint32_t env = rd32(E, rd32(E, E->toc - 0x7208)); double tmax = rf(E, env + 0x26);
    double t1 = t0 * raw_hdr_k; if (tmax > 0 && t1 > tmax) t1 = tmax;
    double t1d = t1; memcpy(&E->cpu->fpr[1], &t1d, 8);
    uint32_t a[2] = { 1, 7 }; emu_call_own(E, tvec(E, 0x032ac8), a, 2);          /* set_integration_time(t1) */
    /* dark calibration again at the new time, as the driver itself does when it changes the time after the white
       calibration (final scans: calc_final_integ_and_factor, then final_scan_calibrate_dark): calibrate_dark takes the
       time from its 5th argument's +0x28 (scan_vcc at 0x0478cc), moves to its dark position and back, keeps the white
       (gain) vectors (ip_save_gain / ip_restore_gain) and loads the new dark vectors. Same arguments as this
       prescan's own dark calibration; +0x28 is put back afterwards. Without it the longer pass is corrected with the
       dark of the normal time (found in review: per-element dark differences would show as streaks). */
    raw_hdr_dark = -9;
    if (dark_rec) {
        uint32_t st = dark_args[4]; float old = rf(E, st + 0x28);
        wf(E, st + 0x28, (float)t1);
        int dr = (int)(int16_t)(emu_call_own(E, tvec(E, 0x047748), dark_args, 7) & 0xFFFF);
        wf(E, st + 0x28, old);
        raw_hdr_dark = dr;
        TLOG(E, "   RAW HDR dark calibration at %g: result %d\n", t1, dr);
    } else TLOG(E, "   RAW HDR: this prescan's dark calibration was not seen - dark not redone\n");
    dark_rec = 0;
    emu_call_own(E, tvec(E, 0x032ac0), NULL, 0); double tr = E->nest_f1;          /* read back */
    E->cpu->fpr[1] = f1save;
    raw_hdr_t0 = t0; raw_hdr_t1 = tr; raw_hdr_tmax = tmax; raw_hdr_fired = 1;
    TLOG(E, "   RAW HDR integration time %g -> %g (x%g requested, scanner maximum %g)\n", t0, tr, raw_hdr_k, tmax);
}
static void speed_bp(Emu *E, void *ctx) {   /* 0x04b388, right after check_speed_spiral_range: r3 = -1 too slow, 0 ok, 1 too fast; f30 = speed */
    (void)ctx; double sp; memcpy(&sp, &E->cpu->fpr[30], 8);
    raw_last_speed = sp; raw_last_range = (int)(int32_t)E->cpu->gpr[3];
    TLOG(E, "   RAW prescan speed %g, range check %d\n", sp, raw_last_range);
}
/* code words checked before the hooks are installed (2.2.0 driver; the first word of a function, mflr r0, alone says
   little - review): 0x04b1d8 mflr r0, 0x04b2c8 bl get_integration_time, 0x04b388 mr r30,r3, 0x043568 li r9,1,
   0x04356c bl calibrate_dark, 0x0478cc lfs f1,0x28(r28) (calibrate_dark takes the time from +0x28) */
static const uint32_t HDR_AT[6] = { 0x04b1d8, 0x04b2c8, 0x04b388, 0x043568, 0x04356c, 0x0478cc };
int raw_hdr_install(Emu *bg) {   /* 1 = installed */
    uint32_t want[6] = { 0x7c0802a6, 0x48000001 | ((0x032ac0 - 0x04b2c8) & 0x03fffffc), 0x7c7e1b78, 0x39200001,
                         0x48000001 | ((0x047748 - 0x04356c) & 0x03fffffc), 0xc03c0028 };
    for (int i = 0; i < 6; i++) if (rd32(bg, CODE + HDR_AT[i]) != want[i]) {
        TLOG(bg, "   RAW HDR hooks NOT installed: unexpected code at %06x (%08x, expected %08x)\n", HDR_AT[i], rd32(bg, CODE + HDR_AT[i]), want[i]);
        return 0; }
    emu_add_bp(bg, 0x04b1d8, hdr_bp, NULL); emu_add_bp(bg, 0x04b388, speed_bp, NULL); emu_add_bp(bg, 0x043568, dark_rec_bp, NULL); return 1;
}

/* ---- which lens the scanner uses (read-only) ----
 * motors_to_start_scan_position (0x068248; r3 = scanner constants, r4 = the scan's parameters, lens index at +0x42)
 * moves the lens carriage (motor 2) to the lens position of that index and the main motor to the start; it runs for
 * every preview, Max Detail and final scan. Lens index -> lens (STR# 400 of the scanner constants, verified against
 * the motor-2 positions on the hardware: 1096 pulses = 6.96 mm = L110 in session s2, 23301 = 147.96 mm = L89 in
 * s3..s9): 0 L67 (4x5), 1 L89 (6x6/6x7), 2 L110 (35 mm), 3 L60 (A3/8x10), 4 the line-work lens. The optical sampling
 * across the bed is magnification / CCD cell pitch: magnification = float smart_env +0x32 + lens x 0x34 (what
 * lens_magnification 0x06b044 returns without the L60 defocus option), pitch = float smart_env +0x0c (0.007 mm). */
volatile int lens_last = -1, lens_seq; volatile double lens_mag, lens_pitch;
static void lens_bp(Emu *E, void *ctx) {
    (void)ctx; int l = rds16(E, E->cpu->gpr[4] + 0x42);
    uint32_t env = rd32(E, rd32(E, E->toc - 0x7208));
    double m = (l >= 0 && l <= 4) ? rf(E, env + 0x32 + l * 0x34) : 0, p = rf(E, env + 0x0c);
    lens_last = l; lens_mag = m; lens_pitch = p; lens_seq++;
    TLOG(E, "   LENS motors_to_start_scan_position: lens %d, magnification %g, CCD pitch %g mm -> %.1f px/mm\n", l, m, p, p > 0 ? m / p : 0.0);
}
int lens_install(Emu *bg) {
    if (rd32(bg, CODE + 0x068248) != 0x7c0802a6 || rd32(bg, CODE + 0x06827c) != 0xa89f0042 || rd32(bg, CODE + 0x068270) != 0x7c9f2378) {
        TLOG(bg, "   LENS hook NOT installed: unexpected code at 0x068248\n"); return 0; }
    emu_add_bp(bg, 0x068248, lens_bp, NULL); return 1;
}

/* ---- focus offset (experimental) ----
 * Motor 1 is the FOCUS motor (STR# 1202,21 "motor parameters - FOCUS VER<5", 80 pulses/mm; the butting mirror is motor
 * 3, "MIRROR"). The driver sets it to the scanner's "L60 focus position" (scanner constants +0x1d6, 10.19766 mm on
 * Simen's scanner) at start-up and at every gain position (start_motor_gain_position 0x068174..0x06817c), for every
 * lens; motors_to_start_scan_position moves it again only for lens 3 (L60), to get_focus_position (+ the original's
 * own "defocus" offset, 0 .. +0.5 mm). Here, at 0x0683a8 (li r3,2 before the last wait_motor_completion(2); both the
 * lens-3 path and the others pass it; r30 = scanner constants), the focus motor is moved by focus_off from where the
 * driver put it, with the driver's own move_motor(1, pos) and wait_motor_completion(1). Limited to nominal -0.5 ..
 * +0.5 mm: below the nominal value the motor passes on every start-up (from home), above it the original's own
 * defocus range. The next gain position puts it back to the nominal value (the driver does that itself). */
volatile double focus_off_l[5], focus_last_pos, focus_last_nom; volatile int focus_seq;   /* offset per lens index 0..4 */
/* ---- lens shift (experimental, research): motor 2 (LENS motion, STR# 1202,22: 157.48 pulses/mm) carries the lenses;
 * the driver puts it at the lens's LPOS (scanner constants, 147.96 mm for L89 ...) for the gain calibration and again
 * here for the scan. lens_shift_pulses moves it that many motor steps further for the scan only (after the white
 * calibration), with the driver's own move_motor(2, pos) / wait_motor_completion(2); limited to +-32 steps (0.2 mm;
 * the carriage normally travels 6.96 .. 208.14 mm). Purpose: find out whether this motion shifts the image ACROSS (along
 * the CCD) or along the scan - the precondition for pixel-shift sampling. Steps are converted with the driver's own
 * factor (motors + 2 x 0x56 + 0x18, the one fill_and_send_move_motor_cmd uses). */
volatile int lens_shift_pulses; volatile double lens_shift_last_mm, lens_shift_factor;
/* approach from above (0.1.23). Hardware: the steps are uneven (s11: the first step 0.44 px, later ones ~1.15; s18/s20
 * L110: +1 = 0.67 .. 0.99 px) and a return to 0 was 0.46 px off. The driver's own backlash correction (move_motor
 * 0x03926c -> motor_need_backlash_correction 0x063ad0, verified in the code and in the traces s11/s18/s19/s20): for
 * motor 2 a move DOWN goes first to the target + the motor's correction (motors + 2 x 0x56 + 0x52; on the scanner
 * -1 mm: 1096 -> 939, 23301 -> 23143) and wait_motor_completion then moves UP to the target. So the driver always
 * reaches its lens position with a move up (from 1 mm below when the carriage was above it, else straight up, e.g.
 * from 8346; review 0.1.23), and a plain +k (as before) is a short move up of k steps after it - the play is already
 * taken up the same way; what differs between passes is the length of the last move.
 * With lens_shift_approach = B > 0 (pixel shift and the lens shift test set it), EVERY pass - step 0 included - goes
 * first to k + B and then down to k: the driver then makes its own correction, so every pass ends with the same 157-step
 * move up into its position, exactly as the driver reaches the lens position itself. Lowest point: LPOS + k - 157,
 * never below the driver's own 939 / 23143 (k >= 0); highest: LPOS + 32 (k + B is kept <= 32). */
volatile int lens_shift_approach;
/* slow lens moves (0.1.26). The driver sends every move with the motor's opcode from its motor table (motors + m x 0x56
 * + 0x50; fill_and_send_move_motor_cmd 0x039f60), set by read_motors_constants (0x062368 / 0x062380) from the letter
 * in the scanner's own motor parameters (STR# 1202: 'F'/'f' -> 0x300f, else 0x300e); the lens carriage has 'F'. Its own
 * move_motor_slow (0x0391a0) sets the field to 0x300e, moves and restores it. On the motion board (IDXR 1202 "Main
 * 11.11", the axis program; read 2026-10-10): 0e = the whole move at one constant step rate (timer period = the motor's
 * parameter 2x02, 2200 counts), 0f = the same rate for moves of up to 15 steps, longer ones ramped up to period 165
 * (about 13 x faster; ~4800 steps/s seen on the hardware). The lens carriage has slipped by about one step on its
 * 157 - 165-step moves (s21 / s25 / s27 / s28). With lens_shift_slow the field is 0x300e for our moves AND their
 * wait_motor_completion (the driver's correction - the last 157 steps up - goes through the same field), then
 * restored. Commands: 320e instead of 320f, same targets. ~0.4 s per 165-step move instead of ~0.2 s. */
volatile int lens_shift_slow, lens_shift_slow_used;
static void lens_move(Emu *E, double pos, int *r1, int *r2) {   /* the driver's own calls; both return 1 on the scanner (s11) */
    double d = pos; memcpy(&E->cpu->fpr[1], &d, 8);
    uint32_t a2[1] = { 2 };
    *r1 = (int)(int16_t)(emu_call_own(E, tvec(E, 0x03926c), a2, 1) & 0xFFFF);      /* move_motor(2, pos) */
    *r2 = (int)(int16_t)(emu_call_own(E, tvec(E, 0x0396b4), a2, 1) & 0xFFFF);      /* wait_motor_completion(2) */
}
static void lens_shift_do(Emu *E, int lens) {
    int k = lens_shift_pulses, B = lens_shift_approach;
    if (k == 0 && B <= 0) return;
    if (k > 32) k = 32; if (k < -32) k = -32;
    if (B > 16) B = 16; if (B < 0) B = 0;
    uint32_t motors = rd32(E, E->toc - 0x7120);
    double f = rf(E, motors + 2 * 0x56 + 0x18), cur = rf(E, motors + 2 * 0x56 + 0x30);
    if (!(f > 50 && f < 1000) || !(cur > 0 && cur < 260)) { TLOG(E, "   LENSSHIFT: unexpected motor-2 factor %g / position %g - not moved\n", f, cur); return; }
    double pos = cur + k / f; int b1 = 0, b2 = 0, r1 = 0, r2 = 0;
    if (B > 0 && k < 0) { TLOG(E, "   LENSSHIFT motor 2: %+d steps is below the lens position - approach from above not used\n", k); B = 0; }
    if (B > 0 && k + B > 32) B = 32 - k;                /* the top stays within +32 steps */
    if (B > 0) {   /* the driver's correction must be active, as move_motor tests it (byte TOC-0x5248, get_motor_corr_flag =
                      halfword TOC-0x4d2c, correction motors + 2 x 0x56 + 0x52 = -1 mm on the scanner): else straight (review) */
        int en = rd8(E, E->toc - 0x5248), cf = rds16(E, E->toc - 0x4d2c); double corr = rf(E, motors + 2 * 0x56 + 0x52);
        if (!en || !cf || !(corr <= -0.5 && corr >= -2.0)) {
            TLOG(E, "   LENSSHIFT motor 2: the driver's backlash correction is not active (flags %d %d, correction %g mm) - approach from above not used\n", en, cf, corr);
            B = 0; }
    }
    uint32_t opa = motors + 2 * 0x56 + 0x50; int slow_op = -1;
    lens_shift_slow_used = 0;
    if (lens_shift_slow) {
        int op = rd16(E, opa);
        if (op == 0x300f) { wr16(E, opa, 0x300e); slow_op = op; lens_shift_slow_used = 1; }
        else TLOG(E, "   LENSSHIFT motor 2: move opcode %04x in the motor table (expected 300f) - slow moves not used\n", op);
    }
    if (B > 0) {
        lens_move(E, cur + (k + B) / f, &b1, &b2);     /* up B steps beyond the target ... */
        TLOG(E, "   LENSSHIFT motor 2: approach from above - first to %+d steps (%g mm, move %d, wait %d)\n", k + B, cur + (k + B) / f, b1, b2);
    }
    lens_move(E, pos, &r1, &r2);                        /* ... then down to it: the driver goes 1 mm below and comes up */
    if (slow_op >= 0) wr16(E, opa, (uint32_t)slow_op);  /* the driver's own value back (as move_motor_slow does) */
    lens_shift_last_mm = pos - cur; lens_shift_factor = f;
    TLOG(E, "   LENSSHIFT motor 2: driver %g mm, %+d steps (%g steps/mm) -> %g mm (move %d, wait %d)%s%s, lens %d\n", cur, k, f, pos, r1, r2,
         B > 0 ? ", from above (the driver's own correction: the last move is up from 1 mm below)" : (lens_shift_approach > 0 ? ", straight (approach from above not possible here, see above)" : ""),
         slow_op >= 0 ? ", SLOW moves (320e: constant rate, no ramp)" : "", lens);
}
static void focus_bp(Emu *E, void *ctx) {
    (void)ctx; int lens = rds16(E, E->cpu->gpr[31] + 0x42);       /* r31 = the scan's parameters (mr r31,r4 at 0x068270) */
    if (lens < 0 || lens > 4) return;
    lens_shift_do(E, lens);
    double focus_off = focus_off_l[lens]; if (focus_off == 0) return;
    uint32_t consts = E->cpu->gpr[30], motors = rd32(E, E->toc - 0x7120);
    double nom = rf(E, consts + 0x1d6), cur = rf(E, motors + 1 * 0x56 + 0x30);
    double pos = cur + focus_off;
    if (!(nom > 0 && nom < 100)) { TLOG(E, "   FOCUS: the stored focus position %g looks wrong - not moved\n", nom); return; }
    if (!(pos >= nom - 0.5)) pos = nom - 0.5; if (!(pos <= nom + 0.5)) pos = nom + 0.5;   /* NaN-safe (review) */
    double d = pos; memcpy(&E->cpu->fpr[1], &d, 8);
    uint32_t a1[1] = { 1 };
    int r1 = (int)(int16_t)(emu_call_own(E, tvec(E, 0x03926c), a1, 1) & 0xFFFF);      /* move_motor(1, pos) */
    int r2 = (int)(int16_t)(emu_call_own(E, tvec(E, 0x0396b4), a1, 1) & 0xFFFF);      /* wait_motor_completion(1) */
    focus_last_pos = pos; focus_last_nom = nom; focus_seq++;
    TLOG(E, "   FOCUS motor 1: nominal %g, driver %g, offset %+g -> %g mm (move %d, wait %d), lens %d\n", nom, cur, focus_off, pos, r1, r2, lens);
}
int focus_install(Emu *bg) {
    if (rd32(bg, CODE + 0x0683a8) != 0x38600002 || rd32(bg, CODE + 0x06826c) != 0x7c7e1b78 || rd32(bg, CODE + 0x06834c) != 0xc03e01d6 ||
        rd32(bg, CODE + 0x068178) != 0xc03f01d6) { TLOG(bg, "   FOCUS hook NOT installed: unexpected code\n"); return 0; }
    emu_add_bp(bg, 0x0683a8, focus_bp, NULL); return 1;
}

/* ---- the scanner's colour alignment off for a RAW (research, 0.1.22; docs/inpol-research/REPORT.md) ----
 * calculate_processing_image (0x03e2f4) sets the processing flag +0x77 = IsInpolFileExist(lens) && get_use_misreg_info()
 * (byte TOC-0x5210 = 1, never changed by the driver). +0x77 = 1: calc_inpol_luts - R and B are interpolated onto G
 * with the lens table STR# 401 + lens (cubic kernel at 1/8-pixel phases); +0x77 = 0: calc_inpol_default_luts - R = G =
 * B vectors, no dy, and the width step snaps to 1.0 (0x03d8b0..0x03d90c) - the CCD samples themselves. Here the result
 * of get_use_misreg_info is taken as 0 at 0x03e33c (clrlwi r3,r3,24 right after the call), for a Max Detail only
 * (current_OperationMode 3), register only - no driver memory is written. Verified in the emulator against the s19
 * recording: only image-processor memory writes change (the 0x90000 / 0x98000 vectors, two DSP words); the motion
 * and the 0x80 block are the same; the interlock holds back the same commands. bip_load_lut_x (0x051518, r3/r4/r5 =
 * R/G/B vectors, r6 = n) is watched read-only to confirm what was sent. */
volatile int raw_noalign, raw_noalign_fired, raw_lutx_seq, raw_lutx_equal, raw_lutx_n, raw_lutx_g8k;
static void noalign_bp(Emu *E, void *ctx) {
    (void)ctx; if (!raw_noalign || !op_is_maxdet(E)) return;
    TLOG(E, "   ALIGN: the scanner's colour alignment OFF for this Max Detail (get_use_misreg_info %d -> 0)\n", (int)(E->cpu->gpr[3] & 0xff));
    E->cpu->gpr[3] = 0; raw_noalign_fired++;
}
static void lutx_bp(Emu *E, void *ctx) {
    (void)ctx; if (!op_is_maxdet(E)) return;
    int n = (int)(int16_t)E->cpu->gpr[6]; uint32_t r = E->cpu->gpr[3], g = E->cpu->gpr[4], b = E->cpu->gpr[5];
    int eq = n > 0, d8 = 0, g8k = n > 0;
    for (int k = 0; k < n && k < 20000; k++) { int vr = rds16(E, r + 2 * k), vg = rds16(E, g + 2 * k), vb = rds16(E, b + 2 * k); if (vr != vg || vb != vg) eq = 0; if (k && vg - rds16(E, g + 2 * k - 2) == 8) d8++; if (vg != 8 * k) g8k = 0; }
    raw_lutx_equal = eq; raw_lutx_n = n; raw_lutx_g8k = g8k; raw_lutx_seq++;
    TLOG(E, "   ALIGN bip_load_lut_x: %d columns, R = G = B %s, G in whole-pixel steps (8) %d of %d, G = 8k (the CCD's own samples, phase 0) %s\n", n, eq ? "yes" : "no", d8, n > 0 ? n - 1 : 0, g8k ? "yes" : "no");
}
int noalign_install(Emu *bg) {
    static const uint32_t at[6] = { 0x03e318, 0x03e334, 0x03e33c, 0x03e4c8, 0x03b7e8, 0x03d8b0 };
    static const uint32_t want[6] = { 0x4801c40d, 0x4bfff0f9, 0x5463063e, 0x9bbf0077, 0x889e0077, 0x897e0077 };
    for (int i = 0; i < 6; i++) if (rd32(bg, CODE + at[i]) != want[i]) {
        TLOG(bg, "   ALIGN hook NOT installed: unexpected code at %06x (%08x, expected %08x)\n", at[i], rd32(bg, CODE + at[i]), want[i]); return 0; }
    emu_add_bp(bg, 0x03e33c, noalign_bp, NULL);
    if (rd32(bg, CODE + 0x051518) == 0x7c0802a6) emu_add_bp(bg, 0x051518, lutx_bp, NULL);
    else TLOG(bg, "   ALIGN: bip_load_lut_x watch not installed (unexpected code %08x)\n", rd32(bg, CODE + 0x051518));
    return 1;
}

/* ---- step-and-hold for a RAW (0.1.28; docs/re-progress.md "Along motion blur: the driver's STEP mode") ----
 * The CCD integrates for the whole line period (integration = pixel clock x ~5290 cells; no shutter), and in the
 * driver's SPIRAL mode the carriage moves continuously, one row per CCD line - every line is smeared over one row
 * along (measured: s32 T3, along 2662 / 5324 / 8214 dpi fit a one-row box). The driver's other mode, STEP (motor
 * struct +0x2c = 6; motor_scan 0x039434 sends opcode 1012 instead of 1016), makes one move per line sync and keeps the
 * carriage still in between; the driver already uses it for every dark and gain capture (seen on the hardware, s32: 36
 * x 1012 with 256 rows). calculate_prescan_mode (0x04b1d8, the mode of previews and Max Details) asks
 * use_spiral_prescan (STR# 2207 index 4 "mode for prescan: 0 - step only, 1 - enable spiral", = 1); here its result is
 * taken as 0 at 0x04b254 (clrlwi r3,r3,24 right after the call) for a Max Detail only (current_OperationMode 3),
 * register only - no driver memory and nothing on the scanner is written. The driver then logs "SPIRAL FLAG IS
 * CLOSED FOR PRESCAN" and keeps mode 6 (set at 0x04b244). motor_scan is watched read-only at 0x039600 (r5 = mode
 * just loaded, r23 = rows, r28 = expose pulses) to report what was really sent.
 * Review (0.1.28): in STEP mode scanner_calc_motors (0x0450e8..0x0451ec) holds the carriage still only when the row pitch
 * is not larger than the CCD pixel's footprint on the film (along dpi >= the lens's CCD dpi); for coarser rows the
 * excess is sent as "expose" pulses and crept during the line (still less smear than SPIRAL's whole row). The BIP
 * prescan block's word 32 becomes 0xFFFF in STEP (the line time in ms in SPIRAL) - the driver's own step-prescan path. */
volatile int raw_stephold, raw_stephold_fired, raw_motor_seq, raw_motor_mode, raw_motor_rows, raw_motor_expose;
static void stephold_bp(Emu *E, void *ctx) {
    (void)ctx; if (!raw_stephold || !op_is_maxdet(E)) return;
    TLOG(E, "   STEPHOLD: step-and-hold for this Max Detail (use_spiral_prescan %d -> 0)\n", (int)(E->cpu->gpr[3] & 0xff));
    E->cpu->gpr[3] = 0; raw_stephold_fired++;
}
static void motor_mode_bp(Emu *E, void *ctx) {   /* only the picture's motion: motor_scan called from prescan at 0x042e00 (review:
                                                     the light, dark and gain captures of a Max Detail go through motor_scan too, always STEP) */
    (void)ctx; if (!op_is_maxdet(E)) return;
    if (rd32(E, E->cpu->gpr[1] + 0xd8) != CODE + 0x042e04) return;   /* the return address saved by motor_scan (stw r0,8(r1) before stwu r1,-0xd0(r1)) */
    raw_motor_mode = (int)(int16_t)E->cpu->gpr[5]; raw_motor_rows = (int)(uint16_t)E->cpu->gpr[23]; raw_motor_expose = (int)(uint16_t)E->cpu->gpr[28];
    raw_motor_seq++;
    TLOG(E, "   MOTION motor_scan: mode %d (%s), %d rows, expose %d pulses\n", raw_motor_mode, raw_motor_mode == 6 ? "STEP, 1012" : raw_motor_mode == 10 ? "SPIRAL, 1016" : "?", raw_motor_rows, raw_motor_expose);
}
int stephold_install(Emu *bg) {
    static const uint32_t at[9] = { 0x04b240, 0x04b244, 0x04b250, 0x04b254, 0x04b258, 0x0395fc, 0x039600, 0x03944c, 0x039450 };
    static const uint32_t want[9] = { 0x38800006, 0xb09d002c, 0x4bffe081, 0x5463063e, 0x28030000, 0xa8bf002c, 0x2c050006, 0x90010008, 0x9421ff30 };
    if (rd32(bg, CODE + 0x042e00) != (0x48000001u | ((0x039434u - 0x042e00u) & 0x03fffffcu))) { TLOG(bg, "   STEPHOLD hook NOT installed: unexpected code at 042e00 (%08x)\n", rd32(bg, CODE + 0x042e00)); return 0; }
    for (int i = 0; i < 9; i++) if (rd32(bg, CODE + at[i]) != want[i]) {
        TLOG(bg, "   STEPHOLD hook NOT installed: unexpected code at %06x (%08x, expected %08x)\n", at[i], rd32(bg, CODE + at[i]), want[i]); return 0; }
    emu_add_bp(bg, 0x04b254, stephold_bp, NULL);
    emu_add_bp(bg, 0x039600, motor_mode_bp, NULL);
    return 1;
}
