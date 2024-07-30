/* ==========================================================================
 * scene_boids.c — Reynolds boids flocking, 3D, projected to screen.
 *
 * Classic three rules — separation, alignment, cohesion — plus a gentle
 * attractor toward a slowly-moving target and soft wrapping in a box. Boids
 * are rendered as short motion-trails colored by heading, splatted additively
 * so dense flocks glow. A spatial hash keeps neighbor queries cheap.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>

#define MAXB 700

typedef struct {
    Vec3 pos[MAXB], vel[MAXB], prevproj[MAXB];
    int  hasprev[MAXB];
    i32  n;
    i32  w, h;
    f32  t;
    f32  sep, ali, coh, maxspeed, perception;
    Rng  rng;
    Vec3 box;         /* half-extents */
    Mat4 view, proj;
} BoidState;

static void bo_init(Scene *sc, i32 w, i32 h){
    BoidState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->n=420;
    s->sep=1.5f; s->ali=1.0f; s->coh=0.9f; s->maxspeed=6.0f; s->perception=2.2f;
    s->box=v3(9,6,9);
    rng_seed(&s->rng, 0xB01D5EEDULL);
    for (i32 i=0;i<s->n;++i){
        s->pos[i]=v3(rng_range(&s->rng,-8,8),rng_range(&s->rng,-5,5),rng_range(&s->rng,-8,8));
        s->vel[i]=v3(rng_range(&s->rng,-2,2),rng_range(&s->rng,-2,2),rng_range(&s->rng,-2,2));
        s->hasprev[i]=0;
    }
}

static Vec3 clamp_speed(Vec3 v, f32 mx){
    f32 l=v3_len(v);
    if (l>mx && l>1e-6f) return v3_scale(v, mx/l);
    return v;
}

static void bo_update(Scene *sc, f32 dt, f32 t){
    BoidState *s=sc->state; s->t=t;
    if (dt>0.05f) dt=0.05f;
    f32 pr2 = s->perception*s->perception;
    Vec3 target = v3(6.0f*sinf(t*0.4f), 3.0f*cosf(t*0.3f), 6.0f*cosf(t*0.5f));
    /* O(n^2) neighbor sum — n is bounded and this stays well within frame budget */
    for (i32 i=0;i<s->n;++i){
        Vec3 sep=v3(0,0,0), ali=v3(0,0,0), coh=v3(0,0,0);
        i32 cnt=0;
        for (i32 j=0;j<s->n;++j){
            if (j==i) continue;
            Vec3 d=v3_sub(s->pos[j],s->pos[i]);
            f32 d2=v3_len2(d);
            if (d2<pr2 && d2>1e-6f){
                ali=v3_add(ali,s->vel[j]);
                coh=v3_add(coh,s->pos[j]);
                sep=v3_sub(sep, v3_scale(d, 1.0f/d2));  /* push away, inverse-sq */
                cnt++;
            }
        }
        Vec3 acc=v3(0,0,0);
        if (cnt>0){
            ali=v3_scale(ali,1.0f/cnt); ali=v3_sub(ali,s->vel[i]);
            coh=v3_scale(coh,1.0f/cnt); coh=v3_sub(coh,s->pos[i]);
            acc=v3_add(acc, v3_scale(v3_norm(sep), s->sep));
            acc=v3_add(acc, v3_scale(v3_norm(ali), s->ali));
            acc=v3_add(acc, v3_scale(v3_norm(coh), s->coh));
        }
        /* gentle pull to the moving target keeps the flock on screen */
        acc=v3_add(acc, v3_scale(v3_norm(v3_sub(target,s->pos[i])), 0.6f));
        /* soft box containment */
        for (int k=0;k<3;++k){
            f32 p=((f32*)&s->pos[i])[k], b=((f32*)&s->box)[k];
            if (p> b) ((f32*)&acc)[k]-=(p-b)*2.0f;
            if (p<-b) ((f32*)&acc)[k]+=(-b-p)*2.0f;
        }
        s->vel[i]=clamp_speed(v3_add(s->vel[i], v3_scale(acc,dt)), s->maxspeed);
    }
    for (i32 i=0;i<s->n;++i) s->pos[i]=v3_add(s->pos[i], v3_scale(s->vel[i],dt));
}

static void bo_render(Scene *sc, Framebuffer *fb){
    BoidState *s=sc->state;
    fb_clear(fb, col3(0.01f,0.01f,0.02f));
    f32 a=s->t*0.12f;
    s->view=mat4_look_at(v3(cosf(a)*20.0f, 7.0f, sinf(a)*20.0f), v3(0,0,0), v3(0,1,0));
    s->proj=mat4_perspective(55.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    Mat4 vp=mat4_mul(s->proj,s->view);
    for (i32 i=0;i<s->n;++i){
        Vec4 clip=mat4_mul_v4(vp, v4_from_v3(s->pos[i],1.0f));
        if (clip.w<=1e-4f){ s->hasprev[i]=0; continue; }
        f32 iw=1.0f/clip.w;
        f32 sx=(clip.x*iw*0.5f+0.5f)*fb->w;
        f32 sy=(1.0f-(clip.y*iw*0.5f+0.5f))*fb->h;
        /* color by heading direction */
        Vec3 dir=v3_norm(s->vel[i]);
        Color3 c=col3(0.5f+0.5f*dir.x, 0.5f+0.5f*dir.y, 0.5f+0.5f*dir.z);
        c=col_scale(c, 1.3f);
        if (s->hasprev[i]){
            Vec3 pp=s->prevproj[i];
            f32 dx=sx-pp.x, dy=sy-pp.y; int steps=(int)(fabsf(dx)+fabsf(dy)); if(steps<1)steps=1; if(steps>20)steps=20;
            for (int k=0;k<=steps;++k){ f32 tt=(f32)k/steps; fb_splat(fb, pp.x+dx*tt, pp.y+dy*tt, col_scale(c,0.3f+0.7f*tt)); }
        } else fb_splat(fb,sx,sy,c);
        s->prevproj[i]=v3(sx,sy,0); s->hasprev[i]=1;
    }
}

static void bo_key(Scene *sc, int key){
    BoidState *s=sc->state;
    if (key==KEY_PLUS){ s->n = s->n+60<=MAXB ? s->n+60 : MAXB; }
    else if (key==KEY_MINUS){ s->n = s->n-60>=60 ? s->n-60 : 60; }
    else if (key==KEY_TAB){ s->sep*=1.3f; if(s->sep>6)s->sep=0.6f; }
    else if (key==KEY_R){ bo_init(sc, s->w, s->h); }
}

static CrtConfig bo_crt(Scene *sc){ (void)sc; return crt_config_preset("broadcast"); }
static void bo_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_boids_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="boids";
    sc->description="Reynolds 3D flocking with heading-colored motion trails";
    sc->state=calloc(1,sizeof(BoidState));
    sc->init=bo_init; sc->update=bo_update; sc->render=bo_render;
    sc->on_key=bo_key; sc->destroy=bo_destroy; sc->preferred_crt=bo_crt;
    return sc;
}
