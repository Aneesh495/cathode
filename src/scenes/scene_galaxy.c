/* ==========================================================================
 * scene_galaxy.c — live Barnes-Hut N-body galaxy / two-galaxy collision.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/physics.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    NBody *nb;
    i32 w,h;
    f32 t;
    int collision;
    i32 count;
} GalaxyState;

static void seed(GalaxyState*s){
    if (s->nb) nbody_destroy(s->nb);
    s->nb=nbody_create(s->count*2+8);
    s->nb->g=1.0f; s->nb->theta=0.6f; s->nb->softening=0.08f;
    if (s->collision) nbody_seed_collision(s->nb, s->count/2);
    else nbody_seed_galaxy(s->nb, s->count, v3(0,0,0), 3.0f, (f32)s->count*3.0f);
}

static void gx_init(Scene*sc,i32 w,i32 h){
    GalaxyState*s=sc->state; s->w=w;s->h=h;s->t=0;s->collision=0;s->count=800;
    seed(s);
}
static void gx_update(Scene*sc,f32 dt,f32 t){
    GalaxyState*s=sc->state; s->t=t;
    f32 step = dt<0.05f?dt:0.05f;
    nbody_step(s->nb, step*0.6f);
}
static void gx_render(Scene*sc, Framebuffer*fb){
    GalaxyState*s=sc->state;
    fb_clear(fb, col3(0.01f,0.01f,0.03f));
    /* slowly orbiting camera */
    f32 a=s->t*0.15f;
    Vec3 eye=v3(cosf(a)*11.0f, 4.0f+2.0f*sinf(a*0.5f), sinf(a)*11.0f);
    Mat4 view=mat4_look_at(eye, v3(0,0,0), v3(0,1,0));
    Mat4 proj=mat4_perspective(55.0f*CT_DEG2RAD, (f32)fb->w/fb->h, 0.1f, 200.0f);
    nbody_render(s->nb, fb, view, proj);
}
static void gx_key(Scene*sc,int key){
    GalaxyState*s=sc->state;
    if(key==KEY_ENTER||key==KEY_TAB){ s->collision=!s->collision; seed(s); s->t=0; }
    else if(key==KEY_PLUS){ s->count=(i32)(s->count*1.4f); if(s->count>4000)s->count=4000; seed(s); }
    else if(key==KEY_MINUS){ s->count=(i32)(s->count/1.4f); if(s->count<100)s->count=100; seed(s); }
    else if(key==KEY_R){ seed(s); s->t=0; }
}
static CrtConfig gx_crt(Scene*sc){(void)sc;return crt_config_preset("broadcast");}
static void gx_destroy(Scene*sc){ if(sc){ GalaxyState*s=sc->state; if(s->nb)nbody_destroy(s->nb); free(s); free(sc);} }

Scene *scene_galaxy_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="galaxy"; sc->description="Live Barnes-Hut N-body galaxy / collision";
    sc->state=calloc(1,sizeof(GalaxyState));
    sc->init=gx_init;sc->update=gx_update;sc->render=gx_render;sc->on_key=gx_key;
    sc->destroy=gx_destroy;sc->preferred_crt=gx_crt;
    return sc;
}
