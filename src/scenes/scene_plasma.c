/* ==========================================================================
 * scene_plasma.c — classic demoscene plasma: sum of sines + fbm, palette-mapped.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>

typedef struct { i32 w,h; f32 t; int palette; f32 complexity; } PlasmaState;

/* HSV->RGB, h in [0,1) */
static Color3 hsv(f32 h, f32 s, f32 v){
    h=h-floorf(h);
    f32 i=floorf(h*6.0f);
    f32 fr=h*6.0f-i;
    f32 p=v*(1-s), q=v*(1-s*fr), t=v*(1-s*(1-fr));
    switch(((int)i)%6){
        case 0: return col3(v,t,p);
        case 1: return col3(q,v,p);
        case 2: return col3(p,v,t);
        case 3: return col3(p,q,v);
        case 4: return col3(t,p,v);
        default:return col3(v,p,q);
    }
}

static Color3 apply_palette(int pal, f32 x){
    x=x-floorf(x);
    switch(pal){
        case 0: return hsv(x, 0.85f, 1.0f);
        case 1: /* fire */ return col3(ct_clampf(x*2.0f,0,1), ct_clampf(x*1.5f-0.3f,0,1), ct_clampf(x*3.0f-2.0f,0,1));
        case 2: /* ocean */ return col3(ct_clampf(x-0.2f,0,1)*0.5f, ct_clampf(x,0,1)*0.8f, ct_clampf(x+0.2f,0,1));
        default:return hsv(x*0.5f+0.5f, 0.7f, 1.0f);
    }
}

static void pl_init(Scene*sc,i32 w,i32 h){ PlasmaState*s=sc->state; s->w=w;s->h=h;s->t=0;s->palette=0;s->complexity=1.0f; }
static void pl_update(Scene*sc,f32 dt,f32 t){ (void)dt; ((PlasmaState*)sc->state)->t=t; }

static void pl_render(Scene*sc, Framebuffer*fb){
    PlasmaState*s=sc->state;
    f32 t=s->t;
    f32 cx1=fb->w*(0.5f+0.3f*sinf(t*0.6f)), cy1=fb->h*(0.5f+0.3f*cosf(t*0.5f));
    f32 cx2=fb->w*(0.5f+0.4f*sinf(t*0.33f+2.0f)), cy2=fb->h*(0.5f+0.35f*cosf(t*0.42f));
    f32 k=s->complexity;
    for (i32 y=0;y<fb->h;++y){
        for (i32 x=0;x<fb->w;++x){
            f32 fx=(f32)x, fy=(f32)y;
            f32 val = sinf(fx*0.04f*k + t)
                    + sinf(fy*0.05f*k - t*0.8f)
                    + sinf((fx+fy)*0.03f*k + t*0.6f);
            f32 d1=sqrtf((fx-cx1)*(fx-cx1)+(fy-cy1)*(fy-cy1));
            f32 d2=sqrtf((fx-cx2)*(fx-cx2)+(fy-cy2)*(fy-cy2));
            val += sinf(d1*0.06f - t*1.2f) + sinf(d2*0.05f + t);
            /* organic detail */
            val += 0.8f*fbm2(fx*0.01f*k + t*0.1f, fy*0.01f*k, 4, 2.0f, 0.5f);
            f32 hue = val*0.125f + t*0.05f;
            fb_set(fb,x,y, apply_palette(s->palette, hue));
        }
    }
}

static void pl_key(Scene*sc,int key){
    PlasmaState*s=sc->state;
    if(key==KEY_PLUS) s->complexity*=1.2f;
    else if(key==KEY_MINUS) s->complexity/=1.2f;
    else if(key==KEY_ENTER||key==KEY_TAB) s->palette=(s->palette+1)%4;
    if(s->complexity<0.2f)s->complexity=0.2f; if(s->complexity>6)s->complexity=6;
}
static CrtConfig pl_crt(Scene*sc){(void)sc;return crt_config_preset("broadcast");}
static void pl_destroy(Scene*sc){ if(sc){free(sc->state);free(sc);} }

Scene *scene_plasma_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="plasma"; sc->description="Animated multi-sine + fbm plasma with palette cycling";
    sc->state=calloc(1,sizeof(PlasmaState));
    sc->init=pl_init;sc->update=pl_update;sc->render=pl_render;sc->on_key=pl_key;
    sc->destroy=pl_destroy;sc->preferred_crt=pl_crt;
    return sc;
}
