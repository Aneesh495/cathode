/* ==========================================================================
 * dsp_ref.c  -  portable C reference for the NEON DSP kernels (dsp.h).
 * NTSC YIQ matrices; symmetric FIR; one-pole IIR; scale/bias/clamp.
 * ========================================================================== */
#include "cathode/dsp.h"

/* NTSC RGB<->YIQ. These exact constants are mirrored in dsp_neon.s so the
 * asm and reference agree closely. */
void dsp_rgb2yiq_ref(float *out, const float *rgb, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) {
        float r = rgb[3*i+0], g = rgb[3*i+1], b = rgb[3*i+2];
        out[3*i+0] = 0.299000f*r + 0.587000f*g + 0.114000f*b;      /* Y */
        out[3*i+1] = 0.595716f*r - 0.274453f*g - 0.321263f*b;      /* I */
        out[3*i+2] = 0.211456f*r - 0.522591f*g + 0.311135f*b;      /* Q */
    }
}

void dsp_yiq2rgb_ref(float *out, const float *yiq, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) {
        float y = yiq[3*i+0], iq = yiq[3*i+1], q = yiq[3*i+2];
        out[3*i+0] = y + 0.956000f*iq + 0.621000f*q;               /* R */
        out[3*i+1] = y - 0.272000f*iq - 0.647000f*q;               /* G */
        out[3*i+2] = y - 1.107000f*iq + 1.704600f*q;               /* B */
    }
}

/* Symmetric FIR, edge-clamped: out[i] = ker[0]*in[i]
 *                              + sum_{k=1..r} ker[k]*(in[i-k]+in[i+k]) */
void dsp_fir_sym_ref(float *out, const float *in, unsigned long n,
                     const float *ker, int r) {
    if (n == 0) return;
    long N = (long)n;
    for (long i = 0; i < N; ++i) {
        float acc = ker[0] * in[i];
        for (int k = 1; k <= r; ++k) {
            long lo = i - k; if (lo < 0) lo = 0;
            long hi = i + k; if (hi > N-1) hi = N-1;
            acc += ker[k] * (in[lo] + in[hi]);
        }
        out[i] = acc;
    }
}

/* One-pole temporal IIR: dst = dst*a + src*(1-a). Phosphor persistence. */
void dsp_iir_blend_ref(float *dst, const float *src, float a, unsigned long n) {
    float b = 1.0f - a;
    for (unsigned long i = 0; i < n; ++i)
        dst[i] = dst[i]*a + src[i]*b;
}

void dsp_scale_bias_clamp_ref(float *out, const float *in, float g, float b,
                              float hi, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) {
        float v = in[i]*g + b;
        if (v < 0.0f) v = 0.0f; else if (v > hi) v = hi;
        out[i] = v;
    }
}
