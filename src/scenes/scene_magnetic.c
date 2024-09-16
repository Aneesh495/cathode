/* ==========================================================================
 * scene_magnetic.c  -  magnetic-pendulum fractal basins of attraction.
 *
 * A pendulum bob swings over a plane with several magnets. From each starting
 * position it traces a chaotic path  -  pulled by each magnet (inverse-square,
 * softened), a restoring spring toward the origin, and friction  -  until it
 * settles near one magnet. Coloring every starting pixel by *which* magnet it
 * ends on produces the famous fractal basin-of-attraction map: smooth regions
 * near each magnet, but an infinitely intricate fractal boundary between them.
 *
 * We integrate the ODE per pixel (bounded step count), so it's embarrassingly
 * parallel  -  split across cores by the thread pool. The magnet configuration
 * slowly rotates, morphing the whole basin structure. Pure C.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NMAG 3
#define MAX_STEPS 220

typedef struct {
    i32 w, h; f32 t;
    f32 mx[NMAG], my[NMAG];       /* magnet positions */
    Color3 mcol[NMAG];
    ThreadPool *pool;
} MagState;

static void mg_init(Scene *sc, i32 w, i32 h){
    MagState *s=sc->state; s->w=w; s->h=h; s->t=0;
    s->mcol[0]=col3(0.95f,0.35f,0.35f);
    s->mcol[1]=col3(0.35f,0.9f,0.45f);
    s->mcol[2]=col3(0.45f,0.55f,1.0f);
    if (!s->pool) s->pool=tp_create(0);
}
static void mg_update(Scene *sc, f32 dt, f32 t){
    (void)dt; MagState *s=sc->state; s->t=t;
    for (int i=0;i<NMAG;++i){
        f32 a = t*0.25f + i*(CT_TAU/NMAG);
        s->mx[i]=1.15f*cosf(a);
        s->my[i]=1.15f*sinf(a);
    }
}

typedef struct { MagState *s; Framebuffer *fb; } MagJob;

/* integrate one bob; return the index of the magnet it settles nearest, or -1 */
static int simulate(const MagState *s, f32 px, f32 py){
    f32 x=px, y=py, vx=0, vy=0;
    const f32 dt=0.10f, friction=0.22f, k=0.30f, h2=0.22f;  /* softening, spring */
    for (int step=0; step<MAX_STEPS; ++step){
        f32 ax = -k*x - friction*vx;
        f32 ay = -k*y - friction*vy;
        for (int i=0;i<NMAG;++i){
            f32 dx=s->mx[i]-x, dy=s->my[i]-y;
            f32 d2=dx*dx+dy*dy+h2;
            f32 inv=1.0f/(d2*sqrtf(d2));       /* ~1/r^3 * r = 1/r^2 pull */
            ax += dx*inv; ay += dy*inv;
        }
        vx += ax*dt; vy += ay*dt;
        x  += vx*dt; y  += vy*dt;
        /* early-out: slow and close to a magnet => settled */
        if (vx*vx+vy*vy < 0.02f){
            for (int i=0;i<NMAG;++i){
                f32 dx=s->mx[i]-x, dy=s->my[i]-y;
                if (dx*dx+dy*dy < 0.09f) return i;
            }
        }
    }
    /* not clearly settled: pick the nearest magnet */
    int best=-1; f32 bd=1e30f;
    for (int i=0;i<NMAG;++i){
        f32 dx=s->mx[i]-x, dy=s->my[i]-y, d=dx*dx+dy*dy;
        if (d<bd){ bd=d; best=i; }
    }
    return best;
}

static void mg_band(void *ctx, i32 y0, i32 y1){
    MagJob *j=ctx; const MagState *s=j->s; Framebuffer *fb=j->fb;
    const i32 W=fb->w, H=fb->h;
    const f32 span=3.2f;                       /* world half-extent shown */
    for (i32 py=y0; py<y1; ++py){
        f32 wy=((f32)py/H*2.0f-1.0f)*span;
        f32 *row=&fb->px[(size_t)py*W*3];
        for (i32 px=0; px<W; ++px){
            f32 wx=((f32)px/W*2.0f-1.0f)*span*((f32)W/H);
            int m=simulate(s, wx, wy);
            Color3 c = (m>=0)? s->mcol[m] : col3(0,0,0);
            /* shade by distance to that magnet's center for a little depth */
            if (m>=0){
                f32 dx=s->mx[m]-wx, dy=s->my[m]-wy, d=sqrtf(dx*dx+dy*dy);
                f32 sh=0.45f + 0.55f/(1.0f+d*0.8f);
                c=col_scale(c,sh);
            }
            row[3*px+0]=c.r; row[3*px+1]=c.g; row[3*px+2]=c.b;
        }
    }
}

static void mg_render(Scene *sc, Framebuffer *fb){
    MagState *s=sc->state;
    MagJob job={s,fb};
    if (s->pool) tp_run_bands(s->pool, fb->h, mg_band, &job);
    else mg_band(&job,0,fb->h);
    /* draw the magnets as bright rings */
    const i32 W=fb->w,H=fb->h; const f32 span=3.2f;
    for (int i=0;i<NMAG;++i){
        i32 cx=(i32)((s->mx[i]/(span*(f32)W/H)*0.5f+0.5f)*W);
        i32 cy=(i32)((s->my[i]/span*0.5f+0.5f)*H);
        for (int a=0;a<24;++a){
            f32 th=a*(CT_TAU/24);
            fb_add(fb, cx+(i32)(3*cosf(th)), cy+(i32)(3*sinf(th)), col3(1,1,1));
        }
    }
}

static CrtConfig mg_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void mg_destroy(Scene *sc){ if(sc){ MagState*s=sc->state; if(s->pool)tp_destroy(s->pool); free(s); free(sc);} }

Scene *scene_magnetic_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="magnetic";
    sc->description="Magnetic-pendulum fractal basins: per-pixel ODE, colored by which magnet wins (threaded)";
    sc->state=calloc(1,sizeof(MagState));
    sc->init=mg_init; sc->update=mg_update; sc->render=mg_render;
    sc->destroy=mg_destroy; sc->preferred_crt=mg_crt;
    return sc;
}
