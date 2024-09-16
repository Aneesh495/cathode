/* ==========================================================================
 * scene_life.c  -  Conway's Game of Life with heat-trail rendering.
 *
 * A toroidal cellular automaton. Live cells glow; recently-dead cells leave a
 * cooling trail (an "age" field), which the CRT phosphor then smears further  - 
 * giving the classic organic bloom of a life simulation on an old monitor.
 * Periodically reseeds with interesting soups / patterns so it never stalls.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    i32 gw, gh;          /* grid dimensions (independent of fb) */
    u8  *cur, *nxt;      /* cell state 0/1 */
    f32 *heat;           /* per-cell heat 0..1, decays over time */
    i32 w, h;
    f32 t, accum;
    f32 step_interval;   /* seconds between generations */
    Rng rng;
    i32 generation;
    i32 palette;
} LifeState;

static void seed_soup(LifeState *s, f32 density){
    for (i32 i=0;i<s->gw*s->gh;++i){
        s->cur[i] = (rng_f32(&s->rng) < density) ? 1 : 0;
        s->heat[i]= s->cur[i] ? 1.0f : 0.0f;
    }
    s->generation=0;
}

static void li_init(Scene *sc, i32 w, i32 h){
    LifeState *s=sc->state;
    s->w=w; s->h=h;
    s->gw = w; s->gh = h;             /* one cell per framebuffer pixel */
    s->cur = calloc(s->gw*s->gh,1);
    s->nxt = calloc(s->gw*s->gh,1);
    s->heat= calloc(s->gw*s->gh,sizeof(f32));
    rng_seed(&s->rng, 0x11FE0000ULL);
    s->step_interval=0.06f; s->accum=0; s->t=0; s->palette=0;
    seed_soup(s, 0.28f);
}

static inline i32 wrap(i32 v, i32 n){ v%=n; return v<0?v+n:v; }

static void li_step(LifeState *s){
    i32 gw=s->gw, gh=s->gh;
    for (i32 y=0;y<gh;++y){
        i32 ym=wrap(y-1,gh)*gw, y0=y*gw, yp=wrap(y+1,gh)*gw;
        for (i32 x=0;x<gw;++x){
            i32 xm=wrap(x-1,gw), xp=wrap(x+1,gw);
            i32 n = s->cur[ym+xm]+s->cur[ym+x]+s->cur[ym+xp]
                  + s->cur[y0+xm]           +s->cur[y0+xp]
                  + s->cur[yp+xm]+s->cur[yp+x]+s->cur[yp+xp];
            u8 alive = s->cur[y0+x];
            u8 next = (alive && (n==2||n==3)) || (!alive && n==3);
            s->nxt[y0+x]=next;
        }
    }
    u8 *tmp=s->cur; s->cur=s->nxt; s->nxt=tmp;
    /* update heat: live cells hot, others cool */
    for (i32 i=0;i<gw*gh;++i){
        if (s->cur[i]) s->heat[i]=1.0f;
        else s->heat[i]*=0.90f;
    }
    s->generation++;
}

static void li_update(Scene *sc, f32 dt, f32 t){
    LifeState *s=sc->state; s->t=t; s->accum+=dt;
    int steps=0;
    while (s->accum >= s->step_interval && steps<4){ li_step(s); s->accum-=s->step_interval; steps++; }
    /* reseed if the population is dying out or every ~1500 generations */
    if (s->generation>1500 || (s->generation>0 && (s->generation%300)==0)){
        i32 pop=0; for(i32 i=0;i<s->gw*s->gh;++i) pop+=s->cur[i];
        if (pop < s->gw*s->gh/200 || s->generation>1500){
            seed_soup(s, 0.25f+0.1f*rng_f32(&s->rng));
        }
    }
}

static Color3 heat_color(int pal, f32 h){
    switch(pal){
        case 0: /* green phosphor */ return col3(0.1f*h, h, 0.25f*h);
        case 1: /* amber */          return col3(h, 0.6f*h, 0.1f*h);
        default:/* ice */            return col3(0.3f*h, 0.7f*h, h);
    }
}

static void li_render(Scene *sc, Framebuffer *fb){
    LifeState *s=sc->state;
    for (i32 y=0;y<fb->h;++y)
        for (i32 x=0;x<fb->w;++x){
            f32 h=s->heat[y*s->gw+x];
            /* live cells full-bright, trails dimmer */
            f32 v = s->cur[y*s->gw+x] ? 1.0f : h*0.6f;
            fb_set(fb,x,y, heat_color(s->palette, v));
        }
}

static void li_key(Scene *sc, int key){
    LifeState *s=sc->state;
    if (key==KEY_PLUS) s->step_interval*=0.7f;
    else if (key==KEY_MINUS) s->step_interval/=0.7f;
    else if (key==KEY_R) seed_soup(s, 0.28f);
    else if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    if (s->step_interval<0.01f)s->step_interval=0.01f;
    if (s->step_interval>0.5f)s->step_interval=0.5f;
}

static CrtConfig li_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.persistence=0.5f; return c; }
static void li_destroy(Scene *sc){ if(sc){ LifeState*s=sc->state; free(s->cur);free(s->nxt);free(s->heat); free(s); free(sc);} }

Scene *scene_life_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="life";
    sc->description="Conway's Game of Life with phosphor heat-trails";
    sc->state=calloc(1,sizeof(LifeState));
    sc->init=li_init; sc->update=li_update; sc->render=li_render;
    sc->on_key=li_key; sc->destroy=li_destroy; sc->preferred_crt=li_crt;
    return sc;
}
