/* ==========================================================================
 * scene_buddhabrot.c — the Buddhabrot: density of escaping Mandelbrot orbits.
 *
 * Unlike the Mandelbrot set (which colors points by escape time), the
 * Buddhabrot plots, for each point c that DOES escape, the entire trajectory
 * z_0..z_escape it traced through the plane, accumulating a density histogram.
 * The result is a ghostly, three-lobed figure resembling a seated Buddha —
 * an emergent structure nobody designs. We accumulate samples every frame
 * (progressive refinement), split the escape-time thresholds into R/G/B
 * channels ("Nebulabrot") for color, and log-map the density.
 *
 * Refs: Melinda Green, "The Buddhabrot Technique" (1993).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    f32 *accum;        /* w*h*3 density per channel */
    i32 gw, gh;
    Rng rng;
    i32 w, h; f32 t;
    i64 samples;
    /* view: Buddhabrot lives in c-plane ~[-2,1]x[-1.5,1.5], rotated 90° so the
     * "Buddha" sits upright (imaginary axis vertical -> we map re to y). */
} BuddhaState;

static void bu_init(Scene *sc, i32 w, i32 h){
    BuddhaState *s=sc->state; s->w=w; s->h=h; s->t=0; s->samples=0;
    s->gw=w; s->gh=h;
    s->accum=calloc((size_t)w*h*3,sizeof(f32));
    rng_seed(&s->rng, 0xB0DDA77ULL);
}

/* map complex (re,im) to screen; the classic Buddhabrot is drawn sideways so
 * re -> vertical, im -> horizontal, giving the upright seated figure. */
static inline int plane_to_screen(BuddhaState *s, f32 re, f32 im, i32 *sx, i32 *sy){
    /* re in [-2.1, 0.9], im in [-1.3, 1.3] */
    f32 fx=(im - (-1.3f))/2.6f;         /* 0..1 horizontal from imaginary */
    f32 fy=(re - (-2.1f))/3.0f;         /* 0..1 vertical from real */
    *sx=(i32)(fx*s->gw);
    *sy=(i32)(fy*s->gh);
    return (unsigned)*sx<(unsigned)s->gw && (unsigned)*sy<(unsigned)s->gh;
}

static void bu_update(Scene *sc, f32 dt, f32 t){
    BuddhaState *s=sc->state; (void)dt; s->t=t;
    i32 gw=s->gw, gh=s->gh;
    /* three escape-time bands -> R,G,B (Nebulabrot). A trajectory contributes
     * to the channel whose max-iteration band it escaped within. */
    const int bands[3]={2000, 400, 80};   /* R=long, G=med, B=short orbits */
    /* buffer of the current trajectory */
    static f32 traj_re[2048], traj_im[2048];
    int budget = gw*gh/2; if(budget>40000)budget=40000;  /* samples per frame */
    for (int smp=0; smp<budget; ++smp){
        /* pick a random c in the interesting region */
        f32 cre=rng_range(&s->rng, -2.1f, 0.9f);
        f32 cim=rng_range(&s->rng, -1.3f, 1.3f);
        /* quick cardioid/bulb reject (these never escape -> wasted) */
        f32 q=(cre-0.25f)*(cre-0.25f)+cim*cim;
        if (q*(q+(cre-0.25f)) <= 0.25f*cim*cim) continue;
        if ((cre+1.0f)*(cre+1.0f)+cim*cim <= 0.0625f) continue;
        for (int ch=0; ch<3; ++ch){
            int maxit=bands[ch];
            if (maxit>2048) maxit=2048;
            f32 zr=0, zi=0; int n=0; int escaped=0; int len=0;
            while (n<maxit){
                f32 zr2=zr*zr, zi2=zi*zi;
                if (zr2+zi2>4.0f){ escaped=1; break; }
                zi=2.0f*zr*zi+cim; zr=zr2-zi2+cre;
                if (len<2048){ traj_re[len]=zr; traj_im[len]=zi; ++len; }
                ++n;
            }
            if (escaped){
                for (int k=0;k<len;++k){
                    i32 sx,sy;
                    if (plane_to_screen(s, traj_re[k], traj_im[k], &sx,&sy))
                        s->accum[((size_t)sy*gw+sx)*3+ch]+=1.0f;
                }
            }
        }
        s->samples++;
    }
}

static void bu_render(Scene *sc, Framebuffer *fb){
    BuddhaState *s=sc->state; i32 gw=s->gw, gh=s->gh;
    /* per-channel max for normalization */
    f32 mx[3]={1e-6f,1e-6f,1e-6f};
    for (i32 i=0;i<gw*gh;++i) for(int c=0;c<3;++c){ f32 v=s->accum[i*3+c]; if(v>mx[c])mx[c]=v; }
    for (i32 i=0;i<gw*gh;++i){
        for (int c=0;c<3;++c){
            f32 v=s->accum[i*3+c]/mx[c];      /* 0..1 */
            v=logf(1.0f+v*9.0f)/logf(10.0f);  /* log tone map */
            fb->px[i*3+c]=v;
        }
    }
}

static void bu_key(Scene *sc, int key){
    BuddhaState *s=sc->state;
    if (key==KEY_R){ memset(s->accum,0,(size_t)s->gw*s->gh*3*sizeof(f32)); s->samples=0; }
}

static CrtConfig bu_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.4f; c.persistence=0.2f; return c; }
static void bu_destroy(Scene *sc){ if(sc){ BuddhaState*s=sc->state; free(s->accum); free(s); free(sc);} }

Scene *scene_buddhabrot_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="buddhabrot";
    sc->description="Buddhabrot / Nebulabrot: density of escaping Mandelbrot orbits";
    sc->state=calloc(1,sizeof(BuddhaState));
    sc->init=bu_init; sc->update=bu_update; sc->render=bu_render;
    sc->on_key=bu_key; sc->destroy=bu_destroy; sc->preferred_crt=bu_crt;
    return sc;
}
