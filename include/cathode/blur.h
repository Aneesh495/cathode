/* ==========================================================================
 * cathode/blur.h  -  NEON-accelerated separable blur kernels.
 *
 * A 2D Gaussian blur factors into a horizontal then a vertical 1D pass. The
 * horizontal pass over contiguous rows vectorizes cleanly (4 pixels at once);
 * the vertical pass is done by the same routine on the transposed buffer, or
 * via a strided variant. Each routine has a NEON impl + C reference, proven
 * equal by test/test_blur.c.
 *
 * Buffers are single-channel f32, row-major, width w, height h.
 * ========================================================================== */
#ifndef CATHODE_BLUR_H
#define CATHODE_BLUR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Horizontal 1D Gaussian blur, radius r, edge-clamped. ker has r+1 taps
 * (ker[0] center), assumed already normalized so the full symmetric kernel
 * sums to 1. Processes every row of a w*h single-channel image. */
void blur_h_neon(float *dst, const float *src, int w, int h, const float *ker, int r);
void blur_h_ref (float *dst, const float *src, int w, int h, const float *ker, int r);

/* Vertical 1D Gaussian blur (same kernel convention), edge-clamped. */
void blur_v_neon(float *dst, const float *src, int w, int h, const float *ker, int r);
void blur_v_ref (float *dst, const float *src, int w, int h, const float *ker, int r);

/* Build a normalized Gaussian kernel of radius r into ker[0..r] (r+1 taps,
 * center first). sigma controls the falloff. Returns r (clamped to <=15). */
int  blur_gaussian_kernel(float *ker, int r, float sigma);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_BLUR_H */
