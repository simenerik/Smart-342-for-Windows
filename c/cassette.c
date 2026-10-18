/* cassette.c - the film holder (cassette format), chosen in the original front end the way a user does.
 *
 * Found by reverse engineering (2026-10-09, docs/cassette-research):
 * - The cassette popup ('0006', MENU 6055 in the resources) does NOT keep its 12 static items. At start-up and on
 *   mount/mode changes BuildAllCassettesMenus (0x01486c) builds four menus (1100..1103) from the cassette directory
 *   STR# 1219 (GetCassetteDirParams 0x068650: light source = mount, "used condition" column of CT/LW x 340/340L,
 *   "Hide this format" = 0) and set_cassette_popup_id (0x01459c) copies the one for the current mount and scan type into
 *   the popup (UpdateCassettePopup 0x087940). For CT + transparent on a 340L (STR# 300 "1 340L Exists") that is 15
 *   items, for CT + reflective 5 items (CAS_HOLDERS below). The menu data (TOC-0x3498) holds per item the directory
 *   type (+0x192 + 2 x item, ItemToCasetType 0x01442c) and per type the item (+0x25a + 2 x type, CasetTypeToItem 0x0143ec).
 * - The scripting property 'pcas' stores 1..14 and DoCassettePopItem (0x0855a4) uses that number directly as the
 *   directory type (12, 13, 14 -> 13), so it is one off the 2.2.0 directory: 'auto' = T 35mm, 't35m' = T 6 x 6,
 *   't6x7' = T 4 x 5 ... 'abrd' = Strip 6 x 7, 'st35'/'st67'/'st45' = A4 (a line-work-only format), 't6x8' is
 *   refused ("Wrong name of cassette"); Automatic, Strip 4 x 5, All Board and Framed 8x35mm cannot be reached at all.
 *   The hardware sessions confirm it: 'T 35mm' in 0.1.x sent CATP 2 and the driver logged "Calibration pass for 6x7 cm
 *   formats" (lens 1 = L89, the T 6 x 6 lens).
 * - A user's choice goes through the popup: DoCassettePopItem reads the current item (TPopup vtable +0x3a4),
 *   ItemToCasetType, set_cassette (0x0147d4) ... the same calls as the scripted path. Verified: selecting T 6 x 6 this
 *   way and with 't35m' gives byte-identical SCSI streams and scan files (preview + scan, simulator).
 * - After a preview the cassette (and the mount and mirror) can no longer be changed: TUserParam's prescan_flag (the
 *   short at *(TOC-0x7428); TUserParam keeps the pointer at +8, SetUPprescan_flag 0x08a4e8) is set to 1 by start_preview
 *   (0x00dbec, called when a preview becomes the current picture: Preview Browser DisplayOnScreen, Scan All) and set to 0
 *   only at start-up (TSetUp::LoadPreferences 0x083630) and in HandleAbort (0x0114a0). While it is 1, TSetUp::DoEvent
 *   ignores the mount and mirror popups (0x086af4, 0x086b88) and DoCassettePopItem returns at once (0x0855d4) - for the
 *   scripting interface too, so 'pcas' after a preview was silently ignored in 0.1.x as well. The Mac user clears it with
 *   the Scan palette's Restart button: TSPButtonActionCommand::DoIt (0x0b2a6c), action 6 (jump table data+0x20ef0 ->
 *   0x0b2db0): VerifyScannerRestart (0x012c8c: if crops were not scanned it asks STR# 6007 "WARNING : Not all the crops
 *   were scanned. Due to this operation all the data relevant to your crops will be lost." Cancel/OK), restart_cassettes_menus,
 *   set_cassette(current), HandleAbort(3) ("PROCESS RESTART": temporary/preview files deleted, prescan_flag = 0, 'RSTR' to
 *   the driver, which stops the motors and re-initialises; the interlock holds back its NV status write as at start-up).
 *   cassette_select does exactly that when the holder really changes after a preview (verified in the simulator: the
 *   next preview sends the new CATP and uses the new lens).
 */
#include "cassette.h"
#include <string.h>
#include <stdio.h>

const CasHolder CAS_HOLDERS[] = {
    /* CT, transparent: the 15 items of the original's menu, in its order */
    { 0, 1, "Automatic" }, { 1, 1, "T 35mm" }, { 2, 1, "T 6 x 6" }, { 3, 1, "T 6 x 7" }, { 4, 1, "T 4 x 5" },
    { 5, 1, "T 8 x 10 H" }, { 6, 1, "T 8 x 10 V" }, { 7, 1, "4 T35mm H" }, { 8, 1, "4 T35mm V" }, { 9, 1, "3 T6x7 H" },
    { 10, 1, "Strip 35mm" }, { 11, 1, "Strip 6 x 7" }, { 12, 1, "Strip 4 x 5" }, { 16, 1, "All Board" }, { 26, 1, "Framed 8x35mm" },
    /* CT, reflective: the 5 items of the original's menu */
    { 19, 0, "Automatic" }, { 21, 0, "150mm" }, { 23, 0, "A4 H" }, { 24, 0, "A4 V" }, { 25, 0, "All Board-A3" },
};
const int CAS_NHOLDERS = (int)(sizeof CAS_HOLDERS / sizeof *CAS_HOLDERS);
const CasHolder *cas_holder(int type) { for (int i = 0; i < CAS_NHOLDERS; i++) if (CAS_HOLDERS[i].type == type) return &CAS_HOLDERS[i]; return NULL; }

/* what each 0.1.x code really selected (scripting value v = directory type v; 't6x8' was refused, the front end kept
   its start-up cassette, type 1) - so old settings files and recorded sessions keep exactly their behaviour */
int cas_legacy_type(const char *c) {
    static const struct { const char *code; int type; } L[] = {
        { "auto", 1 }, { "t35m", 2 }, { "t6x6", 3 }, { "t6x8", 1 }, { "t4x5", 5 }, { "t81h", 6 }, { "t81v", 7 },
        { "4t3h", 8 }, { "4t3v", 9 }, { "3t6h", 10 }, { "abrd", 11 }, { "st35", 13 } };
    for (int i = 0; i < (int)(sizeof L / sizeof *L); i++) if (!strcmp(L[i].code, c)) return L[i].type;
    return -1;
}

int cassette_code_ok(Emu *E) {
    static const uint32_t at[] = { 0x0855a4, 0x0855d4, 0x0855e4, 0x085638, 0x08564c, 0x01442c, 0x0143ec, 0x03a79c, 0x1c59fc };
    static const uint32_t want[] = { 0x7c0802a6, 0x80628bd8, 0x4bfb51b9, 0x818c03a4, 0x4bf8ede1, 0x8082cb68, 0x8082cb68, 0x8862d128, 0x7c0802a6 };
    for (int i = 0; i < (int)(sizeof at / sizeof *at); i++) if (rd32(E, CODE + at[i]) != want[i]) return 0;
    return 1;
}
int cassette_current(Emu *E) { uint32_t up = rd32(E, E->toc - 0x764c); return up ? rds16(E, up) : -1; }
int cassette_prescan_flag(Emu *E) { uint32_t p = rd32(E, E->toc - 0x7428); return p ? rds16(E, p) : -1; }

static uint32_t tvec(Emu *E, uint32_t off) { uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu); wr32(E, tv, CODE + off); wr32(E, tv + 4, E->toc); return tv; }

/* The Scan palette's Restart, as the button runs it: TSPButtonActionCommand::DoIt reads nothing from its object but the
   action number (32 bits at +0x3a, 0x0b2aac); a scratch object with 6 there is enough. Returns 1 when the front end's
   prescan_flag is 0 afterwards (0: the user answered Cancel to the original's warning, or the restart did not happen). */
static int original_restart(Emu *E) {   /* 1 restarted, 0 not (Cancel), -1 unexpected code */
    static const uint32_t at[] = { 0x0b2a6c, 0x0b2aac, 0x0b2ab0, 0x0b2db0, 0x0b2e6c, 0x08a52c };
    static const uint32_t want[] = { 0x7c0802a6, 0x8063003a, 0x28030007, 0x4bf5fedd, 0x4bf5e531, 0xb3c60000 };
    for (int i = 0; i < (int)(sizeof at / sizeof *at); i++) if (rd32(E, CODE + at[i]) != want[i]) return -1;
    uint32_t jt = rd32(E, E->toc - 0x5c24); if (!jt || rd32(E, jt + 6 * 4) != CODE + 0x0b2db0u) return -1;
    uint32_t obj = (E->cpu->gpr[1] - 0x400) & ~0xFu;
    for (int i = 0; i < 0x40; i += 4) wr32(E, obj + i, 0);
    wr32(E, obj + 0x3a, 6);
    uint32_t a1[1] = { obj }; emu_call_own(E, tvec(E, 0x0b2a6c), a1, 1);
    return cassette_prescan_flag(E) == 0;
}

int cassette_select(Emu *E, int type, char *msg, int nmsg) {
    const CasHolder *h = cas_holder(type); const char *want = h ? h->name : "?";
    if (!cassette_code_ok(E)) { snprintf(msg, nmsg, "Holder %s: the front end's code is not the expected 2.2.0 code - not changed.", want); return -1; }
    /* already the front end's cassette: nothing to choose (also while a Max Detail keeps the setup busy - the next RAW
       pass re-sends the holder; found in the 0.1.22 Wine test: pass 2 of a pixel shift was refused) */
    if (h && type >= 0 && cassette_current(E) == type) { snprintf(msg, nmsg, "Holder set: %s (already chosen).", want); return 1; }
    if (rd8(E, E->toc - 0x2ed8)) { snprintf(msg, nmsg, "Holder %s: the front end is handling a script command - not changed.", want); return -1; }
    int restarted = 0;
    if (cassette_prescan_flag(E) != 0) {     /* a preview exists: the original takes a new holder only after its Restart */
        int r = original_restart(E);
        if (r < 0) { snprintf(msg, nmsg, "Holder %s: the front end's Restart code is not the expected 2.2.0 code - not changed.", want); return -1; }
        if (r == 0) { snprintf(msg, nmsg, "Holder %s: the scanner software was not restarted (Cancel?) - the holder of the previews stays.", want); return -1; }
        restarted = 1;
    }
    uint32_t md = rd32(E, E->toc - 0x3498), g = rd32(E, E->toc - 0x73e8), setup = g ? rd32(E, g) : 0;
    if (!md || !setup) { snprintf(msg, nmsg, "Holder %s: the front end's cassette menu is not ready - not changed.", want); return -1; }
    int n = rds16(E, md), item = 0;
    for (int k = 1; k <= n && k < 100; k++) if (rds16(E, md + 0x192 + 2 * k) == type) { item = k; break; }
    if (!item) { snprintf(msg, nmsg, "Holder %s is not offered by the scanner software for this mount/mode - not changed.", want); return 0; }
    char dname[48]; { uint32_t e = rd32(E, md + 2 + 4 * item); int i = 0; for (; i < 40; i++) { uint8_t c = rd8(E, e + i); if (!c) break; dname[i] = (char)c; } dname[i] = 0; }
    /* the popup the user sees: TView::FindSubView(setup, '0006') as DoCassettePopItem does; its item must carry that name */
    uint32_t a2[2] = { setup, 0x30303036u };
    uint32_t pop = emu_call_own(E, tvec(E, 0x1c59fc), a2, 2);
    if (!pop) { snprintf(msg, nmsg, "Holder %s: cassette popup not found - not changed.", want); return -1; }
    uint32_t vt = rd32(E, pop), tv_text = rd32(E, vt + 0x3b4), tv_set = rd32(E, vt + 0x3c0);
    if (!tv_text || !tv_set) { snprintf(msg, nmsg, "Holder %s: cassette popup not usable - not changed.", want); return -1; }
    uint32_t buf = (E->cpu->gpr[1] - 0x180) & ~3u; wr8(E, buf, 0);
    { uint32_t a3[3] = { pop, (uint32_t)item, buf }; emu_call_own(E, tv_text, a3, 3); }
    int len = rd8(E, buf); char pname[64]; if (len > 63) len = 63; for (int i = 0; i < len; i++) pname[i] = (char)rd8(E, buf + 1 + i); pname[len] = 0;
    if (strcmp(pname, dname)) { snprintf(msg, nmsg, "Holder %s: popup item %d is \"%s\", the menu data says \"%s\" - not changed.", want, item, pname, dname); return -1; }
    { uint32_t a3[3] = { pop, (uint32_t)item, 1 }; emu_call_own(E, tv_set, a3, 3); }    /* TPopup SetCurrentItem (as ActivatePopItem 0x084474) */
    { uint32_t a1[1] = { setup }; emu_call_own(E, tvec(E, 0x0855a4), a1, 1); }          /* TSetUp::DoCassettePopItem: what a click on the popup runs */
    int now = cassette_current(E);
    if (now != type) { snprintf(msg, nmsg, "Holder %s: the front end's cassette is %d after the choice, not %d.", dname, now, type); return -1; }
    snprintf(msg, nmsg, "Holder set: %s (menu item %d, cassette directory format %d)%s.", dname, item, type,
             restarted ? " after the scanner software's Restart - the previews of the previous holder are gone" : "");
    return restarted ? 2 : 1;
}
int cassette_prepare_mount(Emu *E, int transparent, char *msg, int nmsg) {
    uint32_t up = rd32(E, E->toc - 0x764c); int mount = up ? rds16(E, up + 0x50) : 0;     /* USER_PARAM +0x50: 1 transparent, 2 reflective */
    msg[0] = 0;
    if (cassette_prescan_flag(E) == 0 || mount == (transparent ? 1 : 2)) return 1;
    if (!cassette_code_ok(E) || rd8(E, E->toc - 0x2ed8)) { snprintf(msg, nmsg, "Mount: the front end cannot be restarted now - not changed."); return -1; }
    int r = original_restart(E);
    if (r < 0) { snprintf(msg, nmsg, "Mount: the front end's Restart code is not the expected 2.2.0 code - not changed."); return -1; }
    if (r == 0) { snprintf(msg, nmsg, "Mount: the scanner software was not restarted (Cancel?) - the mount of the previews stays."); return -1; }
    snprintf(msg, nmsg, "Mount: the scanner software was restarted for the %s mount (as its Restart button) - the previews are gone.", transparent ? "transparent" : "reflective");
    return 2;
}
