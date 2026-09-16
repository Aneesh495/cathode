/* ==========================================================================
 * scene_lenia.c  -  Lenia: continuous-state, continuous-space cellular automaton.
 *
 * Lenia (Bert Chan, 2019) generalizes Conway's Life to real-valued cells, a
 * smooth ring-shaped convolution kernel, and a smooth growth function. The
 * result is a menagerie of gliding, pulsating, self-repairing "creatures"
 * (orbium, etc.) that look startlingly alive. We run it at a modest grid and
 * upscale; the CRT bloom makes the soft gradients glow.
 *
 *   U   = K * A            (convolution of state A with kernel K)
 *   A' = clamp( A + dt * G(U), 0, 1 )
 *   G(u) = 2*exp(-((u-mu)^2)/(2 sigma^2)) - 1     (Gaussian growth mapping)
 *
 * The kernel is a smooth annulus (ring) whose radial profile is a Gaussian
 * bump; it is normalized so the convolution sums to 1.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define KR 13                     /* kernel radius (cells) */

typedef struct {
    i32 gw, gh;
    f32 *A, *U, *B;               /* state, potential, next */
    f32 kernel[(2*KR+1)*(2*KR+1)];
    f32 ksum;
    f32 mu, sigma, dt;            /* growth params */
    i32 w, h; f32 t;
    Rng rng;
    int palette;
} LeniaState;

static void build_kernel(LeniaState *s){
    /* radial Gaussian ring: peak at r = 0.5 of KR */
    f32 sum=0;
    for (int j=-KR;j<=KR;++j)
        for (int i=-KR;i<=KR;++i){
            f32 r=sqrtf((f32)(i*i+j*j))/(f32)KR;   /* 0..~1.4 */
            f32 kv;
            if (r>1.0f) kv=0.0f;
            else {
                /* ring centered at 0.5 with width 0.15 */
                f32 d=(r-0.5f)/0.15f;
                kv=expf(-0.5f*d*d);
            }
            s->kernel[(j+KR)*(2*KR+1)+(i+KR)]=kv;
            sum+=kv;
        }
    s->ksum=sum;
}

static void lenia_seed(LeniaState *s){
    memset(s->A,0,(size_t)s->gw*s->gh*sizeof(f32));
    /* Seed SMOOTH blobs (no per-cell noise). Lenia's kernel averages away
     * high-frequency noise toward a uniform fixed point, so coherent creatures
     * only emerge from smooth initial conditions. Each blob is a soft bump
     * around the kernel radius scale, at a value near the growth center. */
    int nblobs = 10;
    for (int blob=0; blob<nblobs; ++blob){
        i32 bx=(i32)(rng_f32(&s->rng)*s->gw);
        i32 by=(i32)(rng_f32(&s->rng)*s->gh);
        /* blob radius ~0.5*KR (creature-scale): the survival sweep showed this
         * is what lets U land inside the growth window rather than overshoot. */
        i32 br=(i32)(KR*0.5f) + (i32)(rng_f32(&s->rng)*KR*0.4f);
        f32 amp=0.7f+0.3f*rng_f32(&s->rng);
        for (int j=-br;j<=br;++j)for(int i=-br;i<=br;++i){
            i32 x=((bx+i)%s->gw+s->gw)%s->gw, y=((by+j)%s->gh+s->gh)%s->gh;
            f32 rr=sqrtf((f32)(i*i+j*j))/(f32)br;
            if (rr<1.0f){
                f32 bump=amp*(0.5f+0.5f*cosf(rr*CT_PI)); /* smooth cosine falloff */
                if (bump>s->A[y*s->gw+x]) s->A[y*s->gw+x]=bump;
            }
        }
    }
}

static void le_init(Scene *sc, i32 w, i32 h){
    LeniaState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->palette=0;
    /* run at ~1/2 res both axes for the O(KR^2) convolution to stay cheap */
    s->gw = w/2 < 48 ? 48 : (w/2 > 220 ? 220 : w/2);
    s->gh = h/2 < 48 ? 48 : (h/2 > 220 ? 220 : h/2);
    s->A=calloc((size_t)s->gw*s->gh,sizeof(f32));
    s->U=calloc((size_t)s->gw*s->gh,sizeof(f32));
    s->B=calloc((size_t)s->gw*s->gh,sizeof(f32));
    /* Parameters found by an offline survival sweep (see git history / probe):
     * mu=0.12, sigma=0.015, blob scale 0.5 sustains ~18% live coverage of
     * moving structure indefinitely. Higher mu or narrower blobs collapse to a
     * dead field; wider sigma fills the plate. */
    s->mu=0.12f; s->sigma=0.015f; s->dt=0.10f;
    rng_seed(&s->rng, 0x1E51A000ULL);
    build_kernel(s);
    lenia_seed(s);
}

static void le_update(Scene *sc, f32 dt, f32 t){
    LeniaState *s=sc->state; (void)dt; s->t=t;
    i32 gw=s->gw, gh=s->gh; int K=2*KR+1;
    /* convolve A with kernel -> U (toroidal). O(gw*gh*K*K)  -  grid kept small. */
    for (i32 y=0;y<gh;++y){
        for (i32 x=0;x<gw;++x){
            f32 acc=0;
            for (int j=-KR;j<=KR;++j){
                i32 yy=((y+j)%gh+gh)%gh;
                const f32 *arow=&s->A[yy*gw];
                const f32 *krow=&s->kernel[(j+KR)*K + 0];
                for (int i=-KR;i<=KR;++i){
                    i32 xx=((x+i)%gw+gw)%gw;
                    acc += arow[xx]*krow[i+KR];
                }
            }
            s->U[y*gw+x]=acc/s->ksum;
        }
    }
    /* growth + integrate */
    for (i32 idx=0; idx<gw*gh; ++idx){
        f32 u=s->U[idx];
        f32 d=(u-s->mu)/s->sigma;
        f32 g=2.0f*expf(-0.5f*d*d)-1.0f;      /* growth in [-1,1] */
        s->B[idx]=ct_clampf(s->A[idx]+s->dt*g,0.0f,1.0f);
    }
    f32 *sw=s->A; s->A=s->B; s->B=sw;
}

static Color3 lenia_color(int pal, f32 v){
    switch(pal){
        case 0: /* viridis-ish */ return col3(0.1f+0.2f*v, 0.2f+0.7f*v, 0.3f+0.5f*v*v);
        case 1: /* plasma */      return col3(0.5f+0.5f*v, 0.1f+0.3f*v, 0.6f-0.4f*v);
        default:/* mono green */  return col3(0.1f*v, v, 0.3f*v);
    }
}

static void le_render(Scene *sc, Framebuffer *fb){
    LeniaState *s=sc->state;
    for (i32 py=0;py<fb->h;++py){
        f32 gy=(f32)py/(fb->h-1)*(s->gh-1);
        i32 j0=(i32)gy; i32 j1=j0+1<s->gh?j0+1:j0; f32 tj=gy-j0;
        for (i32 px=0;px<fb->w;++px){
            f32 gx=(f32)px/(fb->w-1)*(s->gw-1);
            i32 i0=(i32)gx; i32 i1=i0+1<s->gw?i0+1:i0; f32 ti=gx-i0;
            const f32 *A=s->A;
            f32 v=(1-ti)*(1-tj)*A[j0*s->gw+i0]+ti*(1-tj)*A[j0*s->gw+i1]
                 +(1-ti)*tj*A[j1*s->gw+i0]+ti*tj*A[j1*s->gw+i1];
            fb_set(fb,px,py, lenia_color(s->palette, v));
        }
    }
}

static void le_key(Scene *sc, int key){
    LeniaState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_R) lenia_seed(s);
    else if (key==KEY_PLUS) s->mu+=0.005f;
    else if (key==KEY_MINUS) s->mu-=0.005f;
    s->mu=ct_clampf(s->mu,0.05f,0.35f);
}

static CrtConfig le_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.55f; c.persistence=0.3f; return c; }
static void le_destroy(Scene *sc){ if(sc){ LeniaState*s=sc->state; free(s->A);free(s->U);free(s->B); free(s); free(sc);} }

Scene *scene_lenia_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="lenia";
    sc->description="Lenia  -  continuous cellular automaton with lifelike creatures";
    sc->state=calloc(1,sizeof(LeniaState));
    sc->init=le_init; sc->update=le_update; sc->render=le_render;
    sc->on_key=le_key; sc->destroy=le_destroy; sc->preferred_crt=le_crt;
    return sc;
}
