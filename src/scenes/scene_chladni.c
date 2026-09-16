/* ==========================================================================
 * scene_chladni.c  -  Chladni plate / cymatics standing-wave patterns.
 *
 * A square plate driven at a resonant frequency forms standing waves; sand
 * sprinkled on it collects along the *nodal lines* where the displacement is
 * zero. For a square plate with free edges the (approximate) mode shapes are
 * sums of products of cosines:
 *
 *   z(x,y) = cos(n πx) cos(m πy) − cos(m πx) cos(n πy)
 *
 * whose zero set is the classic Chladni figure for mode (n,m). We render the
 * plate colored by |z| (bright metal where it vibrates, dark along the nodes),
 * and scatter "sand" particles that random-walk downhill in |z| so they
 * accumulate on the nodal lines over time  -  an actual little cymatics sim. The
 * mode (n,m) sweeps continuously, morphing one figure into the next.
 *
 * Pure C over the framebuffer + a self-contained PRNG for the sand.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NSAND 6000

typedef struct { f32 x, y; } Grain;

typedef struct {
    i32 w, h; f32 t;
    Grain *sand;
    u32 rng;
    f32 n, m;            /* current (fractional) mode */
    f32 target_n, target_m;
    f32 hold;
} ChladniState;

static inline u32 xs(u32 *s){ u32 x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }
static inline f32 frnd(u32 *s){ return (f32)(xs(s)>>8)*(1.0f/16777216.0f); }

/* plate displacement at normalized (u,v) in [0,1] for mode (n,m) */
static inline f32 chladni(f32 u, f32 v, f32 n, f32 m){
    return cosf(n*CT_PI*u)*cosf(m*CT_PI*v) - cosf(m*CT_PI*u)*cosf(n*CT_PI*v);
}

static void ch_init(Scene *sc, i32 w, i32 h){
    ChladniState *s=sc->state; s->w=w; s->h=h; s->t=0; s->rng=0x0CDA7A1u;
    s->sand=malloc(sizeof(Grain)*NSAND);
    for (int i=0;i<NSAND;++i){ s->sand[i].x=frnd(&s->rng); s->sand[i].y=frnd(&s->rng); }
    s->n=3; s->m=2; s->target_n=4; s->target_m=3; s->hold=0;
}

static void ch_update(Scene *sc, f32 dt, f32 t){
    ChladniState *s=sc->state; s->t=t;
    if (dt<=0) return; if (dt>0.05f) dt=0.05f;
    /* ease the mode toward its target; when reached, pick a new target */
    s->n += (s->target_n - s->n)*fminf(dt*0.8f,1.0f);
    s->m += (s->target_m - s->m)*fminf(dt*0.8f,1.0f);
    if (fabsf(s->target_n-s->n)<0.03f && fabsf(s->target_m-s->m)<0.03f){
        s->hold += dt;
        if (s->hold > 2.5f){
            s->hold=0;
            s->target_n = 2 + (xs(&s->rng)%6);
            s->target_m = 2 + (xs(&s->rng)%6);
            /* re-scatter a fraction of the sand so new figures form crisply */
            for (int i=0;i<NSAND;i+=3){ s->sand[i].x=frnd(&s->rng); s->sand[i].y=frnd(&s->rng); }
        }
    }
    /* move each grain a small random step, biased toward lower |z| (nodes) */
    for (int i=0;i<NSAND;++i){
        f32 x=s->sand[i].x, y=s->sand[i].y;
        f32 z0=fabsf(chladni(x,y,s->n,s->m));
        /* sample a few random nearby offsets, hop to the lowest |z| */
        f32 bestx=x, besty=y, bestz=z0;
        for (int k=0;k<4;++k){
            f32 dx=(frnd(&s->rng)-0.5f)*0.035f, dy=(frnd(&s->rng)-0.5f)*0.035f;
            f32 nx=x+dx, ny=y+dy;
            if (nx<0)nx=0; if(nx>1)nx=1; if(ny<0)ny=0; if(ny>1)ny=1;
            f32 z=fabsf(chladni(nx,ny,s->n,s->m));
            if (z<bestz){ bestz=z; bestx=nx; besty=ny; }
        }
        /* plus a little jitter so grains keep exploring */
        s->sand[i].x = bestx + (frnd(&s->rng)-0.5f)*0.004f;
        s->sand[i].y = besty + (frnd(&s->rng)-0.5f)*0.004f;
        if (s->sand[i].x<0)s->sand[i].x=0; if(s->sand[i].x>1)s->sand[i].x=1;
        if (s->sand[i].y<0)s->sand[i].y=0; if(s->sand[i].y>1)s->sand[i].y=1;
    }
}

static void ch_render(Scene *sc, Framebuffer *fb){
    ChladniState *s=sc->state; const i32 W=fb->w,H=fb->h;
    /* fit a square plate centered in the frame */
    i32 side = (W<H?W:H);
    i32 ox=(W-side)/2, oy=(H-side)/2;
    fb_clear(fb, col3(0.015f,0.015f,0.02f));
    /* plate: brightness ~ |z| so the vibrating antinodes glow, nodes dark */
    for (i32 py=0; py<side; ++py){
        f32 v=(f32)py/side;
        for (i32 px=0; px<side; ++px){
            f32 u=(f32)px/side;
            f32 z=fabsf(chladni(u,v,s->n,s->m));
            f32 b=z*0.5f;                    /* metallic sheen */
            Color3 c=col3(0.10f+0.35f*b, 0.13f+0.40f*b, 0.20f+0.55f*b);
            fb_set(fb, ox+px, oy+py, c);
        }
    }
    /* sand grains: bright warm dots (they cluster on the nodes) */
    for (int i=0;i<NSAND;++i){
        i32 px=ox+(i32)(s->sand[i].x*side), py=oy+(i32)(s->sand[i].y*side);
        fb_add(fb, px, py, col3(0.9f,0.82f,0.55f));
    }
}

static void ch_key(Scene *sc, int key){
    ChladniState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->hold=10.0f; }   /* force a mode change next update */
}
static CrtConfig ch_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void ch_destroy(Scene *sc){ if(sc){ ChladniState*s=sc->state; free(s->sand); free(s); free(sc);} }

Scene *scene_chladni_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="chladni";
    sc->description="Chladni cymatics: sand collects on the nodal lines of a vibrating plate";
    sc->state=calloc(1,sizeof(ChladniState));
    sc->init=ch_init; sc->update=ch_update; sc->render=ch_render;
    sc->on_key=ch_key; sc->destroy=ch_destroy; sc->preferred_crt=ch_crt;
    return sc;
}
