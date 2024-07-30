/* ==========================================================================
 * scene_slime.c — Physarum polycephalum ("slime mold") transport-network sim.
 *
 * Thousands of agents wander a 2D trail field. Each agent samples the trail
 * at three points ahead (left / center / right sensors), steers toward the
 * strongest, moves forward, and deposits trail where it lands. The trail field
 * diffuses (blur) and decays each step. From these three local rules a
 * global, self-optimizing transport network emerges — the same organism that
 * famously reproduced the Tokyo rail map. Pairs beautifully with CRT phosphor.
 *
 * Refs: Jeff Jones, "Characteristics of pattern formation and evolution in
 * approximations of Physarum transport networks" (2010).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { f32 x, y, heading; } Agent;

typedef struct {
    i32 gw, gh;          /* trail-field grid = framebuffer size */
    f32 *trail, *tmp;    /* diffusing chemoattractant field */
    Agent *agents;
    i32 n_agents;
    Rng rng;
    i32 w, h; f32 t;
    /* behavior params */
    f32 sensor_dist, sensor_angle, turn_angle, move_speed, deposit, decay;
    int palette;
} SlimeState;

static void slime_seed(SlimeState *s){
    /* Spread agents across the whole field with fully random headings — this
     * lets the trail-following dynamics discover a global network rather than
     * collapsing a central disc into one clump. */
    for (i32 i=0;i<s->n_agents;++i){
        s->agents[i].x = rng_f32(&s->rng)*s->gw;
        s->agents[i].y = rng_f32(&s->rng)*s->gh;
        s->agents[i].heading = rng_f32(&s->rng)*CT_TAU;
    }
    memset(s->trail, 0, (size_t)s->gw*s->gh*sizeof(f32));
}

static void sl_init(Scene *sc, i32 w, i32 h){
    SlimeState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->palette=0;
    s->gw=w; s->gh=h;
    s->trail=calloc((size_t)w*h,sizeof(f32));
    s->tmp  =calloc((size_t)w*h,sizeof(f32));
    /* scale agent count to area, capped for frame budget */
    s->n_agents = (i32)((f32)w*h*0.18f);
    if (s->n_agents > 90000) s->n_agents=90000;
    if (s->n_agents < 2000)  s->n_agents=2000;
    s->agents=malloc((size_t)s->n_agents*sizeof(Agent));
    rng_seed(&s->rng, 0x511E5EEDULL);
    /* Network-forming regime: modest deposit, strong-ish decay, and a sensor
     * offset large enough that agents track *ridges* rather than piling up.
     * Deposit << 1 with decay ~0.97 keeps the field from saturating into one
     * blob (the failure mode of high deposit / low decay). */
    s->sensor_dist=12.0f; s->sensor_angle=0.4f; s->turn_angle=0.38f;
    s->move_speed=1.0f; s->deposit=0.22f; s->decay=0.965f;
    slime_seed(s);
}

static inline f32 sample(SlimeState *s, f32 x, f32 y){
    /* wrapped nearest sample of the trail field */
    i32 ix=((i32)x % s->gw + s->gw)%s->gw;
    i32 iy=((i32)y % s->gh + s->gh)%s->gh;
    return s->trail[iy*s->gw+ix];
}

static void sl_update(Scene *sc, f32 dt, f32 t){
    SlimeState *s=sc->state; (void)dt; s->t=t;
    i32 gw=s->gw, gh=s->gh;

    /* --- agent sense + steer + move + deposit --- */
    for (i32 i=0;i<s->n_agents;++i){
        Agent *a=&s->agents[i];
        f32 hc=a->heading;
        f32 fl=sample(s, a->x+cosf(hc-s->sensor_angle)*s->sensor_dist,
                          a->y+sinf(hc-s->sensor_angle)*s->sensor_dist);
        f32 fc=sample(s, a->x+cosf(hc)*s->sensor_dist,
                          a->y+sinf(hc)*s->sensor_dist);
        f32 fr=sample(s, a->x+cosf(hc+s->sensor_angle)*s->sensor_dist,
                          a->y+sinf(hc+s->sensor_angle)*s->sensor_dist);
        if (fc>fl && fc>fr){ /* keep heading */ }
        else if (fc<fl && fc<fr){ /* random turn */ a->heading += (rng_f32(&s->rng)<0.5f?-1.0f:1.0f)*s->turn_angle; }
        else if (fl<fr) a->heading += s->turn_angle;
        else if (fr<fl) a->heading -= s->turn_angle;
        /* move */
        a->x += cosf(a->heading)*s->move_speed;
        a->y += sinf(a->heading)*s->move_speed;
        /* wrap */
        if (a->x<0) a->x+=gw; else if (a->x>=gw) a->x-=gw;
        if (a->y<0) a->y+=gh; else if (a->y>=gh) a->y-=gh;
        /* deposit */
        i32 ix=(i32)a->x, iy=(i32)a->y;
        s->trail[iy*gw+ix] += s->deposit;
    }

    /* --- diffuse (3x3 box blur) + decay into tmp, then swap --- */
    f32 decay=s->decay;
    for (i32 y=0;y<gh;++y){
        i32 ym=((y-1)%gh+gh)%gh, yp=(y+1)%gh;
        for (i32 x=0;x<gw;++x){
            i32 xm=((x-1)%gw+gw)%gw, xp=(x+1)%gw;
            f32 sum = s->trail[ym*gw+xm]+s->trail[ym*gw+x]+s->trail[ym*gw+xp]
                    + s->trail[y *gw+xm]+s->trail[y *gw+x]+s->trail[y *gw+xp]
                    + s->trail[yp*gw+xm]+s->trail[yp*gw+x]+s->trail[yp*gw+xp];
            s->tmp[y*gw+x] = (sum*(1.0f/9.0f))*decay;
        }
    }
    f32 *sw=s->trail; s->trail=s->tmp; s->tmp=sw;
}

static Color3 slime_color(int pal, f32 v){
    v=v/(1.0f+v);              /* soft tonemap the unbounded trail */
    switch(pal){
        case 0: /* gold on black */ return col3(v*1.1f, v*0.75f, v*0.25f);
        case 1: /* cyan bio */      return col3(v*0.2f, v*1.0f, v*0.9f);
        default:/* magenta */       return col3(v*1.0f, v*0.2f, v*0.7f);
    }
}

static void sl_render(Scene *sc, Framebuffer *fb){
    SlimeState *s=sc->state;
    for (i32 i=0;i<fb->w*fb->h;++i){
        f32 v=s->trail[i];
        Color3 c=slime_color(s->palette, v*0.5f);
        fb->px[i*3+0]=c.r; fb->px[i*3+1]=c.g; fb->px[i*3+2]=c.b;
    }
}

static void sl_key(Scene *sc, int key){
    SlimeState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_PLUS) s->sensor_angle+=0.1f;
    else if (key==KEY_MINUS) s->sensor_angle-=0.1f;
    else if (key==KEY_R) slime_seed(s);
    s->sensor_angle=ct_clampf(s->sensor_angle,0.1f,1.4f);
}

static CrtConfig sl_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.persistence=0.45f; c.bloom=0.5f; return c; }
static void sl_destroy(Scene *sc){ if(sc){ SlimeState*s=sc->state; free(s->trail);free(s->tmp);free(s->agents); free(s); free(sc);} }

Scene *scene_slime_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="slime";
    sc->description="Physarum slime-mold transport network (agent-based emergence)";
    sc->state=calloc(1,sizeof(SlimeState));
    sc->init=sl_init; sc->update=sl_update; sc->render=sl_render;
    sc->on_key=sl_key; sc->destroy=sl_destroy; sc->preferred_crt=sl_crt;
    return sc;
}
