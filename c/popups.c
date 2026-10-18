/* popups.c - the choices the original front end offers in its Setup window popups (gradation, sharpness, colour and
 * filmtype tables, media), read from the popups themselves.
 *
 * Found by reverse engineering (2026-10-07): TSetUp::DoGradationPopItem / DoSharpPopItem / DoColorPopItem /
 * DoFilmTypePopItem / DoMediaPopItem each start with FindSubView('00xx') for their popup (gradation '0036',
 * sharpness '0039', colour '0042', filmtype '0060', media '0004'). A scripted table name is looked up with
 * Name2IndexInPopup (0x09e018), which walks the popup through two TPopup virtual methods: vtable+0x3b8 = number of
 * items, vtable+0x3b4 = item text (this, index, Str255 *); GetCurrentNameInPopup (0x09e228) uses vtable+0x3a4 = current item. A name that is not in the popup is ignored by the original.
 *
 * A breakpoint right after each FindSubView reads the popup's items the same way (front end only, nothing else is
 * touched) and keeps the list, so the Windows window can offer exactly the names the original accepts. */
#include "mac.h"
#include <string.h>
#include <stdio.h>
#include "popups.h"

typedef struct { uint32_t at; int reg; const char *id, *what; } PopSite;
static const PopSite SITES[POP_N] = {
    { 0x085ef0, 3, "0036", "gradation" },
    { 0x085bc8, 3, "0039", "sharpness" },
    { 0x084728, 3, "0042", "color" },
    { 0x086214, 3, "0060", "filmtype" },
    { 0x085844, 3, "0004", "media" },
    { 0x086624 + 4, 3, "0033", "endpoints" },
};
PopList pop_lists[POP_N];
static int s_id(const char *c) { return (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3]; }
volatile int pop_seq;                 /* incremented whenever a list changed */

static void read_list(Emu *E, int k, uint32_t pop) {
    const PopSite *s = &SITES[k];
    if (!pop) return;
    uint32_t vt = rd32(E, pop); if (!vt) return;
    uint32_t tv_count = rd32(E, vt + 0x3b8), tv_text = rd32(E, vt + 0x3b4), tv_cur = rd32(E, vt + 0x3a4);
    if (!tv_count || !tv_text || !tv_cur) return;
    uint32_t a1[1] = { pop };
    int n = (int)(int16_t)(emu_call_own(E, tv_count, a1, 1) & 0xFFFF);
    if (n < 0) n = 0; if (n > POP_MAX) n = POP_MAX;
    PopList L; memset(&L, 0, sizeof L); L.n = n;
    L.cur = (int)(int16_t)(emu_call_own(E, tv_cur, a1, 1) & 0xFFFF);
    uint32_t buf = (E->cpu->gpr[1] - 0x180) & ~3u;           /* free stack space above the nested call's frame */
    for (int i = 1; i <= n; i++) {
        wr8(E, buf, 0);
        uint32_t a3[3] = { pop, (uint32_t)i, buf };
        emu_call_own(E, tv_text, a3, 3);
        int len = rd8(E, buf); if (len > 63) len = 63;
        char mr[64]; for (int j = 0; j < len; j++) mr[j] = (char)rd8(E, buf + 1 + j);
        macroman_to_utf8(mr, len, L.item[i - 1], sizeof L.item[i - 1]);
    }
    if (memcmp(&L, &pop_lists[k], sizeof L)) {
        pop_lists[k] = L; pop_seq++;
        char line[POP_MAX * 100 + 100]; int k2 = snprintf(line, sizeof line, "%d items, current %d:", n, L.cur);
        for (int i = 0; i < n && k2 < (int)sizeof line - 100; i++) k2 += snprintf(line + k2, sizeof line - k2, " [%s]", L.item[i]);
        TLOG(E, "   POPUP %s %s: %s\n", s->id, s->what, line);
    }
}
static void read_popup(Emu *E, void *ctx) { int k = (int)(intptr_t)ctx; read_list(E, k, E->cpu->gpr[SITES[k].reg]); }

/* Read all lists now, without going through the Do*PopItem handlers: the Setup view is **(TOC-0x73e8) (the global
   SetScanPictureProperty and check_replace_table_name use), and its popups are found with the same TView::FindSubView
   (code 0x1c59fc, same fragment/TOC) the handlers call. Only lookups and reads. Returns the number of lists read. */
int popups_read_all(Emu *E) {
    uint32_t g = rd32(E, E->toc - 0x73e8); if (!g) return 0;
    uint32_t setup = rd32(E, g); if (!setup) return 0;
    uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu);          /* a TVector for FindSubView in free stack space */
    wr32(E, tv, CODE + 0x1c59fcu); wr32(E, tv + 4, E->toc);
    int got = 0;
    for (int k = 0; k < POP_N; k++) {
        uint32_t id = (uint32_t)s_id(SITES[k].id);
        uint32_t a[2] = { setup, id };
        uint32_t pop = emu_call_own(E, tv, a, 2);
        if (pop) { read_list(E, k, pop); got++; }
    }
    return got;
}
/* Film type is not part of the scripting interface. Choosing it the way a user does: the popup's current item is set
   (TPopup vtable+0x3c0 = SetCurrentItem(item, redraw), as TSetUp::ActivatePopItem does at 0x084474) and then
   TSetUp::DoFilmTypePopItem (0x0861d4) runs - what TSetUp::DoEvent calls for this popup (0x086b5c) after the popup
   was used. Returns 1 if the name was found and selected, 0 otherwise (nothing changed). */
int popups_select_filmtype(Emu *E, const char *name_utf8) {
    uint32_t g = rd32(E, E->toc - 0x73e8); if (!g) return 0;
    uint32_t setup = rd32(E, g); if (!setup) return 0;
    uint32_t tv = ((E->cpu->gpr[1] - 0x40) & ~0xFu);
    wr32(E, tv, CODE + 0x1c59fcu); wr32(E, tv + 4, E->toc);
    uint32_t a[2] = { setup, (uint32_t)s_id("0060") };
    uint32_t pop = emu_call_own(E, tv, a, 2); if (!pop) return 0;
    read_list(E, POP_FILM, pop);
    int idx = 0;
    for (int i = 0; i < pop_lists[POP_FILM].n; i++) if (!strcmp(pop_lists[POP_FILM].item[i], name_utf8)) { idx = i + 1; break; }
    if (!idx) return 0;
    uint32_t vt = rd32(E, pop), tv_set = rd32(E, vt + 0x3c0); if (!tv_set) return 0;
    uint32_t a3[3] = { pop, (uint32_t)idx, 1 };
    emu_call_own(E, tv_set, a3, 3);
    wr32(E, tv, CODE + 0x0861d4u); wr32(E, tv + 4, E->toc);
    uint32_t a1[1] = { setup };
    emu_call_own(E, tv, a1, 1);
    read_list(E, POP_FILM, pop);
    TLOG(E, "   FILMTYPE selected %d [%s] -> current %d\n", idx, name_utf8, pop_lists[POP_FILM].cur);
    return pop_lists[POP_FILM].cur == idx;
}
void popups_install(Emu *E) {
    for (int k = 0; k < POP_N; k++) emu_add_bp(E, SITES[k].at, read_popup, (void *)(intptr_t)k);
}
const char *popup_name(int k) { return k >= 0 && k < POP_N ? SITES[k].what : "?"; }
