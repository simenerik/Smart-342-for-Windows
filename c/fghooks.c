/* fghooks.c - values the original front end (Smart342(L) 2.2.0) takes from its Setup window controls at scan time.
 *
 * Found by reverse engineering (2026-10-07): the scripting interface stores 'pres' (resolution) in the script record
 * and calls TSetUp::ChangeResolution, which uses it for the crop-validity check; but TSetUp::GetScanParams (0x0880e8),
 * which builds the scan parameters, reads the resolution again from the Setup window's resolution control
 * (TPopUPEditIcon at TSetUp+0x3f6, GetFloatValue -> divided by the unit factor at TOC-0x602c -> appl+0x22/0x26) and the
 * scale from text field '0014' (-> appl+0x2a/0x2e). Headless, those controls keep their defaults (12 px/mm, 100 %),
 * so every scripted resolution/scale was silently replaced by the defaults.
 *
 * These breakpoints sit right after the two control reads inside GetScanParams and replace the value the control
 * returned (f1) with the one requested - i.e. exactly what a user typing into those fields would have produced.
 * Only GetScanParams is affected; 0 = leave the control's value. */
#include "mac.h"
#include <string.h>

static double want_res_pxmm, want_scale_pct;

static double getf(Emu *E, int n) { double d; memcpy(&d, &E->cpu->fpr[n], 8); return d; }
static void setf(Emu *E, int n, double d) { memcpy(&E->cpu->fpr[n], &d, 8); }

static void res_read(Emu *E, void *ctx) {        /* at 0x0881ec: f1 = resolution control value (in its unit) */
    (void)ctx;
    if (want_res_pxmm <= 0) return;
    uint32_t p = rd32(E, E->toc - 0x602c); uint32_t bits = rd32(E, p); float unit; memcpy(&unit, &bits, 4);
    if (!(unit > 0)) unit = 1;
    double v = want_res_pxmm * unit;
    TLOG(E, "   FGHOOK resolution control %g -> %g (%g px/mm, unit factor %g)\n", getf(E, 1), v, want_res_pxmm, (double)unit);
    setf(E, 1, v);
}
static void scale_read(Emu *E, void *ctx) {      /* at 0x0881a0: f1 = scale text value (%) */
    (void)ctx;
    if (want_scale_pct <= 0) return;
    TLOG(E, "   FGHOOK scale control %g -> %g %%\n", getf(E, 1), want_scale_pct);
    setf(E, 1, want_scale_pct);
}
void fghooks_install(Emu *E) {
    emu_add_bp(E, 0x0881ec, res_read, NULL);
    emu_add_bp(E, 0x0881a0, scale_read, NULL);
}
void fghooks_set(double res_pxmm, double scale_pct) { want_res_pxmm = res_pxmm; want_scale_pct = scale_pct; }

/* Which frame a scripted Preview covers. The scripting handler DoScriptAEPreview (0x037c94) always calls
 * Handle_Preview_Button(1) (0x037ca0 li r3,1); the Mac window's Preview Browser calls Handle_Preview_Button(n) for each
 * frame (TPrBrWindow::ActivatePreviewButton 0x0a726c, item+0xa0 = preview number), and after automatic mask recognition
 * preview_loop (0x010320) calls it for n = 1 .. SC_get_num_of_cassettes(). This breakpoint at the entry of
 * Handle_Preview_Button changes the number only when the call comes from the scripting handler (return address
 * 0x037ca8), i.e. the scripted Preview previews frame n exactly as the Preview Browser would. 0 = leave it (frame 1). */
static int want_frame;
static void prev_entry(Emu *E, void *ctx) {
    (void)ctx;
    if (want_frame <= 0 || E->cpu->lr != CODE + 0x037ca8u) return;
    TLOG(E, "   FGHOOK scripted preview: frame %d (instead of %d)\n", want_frame, (int)E->cpu->gpr[3]);
    E->cpu->gpr[3] = (uint32_t)want_frame;
}
void fghooks_set_frame(int frame) { want_frame = frame; }
void fghooks_install_frames(Emu *E) { emu_add_bp(E, 0x010548, prev_entry, NULL); }
/* number of frames (previews) of the current format: SC_get_num_of_cassettes (0x0615a0), the count preview_loop uses */
int fghooks_num_frames(Emu *E) {
    uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu);
    wr32(E, tv, CODE + 0x0615a0u); wr32(E, tv + 4, E->toc);
    return (int)(int16_t)(emu_call_own(E, tv, NULL, 0) & 0xFFFF);
}

/* Make frame n the current preview, as opening its thumbnail in the Preview Browser does:
 * TBrowserItemView::DisplayOnScreen (0x0ab05c) calls start_preview(n, 1) (0x00dbb4), which saves the current preview's
 * state, restores frame n's crops, size and parameters (switch_between_preview, read_prescan_parameters ...) and sets
 * the cassette preview index. (DisplayOnScreen then also opens the display window; not needed for scanning.) */
void fghooks_start_preview(Emu *E, int frame) {
    uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu);
    wr32(E, tv, CODE + 0x00dbb4u); wr32(E, tv + 4, E->toc);
    uint32_t a[2] = { (uint32_t)frame, 1 };
    emu_call_own(E, tv, a, 2);
    TLOG(E, "   FGHOOK start_preview(%d, 1)\n", frame);
}
