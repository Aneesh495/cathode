/* ==========================================================================
 * scene_bz.c — excitable-media cellular automaton (Belousov–Zhabotinsky-like
 * spiral waves), Greenberg–Hastings model.
 *
 * Each cell is in one of N states: 0 = resting, 1..K = excited (refractory
 * countdown). A resting cell becomes excited (state 1) if enough neighbors are
 * in the leading excited states; an excited cell in state k advances to k+1
 * each tick and returns to rest after state N-1. From random noise this
 * self-organizes into rotating spiral waves and target patterns — the discrete
 * cousin of the BZ chemical oscillator. The refractory gradient maps to a fiery
 * palette; CRT bloom lights the wavefronts.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BZ_STATES 12          /* 0 resting, 1..BZ_STATES-1 refractory */
#define BZ_THRESH 2           /* excited neighbors needed to ignite */

typedef struct {
    u8 *cur, *nxt;
    i32 gw, gh;
    i32 w, h; f32 t; f32 accum; f32 step_dt;
    Rng rng;
    int palette;
} BzState;

static void bz_seed(BzState *s){
    for (i32 i=0;i<s->gw*s->gh;++i)
        s->cur[i] = (rng_f32(&s->rng)<0.12f) ? (u8)(1+(u32)(rng_f32(&s->rng)*(BZ_STATES-1))) : 0;
}

static void bz_init(Scene *sc, i32 w, i32 h){
    BzState *s=sc->state; s->w=w; s->h=h; s->t=0; s->accum=0; s->step_dt=0.05f; s->palette=0;
    s->gw=w; s->gh=h;
    s->cur=calloc((size_t)w*h,1); s->nxt=calloc((size_t)w*h,1);
    rng_seed(&s->rng, 0xB2B2ULL);
    bz_seed(s);
}

static inline int wrp(int v,int n){ v%=n; return v<0?v+n:v; }

static void bz_tick(BzState *s){
    i32 gw=s->gw, gh=s->gh;
    for (i32 y=0;y<gh;++y){
        i32 ym=wrp(y-1,gh)*gw, y0=y*gw, yp=wrp(y+1,gh)*gw;
        for (i32 x=0;x<gw;++x){
            i32 xm=wrp(x-1,gw), xp=wrp(x+1,gw);
            u8 st=s->cur[y0+x];
            if (st==0){
                /* count excited (leading-edge) neighbors */
                int exc=0;
                u8 nb[8]={s->cur[ym+xm],s->cur[ym+x],s->cur[ym+xp],
                          s->cur[y0+xm],           s->cur[y0+xp],
                          s->cur[yp+xm],s->cur[yp+x],s->cur[yp+xp]};
                for (int k=0;k<8;++k) if(nb[k]>=1 && nb[k]<=2) exc++;
                s->nxt[y0+x] = (exc>=BZ_THRESH) ? 1 : 0;
            } else {
                /* refractory: advance, wrap back to resting after last state */
                s->nxt[y0+x] = (st+1 < BZ_STATES) ? (u8)(st+1) : 0;
            }
        }
    }
    u8 *t=s->cur; s->cur=s->nxt; s->nxt=t;
}

static void bz_update(Scene *sc, f32 dt, f32 t){
    BzState *s=sc->state; s->t=t; s->accum+=dt;
    int steps=0;
    while (s->accum>=s->step_dt && steps<4){ bz_tick(s); s->accum-=s->step_dt; steps++; }
}

static Color3 bz_color(int pal, u8 st){
    if (st==0) return col3(0.02f,0.02f,0.05f);
    f32 f=(f32)st/(f32)(BZ_STATES-1);          /* 0..1 through refractory */
    switch(pal){
        case 0: /* fire wavefront: white->orange->dark red as it ages */
            return col_lerp(col3(1.0f,1.0f,0.8f), col3(0.4f,0.05f,0.02f), f);
        case 1: /* electric blue */
            return col_lerp(col3(0.8f,1.0f,1.0f), col3(0.05f,0.1f,0.4f), f);
        default:/* acid */
            return col_lerp(col3(0.9f,1.0f,0.3f), col3(0.1f,0.3f,0.05f), f);
    }
}

static void bz_render(Scene *sc, Framebuffer *fb){
    BzState *s=sc->state;
    for (i32 i=0;i<fb->w*fb->h;++i)
        fb_set(fb, i%fb->w, i/fb->w, bz_color(s->palette, s->cur[i]));
}

static void bz_key(Scene *sc, int key){
    BzState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_R) bz_seed(s);
    else if (key==KEY_PLUS) s->step_dt*=0.7f;
    else if (key==KEY_MINUS) s->step_dt/=0.7f;
}

static CrtConfig bz_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.5f; c.persistence=0.3f; return c; }
static void bz_destroy(Scene *sc){ if(sc){ BzState*s=sc->state; free(s->cur); free(s->nxt); free(s); free(sc);} }

Scene *scene_bz_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="bz";
    sc->description="Excitable-media spiral waves (Greenberg-Hastings / BZ reaction)";
    sc->state=calloc(1,sizeof(BzState));
    sc->init=bz_init; sc->update=bz_update; sc->render=bz_render;
    sc->on_key=bz_key; sc->destroy=bz_destroy; sc->preferred_crt=bz_crt;
    return sc;
}
