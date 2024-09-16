/* ==========================================================================
 * scene_dla.c  -  diffusion-limited aggregation crystal growth (Rust-backed).
 *
 * Grows a fractal dendrite from a central seed by sticking random-walker
 * particles (rust_dla_* in rustcore.h). The per-cell "age" is mapped through a
 * heat ramp so you watch the crystal grow outward in time-colored layers  - 
 * like frost spreading across a window. When the cluster fills the frame it
 * reseeds. The CRT bloom gives the tips a glow.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/rustcore.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    RustDLA *dla;
    f32 *field;
    i32 gw, gh;
    i32 w, h; f32 t;
    u64 seed;
    int mode;        /* 0 = center seed (dendrite), 1 = line seed (frost) */
    int palette;
    i32 target;      /* stop growing near this many particles, then hold/reseed */
} DlaState;

static void dla_setup(DlaState *s){
    if (s->dla) rust_dla_destroy(s->dla);
    s->dla = rust_dla_create(s->gw, s->gh, s->seed);
    if (s->mode==0) rust_dla_seed_center(s->dla);
    else rust_dla_seed_line(s->dla, s->gh-2);
    s->target = (i32)(s->gw*s->gh*0.10f);   /* ~10% coverage looks best */
}

static void dl_init(Scene *sc, i32 w, i32 h){
    DlaState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->mode=0; s->palette=0; s->seed=0xDA1A0001ULL;
    /* run at ~half res so growth is watchable and per-frame walk cost bounded */
    s->gw = w/2 < 60 ? 60 : (w/2 > 260 ? 260 : w/2);
    s->gh = h/2 < 60 ? 60 : (h/2 > 260 ? 260 : h/2);
    s->field=malloc((size_t)s->gw*s->gh*sizeof(f32));
    s->dla=NULL;
    dla_setup(s);
}

static void dl_update(Scene *sc, f32 dt, f32 t){
    DlaState *s=sc->state; (void)dt; s->t=t;
    if (rust_dla_count(s->dla) < s->target){
        /* grow a batch of walkers per frame */
        i32 stuck = rust_dla_grow(s->dla, 250);
        if (stuck==0){
            /* frontier reached the border or stalled  -  freeze this crystal */
            s->target = 0;
        }
    } else {
        /* finished: after a pause, reseed with a fresh crystal */
        if (((i32)(t*10.0f)) % 60 == 0){
            s->seed = s->seed*6364136223846793005ULL + 1;
            dla_setup(s);
        }
    }
}

static Color3 dla_color(int pal, f32 age){
    /* age in [0,1]: 0 = oldest (seed) .. 1 = newest (tips) */
    switch(pal){
        case 0: /* frost: deep blue core -> white tips */
            return col_lerp(col3(0.1f,0.2f,0.5f), col3(0.9f,0.95f,1.0f), age);
        case 1: /* ember: dark red core -> yellow tips */
            return col_lerp(col3(0.4f,0.05f,0.0f), col3(1.0f,0.9f,0.3f), age);
        default:/* spectrum by age */
            return col3(0.5f+0.5f*sinf(age*6.0f), 0.5f+0.5f*sinf(age*6.0f+2.0f), 0.5f+0.5f*sinf(age*6.0f+4.0f));
    }
}

static void dl_render(Scene *sc, Framebuffer *fb){
    DlaState *s=sc->state;
    rust_dla_field(s->dla, s->field);
    for (i32 py=0;py<fb->h;++py){
        i32 gy=(i32)((f32)py/(fb->h-1)*(s->gh-1));
        for (i32 px=0;px<fb->w;++px){
            i32 gx=(i32)((f32)px/(fb->w-1)*(s->gw-1));
            f32 a=s->field[gy*s->gw+gx];
            if (a<0.0f) fb_set(fb,px,py, col3(0.01f,0.01f,0.03f)); /* empty */
            else fb_set(fb,px,py, dla_color(s->palette, a));
        }
    }
}

static void dl_key(Scene *sc, int key){
    DlaState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_ENTER){ s->mode=!s->mode; dla_setup(s); }
    else if (key==KEY_R){ s->seed=s->seed*2862933555777941757ULL+3037000493ULL; dla_setup(s); }
}

static CrtConfig dl_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.5f; c.persistence=0.25f; return c; }
static void dl_destroy(Scene *sc){ if(sc){ DlaState*s=sc->state; if(s->dla)rust_dla_destroy(s->dla); free(s->field); free(s); free(sc);} }

Scene *scene_dla_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="dla";
    sc->description="Diffusion-limited aggregation crystal growth (Rust)";
    sc->state=calloc(1,sizeof(DlaState));
    sc->init=dl_init; sc->update=dl_update; sc->render=dl_render;
    sc->on_key=dl_key; sc->destroy=dl_destroy; sc->preferred_crt=dl_crt;
    return sc;
}
