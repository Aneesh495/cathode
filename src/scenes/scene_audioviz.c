/* ==========================================================================
 * scene_audioviz.c  -  audio-reactive visualizer driven by the C++ synth.
 *
 * The C++ synth (src/cpp/synth.cpp) runs a generative sequencer and renders
 * audio blocks; we pull its magnitude spectrum and waveform each frame and
 * draw a classic "graphic EQ + oscilloscope" display: a bar spectrum along the
 * bottom, a scrolling waterfall spectrogram above it, and a live waveform
 * trace across the middle. The synth is headless (no audio device) so this is
 * fully deterministic and CI-safe; on a real terminal you *see* the music.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/cppcore.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NBINS   64
#define WAVE_N  512

typedef struct {
    CppSynth *synth;
    f32 spectrum[NBINS];
    f32 smooth[NBINS];       /* peak-decay smoothed bars */
    f32 wave[WAVE_N];
    f32 *waterfall;          /* history: rows x NBINS, scrolls up */
    i32 wf_rows;
    i32 w, h; f32 t;
    int wave_form;
} AvState;

static void av_init(Scene *sc, i32 w, i32 h){
    AvState *s=sc->state; s->w=w; s->h=h; s->t=0; s->wave_form=1;
    s->synth = cpp_synth_create(44100);
    cpp_synth_set_waveform(s->synth, s->wave_form);
    cpp_synth_set_filter(s->synth, 3000.0f, 0.4f);
    cpp_synth_set_reverb(s->synth, 0.35f, 0.75f);   /* spacious hall tail */
    memset(s->spectrum,0,sizeof(s->spectrum));
    memset(s->smooth,0,sizeof(s->smooth));
    memset(s->wave,0,sizeof(s->wave));
    s->wf_rows = h;                       /* one waterfall row per pixel row */
    s->waterfall = calloc((size_t)s->wf_rows*NBINS, sizeof(f32));
}

static void av_update(Scene *sc, f32 dt, f32 t){
    AvState *s=sc->state; s->t=t;
    /* advance the generative sequencer (drives note on/off) */
    cpp_synth_sequencer_tick(s->synth, dt);
    /* Render a block for the visualizer ONLY when the audio device is NOT
     * driving the synth  -  otherwise the device callback owns cpp_synth_render
     * on its own thread and calling it here too would be a data race. When
     * audio is live we still read the spectrum of the device's last block. */
    if (!cpp_audio_running())
        cpp_synth_render(s->synth, s->wave, WAVE_N);
    cpp_synth_spectrum(s->synth, s->spectrum, NBINS);
    /* peak-decay smoothing for the bars */
    for (int i=0;i<NBINS;++i){
        f32 v = s->spectrum[i];
        if (v > s->smooth[i]) s->smooth[i]=v;
        else s->smooth[i]*=0.86f;
    }
    /* scroll waterfall up by one row, insert new spectrum at the bottom */
    memmove(s->waterfall, s->waterfall+NBINS, (size_t)(s->wf_rows-1)*NBINS*sizeof(f32));
    for (int i=0;i<NBINS;++i) s->waterfall[(s->wf_rows-1)*NBINS+i]=s->spectrum[i];
}

static Color3 spec_color(f32 v){
    /* blue -> green -> yellow -> red heatmap */
    v=ct_clampf(v,0,1);
    if (v<0.33f) return col_lerp(col3(0.0f,0.0f,0.3f), col3(0.0f,0.8f,0.5f), v/0.33f);
    if (v<0.66f) return col_lerp(col3(0.0f,0.8f,0.5f), col3(1.0f,0.9f,0.1f), (v-0.33f)/0.33f);
    return col_lerp(col3(1.0f,0.9f,0.1f), col3(1.0f,0.15f,0.1f), (v-0.66f)/0.34f);
}

static void av_render(Scene *sc, Framebuffer *fb){
    AvState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.02f,0.04f));
    i32 W=fb->w, H=fb->h;

    /* --- waterfall spectrogram fills the whole background --- */
    for (i32 y=0;y<H;++y){
        i32 row = (i32)((f32)y/(H-1)*(s->wf_rows-1));
        for (i32 x=0;x<W;++x){
            i32 bin=(i32)((f32)x/(W-1)*(NBINS-1));
            f32 v=s->waterfall[row*NBINS+bin];
            fb_add(fb,x,y, col_scale(spec_color(ct_clampf(v*1.5f,0,1)), 0.4f));
        }
    }

    /* --- bar spectrum along the bottom third --- */
    i32 barTop = H*2/3;
    for (i32 x=0;x<W;++x){
        i32 bin=(i32)((f32)x/(W-1)*(NBINS-1));
        f32 v=ct_clampf(s->smooth[bin]*1.5f,0,1);
        i32 top = barTop + (i32)((1.0f-v)*(H-barTop));
        Color3 c=spec_color(v);
        for (i32 y=top;y<H;++y) fb_set(fb,x,y,c);
    }

    /* --- oscilloscope waveform across the middle --- */
    i32 midY=H/3;
    for (i32 x=0;x<W;++x){
        i32 si=(i32)((f32)x/(W-1)*(WAVE_N-1));
        f32 samp=s->wave[si];
        i32 y=midY + (i32)(samp*(H*0.14f));
        fb_splat(fb,(f32)x,(f32)y, col3(0.3f,1.0f,0.5f));
        fb_splat(fb,(f32)x,(f32)(y+1), col3(0.15f,0.5f,0.25f));
    }
}

static void av_key(Scene *sc, int key){
    AvState *s=sc->state;
    if (key==KEY_TAB){ s->wave_form=(s->wave_form+1)%4; cpp_synth_set_waveform(s->synth,s->wave_form); }
    else if (key==KEY_PLUS) cpp_synth_set_filter(s->synth, 6000.0f, 0.6f);
    else if (key==KEY_MINUS) cpp_synth_set_filter(s->synth, 1200.0f, 0.3f);
    else if (key==KEY_ENTER){
        /* toggle real audio output (only does anything in an AUDIO=1 build on
         * a machine with a device; otherwise a harmless no-op). */
        if (cpp_audio_running()) cpp_audio_stop();
        else cpp_audio_start(s->synth, 44100);
    }
}

static CrtConfig av_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("arcade"); c.bloom=0.5f; return c; }
static void av_destroy(Scene *sc){
    if(sc){ AvState*s=sc->state;
        cpp_audio_stop();   /* stop the device callback before freeing the synth */
        if(s->synth)cpp_synth_destroy(s->synth);
        free(s->waterfall); free(s); free(sc);
    }
}

Scene *scene_audioviz_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="audioviz";
    sc->description="Audio-reactive visualizer: C++ synth spectrum + waveform + waterfall";
    sc->state=calloc(1,sizeof(AvState));
    sc->init=av_init; sc->update=av_update; sc->render=av_render;
    sc->on_key=av_key; sc->destroy=av_destroy; sc->preferred_crt=av_crt;
    return sc;
}
