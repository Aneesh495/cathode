/* ==========================================================================
 * scene_credits.c  -  a perspective "star wars" credits crawl.
 *
 * A block of text scrolls upward while receding into the distance: each text
 * line is placed at a world-space Z that increases up the screen, then
 * perspective-projected so lines shrink and converge toward a vanishing point
 * near the top. Lines fade out as they approach the horizon and fade in at the
 * bottom. The whole thing is drawn with the 5x7 bitmap font into the linear-RGB
 * framebuffer, so it glows and bleeds through the NTSC/CRT chain  -  a classic
 * title-sequence look on a CRT.
 *
 * Deterministic given t (it loops), so it golden-tests cleanly.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/text.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* The crawl text. Blank strings are spacer lines. */
static const char *CRAWL[] = {
    "CATHODE",
    "",
    "A GRAPHICS ENGINE",
    "THAT RUNS ENTIRELY",
    "ON THE CPU",
    "",
    "NO GPU.",
    "NO GAME ENGINE.",
    "NO LIBRARIES.",
    "",
    "SOFTWARE NTSC/CRT",
    "SIGNAL EMULATION",
    "",
    "HAND-WRITTEN",
    "AARCH64 NEON",
    "ASSEMBLY IN THE",
    "HOT LOOPS",
    "",
    "C . RUST . C++",
    "OVER A FROZEN",
    "C ABI",
    "",
    "RASTERIZER",
    "RAY MARCHER",
    "PATH TRACER",
    "N-BODY GRAVITY",
    "STABLE FLUIDS",
    "REACTION-DIFFUSION",
    "",
    "AND A SYNTH,",
    "A TRACKER, AND",
    "AN FFT FOR GOOD",
    "MEASURE",
    "",
    "THANKS FOR",
    "WATCHING",
    "",
    "",
};
enum { NLINES = (int)(sizeof(CRAWL)/sizeof(CRAWL[0])) };

typedef struct { i32 w, h; f32 t; } CreditsState;

static void cr_init(Scene *sc, i32 w, i32 h){ CreditsState*s=sc->state; s->w=w;s->h=h;s->t=0; }
static void cr_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((CreditsState*)sc->state)->t=t; }

static void cr_render(Scene *sc, Framebuffer *fb){
    CreditsState *s=sc->state;
    const i32 w=fb->w, h=fb->h; const f32 t=s->t;

    /* starfield-ish deep-space background gradient (dark blue -> black at top) */
    for (i32 y=0;y<h;++y){
        f32 v = 0.05f * (f32)y/h;          /* darker toward the vanishing top */
        f32 *row=&fb->px[(size_t)y*w*3];
        for (i32 x=0;x<w;++x){ row[3*x+0]=v*0.15f; row[3*x+1]=v*0.18f; row[3*x+2]=v*0.35f; }
    }

    /* Perspective model:
     *   each line i has a world "distance" d that decreases as the crawl scrolls
     *   (line marches from far d_far toward the camera d_near then off-bottom).
     *   screen_scale = focal / d ; screen_y = horizon + focal*Y_up/d with Y_up
     *   fixed per line so nearer lines sit lower. We drive a single scroll
     *   parameter and derive each line's d from its index minus the scroll. */
    const f32 line_gap = 1.15f;            /* world spacing between lines */
    const f32 focal    = (f32)h * 0.9f;    /* projection focal length (px) */
    const f32 horizon  = (f32)h * 0.14f;   /* vanishing row (near the top) */
    const f32 speed    = 1.6f;             /* lines per second */
    const f32 span     = NLINES * line_gap + 12.0f; /* loop length in world units */

    /* scroll advances; each line's distance = base - i*gap + scroll (mod span) */
    f32 scroll = fmodf(t * speed * line_gap, span);

    i32 base_scale = h/70; if (base_scale<1) base_scale=1;

    for (int i=0;i<NLINES;++i){
        const char *str = CRAWL[i];
        if (!str[0]) continue;   /* spacer */

        /* distance in front of camera; wrap so lines recycle */
        f32 d = (f32)i * line_gap - scroll + span;
        d = fmodf(d, span);
        /* only draw lines within the visible depth window (in front of camera) */
        if (d < 0.6f || d > (f32)NLINES*line_gap + 2.0f) continue;

        f32 inv = 1.0f / d;
        f32 sy = horizon + focal * (1.0f - 0.15f) * inv;  /* nearer => lower */
        if (sy < horizon || sy > h+8) continue;

        /* scale shrinks with distance; keep it an integer for crisp blocks */
        f32 fscale = (f32)base_scale * focal * 0.02f * inv;
        i32 scale = (i32)(fscale + 0.5f);
        if (scale < 1) scale = 1; if (scale > 6) scale = 6;
        /* clamp so even the longest line never overflows the frame width: a
         * glyph advances (FONT_W+1) source px, and we want tw <= 0.92*w. */
        {
            int len = (int)strlen(str);
            i32 max_scale = (i32)((0.92f * (f32)w) / (f32)(len * (FONT_W + 1)));
            if (max_scale < 1) max_scale = 1;
            if (scale > max_scale) scale = max_scale;
        }

        /* brightness: fade near the horizon (far) and slightly near the camera */
        f32 fade = 1.0f;
        f32 dnorm = d / ((f32)NLINES*line_gap);
        if (dnorm > 0.72f) fade = 1.0f - (dnorm-0.72f)/0.28f;   /* fade into distance */
        if (fade < 0) fade = 0; if (fade > 1) fade = 1;
        fade = fade*fade;   /* gamma the fade so the far end dims fast */

        /* warm title-card amber, brighter for the first line (the logo) */
        Color3 col = (i==0) ? col3(1.0f*fade, 0.85f*fade, 0.35f*fade)
                            : col3(0.95f*fade, 0.78f*fade, 0.45f*fade);

        i32 tw = text_width(str, scale, 1);
        i32 sx = (w - tw)/2;
        text_draw_add(fb, sx, (i32)sy, str, col, scale, 1);
    }
}

static CrtConfig cr_crt(Scene *sc){ (void)sc; return crt_config_preset("broadcast"); }
static void cr_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_credits_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="credits";
    sc->description="Perspective 'star wars' credits crawl (bitmap font in 3D)";
    sc->state=calloc(1,sizeof(CreditsState));
    sc->init=cr_init; sc->update=cr_update; sc->render=cr_render;
    sc->destroy=cr_destroy; sc->preferred_crt=cr_crt;
    return sc;
}
