/* ==========================================================================
 * postfx.h — NEON-accelerated post-processing kernels (internal app helper,
 * not part of the frozen ABI). Each has a hand-written asm impl + C reference,
 * proven equal by test/test_postfx.c. Used by the bloom / exposure path.
 * ========================================================================== */
#ifndef CATHODE_POSTFX_H
#define CATHODE_POSTFX_H

#ifdef __cplusplus
extern "C" {
#endif

/* dst[i] += src[i] * scale, over n floats (bloom / light accumulation). */
void postfx_accumulate_neon(float *dst, const float *src, float scale, unsigned long n);
void postfx_accumulate_ref (float *dst, const float *src, float scale, unsigned long n);

/* Screen blend: dst[i] = 1 - (1-dst[i])*(1-src[i]), clamped to [0,1], n floats. */
void postfx_screen_neon(float *dst, const float *src, unsigned long n);
void postfx_screen_ref (float *dst, const float *src, unsigned long n);

/* Bright-pass: out[i] = max(0, in[i]-threshold) * knee, n floats. */
void postfx_brightpass_neon(float *out, const float *in, float threshold, float knee, unsigned long n);
void postfx_brightpass_ref (float *out, const float *in, float threshold, float knee, unsigned long n);

/* Horizontal 1-4-6-4-1 (/16) blur on a single row of n floats, edge-clamped.
 * (One separable pass; call again on the transpose for a 2D Gaussian.) */
void postfx_blur5_neon(float *out, const float *in, unsigned long n);
void postfx_blur5_ref (float *out, const float *in, unsigned long n);

/* Sum reduction of n floats (for average-luminance auto-exposure). */
float postfx_sum_neon(const float *in, unsigned long n);
float postfx_sum_ref (const float *in, unsigned long n);

/* Max reduction of n floats (for peak-white auto-exposure). */
float postfx_max_neon(const float *in, unsigned long n);
float postfx_max_ref (const float *in, unsigned long n);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_POSTFX_H */
