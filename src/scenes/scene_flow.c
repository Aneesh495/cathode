/* ==========================================================================
 * scene_flow.c  -  de Jong / Clifford attractor density field.
 *
 * These 2D "pickover" attractors iterate a simple nonlinear map and, plotted
 * as a density histogram of the visited points, produce intricate lace-like
 * structures. We iterate hundreds of thousands of points per frame into an
 * accumulation buffer, slowly morph the parameters (so the structure breathes
 * and metamorphoses), and map log-density through a palette. The CRT bloom
 * turns the fine filaments into glowing threads.
 *
 *   de Jong:  x' = sin(a y) - cos(b x);  y' = sin(c x) - cos(d y)
 *   Clifford: x' = sin(a y) + c cos(a x); y' = sin(b x) + d cos(b y)
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    f32 *density;        /* accumulation histogram, gw*gh */
    i32 gw, gh;
    i32 w, h; f32 t;
    f32 a,b,c,d;         /* current params */
    int kind;            /* 0 de Jong, 1 Clifford */
    int palette;
    i32 iters_per_frame;
} FlowState;

static void fl_init(Scene *sc, i32 w, i32 h){
    FlowState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->kind=0; s->palette=0;
    s->gw=w; s->gh=h;
    s->density=calloc((size_t)w*h,sizeof(f32));
    s->iters_per_frame = w*h*2;             /* ~2 points per pixel per frame */
    if (s->iters_per_frame>600000) s->iters_per_frame=600000;
}

static void fl_update(Scene *sc, f32 dt, f32 t){
    FlowState *s=sc->state; (void)dt; s->t=t;
    /* slowly morph parameters so the attractor metamorphoses over time */
    s->a = 1.4f*sinf(t*0.11f) + 0.6f*sinf(t*0.037f);
    s->b = 1.6f*cosf(t*0.09f);
    s->c = 1.2f*sinf(t*0.13f + 1.0f) + 0.5f;
    s->d = 1.7f*cosf(t*0.061f + 2.0f);

    /* Gently fade the density buffer so structure persists across many frames
     * (builds up fine filaments) but still morphs as the params drift. */
    i32 npx=s->gw*s->gh;
    for (i32 i=0;i<npx;++i) s->density[i]*=0.94f;

    /* iterate the map, splatting into the density histogram.
     * de Jong / Clifford outputs are bounded by ~[-2,2] plus the c,d offsets;
     * a scale that maps ~[-2.6,2.6] to the frame keeps the whole figure in. */
    f32 x=0.1f, y=0.1f;
    f32 sx=s->gw*0.5f/2.6f, sy=s->gh*0.5f/2.6f;
    f32 cx=s->gw*0.5f, cy=s->gh*0.5f;
    /* warm up to land on the attractor */
    for (int i=0;i<50;++i){
        f32 nx,ny;
        if (s->kind==0){ nx=sinf(s->a*y)-cosf(s->b*x); ny=sinf(s->c*x)-cosf(s->d*y); }
        else { nx=sinf(s->a*y)+s->c*cosf(s->a*x); ny=sinf(s->b*x)+s->d*cosf(s->b*y); }
        x=nx; y=ny;
    }
    for (i32 i=0;i<s->iters_per_frame;++i){
        f32 nx,ny;
        if (s->kind==0){ nx=sinf(s->a*y)-cosf(s->b*x); ny=sinf(s->c*x)-cosf(s->d*y); }
        else { nx=sinf(s->a*y)+s->c*cosf(s->a*x); ny=sinf(s->b*x)+s->d*cosf(s->b*y); }
        x=nx; y=ny;
        i32 px=(i32)(cx + x*sx);
        i32 py=(i32)(cy + y*sy);
        if ((unsigned)px<(unsigned)s->gw && (unsigned)py<(unsigned)s->gh)
            s->density[py*s->gw+px] += 1.0f;
    }
}

static Color3 flow_color(int pal, f32 v){
    /* v is log-scaled density 0..~1 */
    switch(pal){
        case 0: return col3(0.15f+0.85f*v, 0.3f*v+0.1f, 0.6f*v);        /* violet-gold */
        case 1: return col3(0.1f*v, 0.5f*v+0.1f, 0.4f+0.6f*v);          /* deep blue */
        default:return col3(v, 0.5f+0.5f*v, 0.2f+0.3f*v);              /* acid green */
    }
}

static void fl_render(Scene *sc, Framebuffer *fb){
    FlowState *s=sc->state;
    for (i32 i=0;i<fb->w*fb->h;++i){
        f32 d=s->density[i];
        f32 v=logf(1.0f+d)*0.5f;       /* log density for huge dynamic range */
        if (v>1.0f) v=1.0f;
        Color3 c=flow_color(s->palette, v);
        fb->px[i*3+0]=c.r*v + c.r*0.15f; /* slight base glow where any density */
        fb->px[i*3+1]=c.g*v;
        fb->px[i*3+2]=c.b*v;
    }
}

static void fl_key(Scene *sc, int key){
    FlowState *s=sc->state;
    if (key==KEY_TAB) s->kind=!s->kind;
    else if (key==KEY_ENTER) s->palette=(s->palette+1)%3;
    else if (key==KEY_R) memset(s->density,0,(size_t)s->gw*s->gh*sizeof(f32));
}

static CrtConfig fl_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.6f; c.persistence=0.4f; return c; }
static void fl_destroy(Scene *sc){ if(sc){ FlowState*s=sc->state; free(s->density); free(s); free(sc);} }

Scene *scene_flow_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="flow";
    sc->description="de Jong / Clifford attractor density field, morphing";
    sc->state=calloc(1,sizeof(FlowState));
    sc->init=fl_init; sc->update=fl_update; sc->render=fl_render;
    sc->on_key=fl_key; sc->destroy=fl_destroy; sc->preferred_crt=fl_crt;
    return sc;
}
