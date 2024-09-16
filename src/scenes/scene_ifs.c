/* ==========================================================================
 * scene_ifs.c  -  Iterated Function System fractals (Barnsley fern & friends).
 *
 * The chaos game with purely affine maps chosen by probability: each iteration
 * applies one of N affine transforms (x' = a x + b y + e ; y' = c x + d y + f)
 * picked by weight, and plots the orbit. Barnsley's 4-map system famously draws
 * a fern; other coefficient sets draw a Sierpinski triangle, a spiral, a tree.
 * We slowly rotate the view and cycle systems. Density is accumulated and
 * log-mapped so fine fronds glow.
 *
 * Refs: Barnsley, "Fractals Everywhere" (1988).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { f32 a,b,c,d,e,f,p; } Affine;   /* p = cumulative probability */

typedef struct {
    const Affine *sys; int nsys;
    f32 xmin,xmax,ymin,ymax;     /* attractor bounds for framing */
    Color3 col;
    const char *name;
} IfsDef;

/* --- Barnsley fern --- */
static const Affine FERN[4]={
    { 0.0f, 0.0f, 0.0f, 0.16f,0.0f,0.0f, 0.01f},
    { 0.85f,0.04f,-0.04f,0.85f,0.0f,1.60f,0.86f},
    { 0.20f,-0.26f,0.23f,0.22f,0.0f,1.60f,0.93f},
    {-0.15f,0.28f,0.26f,0.24f,0.0f,0.44f,1.00f},
};
/* --- Sierpinski triangle (3 maps, equal prob) --- */
static const Affine SIERP[3]={
    {0.5f,0,0,0.5f,0.0f,0.0f,0.3333f},
    {0.5f,0,0,0.5f,0.5f,0.0f,0.6666f},
    {0.5f,0,0,0.5f,0.25f,0.5f,1.0f},
};
/* --- a spiral / dragon-ish system --- */
static const Affine SPIRAL[2]={
    {0.787879f,-0.424242f,0.242424f,0.859848f,1.758647f,1.408065f,0.9f},
    {-0.121212f,0.257576f,0.151515f,0.053030f,-6.721654f,1.377236f,1.0f},
};

static const IfsDef SYSTEMS[]={
    {FERN,   4, -2.2f, 2.7f, 0.0f, 10.0f, {0.3f,1.0f,0.4f}, "fern"},
    {SIERP,  3, 0.0f, 1.0f, 0.0f, 1.0f,   {1.0f,0.7f,0.2f}, "sierpinski"},
    {SPIRAL, 2, -9.0f,7.0f, -1.0f,10.0f,  {0.5f,0.7f,1.0f}, "spiral"},
};
#define NSYS ((int)(sizeof(SYSTEMS)/sizeof(SYSTEMS[0])))

typedef struct {
    f32 *dens;      /* w*h accumulation */
    i32 gw, gh;
    int which;
    Rng rng;
    i32 w, h; f32 t;
} IfsState;

static void ifs_init(Scene *sc, i32 w, i32 h){
    IfsState *s=sc->state; s->w=w; s->h=h; s->t=0; s->which=0;
    s->gw=w; s->gh=h;
    s->dens=calloc((size_t)w*h,sizeof(f32));
    rng_seed(&s->rng, 0x1F5F00DULL);
}

static void ifs_update(Scene *sc, f32 dt, f32 t){
    IfsState *s=sc->state; (void)dt; s->t=t;
    const IfsDef *D=&SYSTEMS[s->which];
    i32 gw=s->gw, gh=s->gh;
    /* slow fade so it stays crisp but can be reset cleanly */
    for (i32 i=0;i<gw*gh;++i) s->dens[i]*=0.9f;
    /* framing: map attractor bounds into the screen with margin + a slow spin */
    f32 spin=0.15f*sinf(t*0.2f);
    f32 cs=cosf(spin), sn=sinf(spin);
    f32 bx=(D->xmin+D->xmax)*0.5f, by=(D->ymin+D->ymax)*0.5f;
    f32 spanx=D->xmax-D->xmin, spany=D->ymax-D->ymin;
    f32 span=(spanx>spany?spanx:spany)*0.6f + 1e-3f;
    f32 scale=(gh*0.9f)/(2.0f*span);
    f32 px=0.0f, py=0.0f;
    i32 iters=gw*gh*2; if(iters>400000)iters=400000;
    for (i32 it=0; it<iters; ++it){
        f32 r=rng_f32(&s->rng);
        const Affine *m=&D->sys[0];
        for (int i=0;i<D->nsys;++i){ if(r<=D->sys[i].p){ m=&D->sys[i]; break; } m=&D->sys[i]; }
        f32 nx=m->a*px+m->b*py+m->e;
        f32 ny=m->c*px+m->d*py+m->f;
        px=nx; py=ny;
        if (it<20) continue;
        /* center, rotate, scale to screen */
        f32 rx=(px-bx), ry=(py-by);
        f32 wx=rx*cs-ry*sn, wy=rx*sn+ry*cs;
        i32 sx=(i32)(gw*0.5f + wx*scale);
        i32 sy=(i32)(gh*0.5f - wy*scale);   /* flip y (screen down) */
        if ((unsigned)sx<(unsigned)gw && (unsigned)sy<(unsigned)gh)
            s->dens[(size_t)sy*gw+sx]+=1.0f;
    }
}

static void ifs_render(Scene *sc, Framebuffer *fb){
    IfsState *s=sc->state; i32 gw=s->gw, gh=s->gh;
    Color3 base=SYSTEMS[s->which].col;
    for (i32 i=0;i<gw*gh;++i){
        f32 d=s->dens[i];
        if (d<1e-4f){ fb->px[i*3]=0.01f; fb->px[i*3+1]=0.02f; fb->px[i*3+2]=0.03f; continue; }
        f32 b=logf(1.0f+d)*0.32f; if(b>1.0f)b=1.0f;
        fb->px[i*3+0]=base.r*b; fb->px[i*3+1]=base.g*b; fb->px[i*3+2]=base.b*b;
    }
}

static void ifs_key(Scene *sc, int key){
    IfsState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->which=(s->which+1)%NSYS; memset(s->dens,0,(size_t)s->gw*s->gh*sizeof(f32)); }
    else if (key==KEY_R) memset(s->dens,0,(size_t)s->gw*s->gh*sizeof(f32));
}

static CrtConfig ifs_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.5f; c.persistence=0.35f; return c; }
static void ifs_destroy(Scene *sc){ if(sc){ IfsState*s=sc->state; free(s->dens); free(s); free(sc);} }

Scene *scene_ifs_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="ifs";
    sc->description="Iterated Function System fractals: Barnsley fern, Sierpinski, spiral";
    sc->state=calloc(1,sizeof(IfsState));
    sc->init=ifs_init; sc->update=ifs_update; sc->render=ifs_render;
    sc->on_key=ifs_key; sc->destroy=ifs_destroy; sc->preferred_crt=ifs_crt;
    return sc;
}
