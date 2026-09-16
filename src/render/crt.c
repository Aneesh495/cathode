/* ==========================================================================
 * crt.c  -  physically-flavored NTSC composite + CRT display emulator.
 *
 * I/Q (QAM) composite path mirrors analog video hardware:
 *
 *   linear RGB
 *     -> pre-gain (brightness/contrast/saturation)
 *     -> RGB->YIQ                                        (dsp_rgb2yiq)
 *     -> COMPOSITE ENCODE: 1-D signal per scanline,
 *          c(x) = Y(x) + I(x)cos(phi(x)) + Q(x)sin(phi(x))
 *        where phi advances with the color subcarrier (+ per-frame dot crawl)
 *     -> channel: additive noise, ringing (overshoot)
 *     -> COMPOSITE DECODE (I/Q demodulation):
 *          Y' = lowpass(c)                               (dsp_fir_sym)
 *          I' = lowpass( 2 c cos(phi) ), Q' = lowpass( 2 c sin(phi) )
 *        chroma bleed & dot crawl fall out of the math, like a real
 *        comb/notch decoder.
 *     -> YIQ->RGB                                        (dsp_yiq2rgb)
 *     -> phosphor persistence (temporal IIR)             (dsp_iir_blend)
 *     -> bloom (bright-pass + separable blur, added back)
 *     -> display geometry: barrel distortion, scanlines,
 *        aperture-grille shadow mask, vignette
 *
 * Documented gates: >=114 MS/s composite throughput; <0.5% NRMSE across
 * >=13K DSP vectors; used in the 1440p <2 ms/frame headless scene path.
 * ========================================================================== */
#include "cathode/crt.h"
#include "cathode/dsp.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct CrtState {
    i32 w, h;
    CrtConfig cfg;
    /* per-scanline scratch (length w) */
    f32 *sY, *sI, *sQ;          /* YIQ rows */
    f32 *comp;                  /* composite signal */
    f32 *dY, *dI, *dQ;          /* decoded YIQ */
    f32 *tmp;                   /* filter scratch */
    f32 *cosph, *sinph;         /* subcarrier tables per column (per frame) */
    /* full-frame buffers */
    f32 *decoded;               /* w*h*3 RGB after decode */
    f32 *phosphor;              /* w*h*3 persistent phosphor accumulator */
    f32 *bloom;                 /* w*h*3 bloom scratch */
    f32 *bloom2;                /* w*h*3 bloom scratch B */
    u32  rng;                   /* xorshift for channel noise */
    int  have_phosphor;
};

/* ---- presets ---- */
CrtConfig crt_config_default(void) {
    CrtConfig c;
    c.subcarrier_cycles = 0.25f;   /* cycles per pixel along a scanline */
    c.chroma_bleed = 0.5f;
    c.noise = 0.015f;
    c.ringing = 0.15f;
    c.dot_crawl = 0.6f;
    c.persistence = 0.35f;
    c.bloom = 0.35f;
    c.bloom_threshold = 0.7f;
    c.scanline_depth = 0.35f;
    c.mask_strength = 0.25f;
    c.barrel = 0.08f;
    c.vignette = 0.25f;
    c.brightness = 0.0f;
    c.contrast = 1.05f;
    c.saturation = 1.15f;
    c.interlace = 0.0f;     /* progressive by default; presets opt in below */
    c.mask_pitch = 2;       /* 2 display columns per phosphor stripe */
    c.frame = 0;
    return c;
}

CrtConfig crt_config_preset(const char *name) {
    CrtConfig c = crt_config_default();
    if (!name) return c;
    if (!strcmp(name, "trinitron")) {
        c.subcarrier_cycles=0.25f; c.chroma_bleed=0.35f; c.noise=0.006f;
        c.ringing=0.12f; c.dot_crawl=0.35f; c.persistence=0.28f;
        c.bloom=0.4f; c.bloom_threshold=0.65f; c.scanline_depth=0.28f;
        c.mask_strength=0.4f; c.barrel=0.05f; c.vignette=0.2f;
        c.contrast=1.08f; c.saturation=1.2f;
    } else if (!strcmp(name, "broadcast")) {
        c.subcarrier_cycles=0.25f; c.chroma_bleed=0.6f; c.noise=0.02f;
        c.ringing=0.22f; c.dot_crawl=0.8f; c.persistence=0.4f;
        c.bloom=0.5f; c.bloom_threshold=0.6f; c.scanline_depth=0.35f;
        c.mask_strength=0.2f; c.barrel=0.09f; c.vignette=0.3f;
        c.contrast=1.05f; c.saturation=1.25f; c.interlace=0.18f;  /* 480i shimmer */
    } else if (!strcmp(name, "vhs")) {
        c.subcarrier_cycles=0.22f; c.chroma_bleed=0.9f; c.noise=0.05f;
        c.ringing=0.35f; c.dot_crawl=1.1f; c.persistence=0.55f;
        c.bloom=0.45f; c.bloom_threshold=0.55f; c.scanline_depth=0.3f;
        c.mask_strength=0.12f; c.barrel=0.11f; c.vignette=0.4f;
        c.contrast=1.0f; c.saturation=1.1f; c.interlace=0.12f;    /* soft interline */
    } else if (!strcmp(name, "arcade")) {
        c.subcarrier_cycles=0.28f; c.chroma_bleed=0.3f; c.noise=0.004f;
        c.ringing=0.1f; c.dot_crawl=0.2f; c.persistence=0.22f;
        c.bloom=0.6f; c.bloom_threshold=0.5f; c.scanline_depth=0.45f;
        c.mask_strength=0.5f; c.barrel=0.06f; c.vignette=0.25f;
        c.contrast=1.15f; c.saturation=1.35f;
    } else if (!strcmp(name, "clean")) {
        c.subcarrier_cycles=0.25f; c.chroma_bleed=0.12f; c.noise=0.0f;
        c.ringing=0.03f; c.dot_crawl=0.0f; c.persistence=0.12f;
        c.bloom=0.25f; c.bloom_threshold=0.8f; c.scanline_depth=0.12f;
        c.mask_strength=0.08f; c.barrel=0.02f; c.vignette=0.12f;
        c.contrast=1.0f; c.saturation=1.05f;
    }
    return c;
}

static f32 *falloc(i32 n) { return (f32 *)calloc((size_t)n, sizeof(f32)); }

CrtState *crt_create(i32 w, i32 h, const CrtConfig *cfg) {
    CrtState *s = (CrtState *)calloc(1, sizeof(CrtState));
    if (!s) return NULL;
    s->w = w; s->h = h;
    s->cfg = cfg ? *cfg : crt_config_default();
    s->sY=falloc(w); s->sI=falloc(w); s->sQ=falloc(w);
    s->comp=falloc(w); s->dY=falloc(w); s->dI=falloc(w); s->dQ=falloc(w);
    s->tmp=falloc(w); s->cosph=falloc(w); s->sinph=falloc(w);
    s->decoded=falloc(w*h*3);
    s->phosphor=falloc(w*h*3);
    s->bloom=falloc(w*h*3);
    s->bloom2=falloc(w*h*3);
    s->rng = 0x1234567u;
    s->have_phosphor = 0;
    return s;
}

void crt_destroy(CrtState *s) {
    if (!s) return;
    free(s->sY);free(s->sI);free(s->sQ);free(s->comp);
    free(s->dY);free(s->dI);free(s->dQ);free(s->tmp);
    free(s->cosph);free(s->sinph);
    free(s->decoded);free(s->phosphor);free(s->bloom);free(s->bloom2);
    free(s);
}

void crt_set_config(CrtState *s, const CrtConfig *cfg) {
    if (s && cfg) s->cfg = *cfg;
}

static inline u32 xorshift(u32 *st) {
    u32 x = *st; x ^= x<<13; x ^= x>>17; x ^= x<<5; *st = x; return x;
}
static inline f32 noisef(u32 *st) { /* [-1,1] */
    return (f32)(xorshift(st) >> 8) * (1.0f/8388608.0f) - 1.0f;
}

/* Chroma low-pass taps (symmetric FIR). Wider kernel => more chroma bleed. */
static void build_chroma_kernel(f32 bleed, f32 *ker, int *r) {
    /* bleed 0..1 maps to radius 1..4; gaussian-ish normalized taps */
    int radius = 1 + (int)(bleed * 3.0f + 0.5f);
    if (radius > 4) radius = 4;
    f32 sigma = 0.6f + bleed * 1.6f;
    f32 sum = 1.0f; ker[0] = 1.0f;
    for (int k = 1; k <= radius; ++k) {
        ker[k] = expf(-(f32)(k*k) / (2.0f*sigma*sigma));
        sum += 2.0f * ker[k];
    }
    for (int k = 0; k <= radius; ++k) ker[k] /= sum;
    *r = radius;
}

/* Luma low-pass: mild, keeps detail but bandlimits like a real Y channel. */
static void build_luma_kernel(f32 *ker, int *r) {
    ker[0]=0.5f; ker[1]=0.22f; ker[2]=0.03f; *r=2;
    f32 sum = ker[0]+2*(ker[1]+ker[2]);
    for (int k=0;k<=2;++k) ker[k]/=sum;
}

void crt_process(CrtState *s, const Framebuffer *src, Framebuffer *dst) {
    const i32 w = s->w, h = s->h;
    const CrtConfig *c = &s->cfg;

    f32 ck[8]; int cr; build_chroma_kernel(c->chroma_bleed, ck, &cr);
    f32 lk[8]; int lr; build_luma_kernel(lk, &lr);

    /* Per-frame subcarrier phase tables: dot crawl shifts the phase each frame.
     *
     * The phase advance is deliberately PERIODIC with a period of 4 frames, the
     * way real NTSC dot crawl repeats every 4 fields. Letting it drift without
     * bound (frame * dot_crawl) meant every pixel's decoded chroma changed on
     * every single frame forever, so the terminal presenter's frame-to-frame
     * diff could never skip a cell  -  that alone was ~80% of the escape-code
     * traffic and the reason the interactive demo crawled. With a 4-frame cycle
     * the crawl still visibly shimmers, but static content returns to bit-identical
     * values and the diff renderer does its job. */
    f32 phase0 = (f32)(((u32)c->frame) & 3u) * c->dot_crawl;
    for (i32 x = 0; x < w; ++x) {
        f32 phi = CT_TAU * c->subcarrier_cycles * (f32)x + phase0;
        s->cosph[x] = cosf(phi);
        s->sinph[x] = sinf(phi);
    }

    const f32 sat = c->saturation, con = c->contrast, bri = c->brightness;

    /* ---------- ENCODE + DECODE, scanline by scanline ---------- */
    for (i32 y = 0; y < h; ++y) {
        const f32 *row = &src->px[(size_t)y * w * 3];

        /* pre-gain in RGB, then RGB->YIQ for the whole row */
        /* Reuse comp as an interleaved RGB scratch, then convert. */
        for (i32 x = 0; x < w; ++x) {
            f32 r = row[3*x+0], g = row[3*x+1], b = row[3*x+2];
            /* contrast around 0.5, brightness offset */
            r = (r - 0.5f)*con + 0.5f + bri;
            g = (g - 0.5f)*con + 0.5f + bri;
            b = (b - 0.5f)*con + 0.5f + bri;
            s->tmp[x] = r; /* stash R,G,B via three separate scratch arrays */
            s->dY[x] = g;
            s->dQ[x] = b;
        }
        /* Convert RGB->YIQ for the row: build an interleaved RGB scratch in the
         * bloom buffer's first 3w floats, run the NEON kernel into bloom2. */
        f32 *rgbrow = s->bloom;   /* borrow w*h*3; use first 3w for a row */
        for (i32 x = 0; x < w; ++x) {
            rgbrow[3*x+0] = s->tmp[x];
            rgbrow[3*x+1] = s->dY[x];
            rgbrow[3*x+2] = s->dQ[x];
        }
        f32 *yiqrow = s->bloom2;
        dsp_rgb2yiq_neon(yiqrow, rgbrow, (unsigned long)w);
        for (i32 x = 0; x < w; ++x) {
            s->sY[x] = yiqrow[3*x+0];
            s->sI[x] = yiqrow[3*x+1] * sat;   /* saturation scales chroma */
            s->sQ[x] = yiqrow[3*x+2] * sat;
        }

        /* COMPOSITE ENCODE: c(x) = Y + I cos + Q sin, + ringing + noise */
        for (i32 x = 0; x < w; ++x) {
            f32 comp = s->sY[x] + s->sI[x]*s->cosph[x] + s->sQ[x]*s->sinph[x];
            s->comp[x] = comp;
        }
        /* ringing: high-pass overshoot (comp - blurred) added back */
        if (c->ringing > 0.0f) {
            dsp_fir_sym_neon(s->tmp, s->comp, (unsigned long)w, lk, lr);
            for (i32 x = 0; x < w; ++x)
                s->comp[x] += c->ringing * (s->comp[x] - s->tmp[x]);
        }
        /* Channel noise. The RNG is re-seeded from (row, frame&3) rather than
         * advanced continuously, so the noise pattern REPEATS on a 4-frame cycle
         * instead of being fresh every frame. Continuous noise made every pixel
         * differ on every frame, which defeated the terminal presenter's diff and
         * was a large part of why the interactive demo ran at a few fps. A short
         * repeat still reads as analog grain to the eye. */
        if (c->noise > 0.0f) {
            u32 nrng = 0x9E3779B9u ^ ((u32)y * 2654435761u)
                     ^ ((u32)(c->frame & 3) * 40503u);
            if (!nrng) nrng = 1u;
            for (i32 x = 0; x < w; ++x)
                s->comp[x] += c->noise * noisef(&nrng);
        }

        /* COMPOSITE DECODE.
         * Y' = lowpass(comp). */
        dsp_fir_sym_neon(s->dY, s->comp, (unsigned long)w, lk, lr);
        /* I' = lowpass(2 comp cos), Q' = lowpass(2 comp sin). */
        for (i32 x = 0; x < w; ++x) s->tmp[x] = 2.0f * s->comp[x] * s->cosph[x];
        dsp_fir_sym_neon(s->dI, s->tmp, (unsigned long)w, ck, cr);
        for (i32 x = 0; x < w; ++x) s->tmp[x] = 2.0f * s->comp[x] * s->sinph[x];
        dsp_fir_sym_neon(s->dQ, s->tmp, (unsigned long)w, ck, cr);

        /* YIQ->RGB back into decoded frame */
        for (i32 x = 0; x < w; ++x) {
            yiqrow[3*x+0] = s->dY[x];
            yiqrow[3*x+1] = s->dI[x];
            yiqrow[3*x+2] = s->dQ[x];
        }
        dsp_yiq2rgb_neon(&s->decoded[(size_t)y*w*3], yiqrow, (unsigned long)w);
    }

    /* ---------- PHOSPHOR PERSISTENCE (temporal IIR) ---------- */
    if (!s->have_phosphor) {
        memcpy(s->phosphor, s->decoded, (size_t)w*h*3*sizeof(f32));
        s->have_phosphor = 1;
    } else {
        /* phosphor = phosphor*persistence + decoded*(1-persistence) */
        dsp_iir_blend_neon(s->phosphor, s->decoded, c->persistence,
                           (unsigned long)w*h*3);
    }
    /* The lit image is max(decoded, phosphor-trail): current frame plus the
     * decaying trail. Use screen-ish combine: take the brighter. */
    f32 *lit = s->decoded; /* write combined into decoded */
    for (size_t i = 0; i < (size_t)w*h*3; ++i) {
        f32 tr = s->phosphor[i];
        if (tr > lit[i]) lit[i] = tr;
    }

    /* ---------- BLOOM: bright-pass + separable blur ---------- */
    if (c->bloom > 0.0f) {
        const f32 th = c->bloom_threshold;
        for (size_t i = 0; i < (size_t)w*h*3; ++i) {
            f32 v = lit[i] - th;
            s->bloom[i] = v > 0.0f ? v : 0.0f;
        }
        /* horizontal blur bloom -> bloom2 */
        f32 bk[8]; int br=3; bk[0]=0.3f;bk[1]=0.23f;bk[2]=0.12f;bk[3]=0.05f;
        f32 bs=bk[0]+2*(bk[1]+bk[2]+bk[3]); for(int k=0;k<=br;++k) bk[k]/=bs;
        for (i32 ch = 0; ch < 3; ++ch) {
            for (i32 y = 0; y < h; ++y) {
                /* gather channel row */
                for (i32 x = 0; x < w; ++x) s->tmp[x] = s->bloom[((size_t)y*w+x)*3+ch];
                dsp_fir_sym_neon(s->comp, s->tmp, (unsigned long)w, bk, br);
                for (i32 x = 0; x < w; ++x) s->bloom2[((size_t)y*w+x)*3+ch] = s->comp[x];
            }
        }
        /* vertical blur bloom2 -> bloom (column gather) */
        f32 *coltmp = falloc(h), *colout = falloc(h);
        for (i32 ch = 0; ch < 3; ++ch) {
            for (i32 x = 0; x < w; ++x) {
                for (i32 y = 0; y < h; ++y) coltmp[y] = s->bloom2[((size_t)y*w+x)*3+ch];
                dsp_fir_sym_neon(colout, coltmp, (unsigned long)h, bk, br);
                for (i32 y = 0; y < h; ++y) s->bloom[((size_t)y*w+x)*3+ch] = colout[y];
            }
        }
        free(coltmp); free(colout);
        /* add bloom back */
        for (size_t i = 0; i < (size_t)w*h*3; ++i)
            lit[i] += c->bloom * s->bloom[i];
    }

    /* ---------- DISPLAY GEOMETRY into dst ---------- */
    const f32 cx = (w - 1) * 0.5f, cy = (h - 1) * 0.5f;
    const f32 inv_rx = 1.0f / cx, inv_ry = 1.0f / cy;
    for (i32 y = 0; y < h; ++y) {
        for (i32 x = 0; x < w; ++x) {
            /* barrel distortion: sample source at r' = r*(1+barrel*r^2) */
            f32 nx = ((f32)x - cx) * inv_rx;   /* -1..1 */
            f32 ny = ((f32)y - cy) * inv_ry;
            f32 r2 = nx*nx + ny*ny;
            f32 k = 1.0f + c->barrel * r2;
            f32 sx = cx + (nx * k) * cx;
            f32 sy = cy + (ny * k) * cy;

            Color3 outc = col3(0,0,0);
            if (sx >= 0 && sx <= w-1 && sy >= 0 && sy <= h-1) {
                /* bilinear sample of lit */
                i32 x0 = (i32)sx, y0 = (i32)sy;
                i32 x1 = x0+1<w? x0+1:x0, y1 = y0+1<h? y0+1:y0;
                f32 fx = sx-x0, fy = sy-y0;
                const f32 *p00=&lit[((size_t)y0*w+x0)*3];
                const f32 *p10=&lit[((size_t)y0*w+x1)*3];
                const f32 *p01=&lit[((size_t)y1*w+x0)*3];
                const f32 *p11=&lit[((size_t)y1*w+x1)*3];
                for (int ch=0; ch<3; ++ch) {
                    f32 a = p00[ch]*(1-fx)+p10[ch]*fx;
                    f32 b = p01[ch]*(1-fx)+p11[ch]*fx;
                    f32 v = a*(1-fy)+b*fy;
                    ((f32*)&outc)[ch] = v;
                }
            }

            /* Scanline darkening. NOTE: the obvious `sinf(y*PI)` is ~0 for every
             * integer y, so it produced a CONSTANT dim instead of alternating
             * lines  -  use row parity directly. Because the terminal shows two
             * framebuffer rows per character cell, we darken every other PAIR of
             * rows so the scanline is actually visible as a line rather than
             * cancelling out inside a cell. */
            f32 scan = 1.0f - c->scanline_depth * (((y >> 1) & 1) ? 1.0f : 0.0f);
            /* interlacing: on each frame only one field is "fresh"; the other is
             * dimmed. Alternate per frame for NTSC's 480i interline flicker.
             * Also per row-pair, for the same cell-granularity reason. */
            if (c->interlace > 0.0f) {
                int active_field = c->frame & 1;
                if (((y >> 1) & 1) != active_field) scan *= (1.0f - c->interlace);
            }
            /* Aperture-grille shadow mask: tint by column triad. The triad must
             * span several DISPLAY columns, otherwise at one framebuffer pixel
             * per terminal column it degenerates into harsh 1px R/G/B stripes
             * (this is what made the output look like vertical candy-cane bars).
             * `mask_pitch` sets how many columns each phosphor stripe covers. */
            f32 mr=1,mg=1,mb=1;
            if (c->mask_strength > 0.0f) {
                i32 pitch = c->mask_pitch > 0 ? c->mask_pitch : 2;
                int tri = (x / pitch) % 3;
                f32 ms = c->mask_strength;
                mr = 1.0f - ms*(tri!=0);
                mg = 1.0f - ms*(tri!=1);
                mb = 1.0f - ms*(tri!=2);
            }
            /* vignette (radial) */
            f32 vig = 1.0f - c->vignette * r2;
            if (vig < 0) vig = 0;

            f32 g = scan * vig;
            outc.r *= g*mr; outc.g *= g*mg; outc.b *= g*mb;
            if (outc.r < 0) outc.r=0; if (outc.g < 0) outc.g=0; if (outc.b < 0) outc.b=0;
            dst->px[((size_t)y*w+x)*3+0]=outc.r;
            dst->px[((size_t)y*w+x)*3+1]=outc.g;
            dst->px[((size_t)y*w+x)*3+2]=outc.b;
        }
    }

    s->cfg.frame++;
}
