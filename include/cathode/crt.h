/* ==========================================================================
 * cathode/crt.h  -  physically-modeled NTSC composite + CRT display chain.
 *
 * Pipeline (all in software DSP; NEON helpers in dsp.h / dsp_neon.s):
 *   linear RGB framebuffer
 *     -> gain / YIQ encode
 *     -> I/Q (QAM) composite modulation on the color subcarrier:
 *          c = Y + I*cos(phi) + Q*sin(phi)
 *     -> channel: noise, ringing, dot-crawl
 *     -> comb/notch I/Q demodulate -> YIQ -> RGB   (chroma bleed emerges)
 *     -> phosphor persistence (temporal IIR), bloom, scanlines,
 *        shadow-mask, barrel distortion, vignette
 *   -> output framebuffer for the TUI or headless capture.
 *
 * Documented DSP gates (see docs/BENCHMARKS.md, docs/TESTING.md):
 *   - sustained composite path throughput >= 114 MS/s
 *   - encode/decode (+ neon vs ref) NRMSE < 0.5% over >= 13,000 vectors
 * Headless scene+CRT timing targets < 2 ms/frame at 1440p (2560x1440).
 * ========================================================================== */
#ifndef CATHODE_CRT_H
#define CATHODE_CRT_H

#include "cathode/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* signal */
    f32 subcarrier_cycles;  /* NTSC color subcarrier cycles per pixel-row scan */
    f32 chroma_bleed;       /* 0..1 lateral chroma smear */
    f32 noise;              /* additive luma noise amplitude */
    f32 ringing;            /* filter overshoot amount */
    f32 dot_crawl;          /* animated subcarrier phase per frame */
    /* display */
    f32 persistence;        /* phosphor decay 0..1 (higher = longer trails) */
    f32 bloom;              /* highlight bleed */
    f32 bloom_threshold;
    f32 scanline_depth;     /* 0..1 */
    f32 mask_strength;      /* aperture-grille / shadow-mask 0..1 */
    i32 mask_pitch;         /* display columns per phosphor stripe (>=1). At 1 the
                             * R/G/B triad aliases into harsh vertical stripes on
                             * a terminal (1 px per column); 2-3 looks like a real
                             * mask. 0 is treated as the default (2). */
    f32 barrel;             /* screen curvature */
    f32 vignette;
    f32 brightness, contrast, saturation;
    f32 interlace;          /* 0..1 interlaced-field dimming: real NTSC draws
                             * odd rows on one field, even on the next; on each
                             * frame the "off" field's rows are dimmed by this
                             * amount, giving the characteristic interline flicker
                             * and 480i shimmer. 0 = progressive (no effect). */
    i32 frame;              /* incremented each present, drives dot crawl */
} CrtConfig;

typedef struct CrtState CrtState;

CrtConfig crt_config_default(void);
CrtConfig crt_config_preset(const char *name); /* "trinitron","broadcast","vhs","arcade","clean" */

CrtState *crt_create(i32 w, i32 h, const CrtConfig *cfg);
void      crt_destroy(CrtState *s);
void      crt_set_config(CrtState *s, const CrtConfig *cfg);

/* Process one frame: src linear RGB -> dst display RGB. src/dst may differ.
 * Advances internal temporal state (phosphor, dot crawl). */
void      crt_process(CrtState *s, const Framebuffer *src, Framebuffer *dst);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_CRT_H */
