/* ==========================================================================
 * scene_starfield.c  -  3D star-warp with motion streaks and an fbm nebula.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>

#define NSTARS 900

typedef struct {
    Vec3 pos;      /* star position; z decreases toward camera */
    f32  px, py;   /* last projected screen pos (for streaks) */
    int  has_prev;
    f32  bright;
} Star;

typedef struct {
    Star stars[NSTARS];
    Rng  rng;
    i32  w,h;
    f32  speed;
    f32  t;
} StarState;

static void respawn(StarState *s, Star *st){
    st->pos = v3(rng_range(&s->rng,-6,6), rng_range(&s->rng,-6,6), rng_range(&s->rng,6.0f,14.0f));
    st->has_prev=0;
    st->bright = rng_range(&s->rng,0.4f,1.0f);
}

static void sf_init(Scene *sc, i32 w, i32 h){
    StarState *s=(StarState*)sc->state;
    s->w=w; s->h=h; s->speed=4.0f; s->t=0;
    rng_seed(&s->rng, 0x57A9F1E7ULL);
    for (int i=0;i<NSTARS;++i) respawn(s,&s->stars[i]);
}

static void sf_update(Scene *sc, f32 dt, f32 t){
    StarState *s=(StarState*)sc->state;
    s->t=t;
    for (int i=0;i<NSTARS;++i){
        Star *st=&s->stars[i];
        st->pos.z -= s->speed*dt;
        if (st->pos.z < 0.2f) respawn(s,st);
    }
}

/* project a star at (x,y,z) to screen using a simple pinhole */
static int project(StarState*s, Vec3 p, f32*ox, f32*oy){
    if (p.z<=0.05f) return 0;
    f32 fov = 1.4f;
    f32 sx = (p.x/p.z)*fov;
    f32 sy = (p.y/p.z)*fov;
    *ox = (sx*0.5f+0.5f)*s->w;
    *oy = (1.0f-(sy*0.5f+0.5f))*s->h;
    return 1;
}

static void sf_render(Scene *sc, Framebuffer *fb){
    StarState *s=(StarState*)sc->state;
    /* nebula backdrop via fbm, slowly drifting */
    for (i32 y=0;y<fb->h;++y){
        for (i32 x=0;x<fb->w;++x){
            f32 u=(f32)x/fb->w*3.0f, v=(f32)y/fb->h*3.0f;
            f32 n=fbm2(u+s->t*0.05f, v-s->t*0.03f, 5, 2.0f, 0.5f);
            n=0.5f+0.5f*n;
            f32 g=n*n*0.10f;
            Color3 neb=col3(g*0.5f, g*0.4f, g*0.9f);
            fb_set(fb,x,y,neb);
        }
    }
    /* stars with streaks */
    for (int i=0;i<NSTARS;++i){
        Star *st=&s->stars[i];
        f32 cx,cy;
        if (!project(s, st->pos, &cx, &cy)) { st->has_prev=0; continue; }
        f32 depth = st->pos.z;
        f32 b = st->bright * ct_clampf(6.0f/depth, 0.15f, 2.2f);
        /* twinkle */
        b *= 0.85f+0.15f*sinf(s->t*10.0f+i);
        /* nearer stars are hotter/whiter; far ones slightly blue */
        Color3 c=col3(b, b, b*1.1f);
        if (st->has_prev){
            /* draw a streak from prev to current (motion blur) */
            f32 dx=cx-st->px, dy=cy-st->py;
            int steps=(int)(fabsf(dx)+fabsf(dy)); if(steps<1)steps=1; if(steps>40)steps=40;
            for (int k=0;k<=steps;++k){
                f32 tt=(f32)k/steps;
                fb_splat(fb, st->px+dx*tt, st->py+dy*tt, col_scale(c, 0.3f+0.7f*tt));
            }
        } else {
            fb_splat(fb, cx, cy, c);
        }
        st->px=cx; st->py=cy; st->has_prev=1;
    }
}

static void sf_on_key(Scene *sc, int key){
    StarState *s=(StarState*)sc->state;
    if (key==KEY_PLUS) s->speed*=1.3f;
    else if (key==KEY_MINUS) s->speed/=1.3f;
    if (s->speed<0.5f)s->speed=0.5f; if(s->speed>40)s->speed=40;
}

static CrtConfig sf_crt(Scene *sc){ (void)sc; return crt_config_preset("clean"); }

static void sf_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_starfield_create(void){
    Scene *sc=(Scene*)calloc(1,sizeof(Scene));
    sc->name="starfield";
    sc->description="3D star-warp with motion streaks over an fbm nebula";
    sc->state=calloc(1,sizeof(StarState));
    sc->init=sf_init; sc->update=sf_update; sc->render=sf_render;
    sc->on_key=sf_on_key; sc->destroy=sf_destroy; sc->preferred_crt=sf_crt;
    return sc;
}
