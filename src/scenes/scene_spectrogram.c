/* ==========================================================================
 * scene_spectrogram.c  -  scrolling FFT spectrogram of a synthesized signal.
 *
 * Generates a rich, evolving audio-like signal (sweeping chirps + harmonics +
 * noise), takes a sliding-window FFT of it each frame via the Rust radix-2 FFT
 * (rust_fft_mag), and scrolls the magnitude spectrum upward as a waterfall  - 
 * frequency on X, time scrolling up, magnitude as a heat color. The classic
 * spectrogram you'd see in audio software, driven entirely by the from-scratch
 * Rust FFT. Bright ridges are the chirps sweeping across the band.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/rustcore.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FFT_N 512
#define NBINS (FFT_N/2)

typedef struct {
    f32 *waterfall;    /* rows x NBINS magnitude history */
    i32 rows;
    f32 sig[FFT_N];    /* current signal window */
    f32 mag[NBINS];
    f64 phase;         /* running phase for signal synthesis */
    i32 w, h; f32 t;
    Rng rng;
    int palette;
} SpecState;

static void sp_init(Scene *sc, i32 w, i32 h){
    SpecState *s=sc->state; s->w=w; s->h=h; s->t=0; s->phase=0; s->palette=0;
    s->rows=h;
    s->waterfall=calloc((size_t)s->rows*NBINS,sizeof(f32));
    rng_seed(&s->rng, 0x5EC7A0ULL);
}

static void sp_update(Scene *sc, f32 dt, f32 t){
    SpecState *s=sc->state; (void)dt; s->t=t;
    /* synthesize a window of signal: two chirps sweeping opposite directions,
     * a steady harmonic stack, and a little noise. */
    f32 sweep = 0.5f+0.45f*sinf(t*0.3f);        /* chirp center freq (norm) */
    for (int i=0;i<FFT_N;++i){
        f32 u=(f32)i/FFT_N;
        f32 f1 = 6.2831853f * (8.0f + 40.0f*sweep) * u;      /* rising chirp */
        f32 f2 = 6.2831853f * (60.0f - 30.0f*sweep) * u;     /* falling chirp */
        f32 harm = sinf(6.2831853f*16*u) + 0.4f*sinf(6.2831853f*32*u);
        f32 sample = sinf(f1 + t) + 0.8f*sinf(f2 - t*0.7f) + 0.3f*harm
                   + 0.15f*rng_normal(&s->rng);
        s->sig[i]=sample;
    }
    /* Rust FFT -> magnitudes */
    rust_fft_mag(s->mag, s->sig, FFT_N);
    /* scroll waterfall up, insert new row at bottom (log-scaled magnitude) */
    memmove(s->waterfall, s->waterfall+NBINS, (size_t)(s->rows-1)*NBINS*sizeof(f32));
    f32 *row=&s->waterfall[(s->rows-1)*NBINS];
    for (int k=0;k<NBINS;++k) row[k]=logf(1.0f+s->mag[k])*0.4f;
}

static Color3 heat(f32 v){
    v=ct_clampf(v,0,1);
    if (v<0.25f) return col_lerp(col3(0.0f,0.0f,0.15f), col3(0.1f,0.0f,0.5f), v/0.25f);
    if (v<0.5f)  return col_lerp(col3(0.1f,0.0f,0.5f), col3(0.7f,0.1f,0.5f), (v-0.25f)/0.25f);
    if (v<0.75f) return col_lerp(col3(0.7f,0.1f,0.5f), col3(1.0f,0.6f,0.1f), (v-0.5f)/0.25f);
    return col_lerp(col3(1.0f,0.6f,0.1f), col3(1.0f,1.0f,0.9f), (v-0.75f)/0.25f);
}

static void sp_render(Scene *sc, Framebuffer *fb){
    SpecState *s=sc->state;
    for (i32 py=0;py<fb->h;++py){
        i32 row=(i32)((f32)py/(fb->h-1)*(s->rows-1));
        for (i32 px=0;px<fb->w;++px){
            /* map x to a frequency bin (log-ish emphasis on low freqs) */
            f32 fx=(f32)px/(fb->w-1);
            i32 bin=(i32)(fx*fx*(NBINS-1));   /* fx^2 = more low-freq detail */
            f32 v=s->waterfall[row*NBINS+bin];
            fb_set(fb,px,py, heat(v));
        }
    }
}

static void sp_key(Scene *sc, int key){ (void)sc; (void)key; }
static CrtConfig sp_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.4f; return c; }
static void sp_destroy(Scene *sc){ if(sc){ SpecState*s=sc->state; free(s->waterfall); free(s); free(sc);} }

Scene *scene_spectrogram_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="spectrogram";
    sc->description="Scrolling FFT spectrogram of a synthesized chirp signal (Rust radix-2 FFT)";
    sc->state=calloc(1,sizeof(SpecState));
    sc->init=sp_init; sc->update=sp_update; sc->render=sp_render;
    sc->on_key=sp_key; sc->destroy=sp_destroy; sc->preferred_crt=sp_crt;
    return sc;
}
