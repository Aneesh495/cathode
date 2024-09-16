/* ==========================================================================
 * cathode/fastmath.h  -  NEON-vectorized transcendental approximations.
 *
 * Like simd.h/dsp.h: each routine has a hand-written NEON implementation and a
 * portable C reference (which calls libm), proven to agree within a stated
 * tolerance by test/test_fastmath.c. These are 4-wide polynomial minimax
 * approximations with range reduction  -  fast enough to run per-pixel in the
 * plasma/tunnel/shader-style scenes without the per-call overhead of libm.
 * ========================================================================== */
#ifndef CATHODE_FASTMATH_H
#define CATHODE_FASTMATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* out[i] = sin(in[i]), abs error < 1e-3 over [-4pi, 4pi]. */
void fm_sin4_neon(float *out, const float *in, unsigned long n);
void fm_sin4_ref (float *out, const float *in, unsigned long n);

/* out[i] = cos(in[i]), abs error < 1e-3 over [-4pi, 4pi]. */
void fm_cos4_neon(float *out, const float *in, unsigned long n);
void fm_cos4_ref (float *out, const float *in, unsigned long n);

/* out[i] = exp(in[i]), rel error < 1e-3 over [-10, 10]. */
void fm_exp4_neon(float *out, const float *in, unsigned long n);
void fm_exp4_ref (float *out, const float *in, unsigned long n);

/* out[i] = log(in[i]) (natural log), abs error < 1e-3 over [1e-6, 1e6].
 * For x <= 0 the result is unspecified (matches libm's domain error being UB
 * here); callers guard their inputs. Implemented via the IEEE-754 exponent +
 * a minimax polynomial on the mantissa in [1,2), the standard fast-log. */
void fm_log4_neon(float *out, const float *in, unsigned long n);
void fm_log4_ref (float *out, const float *in, unsigned long n);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_FASTMATH_H */
