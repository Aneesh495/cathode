/* ==========================================================================
 * cathode/fractalkernel.h — NEON-accelerated escape-time fractal iteration.
 *
 * Computes smooth (continuous) escape counts for 4 complex points at once —
 * the inner loop of a Mandelbrot / Julia renderer. Each routine has a NEON
 * impl and a C reference, proven equal by test/test_fractalkernel.c.
 *
 * "Smooth" iteration count = n + 1 - log2(log2(|z|)) at escape, which removes
 * the banding of integer iteration counts. Points that never escape within
 * max_iter return (f32)max_iter exactly.
 * ========================================================================== */
#ifndef CATHODE_FRACTALKERNEL_H
#define CATHODE_FRACTALKERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mandelbrot: for each of 4 points c=(cre[i],cim[i]), iterate z=z^2+c from
 * z=0; write the smooth escape count to out4[i]. bailout is the escape radius
 * squared (e.g. 256.0). */
void fk_mandel4_neon(float *out4, const float *cre, const float *cim,
                     int max_iter, float bailout2);
void fk_mandel4_ref (float *out4, const float *cre, const float *cim,
                     int max_iter, float bailout2);

/* Julia: same, but z starts at the point and c is a fixed constant (jcre,jcim). */
void fk_julia4_neon(float *out4, const float *zre, const float *zim,
                    float jcre, float jcim, int max_iter, float bailout2);
void fk_julia4_ref (float *out4, const float *zre, const float *zim,
                    float jcre, float jcim, int max_iter, float bailout2);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_FRACTALKERNEL_H */
