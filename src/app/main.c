/* ==========================================================================
 * main.c  -  CATHODE interactive front-end.
 *
 * Pipeline per frame:
 *   scene->render(scene_fb)          // linear RGB, scene-referred
 *   crt_process(scene_fb -> disp_fb) // NTSC/CRT signal emulation
 *   tui_present(disp_fb)             // truecolor half-block to terminal
 *
 * Controls:
 *   space ......... pause/resume
 *   n / p, arrows . next / previous scene
 *   [ / ] ......... cycle CRT preset
 *   +/- ........... scene-specific (speed etc.), forwarded to scene
 *   1..8 .......... jump to scene N
 *   h ............. toggle HUD
 *   r ............. reset current scene
 *   q / ESC ....... quit
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/framebuffer.h"
#include "cathode/crt.h"
#include "cathode/tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_presets[] = { "trinitron","broadcast","vhs","arcade","clean" };
enum { N_PRESETS = 5 };

int main(int argc, char **argv) {
    scenes_register_all();
    i32 nscenes = scene_count();
    if (nscenes == 0) { fprintf(stderr, "no scenes registered\n"); return 1; }

    /* --help / -h: print usage + scene list and exit (don't launch the loop). */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("CATHODE  -  CPU graphics engine with software NTSC/CRT emulation.\n\n");
            printf("usage: cathode [scene] [--quality N]\n");
            printf("  --quality N   terminal cells to paint (default 2000).\n");
            printf("                Higher = bigger picture, lower frame rate;\n");
            printf("                the terminal emulator's per-cell render cost\n");
            printf("                is the limit, not the engine. Try 800-6000.\n");
            printf("  Launches the interactive demo (needs a truecolor terminal).\n");
            printf("  Optionally start on a named scene.\n\n");
            printf("controls: space=pause  n/p or arrows=scene  1-9=jump  tab=CRT preset\n");
            printf("          +/-=scene param  h=HUD  ?=help overlay  r=reset  q=quit\n\n");
            printf("scenes (%d):\n ", nscenes);
            for (i32 s = 0; s < nscenes; ++s) {
                printf(" %s", scene_name_at(s));
                if ((s % 6) == 5) printf("\n ");
            }
            printf("\n\nheadless rendering: use the `capture` tool  - \n");
            printf("  capture <scene> <frames> out.png [w] [h] [preset]\n");
            printf("  capture --all <frames> <dir>        # PNG per scene\n");
            printf("  capture --gif <scene> <frames> out.gif\n");
            printf("  capture --reel out.gif              # self-running demo\n");
            return 0;
        }
    }

    /* Optional: start on a named scene. */
    i32 start = 0;
    for (int i = 1; i < argc; ++i) {
        for (i32 s = 0; s < nscenes; ++s)
            if (strcmp(argv[i], scene_name_at(s)) == 0) start = s;
    }

    Tui *tui = tui_create();

    /* Framebuffer dimensions are derived from the live terminal size. The
     * half-block trick shows two vertical pixels per character row, so the
     * pixel height is 2*(rows-1) (one row reserved for the HUD). We recompute
     * these every frame and rebuild buffers on change  -  this is what makes the
     * demo fill the window and survive a resize instead of drawing a fixed
     * small image with stale cells around it. */
    i32 cols = 80, rows = 24;
    tui_query_size(tui, &cols, &rows);
    /* Cell budget. Every terminal cell we paint costs the emulator a truecolor
     * glyph render, and it must ingest ~25 bytes of escape codes for it. Past a
     * few thousand cells per frame even fast emulators (Terminal.app, iTerm2)
     * fall off a cliff  -  a maximized window is ~11k cells, which is why the
     * demo used to run at 3 fps. We therefore render at most `pixel_step`-
     * reduced resolution and let each framebuffer pixel cover a pixel_step
     * block of terminal cells, keeping the picture full-screen but the cell
     * count sane. `--quality N` overrides the budget. */
    /* Default budget ~2000 cells. Rationale, measured on this machine: a moving
     * full-screen scene costs ~25-30 bytes of escape codes per changed cell, and
     * a terminal emulator realistically parses+renders on the order of 1 MB/s of
     * that. 2000 cells => ~50 KB/frame => ~20-30 fps, which feels smooth. A
     * maximized 213x53 window is 11k cells (~300 KB/frame) and lands at ~3 fps,
     * which is what made the demo look broken. Override with --quality N. */
    i32 cell_budget = 2000;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quality") == 0 && i+1 < argc) {
            cell_budget = atoi(argv[i+1]);
            if (cell_budget < 400)    cell_budget = 400;
            if (cell_budget > 200000) cell_budget = 200000;
        }
    }
    i32 disp_cols = cols, disp_rows = rows - 1;   /* one row reserved for the HUD */
    if (disp_cols < 8) disp_cols = 8;
    if (disp_rows < 4) disp_rows = 4;
    /* Shrink the PAINTED REGION (not just the framebuffer) until it fits the cell
     * budget. The terminal emulator's cost is per painted cell, so this is the
     * only knob that genuinely buys frame rate; the image stays centered and the
     * aspect ratio is preserved (2 framebuffer pixels per cell row). */
    while (disp_cols * disp_rows > cell_budget && (disp_cols > 40 || disp_rows > 12)) {
        if (disp_cols > 40) disp_cols -= 2;
        if (disp_rows > 12 && disp_cols * disp_rows > cell_budget) disp_rows -= 1;
    }
    i32 step = 1;                                  /* 1 fb pixel per terminal cell */
    i32 fb_w = disp_cols;
    i32 fb_h = disp_rows * 2;                      /* 2 pixels per cell row */
    if (fb_w  < 16) fb_w  = 16;
    if (fb_h  < 16) fb_h  = 16;
    /* centre the image in the window */
    i32 pad_x = (cols - disp_cols) / 2;   if (pad_x < 0) pad_x = 0;
    i32 pad_y = ((rows - 1) - disp_rows) / 2; if (pad_y < 0) pad_y = 0;

    Framebuffer *scene_fb = fb_create(fb_w, fb_h);
    Framebuffer *disp_fb  = fb_create(fb_w, fb_h);

    i32 cur = start;
    Scene *scene = scene_create(cur);
    if (scene && scene->init) scene->init(scene, fb_w, fb_h);

    CrtConfig cfg = scene && scene->preferred_crt ? scene->preferred_crt(scene)
                                                   : crt_config_default();
    i32 preset_idx = 0;
    CrtState *crt = crt_create(fb_w, fb_h, &cfg);

    int running = 1, paused = 0, show_hud = 1, show_help = 0;
    f64 t0 = tui_now();
    f64 last = t0;
    f64 sim_time = 0.0;
    i64 frame = 0;
    f32 fps = 0.0f;
    const f64 target_dt = 1.0 / 60.0;
    /* Time spent inside tui_present, smoothed. This is what actually limits the
     * frame rate: a terminal emulator has to parse and re-render every truecolor
     * cell we send, and at a full 200x50 window that is ~10k cells / ~250 KB per
     * frame, which no emulator can do at 60 Hz. We measure it and report it in
     * the HUD so the cost is visible instead of mysterious. */
    f64 present_ms = 0.0;

    while (running) {
        f64 now = tui_now();
        f64 dt = now - last;
        last = now;
        if (dt > 0.1) dt = 0.1; /* clamp huge stalls */

        /* ---- live resize: re-query terminal size; rebuild on change ---- */
        {
            i32 ncols = cols, nrows = rows;
            tui_query_size(tui, &ncols, &nrows);
            if (ncols != cols || nrows != rows) {
                cols = ncols; rows = nrows;
                /* recompute the cell budget / zoom for the new window size */
                disp_cols = cols; disp_rows = rows - 1;
                if (disp_cols < 8) disp_cols = 8;
                if (disp_rows < 4) disp_rows = 4;
                while (disp_cols * disp_rows > cell_budget && (disp_cols > 40 || disp_rows > 12)) {
                    if (disp_cols > 40) disp_cols -= 2;
                    if (disp_rows > 12 && disp_cols * disp_rows > cell_budget) disp_rows -= 1;
                }
                step = 1;
                pad_x = (cols - disp_cols)/2;        if (pad_x < 0) pad_x = 0;
                pad_y = ((rows-1) - disp_rows)/2;    if (pad_y < 0) pad_y = 0;
                i32 nw = disp_cols, nh = disp_rows * 2;
                if (nw < 16) nw = 16;
                if (nh < 16) nh = 16;
                if (nw != fb_w || nh != fb_h) {
                    fb_w = nw; fb_h = nh;
                    fb_destroy(scene_fb); fb_destroy(disp_fb);
                    scene_fb = fb_create(fb_w, fb_h);
                    disp_fb  = fb_create(fb_w, fb_h);
                    crt_destroy(crt);
                    crt = crt_create(fb_w, fb_h, &cfg);
                    /* re-init the scene at the new resolution */
                    if (scene && scene->destroy) scene->destroy(scene);
                    scene = scene_create(cur);
                    if (scene && scene->init) scene->init(scene, fb_w, fb_h);
                    /* scene reset also loses sim time; keep it continuous */
                }
                tui_clear(tui);  /* wipe stale cells outside the new frame */
            }
        }

        /* ---- input ---- */
        Key k;
        while ((k = tui_poll(tui)) != KEY_NONE) {
            switch (k) {
                case KEY_QUIT: running = 0; break;
                case KEY_SPACE: paused = !paused; break;
                case KEY_H: show_hud = !show_hud; break;
                case KEY_HELP: show_help = !show_help; tui_force_redraw(tui); break;
                case KEY_NEXT: case KEY_RIGHT: case KEY_DOWN:
                    cur = (cur + 1) % nscenes; goto switch_scene;
                case KEY_PREV: case KEY_LEFT: case KEY_UP:
                    cur = (cur - 1 + nscenes) % nscenes; goto switch_scene;
                case KEY_1: case KEY_2: case KEY_3: case KEY_4:
                case KEY_5: case KEY_6: case KEY_7: case KEY_8:
                case KEY_9: {
                    i32 idx = (i32)(k - KEY_1);
                    if (idx < nscenes) { cur = idx; goto switch_scene; }
                    break;
                }
                case KEY_TAB: /* cycle CRT preset */
                    preset_idx = (preset_idx + 1) % N_PRESETS;
                    cfg = crt_config_preset(g_presets[preset_idx]);
                    crt_set_config(crt, &cfg);
                    tui_force_redraw(tui);
                    break;
                case KEY_R:
                    if (scene && scene->destroy) scene->destroy(scene);
                    scene = scene_create(cur);
                    if (scene && scene->init) scene->init(scene, fb_w, fb_h);
                    sim_time = 0.0;
                    break;
                default:
                    if (scene && scene->on_key) scene->on_key(scene, (int)k);
                    break;
            }
            continue;
        switch_scene:
            if (scene && scene->destroy) scene->destroy(scene);
            scene = scene_create(cur);
            if (scene && scene->init) scene->init(scene, fb_w, fb_h);
            sim_time = 0.0;
            cfg = scene && scene->preferred_crt ? scene->preferred_crt(scene)
                                                : crt_config_default();
            crt_set_config(crt, &cfg);
            tui_force_redraw(tui);
        }

        /* ---- update ---- */
        if (!paused) sim_time += dt;
        if (scene && scene->update) scene->update(scene, (f32)dt, (f32)sim_time);

        /* ---- render scene -> CRT -> terminal ---- */
        if (scene && scene->render) scene->render(scene, scene_fb);
        crt_process(crt, scene_fb, disp_fb);
        {
            f64 p0 = tui_now();
            tui_set_origin(tui, pad_x, pad_y);
            tui_present_zoom(tui, disp_fb, step);
            f64 pdt = (tui_now() - p0) * 1000.0;
            present_ms = present_ms*0.9 + pdt*0.1;   /* smooth for the HUD */
        }

        if (show_hud) {
            char stats[128];
            snprintf(stats, sizeof(stats), "CRT:%s %dx%d x%d  term:%.1fms  (? help)",
                     g_presets[preset_idx], fb_w, fb_h, (int)step, present_ms);
            tui_hud(tui, scene ? scene->name : "?", fps, (i32)frame, stats);
        }

        if (show_help) {
            /* Build the scene list into overlay lines. The number of COLUMNS is
             * chosen from the terminal height so the whole box always fits  - 
             * with 56+ scenes a fixed 3-column layout overflowed the screen. */
            enum { MAXL = 48, LINEW = 160 };
            static char linebuf[MAXL][LINEW];
            const char *lines[MAXL];
            i32 nl = 0;
            lines[nl++] = "Controls:  space=pause   n/p or arrows=scene   tab=cycle CRT";
            lines[nl++] = "           1-9=jump   +/-=scene param   h=HUD   r=reset   q=quit";
            lines[nl++] = "";
            /* rows available for the list = terminal rows - borders/title/controls */
            i32 avail = rows - 8; if (avail < 4) avail = 4; if (avail > MAXL-4) avail = MAXL-4;
            i32 ncol = (nscenes + avail - 1) / avail; if (ncol < 1) ncol = 1;
            /* don't exceed the terminal width either (~15 cols per entry) */
            i32 maxcol = (cols - 8) / 15; if (maxcol < 1) maxcol = 1;
            if (ncol > maxcol) ncol = maxcol;
            i32 per = (nscenes + ncol - 1) / ncol;      /* entries per column */
            for (i32 r = 0; r < per && nl < MAXL-1; ++r) {
                char *lb = linebuf[nl]; int p = 0;
                for (i32 cc = 0; cc < ncol; ++cc) {
                    i32 idx = cc*per + r;
                    if (idx >= nscenes) break;
                    p += snprintf(lb+p, (size_t)(LINEW-p), " %2d %-11s",
                                  (int)idx, scene_name_at(idx));
                    if (p >= LINEW-1) break;
                }
                lines[nl] = lb; nl++;
            }
            {
                static char title[48];
                snprintf(title, sizeof(title),
                         "CATHODE  -  %d scenes  (press ? to close)", (int)nscenes);
                tui_overlay(tui, title, lines, nl);
            }
        }

        /* ---- fps + pacing ---- */
        frame++;
        if ((frame & 15) == 0) {
            f64 span = now - t0;
            if (span > 0) fps = (f32)(frame / span);
        }
        f64 spent = tui_now() - now;
        if (spent < target_dt) tui_sleep(target_dt - spent);
    }

    if (scene && scene->destroy) scene->destroy(scene);
    crt_destroy(crt);
    fb_destroy(scene_fb);
    fb_destroy(disp_fb);
    tui_destroy(tui);
    printf("cathode: clean exit after %lld frames\n", (long long)frame);
    return 0;
}
