/*
 * minerbar.h - a pixel-art "miner" progress bar.
 *
 * A drop-in replacement for the Windows progress bar (PROGRESS_CLASS):
 * a little miner digs through rock from left to right; the further the
 * progress, the more rock is gone, and at 100 % he finds a diamond.
 *
 * Use:
 *   MinerBar_Register(hInstance);              once, at start-up
 *   CreateWindowExW(0, MINERBAR_CLASSW, ...);  instead of PROGRESS_CLASSW
 * and keep sending the usual messages (PBM_SETRANGE32, PBM_SETPOS, PBM_STEPIT,
 * PBM_DELTAPOS, PBM_SETMARQUEE, PBM_SETSTATE ...). Return values follow the
 * standard control. The PBS_MARQUEE style works as usual (the miner hacks away
 * at the start of the tunnel). PBS_VERTICAL is not supported (drawn horizontally).
 *
 * Looks best 32 px tall or more (the art is 16 rows; it scales by whole pixels).
 */
#ifndef MINERBAR_H
#define MINERBAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scenes for the portable renderer. */
enum {
    MB_SCENE_REST = 0,    /* at the start, not digging (position == minimum) */
    MB_SCENE_DIG = 1,     /* digging (minimum < position < maximum)          */
    MB_SCENE_MARQUEE = 2, /* PBS_MARQUEE: busy, amount unknown               */
    MB_SCENE_DONE = 3     /* position == maximum: diamond found              */
};
/* Frame tints, like the standard bar's states. */
enum { MB_TINT_NORMAL = 0, MB_TINT_ERROR = 1, MB_TINT_PAUSED = 2 };

/*
 * Portable core (no Windows needed): draws the scene into a top-down
 * 0x00RRGGBB buffer of w x h "art pixels" (row stride in pixels).
 * frac = 0..1 of the way through, tick = animation counter.
 */
void minerbar_render(uint32_t *px, int w, int h, int stride, double frac,
                     int scene, unsigned tick, int tint);

#ifdef _WIN32
#include <windows.h>
#define MINERBAR_CLASSW L"MinerBar32"
#define MINERBAR_CLASSA "MinerBar32"
#ifdef UNICODE
#define MINERBAR_CLASS MINERBAR_CLASSW
#else
#define MINERBAR_CLASS MINERBAR_CLASSA
#endif
/* Registers the window class. Returns FALSE only if registration failed. */
BOOL MinerBar_Register(HINSTANCE hInstance);
#endif

#ifdef __cplusplus
}
#endif
#endif
