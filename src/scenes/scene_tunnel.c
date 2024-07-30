/* ==========================================================================
 * scene_tunnel.c — classic demoscene tunnel: per-pixel angle/depth texturing.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>

typedef struct { i32 w,h; f32 t; int tex; f32 speed; int palette; } TunnelState;

static Color3 hsv(f32 h,f32 s,f32 v){
    h=h-floorf(h); f32 i=floorf(h*6),fr=h*6-i;
    f32 p=v*(1-s),q=v*(1-s*fr),tt=v*(1-s*(1-fr));
    switch(((int)i)%6){case 0:return col3(v,tt,p);case 1:return col3(q,v,p);case 2:return col3(p,v,tt);
        case 3:return col3(p,q,v);case 4:return col3(tt,p,v);default:return col3(v,p,q);}
}

static void tn_init(Scene*sc,i32 w,i32 h){ TunnelState*s=sc->state; s->w=w;s->h=h;s->t=0;s->tex=0;s->speed=1.0f;s->palette=0; }
static void tn_update(Scene*sc,f32 dt,f32 t){ (void)dt; ((TunnelState*)sc->state)->t=t; }

static f32 texture(TunnelState*s, f32 u, f32 v){
    switch(s->tex){
        case 0: { /* checker */
            f32 cu=floorf(u*8.0f), cv=floorf(v*8.0f);
            return fmodf(cu+cv,2.0f)<1.0f?1.0f:0.4f;
        }
        case 1: /* fbm */ return 0.5f+0.5f*fbm2(u*4.0f, v*4.0f, 4, 2.0f, 0.5f);
        default:{ /* rings */ return 0.5f+0.5f*sinf(v*30.0f); }
    }
}

static void tn_render(Scene*sc, Framebuffer*fb){
    TunnelState*s=sc->state;
    f32 t=s->t*s->speed;
    /* wobbling center */
    f32 cx=fb->w*0.5f + fb->w*0.08f*sinf(t*0.7f);
    f32 cy=fb->h*0.5f + fb->h*0.08f*cosf(t*0.5f);
    for (i32 y=0;y<fb->h;++y){
        for (i32 x=0;x<fb->w;++x){
            f32 dx=(x-cx), dy=(y-cy);
            f32 r=sqrtf(dx*dx+dy*dy)+1e-3f;
            f32 ang=atan2f(dy,dx)/CT_TAU + 0.5f;   /* 0..1 */
            f32 depth = 1.5f/r * (f32)fb->h;       /* far center */
            f32 u = ang*6.0f;                 /* more cells around the tube */
            f32 v = depth*0.02f + t*0.6f;
            f32 tex=texture(s,u,v);
            /* fade to dark at the far center (depth->inf as r->0) */
            f32 fade=ct_clampf(r/(fb->h*0.5f),0.0f,1.0f);
            f32 hue = v*0.1f + ang*0.2f + t*0.05f;
            Color3 c=hsv(hue + s->palette*0.33f, 0.7f, tex*fade);
            fb_set(fb,x,y,c);
        }
    }
}

static void tn_key(Scene*sc,int key){
    TunnelState*s=sc->state;
    if(key==KEY_PLUS)s->speed*=1.3f; else if(key==KEY_MINUS)s->speed/=1.3f;
    else if(key==KEY_ENTER)s->tex=(s->tex+1)%3;
    else if(key==KEY_TAB)s->palette=(s->palette+1)%3;
    if(s->speed<0.1f)s->speed=0.1f; if(s->speed>8)s->speed=8;
}
static CrtConfig tn_crt(Scene*sc){(void)sc;return crt_config_preset("trinitron");}
static void tn_destroy(Scene*sc){ if(sc){free(sc->state);free(sc);} }

Scene *scene_tunnel_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="tunnel"; sc->description="Infinite textured tunnel (angle/depth mapping)";
    sc->state=calloc(1,sizeof(TunnelState));
    sc->init=tn_init;sc->update=tn_update;sc->render=tn_render;sc->on_key=tn_key;
    sc->destroy=tn_destroy;sc->preferred_crt=tn_crt;
    return sc;
}
