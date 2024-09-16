/* ==========================================================================
 * cathode/dsp.h  -  NEON-accelerated signal-processing kernels.
 * Like simd.h: each routine has a hand-written asm impl and a C reference,
 * proven equal by test/test_dsp.c. Used heavily by the CRT signal chain.
 * ========================================================================== */
#ifndef CATHODE_DSP_H
#define CATHODE_DSP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Convert a row of n interleaved RGB (3n floats) to YIQ in place-compatible
 * out buffer (also 3n floats, laid out as interleaved Y,I,Q). NTSC matrix. */
void dsp_rgb2yiq_neon(float *out, const float *rgb, unsigned long n);
void dsp_rgb2yiq_ref (float *out, const float *rgb, unsigned long n);
void dsp_yiq2rgb_neon(float *out, const float *yiq, unsigned long n);
void dsp_yiq2rgb_ref (float *out, const float *yiq, unsigned long n);

/* Symmetric FIR: out[i] = sum_{k=-r..r} in[i+k]*ker[|k|], edge-clamped.
 * ker has r+1 taps (ker[0] center). n samples, single channel. */
void dsp_fir_sym_neon(float *out, const float *in, unsigned long n,
                      const float *ker, int r);
void dsp_fir_sym_ref (float *out, const float *in, unsigned long n,
                      const float *ker, int r);

/* One-pole IIR temporal blend: dst[i] = dst[i]*a + src[i]*(1-a) over n floats.
 * This is the phosphor-persistence kernel. */
void dsp_iir_blend_neon(float *dst, const float *src, float a, unsigned long n);
void dsp_iir_blend_ref (float *dst, const float *src, float a, unsigned long n);

/* Clamp+scale: out[i] = clamp(in[i]*g + b, 0, hi) over n floats. */
void dsp_scale_bias_clamp_neon(float *out, const float *in, float g, float b,
                               float hi, unsigned long n);
void dsp_scale_bias_clamp_ref (float *out, const float *in, float g, float b,
                               float hi, unsigned long n);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_DSP_H */
