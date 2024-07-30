/* ==========================================================================
 * cathode/tui.h — truecolor terminal presenter + retro CRT chrome.
 *
 * Presents a Framebuffer using the Unicode upper-half-block trick: each
 * character cell shows two vertical pixels (fg=top via 24-bit color,
 * bg=bottom), doubling vertical resolution. Includes raw-mode input,
 * alt-screen, a bezel/HUD chrome, and a diff-based renderer that only
 * emits escape codes for changed cells (huge bandwidth win).
 * ========================================================================== */
#ifndef CATHODE_TUI_H
#define CATHODE_TUI_H

#include "cathode/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Tui Tui;

typedef enum {
    KEY_NONE=0, KEY_QUIT, KEY_SPACE, KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    KEY_NEXT, KEY_PREV, KEY_PLUS, KEY_MINUS, KEY_TAB, KEY_ENTER,
    KEY_1,KEY_2,KEY_3,KEY_4,KEY_5,KEY_6,KEY_7,KEY_8,KEY_9,KEY_0,
    KEY_R, KEY_H, KEY_P, KEY_HELP, KEY_OTHER
} Key;

Tui  *tui_create(void);
void  tui_destroy(Tui *t);
void  tui_query_size(Tui *t, i32 *cols, i32 *rows);
/* Framebuffer height should be 2x the rows you want (half-block). */
void  tui_present(Tui *t, const Framebuffer *fb);
/* Present with each framebuffer cell painted as a zoom x zoom block of terminal
 * cells. Keeps the image full-screen at a fraction of the cell count — the
 * terminal emulator's per-cell cost is what limits frame rate, so a zoom of 2-3
 * is the difference between ~3 fps and a smooth 30-60 on a maximized window. */
void  tui_present_zoom(Tui *t, const Framebuffer *fb, i32 zoom);
/* Top-left cell (0-based) where the image is painted, so a smaller-than-window
 * image can be centred. */
void  tui_set_origin(Tui *t, i32 x, i32 y);
/* Overlay a HUD line at row (0=top). text is UTF-8. */
void  tui_hud(Tui *t, const char *scene_name, f32 fps, i32 frame,
              const char *stats);
/* Draw a centered bordered box of `nlines` UTF-8 text lines over the frame
 * (a help / scene-menu overlay). Call after tui_present; the next present with
 * a forced redraw clears it. */
void  tui_overlay(Tui *t, const char *title, const char **lines, i32 nlines);
Key   tui_poll(Tui *t);      /* non-blocking; KEY_NONE if nothing pending */
void  tui_force_redraw(Tui *t);
/* Wipe the entire terminal (ESC[2J) and force a full redraw next present.
 * Call on a window resize so no stale cells linger outside the new frame. */
void  tui_clear(Tui *t);
f64   tui_now(void);         /* monotonic seconds */
void  tui_sleep(f64 seconds);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_TUI_H */
