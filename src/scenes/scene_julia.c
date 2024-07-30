/* ==========================================================================
 * scene_julia.c — animated Julia set, rendered with the NEON fractal kernel.
 *
 * The Julia constant c traces a slow loop around the boundary of the
 * Mandelbrot set, so the Julia set continuously metamorphoses between
 * connected "dendrite" and disconnected "dust" forms. Each row is rendered 4
 * pixels at a time via fk_julia4_neon (hand-written NEON escape-time iteration
 * with smooth coloring). Threaded across the framebuffer height.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/fractalkernel.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    i32 w, h; f32 t;
    f32 jcre, jcim;      /* current Julia constant */
    f32 zoom;            /* half-height of the view */
    i32 max_iter;
    int palette;
} JuliaState;

static void ju_init(Scene *sc, i32 w, i32 h){
    JuliaState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->zoom=1.5f; s->max_iter=220; s->palette=0;
    s->jcre=-0.4f; s->jcim=0.6f;
}

static void ju_update(Scene *sc, f32 dt, f32 t){
    JuliaState *s=sc->state; (void)dt; s->t=t;
    /* c traces the main-cardioid boundary: c = 0.5*(1-cos)*e^{i theta}-ish path.
     * We use a hand-picked loop through interesting Julia constants. */
    f32 a = t*0.12f + 2.2f;               /* phase offset: start on a connected set */
    s->jcre = 0.7885f * cosf(a);          /* the classic c = 0.7885 e^{ia} loop */
    s->jcim = 0.7885f * sinf(a);
    /* gentle breathing zoom */
    s->zoom = 1.4f + 0.25f*sinf(t*0.2f);
}

static Color3 julia_palette(int pal, f32 mu, int max_iter){
    if (mu >= (f32)max_iter - 0.5f) return col3(0,0,0);   /* inside the set */
    /* Normalize escape count to 0..1 with a sqrt so low-iteration exterior
     * (the bulk of a dusty Julia set) stays dark and the filaments near the
     * boundary — where mu is large — glow bright. */
    f32 t = sqrtf(mu / (f32)max_iter);        /* 0 (fast escape) .. 1 (near set) */
    f32 g = t;                                 /* overall brightness ramp */
    f32 v = mu * 0.14f;                        /* hue phase */
    Color3 hue;
    switch(pal){
        case 0: /* electric blue-white */
            hue = col3(0.3f+0.7f*t, 0.4f+0.6f*t, 0.7f+0.3f*sinf(v)); break;
        case 1: /* fire */
            hue = col3(t, t*t, 0.15f*t); break;
        default:/* spectrum */
            hue = col3(0.5f+0.5f*sinf(v), 0.5f+0.5f*sinf(v+2.094f), 0.5f+0.5f*sinf(v+4.188f)); break;
    }
    return col_scale(hue, g);
}

static void ju_render(Scene *sc, Framebuffer *fb){
    JuliaState *s=sc->state;
    f32 aspect=(f32)fb->w/(f32)fb->h;
    f32 half_h=s->zoom, half_w=half_h*aspect;
    for (i32 py=0; py<fb->h; ++py){
        f32 im = ((f32)py/(fb->h-1)*2.0f-1.0f)*half_h;
        /* process the row 4 pixels at a time with the NEON kernel */
        i32 px=0;
        for (; px+4<=fb->w; px+=4){
            f32 zr[4], zi[4], out[4];
            for (int k=0;k<4;++k){
                f32 re=((f32)(px+k)/(fb->w-1)*2.0f-1.0f)*half_w;
                zr[k]=re; zi[k]=im;
            }
            fk_julia4_neon(out, zr, zi, s->jcre, s->jcim, s->max_iter, 256.0f);
            for (int k=0;k<4;++k)
                fb_set(fb, px+k, py, julia_palette(s->palette, out[k], s->max_iter));
        }
        /* scalar tail for the last <4 pixels */
        for (; px<fb->w; ++px){
            f32 re=((f32)px/(fb->w-1)*2.0f-1.0f)*half_w;
            f32 zr[4]={re,re,re,re}, zi[4]={im,im,im,im}, out[4];
            fk_julia4_neon(out, zr, zi, s->jcre, s->jcim, s->max_iter, 256.0f);
            fb_set(fb, px, py, julia_palette(s->palette, out[0], s->max_iter));
        }
    }
}

static void ju_key(Scene *sc, int key){
    JuliaState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_PLUS) s->max_iter += 40;
    else if (key==KEY_MINUS) s->max_iter = s->max_iter>60 ? s->max_iter-40 : 60;
}

static CrtConfig ju_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.5f; return c; }
static void ju_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_julia_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="julia";
    sc->description="Animated Julia set via hand-written NEON escape-time kernel";
    sc->state=calloc(1,sizeof(JuliaState));
    sc->init=ju_init; sc->update=ju_update; sc->render=ju_render;
    sc->on_key=ju_key; sc->destroy=ju_destroy; sc->preferred_crt=ju_crt;
    return sc;
}
