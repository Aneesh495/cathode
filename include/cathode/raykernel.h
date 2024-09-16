/* ==========================================================================
 * cathode/raykernel.h  -  NEON batched ray-intersection kernels.
 *
 * All 4 lanes computed in parallel  -  the building block of a wide CPU ray
 * tracer. Each routine has a NEON impl and a C reference, proven equal by
 * test/test_raykernel.c.
 *
 * SoA layout (structure-of-arrays) so 4 primitives load into one q-register:
 *   spheres:  sph_soa = [cx0 cx1 cx2 cx3, cy0..cy3, cz0..cz3, r0..r3]  (16 f)
 *   aabbs:    aabb_soa= [minx0..3, miny0..3, minz0..3, maxx0..3, maxy0..3, maxz0..3] (24 f)
 * Rays: ro[3], rd[3] (rd need not be normalized for the AABB slab test; for
 * spheres we assume rd is normalized so the quadratic 'a' term is 1).
 *
 * Outputs: out4[4] = nearest positive hit distance t per primitive, or a
 * negative value (miss sentinel -1) if there is no hit in front of the ray.
 * ========================================================================== */
#ifndef CATHODE_RAYKERNEL_H
#define CATHODE_RAYKERNEL_H

#ifdef __cplusplus
extern "C" {
#endif

void rk_ray4_spheres_neon(float *out4, const float *ro, const float *rd, const float *sph_soa);
void rk_ray4_spheres_ref (float *out4, const float *ro, const float *rd, const float *sph_soa);

void rk_ray4_aabb_neon(float *out4, const float *ro, const float *rd, const float *aabb_soa);
void rk_ray4_aabb_ref (float *out4, const float *ro, const float *rd, const float *aabb_soa);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_RAYKERNEL_H */
