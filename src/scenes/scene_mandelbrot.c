/* ==========================================================================
 * scene_mandelbrot.c  -  animated Mandelbrot-set deep zoom.
 *
 * Classic escape-time fractal with smooth (continuous) iteration coloring.
 * The view continuously zooms toward a pre-chosen "interesting" point on the
 * boundary (a seahorse-valley coordinate), cycling a palette. Pure C, double
 * precision so the zoom stays crisp for a good while before FP runs out.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "threadpool.h"      /* app-internal: multithreaded band rendering */
#include <stdlib.h>
#include <math.h>

typedef struct {
    i32 w, h;
    f32 t;
    f64 cx, cy;      /* target center in the complex plane */
    f64 zoom;        /* current half-height of the view in complex units */
    f64 zoom_rate;   /* multiplicative zoom speed per second */
    i32 max_iter;
    f32 pal_shift;
    ThreadPool *pool;
} MbState;

/* A well-known zoom target on the boundary of the set (seahorse valley). */
#define MB_TARGET_X (-0.743643887037158704752191506114774)
#define MB_TARGET_Y ( 0.131825904205311970493132056385139)

static Color3 palette(f32 t){
    /* smooth cyclic palette (Inigo Quilez style cosine palette) */
    t = t - floorf(t);
    f32 r = 0.5f + 0.5f*cosf(CT_TAU*(1.0f*t + 0.00f));
    f32 g = 0.5f + 0.5f*cosf(CT_TAU*(1.0f*t + 0.33f));
    f32 b = 0.5f + 0.5f*cosf(CT_TAU*(1.0f*t + 0.67f));
    return col3(r*r, g*g, b*b); /* squared for punchier saturation */
}

static void mb_init(Scene *sc, i32 w, i32 h){
    MbState *s = sc->state;
    s->w=w; s->h=h; s->t=0;
    s->cx=MB_TARGET_X; s->cy=MB_TARGET_Y;
    s->zoom=1.4; s->zoom_rate=0.72; s->max_iter=180; s->pal_shift=0;
}

static void mb_update(Scene *sc, f32 dt, f32 t){
    MbState *s = sc->state; s->t=t;
    /* exponential zoom-in; reset when we hit double-precision resolution */
    s->zoom *= pow(s->zoom_rate, (f64)dt);
    if (s->zoom < 1e-13) { s->zoom = 1.4; }   /* loop the journey */
    s->pal_shift = t*0.05f;
    /* deepen iteration count as we zoom so detail keeps resolving */
    s->max_iter = 180 + (i32)(24.0 * -log10(s->zoom));
    if (s->max_iter > 2000) s->max_iter = 2000;
}

/* render rows [y0,y1)  -  the thread-pool band callback (see mb_render) */
static void mb_band(MbState *s, Framebuffer *fb, i32 ry0, i32 ry1){
    f64 aspect = (f64)fb->w/(f64)fb->h;
    f64 half_h = s->zoom;
    f64 half_w = half_h*aspect;
    i32 maxit = s->max_iter;
    for (i32 py=ry0; py<ry1; ++py){
        f64 y0 = s->cy + ((f64)py/(fb->h-1)*2.0 - 1.0)*half_h;
        for (i32 px=0; px<fb->w; ++px){
            f64 x0 = s->cx + ((f64)px/(fb->w-1)*2.0 - 1.0)*half_w;
            /* escape-time with cardioid/bulb quick-reject */
            f64 x=0, y=0, x2=0, y2=0;
            i32 it=0;
            f64 q=(x0-0.25)*(x0-0.25)+y0*y0;
            if (q*(q+(x0-0.25)) <= 0.25*y0*y0 ||
                (x0+1.0)*(x0+1.0)+y0*y0 <= 0.0625){
                it=maxit;
            } else {
                while (x2+y2 <= 256.0 && it<maxit){
                    y = 2.0*x*y + y0;
                    x = x2 - y2 + x0;
                    x2 = x*x; y2 = y*y;
                    ++it;
                }
            }
            Color3 c;
            if (it>=maxit){
                c = col3(0,0,0);
            } else {
                f64 log_zn = log(x2+y2)*0.5;
                f64 nu = log(log_zn/log(2.0))/log(2.0);
                f32 mu = (f32)(it + 1.0 - nu);
                c = palette(mu*0.02f + s->pal_shift);
                c = col_scale(c, 1.2f);
            }
            fb_set(fb, px, py, c);
        }
    }
}

/* thread-pool trampoline: rows are disjoint per band, so each thread writes its
 * own pixels  -  no shared-write race (same pattern as the SDF marcher). */
typedef struct { MbState *s; Framebuffer *fb; } MbBandCtx;
static void mb_band_cb(void *user, i32 y0, i32 y1){
    MbBandCtx *c=(MbBandCtx*)user; mb_band(c->s, c->fb, y0, y1);
}
static void mb_render(Scene *sc, Framebuffer *fb){
    MbState *s = sc->state;
    if (!s->pool) s->pool = tp_create(0);
    MbBandCtx ctx = { s, fb };
    tp_run_bands(s->pool, fb->h, mb_band_cb, &ctx);
}

static void mb_key(Scene *sc, int key){
    MbState *s = sc->state;
    if (key==KEY_PLUS) s->zoom_rate*=0.9;        /* faster zoom */
    else if (key==KEY_MINUS) s->zoom_rate/=0.9;
    else if (key==KEY_R){ s->zoom=1.4; }
    else if (key==KEY_TAB) s->pal_shift+=0.25f;
    if (s->zoom_rate<0.4) s->zoom_rate=0.4;
    if (s->zoom_rate>0.98) s->zoom_rate=0.98;
}

static CrtConfig mb_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void mb_destroy(Scene *sc){ if(sc){ MbState*s=sc->state; if(s->pool)tp_destroy(s->pool); free(s); free(sc);} }

Scene *scene_mandelbrot_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="mandelbrot";
    sc->description="Animated Mandelbrot deep-zoom with smooth iteration coloring";
    sc->state=calloc(1,sizeof(MbState));
    sc->init=mb_init; sc->update=mb_update; sc->render=mb_render;
    sc->on_key=mb_key; sc->destroy=mb_destroy; sc->preferred_crt=mb_crt;
    return sc;
}
