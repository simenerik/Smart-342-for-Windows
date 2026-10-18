/* ui_stub.c - minimal headless UI stand-ins used by the driver-only runner (no Toolbox UI installed).
 * Port of the '# UI' section of scripts/mac_hle.py install(). In the multi-process system both
 * applications get the full Toolbox model (hle_ui.c) instead, which overrides all of these. */
#include "mac.h"
#include <stdlib.h>
#include <string.h>

#define W (E->W)
#define REG(name) static int64_t h_##name(Emu *E)


REG(noop) { (void)E; return NORET; }
REG(zero) { (void)E; return 0; }
REG(one) { (void)E; return 1; }
REG(GetPort) { wr32(E, A(E, 0), W->zone); return NORET; }
REG(SetRect) { uint32_t a = A(E, 0); wr16(E, a, A(E, 2)); wr16(E, a + 2, A(E, 1)); wr16(E, a + 4, A(E, 4)); wr16(E, a + 6, A(E, 3)); return NORET; }
REG(OffsetRect) {
    uint32_t a = A(E, 0); int dh = SA(E, 1), dv = SA(E, 2);
    int t = rds16(E, a), l = rds16(E, a + 2), b = rds16(E, a + 4), r = rds16(E, a + 6);
    wr16(E, a, t + dv); wr16(E, a + 2, l + dh); wr16(E, a + 4, b + dv); wr16(E, a + 6, r + dh); return NORET;
}
REG(InsetRect) {
    uint32_t a = A(E, 0); int dh = SA(E, 1), dv = SA(E, 2);
    int t = rds16(E, a), l = rds16(E, a + 2), b = rds16(E, a + 4), r = rds16(E, a + 6);
    wr16(E, a, t + dv); wr16(E, a + 2, l + dh); wr16(E, a + 4, b - dv); wr16(E, a + 6, r - dh); return NORET;
}
REG(NewRgn) { return new_handle(E, 10, NULL, 0); }
REG(StringWidth) { return 6 * rd8(E, A(E, 0)); }
REG(CharWidth) { (void)E; return 6; }
REG(GetFontInfo) { uint32_t a = A(E, 0); wr16(E, a, 9); wr16(E, a + 2, 3); wr16(E, a + 4, 7); wr16(E, a + 6, 2); return NORET; }
REG(GetDefFontSize) { (void)E; return 12; }
REG(LMGetMBarHeight) { (void)E; return 20; }
REG(FindWindow) { wr32(E, A(E, 1), 0); return 0; }
REG(NewWindow) { return new_ptr(E, 256, 0); }
REG(GetNewDialog) { int rid = SA(E, 0); uint32_t d = new_ptr(E, 512, 0); T(E, "GetNewDialog", "%d -> %08x", rid, d); return d; }
REG(NewDialog) { return new_ptr(E, 512, 0); }
REG(GetDialogItem) {
    if (A(E, 2)) wr16(E, A(E, 2), 4);
    if (A(E, 3)) wr32(E, A(E, 3), new_handle(E, 16, NULL, 0));
    if (A(E, 4)) emu_zero(E, A(E, 4), 8);
    return NORET;
}
static int answer(World *w) {
    if (!w->nui_answers) return 1;
    int a = w->ui_answers[0]; memmove(w->ui_answers, w->ui_answers + 1, (w->nui_answers - 1) * sizeof(int)); w->nui_answers--; return a;
}
REG(ModalDialog) { int a = answer(W); wr16(E, A(E, 1), (uint32_t)a); T(E, "ModalDialog", "-> item %d", a); return NORET; }
static int64_t alert(Emu *E, const char *kind) { int rid = SA(E, 0), a = answer(W); T(E, kind, "id %d -> %d", rid, a); return a; }
REG(Alert) { return alert(E, "Alert"); }
REG(StopAlert) { return alert(E, "StopAlert"); }
REG(NoteAlert) { return alert(E, "NoteAlert"); }
REG(CautionAlert) { return alert(E, "CautionAlert"); }
REG(h64) { return new_handle(E, 64, NULL, 0); }
REG(GetCursor) { return new_handle(E, 68, NULL, 0); }
REG(GetIcon) { return new_handle(E, 128, NULL, 0); }
REG(TENew) { return new_handle(E, 128, NULL, 0); }
/* per-world GDevice */
static struct { World *w; uint32_t gd; } gds[8]; static int ngds;
static uint32_t gdev(Emu *E) {
    for (int i = 0; i < ngds; i++) if (gds[i].w == W) return gds[i].gd;
    uint32_t gd = new_handle(E, 64, NULL, 0), pm = new_handle(E, 64, NULL, 0), ct = new_handle(E, 8 + 256 * 8, NULL, 0);
    wr32(E, rd32(E, gd) + 22, pm); wr32(E, rd32(E, pm) + 42, ct); wr16(E, rd32(E, pm) + 32, 32);
    if (ngds < 8) { gds[ngds].w = W; gds[ngds].gd = gd; ngds++; }
    return gd;
}
REG(gdev) { return gdev(E); }
REG(GetGWorld) { wr32(E, A(E, 0), W->zone); wr32(E, A(E, 1), gdev(E)); return NORET; }
REG(NewGWorld) { wr32(E, A(E, 0), new_ptr(E, 256, 0)); return 0; }
REG(Index2Color) { emu_zero(E, A(E, 1), 6); return NORET; }

/* Registered BEFORE hle_core/hle_files (the reference installs these with setdefault for the noop list;
 * explicit definitions match the core ones where both exist). */
void hle_uistub_register(void) {
    static const char *noops[] = { "InitGraf", "InitFonts", "InitWindows", "InitMenus", "TEInit", "InitDialogs", "InitCursor",
        "SetCursor", "SetPort", "DrawMenuBar", "ShowWindow", "HideWindow", "SelectWindow", "BringToFront",
        "DrawString", "DrawText", "MoveTo", "LineTo", "Line", "TextFont", "TextSize", "TextFace", "PenNormal",
        "PenSize", "PenMode", "PenPat", "ForeColor", "BackColor", "RGBForeColor", "RGBBackColor", "EraseRect",
        "FrameRect", "PaintRect", "InvertRect", "FillRect", "FrameRoundRect", "InvertRoundRect", "InvalRect",
        "ValidRect", "InvalRgn", "ClipRect", "SetClip", "GetClip", "DrawControls", "DrawGrowIcon", "BeginUpdate",
        "EndUpdate", "SetWTitle", "SizeWindow", "MoveWindow", "DisposeWindow", "CloseWindow", "HiliteMenu",
        "CheckItem", "EnableItem", "DisableItem", "SetDialogItem", "SetDialogItemText", "ShowDialogItem",
        "HideDialogItem", "SelectDialogItemText", "DisposeDialog", "CloseDialog", "DrawDialog", "UpdateDialog",
        "ParamText", "SetControlValue", "SetControlMaximum", "SetControlTitle", "setcontroltitle", "HiliteControl",
        "ShowControl", "HideControl", "MoveControl", "SizeControl", "DisposeControl", "KillControls",
        "PlotIcon", "PlotCIcon", "DisposeCIcon", "SetEntries", "TEUpdate", "TESetSelect", "TEActivate",
        "TEDeactivate", "TEIdle", "TECalText", "TEDispose", "TEInsert", "TEDelete", "TEScroll", "TEAutoView",
        "TETextBox", "TEKey", "TEClick", "TECopy", "TECut", "TEPaste", "GlobalToLocal", "LocalToGlobal",
        "SetRect", "OffsetRect", "InsetRect", "GetPenState", "SetPenState", "DisposeRgn", "CopyRgn",
        "OffsetRgn", "DiffRgn", "DrawPicture", "CopyBits", "SetGDevice", "DisposeGDevice", "SetGWorld",
        "UnlockPixels", "CTabChanged", "DisposeCTable", "InsertMenu", "DeleteMenu", "AppendMenu",
        "AppendResMenu", "SetMenuItemText", "GetMenuItemText", "ObscureCursor", "HideCursor", "ShowCursor",
        "FlushVol", "ZeroScrap", "GetBackColor", "GetForeColor", "DisposeGWorld", "GetWTitle",
        "GetDialogItemText", "LMSetSFSaveDisk", "LMSetCurDirStore", "LMSetTEScrpLength", "DragWindow", NULL };
    for (int i = 0; noops[i]; i++) emu_reg(noops[i], h_noop);
#define R(n) emu_reg(#n, h_##n)
    R(GetPort); R(SetRect); R(OffsetRect); R(InsetRect);
    emu_reg("PtInRect", h_zero); emu_reg("SectRect", h_zero); emu_reg("EmptyRgn", h_one); R(NewRgn); emu_reg("BitMapToRegion", h_zero);
    R(StringWidth); R(CharWidth); R(GetFontInfo); emu_reg("GetAppFont", h_one); R(GetDefFontSize); R(LMGetMBarHeight);
    emu_reg("FrontWindow", h_zero); R(FindWindow); R(NewWindow); R(GetNewDialog); R(NewDialog); R(GetDialogItem); R(ModalDialog);
    emu_reg("IsDialogEvent", h_zero); emu_reg("DialogSelect", h_zero);
    R(Alert); R(StopAlert); R(NoteAlert); R(CautionAlert);
    static const char *zeros[] = { "GetControlValue", "GetControlMaximum", "FindControl", "TrackControl", "TrackGoAway", "GrowWindow",
        "GetWRefCon", "MenuSelect", "MenuKey", "PopUpMenuSelect", "CountMItems", "GetNextDevice", "Color2Index", "TEScrapHandle", NULL };
    for (int i = 0; zeros[i]; i++) emu_reg(zeros[i], h_zero);
    emu_reg("NewControl", h_h64); emu_reg("GetMenu", h_h64); emu_reg("NewMenu", h_h64); R(GetCursor); emu_reg("GetCIcon", h_h64); R(GetIcon);
    emu_reg("GetGDevice", h_gdev); emu_reg("GetMainDevice", h_gdev); emu_reg("GetDeviceList", h_gdev); emu_reg("NewGDevice", h_gdev);
    R(GetGWorld); R(NewGWorld); emu_reg("GetGWorldPixMap", h_h64); emu_reg("LockPixels", h_one); R(Index2Color); R(TENew);
#undef R
}
