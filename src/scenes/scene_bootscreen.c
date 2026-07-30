/* ==========================================================================
 * scene_bootscreen.c — a retro computer power-on / POST sequence.
 *
 * A loving pastiche of an 8/16-bit machine booting: the ROM banner appears,
 * a RAM test counts up kilobyte by kilobyte, a sequence of POST lines type
 * themselves out, and a block cursor blinks at a "READY." prompt — then the
 * whole thing loops. Everything is drawn with the 5x7 bitmap font straight
 * into the linear-RGB framebuffer, so it flows through the NTSC/CRT chain and
 * gets phosphor glow, scanlines, and chroma fringing like the real thing.
 *
 * Deterministic given the sim time t, so it golden-tests cleanly.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/text.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct { i32 w, h; f32 t; int amber; } BootState;

/* The POST script. Each line appears after `at` seconds and (except the RAM
 * line, which is handled specially) types out character-by-character. */
typedef struct { f32 at; const char *text; } PostLine;

static const PostLine SCRIPT[] = {
    { 0.3f,  "CATHODE SYSTEM ROM V4.8" },
    { 0.9f,  "(C) 2026 CATHODE INDUSTRIES" },
    { 1.6f,  "" },
    { 1.8f,  "CPU .... AARCH64 @ NEON" },
    { 2.4f,  "FPU .... PRESENT" },
    { 3.0f,  "GPU .... NONE (CPU RENDER)" },
    /* 3.6 .. ~6.0: RAM TEST (animated counter), drawn by the renderer */
    { 6.2f,  "RAM .... 65536 KB OK" },
    { 6.9f,  "BUS .... TRUECOLOR TTY" },
    { 7.5f,  "DSP .... NTSC/CRT ONLINE" },
    { 8.2f,  "" },
    { 8.5f,  "BOOT FROM TAPE .... OK" },
    { 9.3f,  "LOADING CATHODE.SYS" },
    { 10.6f, "" },
    { 11.0f, "READY." },
};
enum { NLINES = (int)(sizeof(SCRIPT)/sizeof(SCRIPT[0])) };

#define RAM_LINE_IDX 6      /* index whose slot the animated RAM test occupies */
#define RAM_START    3.6f
#define RAM_END      6.0f
#define RAM_TOTAL_KB 65536
#define LOOP_PERIOD  13.5f  /* seconds before the whole boot restarts */

static void bt_init(Scene *sc, i32 w, i32 h){
    BootState *s=sc->state; s->w=w; s->h=h; s->t=0; s->amber=0;
}
static void bt_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((BootState*)sc->state)->t=t; }

/* how many characters of a string to reveal given elapsed-since-appear time */
static int typed_chars(const char *str, f32 since){
    if (since < 0) return 0;
    int n = (int)strlen(str);
    int shown = (int)(since / 0.045f);   /* ~22 chars/sec */
    return shown > n ? n : shown;
}

static void bt_render(Scene *sc, Framebuffer *fb){
    BootState *s=sc->state;
    const i32 w=fb->w, h=fb->h;
    f32 tt = fmodf(s->t, LOOP_PERIOD);

    /* dark CRT background with a faint vertical gradient */
    for (i32 y=0;y<h;++y){
        f32 g = 0.015f + 0.01f*(1.0f-(f32)y/h);
        f32 *row=&fb->px[(size_t)y*w*3];
        for (i32 x=0;x<w;++x){ row[3*x+0]=g*0.3f; row[3*x+1]=g; row[3*x+2]=g*0.4f; }
    }

    /* text metrics: scale to height, left margin, line pitch */
    i32 scale = h/64; if (scale<1) scale=1; if (scale>3) scale=3;
    i32 margin = 3*scale;
    i32 lh = (FONT_H + 3) * scale;          /* line pitch */
    i32 y0 = margin;
    Color3 fg = s->amber ? col3(1.0f,0.72f,0.20f)     /* amber phosphor */
                         : col3(0.55f,1.0f,0.60f);     /* green phosphor */

    /* draw each scripted line that has appeared, typed-on */
    i32 pen_y = y0;
    i32 last_drawn_y = y0;
    for (int i=0;i<NLINES;++i){
        if (i == RAM_LINE_IDX){
            /* animated RAM test occupies this slot until RAM_END, then the
             * static "RAM .... NNNNN KB OK" line takes over. */
            if (tt < RAM_END){
                if (tt >= RAM_START){
                    f32 frac=(tt-RAM_START)/(RAM_END-RAM_START); if(frac>1)frac=1;
                    int kb=(int)(frac*RAM_TOTAL_KB);
                    char buf[48];
                    snprintf(buf,sizeof(buf),"RAM .... %5d KB", kb);
                    text_draw(fb, margin, pen_y, buf, fg, scale, 1);
                    last_drawn_y = pen_y;
                    pen_y += lh;
                }
                continue;   /* don't draw the static RAM-OK line yet */
            }
        }
        if (tt >= SCRIPT[i].at){
            int nc = typed_chars(SCRIPT[i].text, tt - SCRIPT[i].at);
            if (SCRIPT[i].text[0]=='\0'){ pen_y += lh; continue; }   /* blank line */
            char buf[64]; int m=(int)strlen(SCRIPT[i].text);
            if (nc>m) nc=m; if (nc>63) nc=63;
            memcpy(buf, SCRIPT[i].text, (size_t)nc); buf[nc]='\0';
            /* the READY. line pops in brighter (it's the payoff) */
            Color3 c = fg;
            if (!strcmp(SCRIPT[i].text,"READY.")) c=col3(fg.r*1.3f,fg.g*1.3f,fg.b*1.3f);
            text_draw(fb, margin, pen_y, buf, c, scale, 1);
            last_drawn_y = pen_y;
            pen_y += lh;
        }
    }

    /* blinking block cursor on the line after the last visible text */
    {
        int blink = ((int)(tt*2.0f)) & 1;   /* 2 Hz */
        i32 cur_y = last_drawn_y;
        /* if READY. is showing, put cursor on the following line */
        if (tt >= SCRIPT[NLINES-1].at) cur_y = last_drawn_y + lh;
        if (blink){
            i32 cw=FONT_W*scale, ch=FONT_H*scale;
            /* cursor x: just after READY. if present, else at the margin */
            i32 cx = margin;
            if (tt >= SCRIPT[NLINES-1].at) cx = margin;
            for (i32 yy=0; yy<ch; ++yy)
                for (i32 xx=0; xx<cw; ++xx)
                    fb_set(fb, cx+xx, cur_y+yy, fg);
        }
    }
}

static void bt_key(Scene *sc, int key){
    BootState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->amber=!s->amber;   /* green <-> amber */
}
static CrtConfig bt_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void bt_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_bootscreen_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="bootscreen";
    sc->description="Retro power-on POST sequence: ROM banner, RAM test, blinking cursor";
    sc->state=calloc(1,sizeof(BootState));
    sc->init=bt_init; sc->update=bt_update; sc->render=bt_render;
    sc->on_key=bt_key; sc->destroy=bt_destroy; sc->preferred_crt=bt_crt;
    return sc;
}
