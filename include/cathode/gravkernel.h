/* ==========================================================================
 * cathode/gravkernel.h — NEON-accelerated gravitational force kernel.
 *
 * Computes the acceleration on a single body from a batch of source bodies,
 * 4 sources at a time, with Plummer softening:
 *     a = sum_j  G * m_j * (p_j - p) / (|p_j - p|^2 + eps^2)^(3/2)
 * This is the inner loop of a direct O(N^2) N-body step and of Barnes-Hut leaf
 * interactions. Sources are SoA (sx[],sy[],sz[],sm[]) so 4 load into one lane.
 * Validated bit-closely vs the C reference in gravkernel_ref.c.
 * ========================================================================== */
#ifndef CATHODE_GRAVKERNEL_H
#define CATHODE_GRAVKERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* Accumulate acceleration on the body at (px,py,pz) from `n` source bodies.
 * sx,sy,sz,sm are parallel arrays of length n. eps2 is the softening squared,
 * g the gravitational constant. Writes the 3-vector acceleration to out3.
 * A source coincident with the target contributes ~0 (softening prevents the
 * singularity), matching the reference. */
void grav_accum_neon(float *out3, float px, float py, float pz,
                     const float *sx, const float *sy, const float *sz,
                     const float *sm, unsigned long n, float g, float eps2);
void grav_accum_ref (float *out3, float px, float py, float pz,
                     const float *sx, const float *sy, const float *sz,
                     const float *sm, unsigned long n, float g, float eps2);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_GRAVKERNEL_H */
