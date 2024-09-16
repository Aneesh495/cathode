/* ==========================================================================
 * scene_chiptune.c  -  a live tracker player UI, the capstone of the audio stack.
 *
 * This scene ties together every audio subsystem CATHODE has:
 *
 *   tracker (C)  ──note events──▶  synth (C++)  ──samples──▶  FFT (Rust)
 *       │                                                        │
 *       └── pattern grid (bitmap font) ◀── current row      spectrum bars
 *
 * A built-in song plays on the pure-C tracker; its note events drive the C++
 * synth; we render a block per frame and pull the magnitude spectrum via the
 * (Rust) FFT. On screen: a scrolling MOD-style pattern grid drawn with the 5x7
 * font (note names + instrument per channel, the playing row highlighted), and
 * a spectrum-analyzer strip that dances to the actual audio. All deterministic
 * and headless-safe, so it golden-tests and needs no audio device.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/text.h"
#include "cathode/tracker.h"
#include "cathode/cppcore.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define CT_ROWS 16
#define CT_CHANS 3
#define NBINS 48

/* convenience cell builders */
#define N(note,ins,vol) { (u8)(note), (u8)(ins), (u8)(vol), 0 }
#define A(note,ins,vol,st) { (u8)(note), (u8)(ins), (u8)(vol), TRK_FX(FX_ARPEGGIO,(st)) }
#define H { NOTE_NONE, 0, 0, 0 }
#define X { NOTE_OFF, 0, 0, 0 }

/* one 16-row, 3-channel pattern: bass / arp-lead / pad, a minor groove */
static const TrackerCell PATTERN[CT_ROWS * CT_CHANS] = {
    /* r0 */ N(45,1,60), A(69,2,50,3), N(57,3,28),
    /* r1 */ H,          N(72,2,42),   H,
    /* r2 */ N(45,1,40), N(76,2,42),   H,
    /* r3 */ H,          N(72,2,42),   H,
    /* r4 */ N(48,1,60), A(72,2,50,4), N(60,3,28),
    /* r5 */ H,          N(76,2,42),   H,
    /* r6 */ N(48,1,40), N(79,2,42),   H,
    /* r7 */ H,          N(76,2,42),   H,
    /* r8 */ N(43,1,60), A(67,2,50,3), N(55,3,28),
    /* r9 */ H,          N(71,2,42),   H,
    /* r10*/ N(43,1,40), N(74,2,42),   H,
    /* r11*/ H,          N(71,2,42),   H,
    /* r12*/ N(41,1,60), A(65,2,50,4), N(53,3,28),
    /* r13*/ H,          N(69,2,42),   H,
    /* r14*/ N(41,1,40), N(72,2,42),   H,
    /* r15*/ X,          X,            X,
};
#undef N
#undef A
#undef H
#undef X

typedef struct {
    i32 w, h; f32 t;
    Tracker *trk;
    CppSynth *syn;
    u32 cur_row;             /* highlighted row (last event's row) */
    u64 samples_played;
    u32 samples_per_row;
    f32 spectrum[NBINS];
    f32 smooth[NBINS];
    int waveform_for_instr[4];
} ChipState;

static const char *NOTE_NAMES[12] =
    {"C-","C#","D-","D#","E-","F-","F#","G-","G#","A-","A#","B-"};

static void note_str(u8 midi, char out[4]){
    if (midi == NOTE_NONE) { memcpy(out,"...",4); return; }
    if (midi == NOTE_OFF)  { memcpy(out,"===",4); return; }
    int n = midi % 12, oct = midi/12 - 1;
    out[0]=NOTE_NAMES[n][0]; out[1]=NOTE_NAMES[n][1];
    out[2]=(char)('0'+(oct<0?0:(oct>9?9:oct)));
    out[3]=0;
}

static void ch_init(Scene *sc, i32 w, i32 h){
    ChipState *s=sc->state; s->w=w; s->h=h; s->t=0; s->cur_row=0; s->samples_played=0;
    static const u8 ORDER[] = {0};
    s->trk = tracker_create(PATTERN, 1, CT_ROWS, CT_CHANS, ORDER, 1,
                            44100, 6, 120, 1 /*loop*/);
    s->samples_per_row = (u32)(44100.0 * 2.5 / 120.0) * 6;  /* matches engine */
    s->syn = cpp_synth_create(44100);
    cpp_synth_set_filter(s->syn, 3200.0f, 0.35f);
    cpp_synth_set_reverb(s->syn, 0.30f, 0.6f);
    memset(s->spectrum,0,sizeof(s->spectrum));
    memset(s->smooth,0,sizeof(s->smooth));
}

static i32 instr_wave(u8 instr){
    switch(instr){ case 1: return 1; case 2: return 2; case 3: return 3; default: return 0; }
}

static void ch_update(Scene *sc, f32 dt, f32 t){
    ChipState *s=sc->state; s->t=t;
    if (dt <= 0) return;
    /* advance the tracker by dt worth of samples, firing events into the synth */
    u32 nsamp = (u32)(dt * 44100.0f); if (nsamp < 1) nsamp = 1; if (nsamp > 8192) nsamp = 8192;
    TrackerEvent ev[64];
    u32 ne = tracker_advance(s->trk, nsamp, ev, 64);
    for (u32 i=0;i<ne;++i){
        if (ev[i].kind == TEV_NOTE_ON){
            cpp_synth_set_waveform(s->syn, instr_wave(ev[i].instrument));
            cpp_synth_note_on(s->syn, ev[i].note, ev[i].velocity);
        } else {
            cpp_synth_note_off(s->syn, ev[i].note);
        }
    }
    s->samples_played += nsamp;
    s->cur_row = (u32)((s->samples_played / s->samples_per_row) % CT_ROWS);
    /* render a block and pull the spectrum */
    f32 blk[1024];
    u32 rn = nsamp>1024?1024:nsamp;
    cpp_synth_render(s->syn, blk, (i32)rn);
    cpp_synth_spectrum(s->syn, s->spectrum, NBINS);
    for (int i=0;i<NBINS;++i){
        f32 v=s->spectrum[i];
        if (v>s->smooth[i]) s->smooth[i]=v; else s->smooth[i]*=0.85f;
    }
}

static void ch_render(Scene *sc, Framebuffer *fb){
    ChipState *s=sc->state; const i32 w=fb->w, h=fb->h;
    fb_clear(fb, col3(0.015f,0.02f,0.03f));

    i32 scale = h/90; if (scale<1) scale=1; if (scale>2) scale=2;
    i32 lh = (FONT_H+2)*scale;
    i32 x0 = 3*scale;
    /* header */
    Color3 head = col3(0.5f,0.9f,1.0f);
    text_draw(fb, x0, 2, "CATHODE TRACKER  CH1 BASS  CH2 LEAD  CH3 PAD", head, scale, 1);

    /* pattern grid: show all 16 rows, highlight current */
    i32 grid_y0 = 2 + lh + scale;
    for (int r=0;r<CT_ROWS;++r){
        i32 y = grid_y0 + r*lh;
        if (y > h-2) break;
        int is_cur = ((u32)r == s->cur_row);
        Color3 rowc = is_cur ? col3(1.0f,1.0f,0.5f) : col3(0.45f,0.5f,0.6f);
        /* draw a highlight bar behind the current row */
        if (is_cur){
            for (i32 yy=y-1; yy<y+FONT_H*scale+1 && yy<h; ++yy){
                if (yy<0) continue;
                f32 *rr=&fb->px[(size_t)yy*w*3];
                for (i32 xx=0; xx<w; ++xx){ rr[3*xx+0]+=0.08f; rr[3*xx+1]+=0.07f; rr[3*xx+2]+=0.02f; }
            }
        }
        char line[64]; int p=0;
        p += snprintf(line+p, sizeof(line)-p, "%02d ", r);
        for (int c=0;c<CT_CHANS;++c){
            const TrackerCell *cell=&PATTERN[r*CT_CHANS+c];
            char ns[4]; note_str(cell->note, ns);
            char ins = cell->instrument ? (char)('0'+cell->instrument) : '.';
            p += snprintf(line+p, sizeof(line)-p, "%s%c ", ns, ins);
        }
        text_draw(fb, x0, y, line, rowc, scale, 1);
    }

    /* spectrum strip along the right third (vertical bars) */
    i32 spec_x = (w*2)/3;
    i32 spec_w = w - spec_x - 2;
    if (spec_w > NBINS){
        i32 bw = spec_w / NBINS;
        for (int i=0;i<NBINS;++i){
            f32 v = s->smooth[i]; if (v>1) v=1;
            i32 bh = (i32)(v * (h*0.5f));
            i32 bx = spec_x + i*bw;
            Color3 c = col3(0.2f+0.8f*v, 0.6f*(1-v)+0.3f, 1.0f-0.7f*v);
            for (i32 yy=h-1; yy>h-1-bh && yy>=0; --yy)
                for (i32 xx=bx; xx<bx+bw-1 && xx<w; ++xx)
                    fb_add(fb, xx, yy, c);
        }
    }
}

static CrtConfig ch_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void ch_destroy(Scene *sc){
    if(sc){ ChipState*s=sc->state;
        if(s->trk)tracker_destroy(s->trk);
        if(s->syn)cpp_synth_destroy(s->syn);
        free(s); free(sc);
    }
}

Scene *scene_chiptune_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="chiptune";
    sc->description="Live tracker player: pattern grid + synth + FFT spectrum (full audio stack)";
    sc->state=calloc(1,sizeof(ChipState));
    sc->init=ch_init; sc->update=ch_update; sc->render=ch_render;
    sc->destroy=ch_destroy; sc->preferred_crt=ch_crt;
    return sc;
}
