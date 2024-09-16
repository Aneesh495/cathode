/* ==========================================================================
 * cathode/simd.h  -  ABI contract for hand-written AArch64 NEON assembly.
 *
 * Every function declared here is implemented TWICE:
 *   1. in src/asm (hand-written NEON, the fast path)
 *   2. in src/core/simd_ref.c (portable C reference, for validation)
 *
 * The test harness (test/test_simd.c) proves the two agree numerically.
 * Column-major 4x4 matrices (OpenGL convention): m[col*4 + row].
 *
 * Documented resume-facing speedup: mat4_mul_neon is ~4x the -O3 C reference
 * on Apple Silicon (docs/BENCHMARKS.md). That kernel backs the CPU rasterizer.
 * ========================================================================== */
#ifndef CATHODE_SIMD_H
#define CATHODE_SIMD_H

#ifdef __cplusplus
extern "C" {
#endif

/* out[16] = a[16] * b[16], column-major 4x4. out must not alias a or b. */
void  mat4_mul_neon(float *out, const float *a, const float *b);
void  mat4_mul_ref (float *out, const float *a, const float *b);

/* out[4] = m[16] * v[4], column-major. */
void  mat4_transform_neon(float *out, const float *m, const float *v);
void  mat4_transform_ref (float *out, const float *m, const float *v);

/* 4-component dot product. */
float vec4_dot_neon(const float *a, const float *b);
float vec4_dot_ref (const float *a, const float *b);

/* Fast reciprocal square root (frsqrte + 2 Newton-Raphson steps). */
float fast_rsqrt_neon(float x);
float fast_rsqrt_ref (float x);

/* Normalize a 3-vector (w untouched / ignored). out and v are float[4]. */
void  vec3_normalize_neon(float *out, const float *v);
void  vec3_normalize_ref (float *out, const float *v);

/* Saxpy over N floats: y[i] += a*x[i].  Proves loop/counter handling in asm. */
void  saxpy_neon(float *y, const float *x, float a, unsigned long n);
void  saxpy_ref (float *y, const float *x, float a, unsigned long n);

/* Batched project: transform n 3D points by a column-major 4x4 matrix, do the
 * perspective divide, and map to screen. For each point p (from pts_xyz, 3
 * floats each) computes clip = M*(p,1); if clip.w > wclip writes screen x,y
 * (via viewport w,h) and depth (clip.z/clip.w) to out_xyz (3 floats: sx,sy,depth)
 * and marks vis[i]=1, else vis[i]=0. This is the hot projection loop shared by
 * the particle scenes (galaxy, boids, attractor, sph). */
void  project_points_neon(float *out_xyz, unsigned char *vis,
                          const float *pts_xyz, unsigned long n,
                          const float *m16, float vw, float vh, float wclip);
void  project_points_ref (float *out_xyz, unsigned char *vis,
                          const float *pts_xyz, unsigned long n,
                          const float *m16, float vw, float vh, float wclip);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_SIMD_H */
