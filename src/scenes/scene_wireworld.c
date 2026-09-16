/* ==========================================================================
 * scene_wireworld.c  -  Wireworld cellular automaton (Silverman, 1987).
 *
 * A 4-state CA that models electronics: cells are EMPTY, WIRE (conductor),
 * HEAD (electron head), or TAIL (electron tail). Rules per tick:
 *   empty        -> empty
 *   head         -> tail
 *   tail         -> wire
 *   wire         -> head IFF exactly 1 or 2 of its 8 neighbors are heads
 * Electrons (head→tail pairs) travel along wires; the 1-or-2 rule makes wires
 * act as diodes and enables logic gates. We build a self-running circuit  - 
 * loops of wire seeded with electrons  -  that pulses forever. HEAD/TAIL glow;
 * wire is dim copper. A living circuit board.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

enum { WW_EMPTY=0, WW_HEAD=1, WW_TAIL=2, WW_WIRE=3 };

typedef struct {
    u8 *cur, *nxt;
    i32 gw, gh;
    i32 w, h; f32 t; f32 accum, step_dt;
    Rng rng;
    int palette;
} WwState;

/* Lay down a generative circuit: a grid of wire loops (rings) with a few
 * electrons injected, so signals chase around the rings and through junctions
 * indefinitely. Deterministic from the seed. */
static void ww_build(WwState *s){
    i32 gw=s->gw, gh=s->gh;
    memset(s->cur, WW_EMPTY, (size_t)gw*gh);
    /* tile the plane with rectangular wire loops */
    int cell=14;
    for (int by=2; by+cell<gh-2; by+=cell){
        for (int bx=2; bx+cell<gw-2; bx+=cell){
            int x0=bx,y0=by,x1=bx+cell-4,y1=by+cell-4;
            for (int x=x0;x<=x1;++x){ s->cur[y0*gw+x]=WW_WIRE; s->cur[y1*gw+x]=WW_WIRE; }
            for (int y=y0;y<=y1;++y){ s->cur[y*gw+x0]=WW_WIRE; s->cur[y*gw+x1]=WW_WIRE; }
            /* connect adjacent loops with a short wire bridge (makes a mesh) */
            if (bx+cell < gw-2) for(int x=x1;x<=x1+4 && x<gw;++x) s->cur[((y0+y1)/2)*gw+x]=WW_WIRE;
            /* inject one electron (head+tail) on this loop to start a pulse */
            if ((rng_f32(&s->rng)<0.6f)){
                s->cur[y0*gw+x0+1]=WW_HEAD; s->cur[y0*gw+x0+2]=WW_TAIL;
            }
        }
    }
}

static void ww_init(Scene *sc, i32 w, i32 h){
    WwState *s=sc->state; s->w=w; s->h=h; s->t=0; s->accum=0; s->step_dt=0.07f; s->palette=0;
    s->gw=w; s->gh=h;
    s->cur=calloc((size_t)w*h,1); s->nxt=calloc((size_t)w*h,1);
    rng_seed(&s->rng, 0x51EED12ULL);
    ww_build(s);
}

static inline int wclamp(int v,int n){ return v<0?0:(v>=n?n-1:v); }

static void ww_tick(WwState *s){
    i32 gw=s->gw, gh=s->gh;
    for (i32 y=0;y<gh;++y){
        for (i32 x=0;x<gw;++x){
            u8 st=s->cur[y*gw+x];
            u8 out=st;
            switch(st){
                case WW_EMPTY: out=WW_EMPTY; break;
                case WW_HEAD:  out=WW_TAIL;  break;
                case WW_TAIL:  out=WW_WIRE;  break;
                case WW_WIRE: {
                    int heads=0;
                    for (int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
                        if(!dx&&!dy)continue;
                        int nx=wclamp(x+dx,gw), ny=wclamp(y+dy,gh);
                        if (s->cur[ny*gw+nx]==WW_HEAD) heads++;
                    }
                    out=(heads==1||heads==2)?WW_HEAD:WW_WIRE;
                } break;
            }
            s->nxt[y*gw+x]=out;
        }
    }
    u8 *t=s->cur; s->cur=s->nxt; s->nxt=t;
}

static void ww_update(Scene *sc, f32 dt, f32 t){
    WwState *s=sc->state; s->t=t; s->accum+=dt;
    int steps=0;
    while (s->accum>=s->step_dt && steps<3){ ww_tick(s); s->accum-=s->step_dt; steps++; }
}

static void ww_render(Scene *sc, Framebuffer *fb){
    WwState *s=sc->state;
    Color3 cw = (s->palette==0)?col3(0.25f,0.12f,0.03f):col3(0.05f,0.12f,0.15f); /* wire */
    for (i32 i=0;i<fb->w*fb->h;++i){
        Color3 c;
        switch(s->cur[i]){
            case WW_EMPTY: c=col3(0.01f,0.01f,0.02f); break;
            case WW_WIRE:  c=cw; break;
            case WW_HEAD:  c=col3(1.0f,1.0f,0.6f); break;   /* bright electron head */
            case WW_TAIL:  c=col3(0.9f,0.35f,0.1f); break;  /* fading tail */
            default:       c=col3(0,0,0);
        }
        fb->px[i*3+0]=c.r; fb->px[i*3+1]=c.g; fb->px[i*3+2]=c.b;
    }
}

static void ww_key(Scene *sc, int key){
    WwState *s=sc->state;
    if (key==KEY_TAB) s->palette=!s->palette;
    else if (key==KEY_R){ s->rng.s[0]^=0x9E3779B9u; ww_build(s); }
    else if (key==KEY_PLUS) s->step_dt*=0.7f;
    else if (key==KEY_MINUS) s->step_dt/=0.7f;
}

static CrtConfig ww_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.55f; c.persistence=0.3f; return c; }
static void ww_destroy(Scene *sc){ if(sc){ WwState*s=sc->state; free(s->cur);free(s->nxt); free(s); free(sc);} }

Scene *scene_wireworld_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="wireworld";
    sc->description="Wireworld CA: electrons flowing through a self-pulsing circuit mesh";
    sc->state=calloc(1,sizeof(WwState));
    sc->init=ww_init; sc->update=ww_update; sc->render=ww_render;
    sc->on_key=ww_key; sc->destroy=ww_destroy; sc->preferred_crt=ww_crt;
    return sc;
}
