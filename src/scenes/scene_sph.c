/* ==========================================================================
 * scene_sph.c  -  particle fluid via Smoothed Particle Hydrodynamics.
 *
 * A blob of SPH fluid (physics.h sph_*) sloshes under a gravity vector that
 * slowly rotates, so the liquid pours from side to side, splashes off the
 * walls, and settles  -  behavior the grid solver can't show (free surface,
 * droplets). Particles are splatted with a speed-based color ramp (deep blue
 * at rest → white foam when fast) and the CRT bloom lights the spray.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/physics.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    SphSim *sph;
    f32 *xy, *speed;
    i32 maxp;
    i32 w, h; f32 t;
    f32 domw, domh;
    int tilt;   /* rotating gravity on/off */
} SphScState;

static void sp_seed(SphScState *s){
    /* domain in world units; a dense column of fluid on the left. Tighter
     * spacing => more particles => the fluid reads as a solid body. */
    sph_add_block(s->sph, s->domw*0.08f, s->domh*0.30f, s->domw*0.55f, s->domh*0.92f, s->domw*0.013f);
}

static void sp_init(Scene *sc, i32 w, i32 h){
    SphScState *s=sc->state; s->w=w; s->h=h; s->t=0; s->tilt=1;
    s->domw=40.0f; s->domh=40.0f*(f32)h/(f32)w;   /* match screen aspect */
    s->maxp=4000;
    s->sph=sph_create(s->maxp, s->domw, s->domh);
    s->xy=malloc((size_t)s->maxp*2*sizeof(f32));
    s->speed=malloc((size_t)s->maxp*sizeof(f32));
    sp_seed(s);
}

static void sp_update(Scene *sc, f32 dt, f32 t){
    SphScState *s=sc->state; s->t=t;
    if (s->tilt){
        /* slowly rotate gravity so the fluid sloshes around the box */
        f32 a=t*0.5f;
        sph_set_gravity(s->sph, sinf(a)*12.0f, -cosf(a)*12.0f - 4.0f);
    }
    /* SPH wants small stable steps; substep the frame */
    f32 h=dt; if(h>1.0f/60.0f) h=1.0f/60.0f;
    for (int k=0;k<3;++k) sph_step(s->sph, h*0.5f);
}

static void sp_render(Scene *sc, Framebuffer *fb){
    SphScState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.03f,0.06f));
    i32 n=sph_count(s->sph);
    sph_positions(s->sph, s->xy, s->speed);
    f32 sx=(f32)fb->w/s->domw, sy=(f32)fb->h/s->domh;
    for (i32 i=0;i<n;++i){
        f32 px=s->xy[i*2]*sx;
        f32 py=fb->h - s->xy[i*2+1]*sy;   /* flip y */
        f32 spd=s->speed[i];
        /* bright water-blue (slow) -> cyan -> white foam (fast) */
        f32 f=ct_clampf(spd/10.0f, 0.0f, 1.0f);
        Color3 c=col_lerp(col3(0.15f,0.45f,1.0f), col3(0.8f,0.98f,1.0f), f);
        c=col_add(c, col_scale(col3(1,1,1), f*f*0.7f));   /* foam glow */
        /* splat a filled disc (5 taps) so the fluid reads as a continuous body */
        fb_splat(fb, px,   py,   c);
        fb_splat(fb, px+1, py,   col_scale(c,0.6f));
        fb_splat(fb, px-1, py,   col_scale(c,0.6f));
        fb_splat(fb, px,   py+1, col_scale(c,0.6f));
        fb_splat(fb, px,   py-1, col_scale(c,0.6f));
    }
}

static void sp_key(Scene *sc, int key){
    SphScState *s=sc->state;
    if (key==KEY_TAB) s->tilt=!s->tilt;
    else if (key==KEY_SPACE){
        /* splash: add a burst of particles at the top center */
        for (int i=0;i<80 && sph_count(s->sph)<s->maxp; ++i)
            sph_add(s->sph, s->domw*0.5f + (i%9)*0.2f, s->domh*0.9f);
    } else if (key==KEY_R){
        sph_destroy(s->sph); s->sph=sph_create(s->maxp,s->domw,s->domh); sp_seed(s);
    }
}

static CrtConfig sp_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.5f; c.persistence=0.25f; return c; }
static void sp_destroy(Scene *sc){ if(sc){ SphScState*s=sc->state; if(s->sph)sph_destroy(s->sph); free(s->xy); free(s->speed); free(s); free(sc);} }

Scene *scene_sph_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="sph";
    sc->description="Smoothed-particle hydrodynamics fluid (sloshing, splashes, foam)";
    sc->state=calloc(1,sizeof(SphScState));
    sc->init=sp_init; sc->update=sp_update; sc->render=sp_render;
    sc->on_key=sp_key; sc->destroy=sp_destroy; sc->preferred_crt=sp_crt;
    return sc;
}
