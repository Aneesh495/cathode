/* ==========================================================================
 * scene_lsystem.c  -  animated L-system plants (Rust turtle-graphics generator).
 *
 * The Rust L-system module (rust_lsystem_*) expands a production grammar and
 * returns normalized 2D line segments tagged with branch depth. We draw them
 * with an animated "growth" wipe (segments appear in draw order over time) and
 * color by depth  -  trunk warm, leaves cool/green  -  so the plant appears to grow
 * and leaf out. Cycles through the built-in presets (fractal plant, Koch,
 * dragon, Sierpinski, bushy tree).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/rustcore.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    i32 w, h; f32 t;
    RustLSystem *ls;
    RustLSeg *segs; i32 nsegs, max_depth;
    int preset, npresets;
    f32 grow;         /* 0..1 growth progress */
} LsysState;

static void ls_build(LsysState *s){
    if (s->ls) rust_lsystem_destroy(s->ls);
    s->ls = rust_lsystem_create(s->preset);
    s->nsegs = rust_lsystem_nsegs(s->ls);
    s->max_depth = rust_lsystem_max_depth(s->ls);
    free(s->segs);
    s->segs = malloc((size_t)(s->nsegs>0?s->nsegs:1)*sizeof(RustLSeg));
    rust_lsystem_segs(s->ls, s->segs, s->nsegs);
    s->grow = 0;
}

static void ls_init(Scene *sc, i32 w, i32 h){
    LsysState *s=sc->state; s->w=w; s->h=h; s->t=0; s->preset=0;
    s->npresets = rust_lsystem_preset_count();
    s->ls=NULL; s->segs=NULL;
    ls_build(s);
}

static void ls_update(Scene *sc, f32 dt, f32 t){
    LsysState *s=sc->state; s->t=t;
    if (dt<=0) return;
    if (s->grow < 1.0f) s->grow += dt*0.35f;
    else {
        /* hold, then advance to the next preset */
        if (s->grow < 2.0f) s->grow += dt*0.4f;
        else { s->preset=(s->preset+1)%s->npresets; ls_build(s); }
    }
}

/* draw an anti-aliased-ish thick line (2px) into fb */
static void draw_seg(Framebuffer *fb, f32 x0,f32 y0,f32 x1,f32 y1, Color3 c){
    int steps=(int)(fabsf(x1-x0)+fabsf(y1-y0))+1;
    for (int k=0;k<=steps;++k){
        f32 tt=(f32)k/steps;
        int px=(int)(x0+tt*(x1-x0)), py=(int)(y0+tt*(y1-y0));
        fb_add(fb, px, py, c);
        fb_add(fb, px+1, py, col_scale(c,0.5f));   /* slight thickness */
    }
}

static void ls_render(Scene *sc, Framebuffer *fb){
    LsysState *s=sc->state; const i32 W=fb->w, H=fb->h;
    fb_clear(fb, col3(0.02f,0.03f,0.04f));
    /* fit the unit-square plant into the framebuffer with margin, flip Y so it
     * grows upward (turtle +y is up; screen +y is down) */
    f32 m=0.08f;
    f32 sx=W*(1-2*m), sy=H*(1-2*m);
    f32 ox=W*m, oy=H*m;
    f32 shown = s->grow<1.0f ? s->grow : 1.0f;
    i32 nvis = (i32)(shown * s->nsegs);
    for (i32 i=0;i<nvis && i<s->nsegs;++i){
        RustLSeg *g=&s->segs[i];
        f32 x0=ox + g->x0*sx, y0=oy + (1.0f-g->y0)*sy;
        f32 x1=ox + g->x1*sx, y1=oy + (1.0f-g->y1)*sy;
        /* color by depth: trunk (depth 0) warm brown, deeper = greener */
        f32 d = s->max_depth>0 ? (f32)g->depth/s->max_depth : 0;
        Color3 c = col3(0.55f - 0.45f*d, 0.30f + 0.65f*d, 0.12f + 0.25f*d);
        /* newest segments glow (growth tip) */
        if (i > nvis-8 && s->grow<1.0f) c=col3(c.r+0.5f,c.g+0.5f,c.b+0.2f);
        draw_seg(fb, x0,y0,x1,y1, c);
    }
}

static void ls_key(Scene *sc, int key){
    LsysState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->preset=(s->preset+1)%s->npresets; ls_build(s); }
}
static CrtConfig ls_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void ls_destroy(Scene *sc){
    if(sc){ LsysState*s=sc->state; if(s->ls)rust_lsystem_destroy(s->ls); free(s->segs); free(s); free(sc);} }

Scene *scene_lsystem_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="lsystem";
    sc->description="Animated L-system plants: turtle-graphics grammar (Rust) grown segment by segment";
    sc->state=calloc(1,sizeof(LsysState));
    sc->init=ls_init; sc->update=ls_update; sc->render=ls_render;
    sc->on_key=ls_key; sc->destroy=ls_destroy; sc->preferred_crt=ls_crt;
    return sc;
}
