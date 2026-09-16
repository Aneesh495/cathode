/* ==========================================================================
 * capture.c  -  headless renderer. Runs a scene for N frames (advancing sim
 * time deterministically) and writes selected frames to PNG, both the raw
 * scene output and the CRT-processed output. Used for visual verification
 * and for generating a contact sheet of all scenes.
 *
 * Usage:
 *   capture <scene-name|index> <frames> <out-prefix> [w] [h] [crt-preset]
 *   capture --all <frames> <out-dir>        # one PNG per scene (last frame)
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/framebuffer.h"
#include "cathode/crt.h"
#include "cathode/image.h"
#include "cathode/wav.h"
#include "cathode/tracker.h"
#include "cathode/cppcore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static i32 find_scene(const char *arg) {
    /* numeric index? */
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    if (end && *end == '\0') return (i32)v;
    for (i32 i = 0; i < scene_count(); ++i)
        if (strcmp(arg, scene_name_at(i)) == 0) return i;
    return -1;
}

static int render_scene_to_png(i32 idx, i32 frames, i32 w, i32 h,
                               const char *preset, const char *out_png,
                               const char *raw_png) {
    Scene *sc = scene_create(idx);
    if (!sc) { fprintf(stderr, "scene %d not available\n", idx); return 1; }
    if (sc->init) sc->init(sc, w, h);

    CrtConfig cfg = preset ? crt_config_preset(preset)
                  : (sc->preferred_crt ? sc->preferred_crt(sc) : crt_config_default());
    CrtState *crt = crt_create(w, h, &cfg);

    Framebuffer *scene_fb = fb_create(w, h);
    Framebuffer *disp_fb  = fb_create(w, h);

    const f32 dt = 1.0f / 60.0f;
    f32 t = 0.0f;
    for (i32 f = 0; f < frames; ++f) {
        if (sc->update) sc->update(sc, dt, t);
        if (sc->render) sc->render(sc, scene_fb);
        crt_process(crt, scene_fb, disp_fb);   /* advance phosphor/dot-crawl */
        t += dt;
    }

    int rc = 0;
    if (out_png && image_write_png(disp_fb, out_png) != 0) rc = 1;
    if (raw_png && image_write_png(scene_fb, raw_png) != 0) rc = 1;
    printf("scene '%-10s' frames=%d %dx%d crt=%-9s -> %s\n",
           scene_name_at(idx), frames, w, h,
           preset ? preset : "(scene default)", out_png ? out_png : "(none)");

    if (sc->destroy) sc->destroy(sc);
    crt_destroy(crt);
    fb_destroy(scene_fb);
    fb_destroy(disp_fb);
    return rc;
}

/* Render a scene to an animated GIF through the full CRT chain. */
static int render_scene_to_gif(i32 idx, i32 frames, i32 w, i32 h,
                               const char *preset, const char *out_gif,
                               i32 delay_cs) {
    Scene *sc = scene_create(idx);
    if (!sc) { fprintf(stderr, "scene %d not available\n", idx); return 1; }
    if (sc->init) sc->init(sc, w, h);
    CrtConfig cfg = preset ? crt_config_preset(preset)
                  : (sc->preferred_crt ? sc->preferred_crt(sc) : crt_config_default());
    CrtState *crt = crt_create(w, h, &cfg);
    Framebuffer *scene_fb = fb_create(w, h);
    Framebuffer *disp_fb  = fb_create(w, h);
    GifWriter *g = gif_begin(out_gif, w, h, delay_cs, 1);
    if (!g) { fprintf(stderr, "cannot open %s\n", out_gif); return 1; }
    const f32 dt = 1.0f / 30.0f;
    f32 t = 0.0f;
    for (i32 f = 0; f < frames; ++f) {
        if (sc->update) sc->update(sc, dt, t);
        if (sc->render) sc->render(sc, scene_fb);
        crt_process(crt, scene_fb, disp_fb);
        gif_add_frame(g, disp_fb);
        t += dt;
    }
    gif_end(g);
    printf("scene '%-10s' %d frames %dx%d -> %s (animated GIF)\n",
           scene_name_at(idx), frames, w, h, out_gif);
    if (sc->destroy) sc->destroy(sc);
    crt_destroy(crt); fb_destroy(scene_fb); fb_destroy(disp_fb);
    return 0;
}

/* Render a self-running "demo reel": cycle through every scene, holding each
 * for `hold` frames then crossfading to the next over `xf` frames, into one
 * animated GIF. This is CATHODE's attract mode, made into a shareable artifact.
 * The crossfade is a linear blend of the two scenes' CRT-processed frames. */
static int render_reel(const char *out_gif, i32 hold, i32 xf, i32 w, i32 h) {
    i32 n = scene_count();
    if (n == 0) return 1;
    GifWriter *g = gif_begin(out_gif, w, h, 4, 1);
    if (!g) { fprintf(stderr, "cannot open %s\n", out_gif); return 1; }
    Framebuffer *sa=fb_create(w,h), *da=fb_create(w,h);
    Framebuffer *sb=fb_create(w,h), *db=fb_create(w,h);
    Framebuffer *blend=fb_create(w,h);
    const f32 dt = 1.0f/30.0f;
    for (i32 i=0;i<n;++i){
        Scene *A=scene_create(i);            if(!A) continue;
        if(A->init) A->init(A,w,h);
        CrtConfig ca=A->preferred_crt?A->preferred_crt(A):crt_config_default();
        CrtState *crtA=crt_create(w,h,&ca);
        f32 tA=0;
        /* hold phase */
        for (i32 f=0; f<hold; ++f){
            if(A->update)A->update(A,dt,tA); if(A->render)A->render(A,sa);
            crt_process(crtA,sa,da); gif_add_frame(g,da); tA+=dt;
        }
        /* crossfade into the next scene */
        i32 j=(i+1)%n;
        Scene *B=scene_create(j);
        if (B){
            if(B->init)B->init(B,w,h);
            CrtConfig cb=B->preferred_crt?B->preferred_crt(B):crt_config_default();
            CrtState *crtB=crt_create(w,h,&cb);
            f32 tB=0;
            for (i32 f=0; f<xf; ++f){
                f32 a=(f32)f/(f32)(xf>1?xf-1:1);          /* 0->1 blend weight */
                if(A->update)A->update(A,dt,tA); if(A->render)A->render(A,sa); crt_process(crtA,sa,da); tA+=dt;
                if(B->update)B->update(B,dt,tB); if(B->render)B->render(B,sb); crt_process(crtB,sb,db); tB+=dt;
                for (i32 p=0;p<w*h*3;++p) blend->px[p]=da->px[p]*(1.0f-a)+db->px[p]*a;
                gif_add_frame(g,blend);
            }
            crt_destroy(crtB);
            if(B->destroy)B->destroy(B);
        }
        crt_destroy(crtA);
        if(A->destroy)A->destroy(A);
        printf("reel: %s -> %s\n", scene_name_at(i), scene_name_at(j));
    }
    gif_end(g);
    fb_destroy(sa);fb_destroy(da);fb_destroy(sb);fb_destroy(db);fb_destroy(blend);
    printf("demo reel: %d scenes -> %s\n", n, out_gif);
    return 0;
}

/* Render the C++ synth's generative sequencer to a real WAV file, streamed
 * block-by-block through the from-scratch RIFF/PCM encoder. This ties the audio
 * subsystem to a concrete, playable artifact without any OS audio device. */
static int render_audio_to_wav(const char *out, f32 seconds, i32 sample_rate,
                               i32 waveform, f32 reverb_wet) {
    if (sample_rate <= 0) sample_rate = 44100;
    if (seconds <= 0.0f)  seconds = 8.0f;
    CppSynth *syn = cpp_synth_create(sample_rate);
    if (!syn) { fprintf(stderr, "synth create failed\n"); return 1; }
    cpp_synth_set_waveform(syn, waveform);
    cpp_synth_set_filter(syn, 2400.0f, 0.4f);
    cpp_synth_set_reverb(syn, reverb_wet, 0.5f);

    WavWriter *w = wav_begin(out, 1 /*mono*/, (u32)sample_rate);
    if (!w) { fprintf(stderr, "cannot open %s\n", out); cpp_synth_destroy(syn); return 1; }

    const i32 BLK = 512;
    f32 buf[512];
    const f32 block_dt = (f32)BLK / (f32)sample_rate;
    i64 total = (i64)(seconds * sample_rate);
    i64 done = 0;
    int rc = 0;
    while (done < total) {
        i32 n = (i32)((total - done) < BLK ? (total - done) : BLK);
        cpp_synth_sequencer_tick(syn, block_dt);   /* generative note events */
        cpp_synth_render(syn, buf, n);             /* mono samples in [-1,1] */
        if (wav_write_frames(w, buf, (u32)n) != 0) { rc = 1; break; }
        done += n;
    }
    if (wav_end(w) != 0) rc = 1;
    cpp_synth_destroy(syn);
    if (!rc) printf("audio: %.1fs %dHz waveform=%d reverb=%.2f -> %s\n",
                    seconds, sample_rate, waveform, reverb_wet, out);
    return rc;
}

/* ---- a hand-written chiptune, played by the pure-C tracker engine ----
 * 3 channels: bass (saw), lead arpeggio (square), pad (triangle). Two 16-row
 * patterns in an A A B A order. MIDI notes; NOTE_NONE keeps the channel, and a
 * new note auto-releases the previous one. This proves the tracker → synth →
 * WAV chain end to end with actual composed music, not a random walk. */
#define SONG_ROWS 16
#define SONG_CH   3
/* convenience macros for readable cells: N=note, H=hold, X=note-off,
 * A=note with an arpeggio effect (param = semitone step). */
#define N(note,ins,vol) { (u8)(note), (u8)(ins), (u8)(vol), 0 }
#define A(note,ins,vol,step) { (u8)(note), (u8)(ins), (u8)(vol), TRK_FX(FX_ARPEGGIO,(step)) }
#define H               { NOTE_NONE, 0, 0, 0 }
#define X               { NOTE_OFF, 0, 0, 0 }

static const TrackerCell SONG[2 * SONG_ROWS * SONG_CH] = {
    /* ---- pattern 0 (Am feel): bass A2, arp A-C-E, pad A3 ---- */
    /* r0 */ N(45,1,60), N(69,2,48), A(57,3,30,3),  /* pad arpeggiates a minor triad */
    /* r1 */ H,          N(72,2,40), H,
    /* r2 */ H,          N(76,2,40), H,
    /* r3 */ H,          N(72,2,40), H,
    /* r4 */ N(45,1,55), N(69,2,44), H,
    /* r5 */ H,          N(76,2,40), H,
    /* r6 */ H,          N(72,2,40), H,
    /* r7 */ H,          N(69,2,40), H,
    /* r8 */ N(48,1,60), N(72,2,48), N(60,3,30),  /* to C */
    /* r9 */ H,          N(76,2,40), H,
    /* r10*/ H,          N(79,2,40), H,
    /* r11*/ H,          N(76,2,40), H,
    /* r12*/ N(48,1,55), N(72,2,44), H,
    /* r13*/ H,          N(79,2,40), H,
    /* r14*/ H,          N(76,2,40), H,
    /* r15*/ X,          X,          X,
    /* ---- pattern 1 (F-G turn): bass F2->G2, brighter arp ---- */
    /* r0 */ N(41,1,60), N(65,2,50), N(53,3,32),  /* F */
    /* r1 */ H,          N(69,2,42), H,
    /* r2 */ H,          N(72,2,42), H,
    /* r3 */ H,          N(69,2,42), H,
    /* r4 */ N(41,1,55), N(65,2,46), H,
    /* r5 */ H,          N(72,2,42), H,
    /* r6 */ H,          N(69,2,42), H,
    /* r7 */ H,          N(65,2,42), H,
    /* r8 */ N(43,1,60), N(67,2,50), N(55,3,32),  /* G */
    /* r9 */ H,          N(71,2,42), H,
    /* r10*/ H,          N(74,2,42), H,
    /* r11*/ H,          N(71,2,42), H,
    /* r12*/ N(43,1,55), N(67,2,46), H,
    /* r13*/ H,          N(74,2,42), H,
    /* r14*/ H,          N(71,2,42), H,
    /* r15*/ X,          X,          X,
};
#undef N
#undef A
#undef H
#undef X

/* Map a tracker instrument id to a synth waveform. */
static i32 instr_to_wave(u8 instr) {
    switch (instr) { case 1: return 1; /*saw bass*/ case 2: return 2; /*square lead*/
                     case 3: return 3; /*tri pad*/  default: return 0; }
}

/* Render the built-in SONG through the tracker + synth to a WAV file. Because
 * this engine is monophonic-per-note (the synth voice-allocates), we drive all
 * channels into one synth; its voice stealing handles the polyphony. */
static int render_song_to_wav(const char *out, i32 sample_rate, i32 repeats,
                              f32 reverb_wet) {
    if (sample_rate <= 0) sample_rate = 44100;
    if (repeats <= 0) repeats = 2;

    static const u8 ORDER[] = { 0, 0, 1, 0 };
    const u32 orderlen = (u32)(sizeof(ORDER) / sizeof(ORDER[0]));

    Tracker *trk = tracker_create(SONG, 2, SONG_ROWS, SONG_CH, ORDER, orderlen,
                                  (u32)sample_rate, 6, 120, 0);
    if (!trk) { fprintf(stderr, "tracker create failed\n"); return 1; }

    CppSynth *syn = cpp_synth_create(sample_rate);
    if (!syn) { fprintf(stderr, "synth create failed\n"); tracker_destroy(trk); return 1; }
    cpp_synth_set_filter(syn, 3000.0f, 0.3f);
    cpp_synth_set_reverb(syn, reverb_wet, 0.55f);

    WavWriter *w = wav_begin(out, 1, (u32)sample_rate);
    if (!w) { fprintf(stderr, "cannot open %s\n", out);
              cpp_synth_destroy(syn); tracker_destroy(trk); return 1; }

    const i32 BLK = 512;
    f32 buf[512];
    u64 one_pass = tracker_total_samples(trk);
    u64 tail = (u64)(sample_rate);      /* 1s tail for the reverb/release */
    int rc = 0;

    for (int rep = 0; rep < repeats && !rc; ++rep) {
        /* fresh tracker cursor each repeat (cheap: rebuild) */
        if (rep > 0) {
            tracker_destroy(trk);
            trk = tracker_create(SONG, 2, SONG_ROWS, SONG_CH, ORDER, orderlen,
                                 (u32)sample_rate, 6, 120, 0);
            if (!trk) { rc = 1; break; }
        }
        u64 done = 0;
        while (done < one_pass && !rc) {
            i32 n = (i32)((one_pass - done) < BLK ? (one_pass - done) : BLK);
            TrackerEvent ev[64];
            u32 ne = tracker_advance(trk, (u32)n, ev, 64);
            /* Events all fall in this block; fire them (block-granular timing is
             * fine at 512 samples ~= 11ms, well under a row). */
            for (u32 i = 0; i < ne; ++i) {
                if (ev[i].kind == TEV_NOTE_ON) {
                    cpp_synth_set_waveform(syn, instr_to_wave(ev[i].instrument));
                    cpp_synth_note_on(syn, ev[i].note, ev[i].velocity);
                } else {
                    cpp_synth_note_off(syn, ev[i].note);
                }
            }
            cpp_synth_render(syn, buf, n);
            if (wav_write_frames(w, buf, (u32)n) != 0) rc = 1;
            done += n;
        }
    }
    /* render the tail */
    { u64 done = 0;
      while (done < tail && !rc) {
          i32 n = (i32)((tail - done) < BLK ? (tail - done) : BLK);
          cpp_synth_render(syn, buf, n);
          if (wav_write_frames(w, buf, (u32)n) != 0) rc = 1;
          done += n;
      } }

    if (wav_end(w) != 0) rc = 1;
    cpp_synth_destroy(syn);
    tracker_destroy(trk);
    if (!rc) printf("song: %d repeats + tail @ %dHz -> %s (%.1fs)\n", repeats,
                    sample_rate, out,
                    (double)(one_pass * repeats + tail) / sample_rate);
    return rc;
}

int main(int argc, char **argv) {
    scenes_register_all();

    /* --song <out.wav> [repeats] [rate] [reverb] : render the built-in tune */
    if (argc >= 3 && strcmp(argv[1], "--song") == 0) {
        const char *out = argv[2];
        i32 repeats = argc >= 4 ? atoi(argv[3]) : 2;
        i32 sr      = argc >= 5 ? atoi(argv[4]) : 44100;
        f32 wet     = argc >= 6 ? (f32)atof(argv[5]) : 0.35f;
        return render_song_to_wav(out, sr, repeats, wet);
    }

    /* --audio <out.wav> [seconds] [sample_rate] [waveform 0-3] [reverb 0-1] */
    if (argc >= 3 && strcmp(argv[1], "--audio") == 0) {
        const char *out = argv[2];
        f32 secs   = argc >= 4 ? (f32)atof(argv[3]) : 8.0f;
        i32 sr     = argc >= 5 ? atoi(argv[4]) : 44100;
        i32 wave   = argc >= 6 ? atoi(argv[5]) : 1;   /* default saw */
        f32 wet    = argc >= 7 ? (f32)atof(argv[6]) : 0.35f;
        return render_audio_to_wav(out, secs, sr, wave, wet);
    }

    /* --reel <out.gif> [hold] [xfade] [w] [h] : self-running demo with crossfades */
    if (argc >= 3 && strcmp(argv[1], "--reel") == 0) {
        const char *out=argv[2];
        i32 hold = argc>=4?atoi(argv[3]):40;
        i32 xf   = argc>=5?atoi(argv[4]):15;
        i32 w    = argc>=6?atoi(argv[5]):240;
        i32 h    = argc>=7?atoi(argv[6]):180;
        return render_reel(out, hold, xf, w, h);
    }

    /* --gif <scene> <frames> <out.gif> [w] [h] [preset] */
    if (argc >= 2 && strcmp(argv[1], "--gif") == 0) {
        if (argc < 5) { fprintf(stderr, "usage: %s --gif <scene> <frames> <out.gif> [w] [h] [preset]\n", argv[0]); return 2; }
        i32 idx = find_scene(argv[2]);
        if (idx < 0 || idx >= scene_count()) { fprintf(stderr, "unknown scene: %s\n", argv[2]); return 2; }
        i32 frames = atoi(argv[3]);
        const char *out = argv[4];
        i32 w = argc >= 6 ? atoi(argv[5]) : 240;
        i32 h = argc >= 7 ? atoi(argv[6]) : 180;
        const char *preset = argc >= 8 ? argv[7] : NULL;
        return render_scene_to_gif(idx, frames, w, h, preset, out, 4);
    }

    if (argc >= 2 && strcmp(argv[1], "--all") == 0) {
        i32 frames = argc >= 3 ? atoi(argv[2]) : 60;
        const char *dir = argc >= 4 ? argv[3] : "assets";
        i32 w = argc >= 5 ? atoi(argv[4]) : 320;
        i32 h = argc >= 6 ? atoi(argv[5]) : 240;
        int rc = 0;
        char path[512], raw[512];
        for (i32 i = 0; i < scene_count(); ++i) {
            snprintf(path, sizeof(path), "%s/%02d_%s_crt.png", dir, i, scene_name_at(i));
            snprintf(raw,  sizeof(raw),  "%s/%02d_%s_raw.png", dir, i, scene_name_at(i));
            rc |= render_scene_to_png(i, frames, w, h, NULL, path, raw);
        }
        printf("contact sheet: %d scenes -> %s/\n", scene_count(), dir);
        return rc;
    }

    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <scene|index> <frames> <out.png> [w] [h] [preset]\n"
            "       %s --all <frames> <out-dir> [w] [h]\n"
            "       %s --gif <scene> <frames> <out.gif> [w] [h] [preset]\n"
            "       %s --reel <out.gif> [hold] [xfade] [w] [h]\n"
            "       %s --audio <out.wav> [seconds] [rate] [waveform 0-3] [reverb 0-1]\n"
            "       %s --song <out.wav> [repeats] [rate] [reverb]  (built-in tune)\n"
            "scenes:", argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
        for (i32 i = 0; i < scene_count(); ++i)
            fprintf(stderr, " %d:%s", i, scene_name_at(i));
        fprintf(stderr, "\n");
        return 2;
    }

    i32 idx = find_scene(argv[1]);
    if (idx < 0 || idx >= scene_count()) { fprintf(stderr, "unknown scene: %s\n", argv[1]); return 2; }
    i32 frames = atoi(argv[2]);
    const char *out = argv[3];
    i32 w = argc >= 5 ? atoi(argv[4]) : 320;
    i32 h = argc >= 6 ? atoi(argv[5]) : 240;
    const char *preset = argc >= 7 ? argv[6] : NULL;
    return render_scene_to_png(idx, frames, w, h, preset, out, NULL);
}
