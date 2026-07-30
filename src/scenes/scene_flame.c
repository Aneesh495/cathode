/* ==========================================================================
 * scene_flame.c — fractal flame renderer (Scott Draves' algorithm).
 *
 * The "chaos game" with nonlinear variations: repeatedly pick a random affine
 * transform (weighted), apply it to the running point, then apply a nonlinear
 * "variation" function (sinusoidal, spherical, swirl, horseshoe, …). Plot the
 * visited points into an accumulation buffer with per-transform color, and
 * tone-map by log(density) — the signature glowing, organic flame structures
 * behind Electric Sheep. Transforms morph slowly over time so the flame
 * breathes and metamorphoses.
 *
 * Refs: Draves & Reckase, "The Fractal Flame Algorithm" (2003).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NXFORM 4

typedef struct {
    f32 a,b,c,d,e,f;   /* affine: x' = a*x + b*y + c ; y' = d*x + e*y + f */
    f32 weight;
    int variation;     /* which nonlinear variation */
    f32 cr,cg,cb;      /* color contributed by this transform */
} XForm;

typedef struct {
    f32 *accum;        /* w*h*4: r,g,b,density */
    i32 gw, gh;
    XForm xf[NXFORM];
    Rng rng;
    i32 w, h; f32 t;
    int palette;
} FlameState;

/* nonlinear variations (V_i in the flame paper), applied to (x,y) */
static void variation(int v, f32 x, f32 y, f32 *ox, f32 *oy){
    f32 r2=x*x+y*y, r=sqrtf(r2)+1e-9f, theta=atan2f(x,y);
    switch(v){
        case 0: *ox=x; *oy=y; break;                              /* linear */
        case 1: *ox=sinf(x); *oy=sinf(y); break;                  /* sinusoidal */
        case 2: *ox=x/r2; *oy=y/r2; break;                        /* spherical */
        case 3: *ox=x*sinf(r2)-y*cosf(r2); *oy=x*cosf(r2)+y*sinf(r2); break; /* swirl */
        case 4: *ox=(x-y)*(x+y)/r; *oy=2.0f*x*y/r; break;         /* horseshoe */
        case 5: *ox=theta/CT_PI; *oy=r-1.0f; break;               /* polar */
        default:*ox=r*cosf(theta+r); *oy=r*sinf(theta+r); break;  /* spiral-ish */
    }
}

static void flame_randomize(FlameState *s){
    for (int i=0;i<NXFORM;++i){
        XForm *x=&s->xf[i];
        /* affine coeffs in [-1,1], gently contracting on average */
        x->a=rng_range(&s->rng,-1,1)*0.8f; x->b=rng_range(&s->rng,-1,1)*0.8f;
        x->c=rng_range(&s->rng,-0.5f,0.5f);
        x->d=rng_range(&s->rng,-1,1)*0.8f; x->e=rng_range(&s->rng,-1,1)*0.8f;
        x->f=rng_range(&s->rng,-0.5f,0.5f);
        x->weight=rng_range(&s->rng,0.3f,1.0f);
        x->variation=(int)(rng_f32(&s->rng)*7.0f);
        /* palette-spread colors */
        f32 h=(f32)i/NXFORM;
        x->cr=0.5f+0.5f*sinf(h*CT_TAU); x->cg=0.5f+0.5f*sinf(h*CT_TAU+2.09f); x->cb=0.5f+0.5f*sinf(h*CT_TAU+4.18f);
    }
}

static void fm_init(Scene *sc, i32 w, i32 h){
    FlameState *s=sc->state; s->w=w; s->h=h; s->t=0; s->palette=0;
    s->gw=w; s->gh=h;
    s->accum=calloc((size_t)w*h*4,sizeof(f32));
    rng_seed(&s->rng, 0xF1A3E00DULL);
    flame_randomize(s);
}

static void fm_update(Scene *sc, f32 dt, f32 t){
    FlameState *s=sc->state; (void)dt; s->t=t;
    i32 gw=s->gw, gh=s->gh;
    /* fade accumulation so the flame morphs rather than saturating forever */
    size_t n=(size_t)gw*gh*4;
    for (size_t i=0;i<n;++i) s->accum[i]*=0.955f;
    /* slowly rotate every transform's affine part -> breathing metamorphosis */
    f32 rot=0.02f*sinf(t*0.3f);
    for (int i=0;i<NXFORM;++i){
        XForm *x=&s->xf[i];
        f32 ca=cosf(rot), sa=sinf(rot);
        f32 na=x->a*ca-x->d*sa, nd=x->a*sa+x->d*ca;
        f32 nb=x->b*ca-x->e*sa, ne=x->b*sa+x->e*ca;
        x->a=na; x->d=nd; x->b=nb; x->e=ne;
    }
    /* total weight for roulette selection */
    f32 wsum=0; for(int i=0;i<NXFORM;++i) wsum+=s->xf[i].weight;
    /* chaos game: iterate a point, plot after a burn-in */
    f32 px=rng_range(&s->rng,-1,1), py=rng_range(&s->rng,-1,1);
    f32 cr=0.5f,cg=0.5f,cb=0.5f;
    i32 iters=gw*gh*3;  if(iters>500000)iters=500000;
    f32 half=gh*0.5f, cx=gw*0.5f;
    for (i32 it=0; it<iters; ++it){
        /* pick a transform by weight */
        f32 r=rng_f32(&s->rng)*wsum; int sel=0;
        for (int i=0;i<NXFORM;++i){ r-=s->xf[i].weight; if(r<=0){sel=i;break;} sel=i; }
        XForm *xf=&s->xf[sel];
        f32 ax=xf->a*px+xf->b*py+xf->c;
        f32 ay=xf->d*px+xf->e*py+xf->f;
        variation(xf->variation, ax, ay, &px, &py);
        /* blend color toward this transform's color (flame color coordinate) */
        cr=0.5f*(cr+xf->cr); cg=0.5f*(cg+xf->cg); cb=0.5f*(cb+xf->cb);
        if (it<20) continue;   /* burn-in: let the point settle on the attractor */
        /* plot: map [-2,2] world to screen */
        i32 sx=(i32)(cx + px*half*0.9f);
        i32 sy=(i32)(half + py*half*0.9f);
        if ((unsigned)sx<(unsigned)gw && (unsigned)sy<(unsigned)gh){
            f32 *a=&s->accum[((size_t)sy*gw+sx)*4];
            a[0]+=cr; a[1]+=cg; a[2]+=cb; a[3]+=1.0f;
        }
    }
}

static void fm_render(Scene *sc, Framebuffer *fb){
    FlameState *s=sc->state; i32 gw=s->gw, gh=s->gh;
    for (i32 i=0;i<gw*gh;++i){
        f32 *a=&s->accum[(size_t)i*4];
        f32 dens=a[3];
        if (dens<1e-4f){ fb->px[i*3]=0.01f; fb->px[i*3+1]=0.01f; fb->px[i*3+2]=0.02f; continue; }
        /* log-density tone map (the flame signature): brightness ~ log(density) */
        f32 bright=logf(1.0f+dens)*0.20f;
        if (bright>1.0f) bright=1.0f;
        f32 inv=1.0f/dens;   /* average color of points that landed here */
        fb->px[i*3+0]=a[0]*inv*bright*1.3f;
        fb->px[i*3+1]=a[1]*inv*bright*1.3f;
        fb->px[i*3+2]=a[2]*inv*bright*1.3f;
    }
}

static void fm_key(Scene *sc, int key){
    FlameState *s=sc->state;
    if (key==KEY_R || key==KEY_TAB){
        rng_seed(&s->rng, (u64)(s->t*1000.0f)*2654435761ULL + 1);
        flame_randomize(s);
        memset(s->accum,0,(size_t)s->gw*s->gh*4*sizeof(f32));
    }
}

static CrtConfig fm_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.6f; c.persistence=0.3f; return c; }
static void fm_destroy(Scene *sc){ if(sc){ FlameState*s=sc->state; free(s->accum); free(s); free(sc);} }

Scene *scene_flame_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="flame";
    sc->description="Fractal flame (chaos game + nonlinear variations, log-density)";
    sc->state=calloc(1,sizeof(FlameState));
    sc->init=fm_init; sc->update=fm_update; sc->render=fm_render;
    sc->on_key=fm_key; sc->destroy=fm_destroy; sc->preferred_crt=fm_crt;
    return sc;
}
