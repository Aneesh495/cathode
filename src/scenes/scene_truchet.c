/* ==========================================================================
 * scene_truchet.c  -  animated multi-scale Truchet tilings.
 *
 * Truchet tiles fill a grid with a small motif placed in one of a few random
 * orientations; adjacent tiles' arcs/lines connect into surprising large-scale
 * curves and mazes. We render the classic "quarter-arc" Truchet: each cell
 * carries two quarter-circle arcs (in one of two diagonal orientations) that
 * join edge-midpoints, producing endless interlocking loops. The orientation
 * field is driven by hashed coordinates plus a slow time term so the pattern
 * continuously re-tiles, and the arcs are drawn with an anti-aliased distance
 * test and colored by a flowing hue. Pure C over the framebuffer.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { i32 w,h; f32 t; int tiles; } TruchetState;

static void tr_init(Scene *sc, i32 w, i32 h){ TruchetState*s=sc->state; s->w=w;s->h=h;s->t=0; s->tiles=10; }
static void tr_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((TruchetState*)sc->state)->t=t; }

static inline u32 hash2(int x, int y, u32 salt){
    u32 h=(u32)x*374761393u + (u32)y*668265263u + salt*362437u;
    h=(h^(h>>13))*1274126177u; return h^(h>>16);
}
static Color3 hsv(f32 h,f32 s,f32 v){
    h-=floorf(h); f32 i=floorf(h*6), f=h*6-i;
    f32 p=v*(1-s),q=v*(1-s*f),t=v*(1-s*(1-f));
    switch(((int)i)%6){case 0:return col3(v,t,p);case 1:return col3(q,v,p);
        case 2:return col3(p,v,t);case 3:return col3(p,q,v);case 4:return col3(t,p,v);default:return col3(v,p,q);}
}

static void tr_render(Scene *sc, Framebuffer *fb){
    TruchetState *s=sc->state; const i32 W=fb->w,H=fb->h; const f32 t=s->t;
    fb_clear(fb, col3(0.02f,0.02f,0.035f));

    int cells = s->tiles;                    /* cells across the shorter axis */
    f32 cs = (f32)(W<H?W:H)/cells;           /* cell size in px */
    int nx = (int)(W/cs)+1, ny=(int)(H/cs)+1;
    f32 arc_w = cs*0.16f;                    /* line half-thickness */
    /* slow scroll so the tiling drifts */
    f32 offx = t*cs*0.35f, offy = t*cs*0.12f;

    for (i32 py=0; py<H; ++py){
        f32 *row=&fb->px[(size_t)py*W*3];
        for (i32 px=0; px<W; ++px){
            f32 gx = (px+offx)/cs, gy=(py+offy)/cs;
            int cx=(int)floorf(gx), cy=(int)floorf(gy);
            f32 fx=gx-cx, fy=gy-cy;          /* local [0,1) within cell */
            /* orientation: hash + a time-based flip so tiles occasionally rotate */
            u32 hh=hash2(cx,cy,1u);
            int flip = ((hh>>3)&1);
            /* time re-tiling: some cells toggle based on a phase */
            f32 phase = ((hh&255)/255.0f);
            if (sinf(t*0.6f + phase*6.2831853f) > 0.6f) flip ^= 1;

            /* two quarter arcs. orientation 0: centers at (0,0) and (1,1);
             * orientation 1: centers at (1,0) and (0,1). radius 0.5 in cell units. */
            f32 d0,d1;
            if (!flip){
                d0=fabsf(sqrtf(fx*fx+fy*fy)-0.5f);
                d1=fabsf(sqrtf((fx-1)*(fx-1)+(fy-1)*(fy-1))-0.5f);
            } else {
                d0=fabsf(sqrtf((fx-1)*(fx-1)+fy*fy)-0.5f);
                d1=fabsf(sqrtf(fx*fx+(fy-1)*(fy-1))-0.5f);
            }
            f32 d=(d0<d1?d0:d1)*cs;          /* distance to nearest arc, px */
            f32 aa = arc_w - d;              /* >0 inside the line */
            if (aa > -1.0f){
                f32 cov = aa>0 ? 1.0f : (aa+1.0f);   /* 1px soft edge */
                if (cov<0) cov=0; if(cov>1)cov=1;
                /* color by arc "flow" position + cell, hue-cycled over time */
                f32 hue = 0.55f + 0.12f*sinf((cx+cy)*0.4f + t*0.5f) + 0.05f*t;
                Color3 c = hsv(hue, 0.55f, 1.0f);
                row[3*px+0]=row[3*px+0]*(1-cov)+c.r*cov;
                row[3*px+1]=row[3*px+1]*(1-cov)+c.g*cov;
                row[3*px+2]=row[3*px+2]*(1-cov)+c.b*cov;
            }
        }
    }
    (void)nx;(void)ny;
}

static void tr_key(Scene *sc, int key){
    TruchetState *s=sc->state;
    if (key==KEY_PLUS){ s->tiles++; if(s->tiles>40)s->tiles=40; }
    else if (key==KEY_MINUS){ s->tiles--; if(s->tiles<3)s->tiles=3; }
    else if (key==KEY_TAB||key==KEY_ENTER){ s->tiles = s->tiles>=18?6:18; }
}
static CrtConfig tr_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void tr_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_truchet_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="truchet";
    sc->description="Animated Truchet tilings: quarter-arc tiles interlocking into flowing loops";
    sc->state=calloc(1,sizeof(TruchetState));
    sc->init=tr_init; sc->update=tr_update; sc->render=tr_render;
    sc->on_key=tr_key; sc->destroy=tr_destroy; sc->preferred_crt=tr_crt;
    return sc;
}
