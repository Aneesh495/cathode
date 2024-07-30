/* ==========================================================================
 * scene_fluid.c — interactive colored fluid (stable Navier-Stokes) with
 * animated swirling emitters.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/physics.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <math.h>

typedef struct { FluidSim*f; i32 w,h; f32 t; f32 visc; f32 diff; int emitters; } FluidScState;

static Color3 hsv(f32 h,f32 s,f32 v){
    h=h-floorf(h); f32 i=floorf(h*6),fr=h*6-i;
    f32 p=v*(1-s),q=v*(1-s*fr),tt=v*(1-s*(1-fr));
    switch(((int)i)%6){case 0:return col3(v,tt,p);case 1:return col3(q,v,p);case 2:return col3(p,v,tt);
        case 3:return col3(p,q,v);case 4:return col3(tt,p,v);default:return col3(v,p,q);}
}

static void fl_init(Scene*sc,i32 w,i32 h){
    FluidScState*s=sc->state; s->w=w;s->h=h;s->t=0;
    s->visc=0.00002f; s->diff=0.00003f; s->emitters=2;
    int nx = w/2; if(nx<32)nx=32; if(nx>160)nx=160;
    int ny = h/2; if(ny<32)ny=32; if(ny>160)ny=160;
    s->f=fluid_create(nx,ny);
}
static void fl_update(Scene*sc,f32 dt,f32 t){
    FluidScState*s=sc->state; s->t=t;
    i32 nx=fluid_nx(s->f), ny=fluid_ny(s->f);
    /* animated emitters injecting swirling colored dye + velocity */
    for (int e=0;e<s->emitters;++e){
        f32 phase = t*0.7f + e*CT_TAU/s->emitters;
        i32 ex = (i32)(nx*(0.5f+0.18f*cosf(phase)));
        i32 ey = (i32)(ny*(0.5f+0.18f*sinf(phase*1.1f)));
        Color3 c = hsv(t*0.1f + e*0.4f, 0.9f, 1.0f);
        /* inject over a small disk */
        for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){
            fluid_add_density(s->f, ex+dx, ey+dy, 6.0f, c);
        }
        /* tangential velocity for swirl */
        f32 vx = -sinf(phase)*30.0f, vy=cosf(phase)*30.0f;
        fluid_add_velocity(s->f, ex, ey, vx, vy);
    }
    f32 step = dt<0.033f?dt:0.033f;
    fluid_step(s->f, step*1.5f, s->visc, s->diff);
}
static void fl_render(Scene*sc, Framebuffer*fb){ fluid_render(((FluidScState*)sc->state)->f, fb); }
static void fl_key(Scene*sc,int key){
    FluidScState*s=sc->state;
    if(key==KEY_PLUS) s->emitters=s->emitters<6?s->emitters+1:6;
    else if(key==KEY_MINUS) s->emitters=s->emitters>1?s->emitters-1:1;
    else if(key==KEY_TAB){ s->visc = s->visc>0.001f?0.00002f:0.01f; }
}
static CrtConfig fl_crt(Scene*sc){(void)sc;return crt_config_preset("vhs");}
static void fl_destroy(Scene*sc){ if(sc){ FluidScState*s=sc->state; if(s->f)fluid_destroy(s->f); free(s); free(sc);} }

Scene *scene_fluid_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="fluid"; sc->description="Interactive colored fluid (stable Navier-Stokes)";
    sc->state=calloc(1,sizeof(FluidScState));
    sc->init=fl_init;sc->update=fl_update;sc->render=fl_render;sc->on_key=fl_key;
    sc->destroy=fl_destroy;sc->preferred_crt=fl_crt;
    return sc;
}
