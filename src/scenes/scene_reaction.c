/* ==========================================================================
 * scene_reaction.c — Gray-Scott reaction-diffusion, computed in Rust.
 *
 * This scene is a thin C shell over the Rust reaction-diffusion engine
 * (rustsrc/src/reaction.rs, contract in rustcore.h). It cycles through several
 * classic (feed, kill) parameter regimes — solitons, mitosis, coral, worms —
 * and maps the chemical concentration field through a palette. The self-
 * organizing Turing patterns pair beautifully with CRT phosphor bleed.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/rustcore.h"
#include <stdlib.h>
#include <math.h>

/* A curated tour of Gray-Scott parameter space; each (feed,kill) makes a
 * qualitatively different pattern. */
typedef struct { f32 feed, kill; const char *name; } RdPreset;
static const RdPreset RD_PRESETS[] = {
    {0.0290f, 0.0570f, "mitosis"},   /* develops fast + vivid; a good default */
    {0.0545f, 0.0620f, "worms"},
    {0.0367f, 0.0649f, "solitons"},
    {0.0620f, 0.0610f, "coral"},
    {0.0340f, 0.0618f, "maze"},
    {0.0180f, 0.0510f, "spots"},
};
#define RD_NPRESETS ((int)(sizeof(RD_PRESETS)/sizeof(RD_PRESETS[0])))

typedef struct {
    RustReactionDiffusion *rd;
    f32 *field;          /* scratch for the V field */
    i32 gw, gh;
    i32 w, h;
    f32 t;
    int preset;
    int palette;
} ReactionState;

static void rd_setup(ReactionState *s){
    if (s->rd) rust_rd_destroy(s->rd);
    const RdPreset *p = &RD_PRESETS[s->preset];
    /* du/dv are the classic Gray-Scott diffusion rates. */
    s->rd = rust_rd_create(s->gw, s->gh, p->feed, p->kill, 0.16f, 0.08f);
    rust_rd_seed_point(s->rd, s->gw/2, s->gh/2, 12);
    /* a few extra seed blobs for faster, more interesting onset */
    rust_rd_seed_point(s->rd, s->gw/3, s->gh/2, 6);
    rust_rd_seed_point(s->rd, 2*s->gw/3, s->gh/3, 6);
}

static void re_init(Scene *sc, i32 w, i32 h){
    ReactionState *s = sc->state;
    s->w=w; s->h=h; s->t=0; s->preset=0; s->palette=0;
    /* run the sim at half resolution for speed, upscale on render */
    s->gw = w/2 < 24 ? 24 : (w/2 > 320 ? 320 : w/2);
    s->gh = h/2 < 24 ? 24 : (h/2 > 320 ? 320 : h/2);
    s->field = malloc((size_t)s->gw*s->gh*sizeof(f32));
    s->rd = NULL;
    rd_setup(s);
}

static void re_update(Scene *sc, f32 dt, f32 t){
    ReactionState *s = sc->state; (void)dt; s->t=t;
    /* Gray-Scott is slow per iteration, so take many substeps per frame to
     * evolve patterns at a watchable pace. */
    rust_rd_step(s->rd, 40);
}

static Color3 rd_palette(int pal, f32 v){
    /* v in 0..1 (normalized chemical V). Two-color gradients that stay bright
     * across the whole range so the Turing structure is always visible. */
    switch(pal){
        case 0: /* deep blue -> hot magenta -> white */
            return col_lerp(col_lerp(col3(0.05f,0.1f,0.4f), col3(0.9f,0.2f,0.7f), ct_clampf(v*1.6f,0,1)),
                            col3(1.0f,0.95f,0.9f), ct_clampf((v-0.6f)*2.5f,0,1));
        case 1: /* fire: black -> red -> yellow -> white */
            return col_lerp(col_lerp(col3(0.1f,0.02f,0.0f), col3(1.0f,0.25f,0.0f), ct_clampf(v*1.8f,0,1)),
                            col3(1.0f,1.0f,0.7f), ct_clampf((v-0.55f)*2.2f,0,1));
        case 2: /* emerald: teal -> green -> pale */
            return col_lerp(col3(0.0f,0.25f,0.25f), col3(0.6f,1.0f,0.5f), v);
        default:/* ice: navy -> cyan -> white */
            return col_lerp(col3(0.05f,0.1f,0.3f), col3(0.8f,0.95f,1.0f), v);
    }
}

static void re_render(Scene *sc, Framebuffer *fb){
    ReactionState *s = sc->state;
    rust_rd_field(s->rd, s->field);
    /* upscale the grid to the framebuffer with bilinear sampling */
    for (i32 py=0; py<fb->h; ++py){
        f32 gy = (f32)py/(fb->h-1)*(s->gh-1);
        i32 j0=(i32)gy; i32 j1=j0+1<s->gh?j0+1:j0; f32 tj=gy-j0;
        for (i32 px=0; px<fb->w; ++px){
            f32 gx=(f32)px/(fb->w-1)*(s->gw-1);
            i32 i0=(i32)gx; i32 i1=i0+1<s->gw?i0+1:i0; f32 ti=gx-i0;
            const f32 *F=s->field;
            f32 v = (1-ti)*(1-tj)*F[j0*s->gw+i0] + ti*(1-tj)*F[j0*s->gw+i1]
                  + (1-ti)*tj*F[j1*s->gw+i0]     + ti*tj*F[j1*s->gw+i1];
            /* Measured Gray-Scott V lives in ~0.0..0.38 with structure around
             * the 0.2..0.35 band (the pattern "walls"). Map that band across
             * the full palette so the Turing structure is vivid. */
            v = ct_clampf(v / 0.38f, 0.0f, 1.0f);      /* normalize to 0..1 */
            v = powf(v, 0.6f);                          /* lift mid-tones */
            fb_set(fb, px, py, rd_palette(s->palette, v));
        }
    }
}

static void re_key(Scene *sc, int key){
    ReactionState *s = sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->preset=(s->preset+1)%RD_NPRESETS; rd_setup(s); }
    else if (key==KEY_PLUS) s->palette=(s->palette+1)%4;
    else if (key==KEY_R) rd_setup(s);
}

static CrtConfig re_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.persistence=0.4f; return c; }
static void re_destroy(Scene *sc){ if(sc){ ReactionState*s=sc->state; if(s->rd)rust_rd_destroy(s->rd); free(s->field); free(s); free(sc);} }

Scene *scene_reaction_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="reaction";
    sc->description="Gray-Scott reaction-diffusion (computed in Rust) — Turing patterns";
    sc->state=calloc(1,sizeof(ReactionState));
    sc->init=re_init; sc->update=re_update; sc->render=re_render;
    sc->on_key=re_key; sc->destroy=re_destroy; sc->preferred_crt=re_crt;
    return sc;
}
