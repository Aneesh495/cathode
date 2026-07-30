/* ==========================================================================
 * scene_attractor.c — chaotic strange attractors traced as glowing point-clouds.
 *
 * Integrates a chaotic ODE (Lorenz, Aizawa, Thomas, Halvorsen) with RK4 and
 * keeps a rolling buffer of the last N points, splatting them additively with
 * an age-based color ramp so the trajectory glows like a phosphor oscilloscope
 * (a Lissajous/vectorscope aesthetic). The camera slowly orbits the strange
 * geometry. Pure C — a standalone twin of the Rust attractor module.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <math.h>

#define TRAIL 6000

typedef enum { A_LORENZ=0, A_AIZAWA=1, A_THOMAS=2, A_HALVORSEN=3, A_COUNT=4 } AttrKind;

typedef struct {
    Vec3 pts[TRAIL];
    i32  head, count;
    Vec3 state;
    AttrKind kind;
    f32 dt_scale;
    i32 w,h; f32 t;
    Vec3 lo, hi;      /* running bounds for auto-framing */
} AttrState;

/* derivative of each system at p */
static Vec3 deriv(AttrKind k, Vec3 p){
    switch(k){
        case A_LORENZ: {
            f32 s=10.0f, r=28.0f, b=8.0f/3.0f;
            return v3(s*(p.y-p.x), p.x*(r-p.z)-p.y, p.x*p.y-b*p.z);
        }
        case A_AIZAWA: {
            f32 a=0.95f,b=0.7f,c=0.6f,d=3.5f,e=0.25f,f=0.1f;
            f32 x=p.x,y=p.y,z=p.z;
            return v3((z-b)*x - d*y,
                      d*x + (z-b)*y,
                      c + a*z - (z*z*z)/3.0f - (x*x+y*y)*(1.0f+e*z) + f*z*x*x*x);
        }
        case A_THOMAS: {
            f32 b=0.208f;
            return v3(sinf(p.y)-b*p.x, sinf(p.z)-b*p.y, sinf(p.x)-b*p.z);
        }
        default: { /* Halvorsen */
            f32 a=1.89f;
            return v3(-a*p.x-4*p.y-4*p.z-p.y*p.y,
                      -a*p.y-4*p.z-4*p.x-p.z*p.z,
                      -a*p.z-4*p.x-4*p.y-p.x*p.x);
        }
    }
}

static Vec3 rk4(AttrKind k, Vec3 p, f32 h){
    Vec3 k1=deriv(k,p);
    Vec3 k2=deriv(k, v3_add(p, v3_scale(k1,h*0.5f)));
    Vec3 k3=deriv(k, v3_add(p, v3_scale(k2,h*0.5f)));
    Vec3 k4=deriv(k, v3_add(p, v3_scale(k3,h)));
    Vec3 sum=v3_add(k1, v3_add(v3_scale(k2,2), v3_add(v3_scale(k3,2), k4)));
    return v3_add(p, v3_scale(sum, h/6.0f));
}

static void reset(AttrState *s){
    s->head=0; s->count=0;
    s->state = (s->kind==A_LORENZ) ? v3(0.1f,0.0f,0.0f)
             : (s->kind==A_THOMAS) ? v3(1.1f,1.1f,-0.5f)
             : v3(0.1f,0.0f,0.0f);
    s->lo=v3(1e9f,1e9f,1e9f); s->hi=v3(-1e9f,-1e9f,-1e9f);
}

static f32 dt_for(AttrKind k){
    switch(k){ case A_LORENZ: return 0.006f; case A_AIZAWA: return 0.012f;
               case A_THOMAS: return 0.03f;  default: return 0.006f; }
}

static void at_init(Scene *sc, i32 w, i32 h){
    AttrState *s=sc->state; s->w=w;s->h=h;s->t=0; s->kind=A_LORENZ; s->dt_scale=1.0f;
    reset(s);
}

static void at_update(Scene *sc, f32 dt, f32 t){
    AttrState *s=sc->state; s->t=t;
    /* fixed number of integration steps per frame for stable shape */
    i32 steps=90;
    f32 h=dt_for(s->kind)*s->dt_scale;
    for (i32 i=0;i<steps;++i){
        s->state=rk4(s->kind, s->state, h);
        s->pts[s->head]=s->state;
        s->head=(s->head+1)%TRAIL;
        if (s->count<TRAIL) s->count++;
        s->lo=v3_min(s->lo,s->state); s->hi=v3_max(s->hi,s->state);
    }
}

static void at_render(Scene *sc, Framebuffer *fb){
    AttrState *s=sc->state;
    fb_clear(fb, col3(0.01f,0.01f,0.02f));
    /* auto-frame: center on bounds, fit */
    Vec3 c=v3_scale(v3_add(s->lo,s->hi),0.5f);
    Vec3 ext=v3_sub(s->hi,s->lo);
    f32 radius=ct_maxf(ext.x,ct_maxf(ext.y,ext.z))*0.6f + 1e-3f;
    f32 a=s->t*0.25f;
    Vec3 eye=v3_add(c, v3(cosf(a)*radius*3.0f, radius*1.2f, sinf(a)*radius*3.0f));
    Mat4 view=mat4_look_at(eye, c, v3(0,1,0));
    Mat4 proj=mat4_perspective(50.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.01f,1000.0f);
    Mat4 vp=mat4_mul(proj,view);
    for (i32 i=0;i<s->count;++i){
        /* age: 0 = oldest, 1 = newest (head-1) */
        i32 idx=( s->head - 1 - i + TRAIL*2 )%TRAIL;
        f32 age=1.0f-(f32)i/(f32)s->count;
        Vec4 clip=mat4_mul_v4(vp, v4_from_v3(s->pts[idx],1.0f));
        if (clip.w<=1e-4f) continue;
        f32 iw=1.0f/clip.w;
        f32 sx=(clip.x*iw*0.5f+0.5f)*fb->w;
        f32 sy=(1.0f-(clip.y*iw*0.5f+0.5f))*fb->h;
        if (sx<0||sx>=fb->w||sy<0||sy>=fb->h) continue;
        /* color ramp along the trail (cool old -> hot new) */
        Color3 col=col_lerp(col3(0.1f,0.3f,0.9f), col3(1.0f,0.9f,0.4f), age);
        fb_splat(fb, sx, sy, col_scale(col, 0.5f+0.8f*age));
    }
}

static const char *kind_name(AttrKind k){
    return k==A_LORENZ?"Lorenz":k==A_AIZAWA?"Aizawa":k==A_THOMAS?"Thomas":"Halvorsen";
}

static void at_key(Scene *sc, int key){
    AttrState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->kind=(s->kind+1)%A_COUNT; reset(s); }
    else if (key==KEY_PLUS) s->dt_scale*=1.25f;
    else if (key==KEY_MINUS) s->dt_scale/=1.25f;
    else if (key==KEY_R) reset(s);
    if (s->dt_scale<0.2f)s->dt_scale=0.2f; if(s->dt_scale>4)s->dt_scale=4;
    (void)kind_name;
}

static CrtConfig at_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.persistence=0.55f; c.bloom=0.5f; return c; }
static void at_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_attractor_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="attractor";
    sc->description="Chaotic strange attractors traced as glowing oscilloscope trails";
    sc->state=calloc(1,sizeof(AttrState));
    sc->init=at_init; sc->update=at_update; sc->render=at_render;
    sc->on_key=at_key; sc->destroy=at_destroy; sc->preferred_crt=at_crt;
    return sc;
}
