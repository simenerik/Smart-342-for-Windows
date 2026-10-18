/* noui.c - driver-only build: no Toolbox UI model (scripts/run.py equivalent). */
#include "mac.h"
int ui_next_event(World *w, uint32_t mask, int remove, uint32_t *what, uint32_t *msg, uint32_t *mods) {
    (void)w; (void)mask; (void)remove; (void)what; (void)msg; (void)mods; return 0;
}
void world_install_ui(World *w) { (void)w; }
void hle_ui_register(void) {}
