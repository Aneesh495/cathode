/* ==========================================================================
 * cathode/noise.h — deterministic PRNG + value/Perlin/simplex/fbm noise.
 * Used by scenes (terrain, plasma, nebula) and the CRT channel noise.
 * ========================================================================== */
#ifndef CATHODE_NOISE_H
#define CATHODE_NOISE_H

#include "cathode/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* xoshiro256** PRNG — fast, high quality. */
typedef struct { u64 s[4]; } Rng;
void  rng_seed(Rng *r, u64 seed);
u64   rng_next(Rng *r);
f32   rng_f32(Rng *r);          /* [0,1) */
f32   rng_range(Rng *r, f32 lo, f32 hi);
f32   rng_normal(Rng *r);       /* standard normal via Box-Muller */

/* Coherent noise (deterministic in coords, no state). */
f32 noise2(f32 x, f32 y);              /* value noise, [-1,1] */
f32 perlin2(f32 x, f32 y);             /* gradient noise, [-1,1] */
f32 perlin3(f32 x, f32 y, f32 z);
f32 simplex2(f32 x, f32 y);
f32 fbm2(f32 x, f32 y, int octaves, f32 lac, f32 gain);
f32 fbm3(f32 x, f32 y, f32 z, int octaves, f32 lac, f32 gain);
f32 ridged2(f32 x, f32 y, int octaves, f32 lac, f32 gain);

/* Worley (cellular / Voronoi) noise. Scatters feature points on a jittered
 * integer lattice and returns distances to the nearest ones:
 *   worley2   -> F1 (distance to nearest feature point), ~[0,1.4]
 *   worley2_f2f1 -> F2 - F1 (ridge/edge value; ~0 mid-cell, peaks at borders)
 * Great for cracked/scaly textures, cell membranes, caustics. */
f32 worley2(f32 x, f32 y);
f32 worley2_f2f1(f32 x, f32 y);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_NOISE_H */
