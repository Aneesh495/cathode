/* ==========================================================================
 * scene_raymarch.c  -  SDF showcase: cycles primitives / mandelbulb / infinite.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/sdf.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "threadpool.h"      /* app-internal: multithreaded band rendering */
#include <stdlib.h>
#include <math.h>

typedef struct { SdfScene sc; i32 w,h; f32 t; int which; ThreadPool *pool; Framebuffer *fb; } RmState;

static SdfFn fields[3];

static void configure(RmState*s){
    SdfScene *c=&s->sc;
    c->field = fields[s->which];
    c->user=NULL;
    c->fov=1.05f;
    c->light_dir=v3_norm(v3(0.6f,0.75f,0.4f));
    c->sky_top=col3(0.25f,0.45f,0.85f);
    c->sky_bottom=col3(0.85f,0.82f,0.75f);
    for(int i=0;i<8;++i) c->mat_albedo[i]=col3(0.8f,0.55f,0.3f);
    c->mat_albedo[0]=col3(0.35f,0.6f,0.35f);  /* ground */
    c->mat_albedo[1]=col3(0.9f,0.35f,0.35f);
    c->mat_albedo[2]=col3(0.4f,0.55f,0.95f);
    c->mat_albedo[3]=col3(0.95f,0.85f,0.3f);
    c->mat_albedo[4]=col3(0.9f,0.6f,0.25f);   /* mandelbulb */
    c->mat_albedo[5]=col3(0.7f,0.4f,0.9f);
    c->mat_albedo[6]=col3(0.5f,0.8f,0.7f);
    c->max_steps=128; c->max_dist=60.0f; c->epsilon=0.0012f; c->aa=1;
}

static void rm_init(Scene*sc,i32 w,i32 h){
    fields[0]=sdf_scene_primitives; fields[1]=sdf_scene_mandelbulb; fields[2]=sdf_scene_infinite;
    RmState*s=sc->state; s->w=w;s->h=h;s->t=0;s->which=0; configure(s);
}
static void rm_update(Scene*sc,f32 dt,f32 t){
    (void)dt; RmState*s=sc->state; s->t=t; s->sc.time=t;
    f32 a=t*0.2f;
    if (s->which==1){ /* mandelbulb: closer orbit */
        s->sc.cam_pos=v3(cosf(a)*2.6f, sinf(a*0.5f)*1.2f, sinf(a)*2.6f);
        s->sc.cam_target=v3(0,0,0);
    } else if (s->which==2){
        s->sc.cam_pos=v3(cosf(a)*0.5f, 1.5f, t*1.5f);
        s->sc.cam_target=v3(0,0.5f, t*1.5f+3.0f);
    } else {
        s->sc.cam_pos=v3(cosf(a)*4.0f, 2.5f, sinf(a)*4.0f);
        s->sc.cam_target=v3(0,-0.1f,0);
    }
}
/* thread-pool band callback: render rows [y0,y1) of the SDF scene */
static void rm_band(void *user, i32 y0, i32 y1){
    RmState *s=(RmState*)user;
    sdf_render_band(s->fb, &s->sc, y0, y1);
}
static void rm_render(Scene*sc, Framebuffer*fb){
    RmState *s=(RmState*)sc->state;
    if (!s->pool) s->pool = tp_create(0);   /* lazily create; 0 = hw threads */
    s->fb = fb;
    /* fan the scanlines across all cores  -  the SDF marcher is embarrassingly
     * parallel per row, and sdf_render_band is exactly this contract. */
    tp_run_bands(s->pool, fb->h, rm_band, s);
}
static void rm_key(Scene*sc,int key){
    RmState*s=sc->state;
    if(key==KEY_ENTER||key==KEY_TAB){ s->which=(s->which+1)%3; configure(s); }
    else if(key==KEY_PLUS){ s->sc.aa=2; }
    else if(key==KEY_MINUS){ s->sc.aa=1; }
}
static CrtConfig rm_crt(Scene*sc){(void)sc;return crt_config_preset("trinitron");}
static void rm_destroy(Scene*sc){ if(sc){ RmState*s=sc->state; if(s->pool)tp_destroy(s->pool); free(s); free(sc);} }

Scene *scene_raymarch_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="raymarch"; sc->description="Ray-marched SDFs: primitives, mandelbulb, infinite field";
    sc->state=calloc(1,sizeof(RmState));
    sc->init=rm_init;sc->update=rm_update;sc->render=rm_render;sc->on_key=rm_key;
    sc->destroy=rm_destroy;sc->preferred_crt=rm_crt;
    return sc;
}
