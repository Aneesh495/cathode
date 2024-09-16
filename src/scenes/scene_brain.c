/* ==========================================================================
 * scene_brain.c  -  Brian's Brain, a 3-state cellular automaton (Brian Silverman).
 *
 * Cells are OFF, ON (firing), or DYING. Rules per tick:
 *   OFF   -> ON    iff exactly 2 of its 8 neighbors are ON
 *   ON    -> DYING (always)
 *   DYING -> OFF   (always)
 * The forced ON→DYING→OFF refractory cycle means nothing ever stays lit, so  - 
 * unlike Life  -  the field never settles: it teems with perpetual gliders and
 * exploding fronts. ON cells flash bright, DYING cells glow as an afterimage
 * that the CRT phosphor smears into comet trails.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { BB_OFF=0, BB_ON=1, BB_DYING=2 };

typedef struct {
    u8 *cur, *nxt;
    i32 gw, gh;
    i32 w, h; f32 t; f32 accum, step_dt;
    Rng rng;
    int palette;
} BrainState;

static void bb_seed(BrainState *s){
    for (i32 i=0;i<s->gw*s->gh;++i)
        s->cur[i] = (rng_f32(&s->rng)<0.18f) ? BB_ON : BB_OFF;
}

static void bb_init(Scene *sc, i32 w, i32 h){
    BrainState *s=sc->state; s->w=w; s->h=h; s->t=0; s->accum=0; s->step_dt=0.06f; s->palette=0;
    s->gw=w; s->gh=h;
    s->cur=calloc((size_t)w*h,1); s->nxt=calloc((size_t)w*h,1);
    rng_seed(&s->rng, 0xB2A1U);
    bb_seed(s);
}

static inline int wr(int v,int n){ v%=n; return v<0?v+n:v; }

static void bb_tick(BrainState *s){
    i32 gw=s->gw, gh=s->gh;
    for (i32 y=0;y<gh;++y){
        i32 ym=wr(y-1,gh)*gw, y0=y*gw, yp=wr(y+1,gh)*gw;
        for (i32 x=0;x<gw;++x){
            i32 xm=wr(x-1,gw), xp=wr(x+1,gw);
            u8 st=s->cur[y0+x];
            u8 out;
            if (st==BB_ON) out=BB_DYING;
            else if (st==BB_DYING) out=BB_OFF;
            else {
                int on = (s->cur[ym+xm]==BB_ON)+(s->cur[ym+x]==BB_ON)+(s->cur[ym+xp]==BB_ON)
                       + (s->cur[y0+xm]==BB_ON)                        +(s->cur[y0+xp]==BB_ON)
                       + (s->cur[yp+xm]==BB_ON)+(s->cur[yp+x]==BB_ON)+(s->cur[yp+xp]==BB_ON);
                out = (on==2)?BB_ON:BB_OFF;
            }
            s->nxt[y0+x]=out;
        }
    }
    u8 *t=s->cur; s->cur=s->nxt; s->nxt=t;
}

static void bb_update(Scene *sc, f32 dt, f32 t){
    BrainState *s=sc->state; s->t=t; s->accum+=dt;
    int steps=0;
    while (s->accum>=s->step_dt && steps<3){ bb_tick(s); s->accum-=s->step_dt; steps++; }
    /* occasional reseed keeps it lively if it thins out */
    if (((i32)(t*4.0f))%400==0 && t>1.0f){
        int on=0; for(i32 i=0;i<s->gw*s->gh;++i) on+=(s->cur[i]==BB_ON);
        if (on < s->gw*s->gh/500) bb_seed(s);
    }
}

static void bb_render(Scene *sc, Framebuffer *fb){
    BrainState *s=sc->state;
    Color3 on, dying;
    switch(s->palette){
        case 0: on=col3(0.6f,0.9f,1.0f); dying=col3(0.1f,0.2f,0.5f); break;  /* icy */
        case 1: on=col3(1.0f,0.95f,0.7f); dying=col3(0.4f,0.15f,0.05f); break;/* ember */
        default:on=col3(0.7f,1.0f,0.6f); dying=col3(0.1f,0.35f,0.1f); break;  /* phosphor */
    }
    for (i32 i=0;i<fb->w*fb->h;++i){
        Color3 c;
        switch(s->cur[i]){
            case BB_ON:    c=on; break;
            case BB_DYING: c=dying; break;
            default:       c=col3(0.01f,0.01f,0.02f);
        }
        fb->px[i*3+0]=c.r; fb->px[i*3+1]=c.g; fb->px[i*3+2]=c.b;
    }
}

static void bb_key(Scene *sc, int key){
    BrainState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_R) bb_seed(s);
    else if (key==KEY_PLUS) s->step_dt*=0.7f;
    else if (key==KEY_MINUS) s->step_dt/=0.7f;
}

static CrtConfig bb_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.persistence=0.5f; c.bloom=0.5f; return c; }
static void bb_destroy(Scene *sc){ if(sc){ BrainState*s=sc->state; free(s->cur);free(s->nxt); free(s); free(sc);} }

Scene *scene_brain_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="brain";
    sc->description="Brian's Brain 3-state CA  -  perpetual gliders and firing fronts";
    sc->state=calloc(1,sizeof(BrainState));
    sc->init=bb_init; sc->update=bb_update; sc->render=bb_render;
    sc->on_key=bb_key; sc->destroy=bb_destroy; sc->preferred_crt=bb_crt;
    return sc;
}
