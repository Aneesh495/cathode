/* ==========================================================================
 * scene_pendulum.c  -  a field of double pendulums demonstrating chaos.
 *
 * Each screen column seeds a double pendulum whose initial angle differs by an
 * infinitesimal amount from its neighbor. All are integrated with the exact
 * double-pendulum equations of motion (RK4). Because the double pendulum is
 * chaotic, states that start indistinguishably close diverge exponentially  - 
 * so an initially smooth band of color dissolves into shimmering turbulence.
 * We map each pendulum's angle to a hue; the whole thing is a living picture of
 * sensitive dependence on initial conditions.
 *
 * Equations of motion: the standard Lagrangian double pendulum (equal-length
 * arms here for clarity), integrated with classical RK4 for stability.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { f64 a1, a2, w1, w2; } Pend;   /* angles + angular velocities */

typedef struct {
    Pend *p; int n;
    i32 w, h; f32 t;
    f64 g, m1, m2, l1, l2;
    int mode;   /* 0 = angle field, 1 = trace tips */
} PendState;

/* double-pendulum derivatives (equal mass/length simplifies but we keep params) */
static Pend deriv(PendState *s, Pend y){
    f64 m1=s->m1,m2=s->m2,l1=s->l1,l2=s->l2,g=s->g;
    f64 d=y.a1-y.a2;
    f64 den1=(m1+m2)*l1 - m2*l1*cos(d)*cos(d);
    f64 den2=(l2/l1)*den1;
    Pend o;
    o.a1=y.w1; o.a2=y.w2;
    o.w1=( m2*l1*y.w1*y.w1*sin(d)*cos(d)
         + m2*g*sin(y.a2)*cos(d)
         + m2*l2*y.w2*y.w2*sin(d)
         - (m1+m2)*g*sin(y.a1) ) / den1;
    o.w2=( -m2*l2*y.w2*y.w2*sin(d)*cos(d)
         + (m1+m2)*(g*sin(y.a1)*cos(d)
         - l1*y.w1*y.w1*sin(d)
         - g*sin(y.a2)) ) / den2;
    return o;
}
static Pend rk4(PendState *s, Pend y, f64 h){
    Pend k1=deriv(s,y);
    Pend y2={y.a1+k1.a1*h/2,y.a2+k1.a2*h/2,y.w1+k1.w1*h/2,y.w2+k1.w2*h/2};
    Pend k2=deriv(s,y2);
    Pend y3={y.a1+k2.a1*h/2,y.a2+k2.a2*h/2,y.w1+k2.w1*h/2,y.w2+k2.w2*h/2};
    Pend k3=deriv(s,y3);
    Pend y4={y.a1+k3.a1*h,y.a2+k3.a2*h,y.w1+k3.w1*h,y.w2+k3.w2*h};
    Pend k4=deriv(s,y4);
    Pend o;
    o.a1=y.a1+(k1.a1+2*k2.a1+2*k3.a1+k4.a1)*h/6;
    o.a2=y.a2+(k1.a2+2*k2.a2+2*k3.a2+k4.a2)*h/6;
    o.w1=y.w1+(k1.w1+2*k2.w1+2*k3.w1+k4.w1)*h/6;
    o.w2=y.w2+(k1.w2+2*k2.w2+2*k3.w2+k4.w2)*h/6;
    return o;
}

static void pd_seed(PendState *s){
    for (int i=0;i<s->n;++i){
        /* Released from high energy (both arms up) where the double pendulum is
         * most violently chaotic. A modest per-column spread across the array
         * makes neighbors' exponential divergence visible within a second or
         * two  -  the whole band shears from smooth into turbulence. */
        f64 base=CT_PI + 0.4;              /* both arms up-ish: max chaos */
        f64 eps=(f64)i/(f64)s->n * 0.5;    /* 0..0.5 rad spread across columns */
        s->p[i].a1=base+eps; s->p[i].a2=base+eps*0.5;
        s->p[i].w1=0; s->p[i].w2=0;
    }
}

static void pd_init(Scene *sc, i32 w, i32 h){
    PendState *s=sc->state; s->w=w; s->h=h; s->t=0; s->mode=0;
    s->g=9.81; s->m1=1; s->m2=1; s->l1=1; s->l2=1;
    s->n=w;                     /* one pendulum per column */
    s->p=malloc(sizeof(Pend)*s->n);
    pd_seed(s);
}

static void pd_update(Scene *sc, f32 dt, f32 t){
    PendState *s=sc->state; s->t=t;
    f64 h=dt; if(h>1.0/60.0)h=1.0/60.0;
    /* substep for stability of the stiff chaotic system */
    for (int k=0;k<4;++k)
        for (int i=0;i<s->n;++i)
            s->p[i]=rk4(s, s->p[i], h*0.25);
}

static Color3 hue(f32 x){
    x=x-floorf(x);
    return col3(0.5f+0.5f*sinf(x*CT_TAU), 0.5f+0.5f*sinf(x*CT_TAU+2.094f), 0.5f+0.5f*sinf(x*CT_TAU+4.188f));
}

static void pd_render(Scene *sc, Framebuffer *fb){
    PendState *s=sc->state;
    if (s->mode==0){
        /* angle field: column i, color by (a1,a2) mapped to hue; vertical bands
         * so the divergence reads as the smooth->turbulent transition */
        for (i32 x=0;x<fb->w;++x){
            int i=x<s->n?x:s->n-1;
            f32 hn=(f32)(sin(s->p[i].a1)*0.5+0.5);
            f32 h2=(f32)(sin(s->p[i].a2)*0.5+0.5);
            Color3 top=hue(hn), bot=hue(h2*0.5f+0.25f);
            for (i32 y=0;y<fb->h;++y){
                f32 f=(f32)y/fb->h;
                fb_set(fb,x,y, col_lerp(top,bot,f));
            }
        }
    } else {
        /* trace the pendulum tips as glowing points over a dark field */
        fb_clear(fb, col3(0.01f,0.01f,0.03f));
        f64 cx=fb->w*0.5, cy=fb->h*0.32; f64 scale=(fb->h)*0.2;
        for (int i=0;i<s->n;++i){
            f64 x1=cx+sin(s->p[i].a1)*s->l1*scale;
            f64 y1=cy+cos(s->p[i].a1)*s->l1*scale;
            f64 x2=x1+sin(s->p[i].a2)*s->l2*scale;
            f64 y2=y1+cos(s->p[i].a2)*s->l2*scale;
            Color3 c=hue((f32)i/s->n);
            fb_splat(fb,(f32)x2,(f32)y2, col_scale(c,0.8f));
        }
    }
}

static void pd_key(Scene *sc, int key){
    PendState *s=sc->state;
    if (key==KEY_TAB) s->mode=!s->mode;
    else if (key==KEY_R) pd_seed(s);
}

static CrtConfig pd_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.45f; return c; }
static void pd_destroy(Scene *sc){ if(sc){ PendState*s=sc->state; free(s->p); free(s); free(sc);} }

Scene *scene_pendulum_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="pendulum";
    sc->description="Double-pendulum chaos array (sensitive dependence, RK4)";
    sc->state=calloc(1,sizeof(PendState));
    sc->init=pd_init; sc->update=pd_update; sc->render=pd_render;
    sc->on_key=pd_key; sc->destroy=pd_destroy; sc->preferred_crt=pd_crt;
    return sc;
}
