/* ==========================================================================
 * noise.c — implementation of cathode/noise.h
 *
 *   * Rng: xoshiro256** (Blackman & Vigna), seeded through splitmix64.
 *   * Coherent noise: value noise, Perlin 2D/3D (improved fade + permutation
 *     table + gradient lattice), 2D simplex (skewed), plus fbm / ridged
 *     fractal sums.
 *
 * All coherent-noise functions are pure functions of their coordinates: the
 * only "state" they touch is a compile-time-constant permutation table and
 * constant gradient tables, never mutated at runtime. This keeps them
 * thread-safe and deterministic for a given (x,y[,z]).
 * ========================================================================== */
#include "cathode/noise.h"
#include <math.h>

/* ==========================================================================
 * xoshiro256** PRNG
 * ========================================================================== */

static inline u64 rotl64(u64 x, int k) {
    return (x << k) | (x >> (64 - k));
}

/* splitmix64: a strong mixer used to expand a single 64-bit seed into the
 * four state words xoshiro256** needs. Guarantees a non-zero state even for
 * seed == 0 (splitmix64 never returns all-zero across four consecutive draws
 * from a fresh seed). */
void rng_seed(Rng *r, u64 seed) {
    u64 z = seed;
    for (int i = 0; i < 4; ++i) {
        z += 0x9E3779B97F4A7C15ULL;                  /* golden-ratio increment */
        u64 x = z;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        x =  x ^ (x >> 31);
        r->s[i] = x;
    }
}

u64 rng_next(Rng *r) {
    /* "**" scrambler: rotl(s1 * 5, 7) * 9 gives the output word. */
    const u64 result = rotl64(r->s[1] * 5ULL, 7) * 9ULL;
    const u64 t = r->s[1] << 17;

    r->s[2] ^= r->s[0];
    r->s[3] ^= r->s[1];
    r->s[1] ^= r->s[2];
    r->s[0] ^= r->s[3];
    r->s[2] ^= t;
    r->s[3] = rotl64(r->s[3], 45);

    return result;
}

/* Take the top 24 bits of a 64-bit draw and map to [0,1) with a power-of-two
 * denominator so the mapping is exact and never reaches 1.0. */
f32 rng_f32(Rng *r) {
    return (f32)(rng_next(r) >> 40) * (1.0f / 16777216.0f); /* /2^24 */
}

f32 rng_range(Rng *r, f32 lo, f32 hi) {
    return lo + (hi - lo) * rng_f32(r);
}

/* Standard normal via the basic (trig) Box-Muller transform. We generate one
 * normal per call using two uniforms; the second Gaussian is simply not
 * cached (keeps Rng stateless beyond its s[4] and keeps sequences simple to
 * reason about). */
f32 rng_normal(Rng *r) {
    f32 u1 = rng_f32(r);
    f32 u2 = rng_f32(r);
    if (u1 < 1e-7f) u1 = 1e-7f;                 /* avoid log(0) = -inf */
    f32 mag = sqrtf(-2.0f * logf(u1));
    return mag * cosf(CT_TAU * u2);
}

/* ==========================================================================
 * Shared lattice tables
 * ========================================================================== */

/* Ken Perlin's classic 256-entry permutation. We index it modulo 256 via
 * pget(), which is exactly equivalent to the traditional 512-entry table
 * (perm[i] == p256[i & 255]) but avoids storing the duplicated half. */
static const u8 p256[256] = {
    151,160,137, 91, 90, 15,131, 13,201, 95, 96, 53,194,233,  7,225,
    140, 36,103, 30, 69,142,  8, 99, 37,240, 21, 10, 23,190,  6,148,
    247,120,234, 75,  0, 26,197, 62, 94,252,219,203,117, 35, 11, 32,
     57,177, 33, 88,237,149, 56, 87,174, 20,125,136,171,168, 68,175,
     74,165, 71,134,139, 48, 27,166, 77,146,158,231, 83,111,229,122,
     60,211,133,230,220,105, 92, 41, 55, 46,245, 40,244,102,143, 54,
     65, 25, 63,161,  1,216, 80, 73,209, 76,132,187,208, 89, 18,169,
    200,196,135,130,116,188,159, 86,164,100,109,198,173,186,  3, 64,
     52,217,226,250,124,123,  5,202, 38,147,118,126,255, 82, 85,212,
    207,206, 59,227, 47, 16, 58, 17,182,189, 28, 42,223,183,170,213,
    119,248,152,  2, 44,154,163, 70,221,153,101,155,167, 43,172,  9,
    129, 22, 39,253, 19, 98,108,110, 79,113,224,232,178,185,112,104,
    218,246, 97,228,251, 34,242,193,238,210,144, 12,191,179,162,241,
     81, 51,145,235,249, 14,239,107, 49,192,214, 31,181,199,106,157,
    184, 84,204,176,115,121, 50, 45,127,  4,150,254,138,236,205, 93,
    222,114, 67, 29, 24, 72,243,141,128,195, 78, 66,215, 61,156,180
};

static inline int pget(int i) { return (int)p256[i & 255]; }

/* 8 unit-length 2D gradient directions (four axes + four diagonals).
 * With unit gradients the theoretical Perlin-2D maximum is sqrt(2)/2, so the
 * output is rescaled by sqrt(2) below to fill [-1,1]. */
static const f32 grad2_lut[8][2] = {
    { 1.0f, 0.0f}, {-1.0f, 0.0f}, { 0.0f, 1.0f}, { 0.0f,-1.0f},
    { 0.70710678f, 0.70710678f}, {-0.70710678f, 0.70710678f},
    { 0.70710678f,-0.70710678f}, {-0.70710678f,-0.70710678f}
};

/* The 12 classic cube-edge 3D gradients (magnitude sqrt(2)). Shared by
 * perlin3 and simplex2. */
static const f32 grad3_lut[12][3] = {
    { 1, 1, 0},{-1, 1, 0},{ 1,-1, 0},{-1,-1, 0},
    { 1, 0, 1},{-1, 0, 1},{ 1, 0,-1},{-1, 0,-1},
    { 0, 1, 1},{ 0,-1, 1},{ 0, 1,-1},{ 0,-1,-1}
};

/* ==========================================================================
 * Small math helpers
 * ========================================================================== */

static inline int fastfloor(f32 x) {
    int xi = (int)x;
    return (x < (f32)xi) ? xi - 1 : xi;   /* floor toward -inf */
}

/* Ken Perlin's improved fade curve 6t^5 - 15t^4 + 10t^3: zero 1st and 2nd
 * derivatives at t=0 and t=1, giving C2 continuity across lattice cells. */
static inline f32 fade(f32 t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* Classic Hermite smoothstep 3t^2 - 2t^3 (C1) — used for value noise. */
static inline f32 smoothstep(f32 t) {
    return t * t * (3.0f - 2.0f * t);
}

static inline f32 grad2(int hash, f32 x, f32 y) {
    const f32 *g = grad2_lut[hash & 7];
    return g[0] * x + g[1] * y;
}

static inline f32 grad3(int hash, f32 x, f32 y, f32 z) {
    const f32 *g = grad3_lut[hash % 12];
    return g[0] * x + g[1] * y + g[2] * z;
}

/* ==========================================================================
 * Value noise — hashed lattice values + smoothstep interpolation
 * ========================================================================== */

/* Integer hash of a 2D lattice point → deterministic value in [-1,1). */
static inline f32 vlattice(int x, int y) {
    u32 h = (u32)x * 374761393u + (u32)y * 668265263u;  /* mix coords */
    h = (h ^ (h >> 13)) * 1274126177u;
    h =  h ^ (h >> 16);
    /* top 24 bits -> [0,2) -> [-1,1) */
    return (f32)(h >> 8) * (1.0f / 8388608.0f) - 1.0f;   /* /2^23 - 1 */
}

f32 noise2(f32 x, f32 y) {
    int xi = fastfloor(x), yi = fastfloor(y);
    f32 xf = x - (f32)xi, yf = y - (f32)yi;
    f32 u = smoothstep(xf), v = smoothstep(yf);

    f32 v00 = vlattice(xi,     yi);
    f32 v10 = vlattice(xi + 1, yi);
    f32 v01 = vlattice(xi,     yi + 1);
    f32 v11 = vlattice(xi + 1, yi + 1);

    /* Bilinear blend of four values each in [-1,1] with weights summing to 1
     * => result provably stays in [-1,1]. */
    f32 a = ct_lerpf(v00, v10, u);
    f32 b = ct_lerpf(v01, v11, u);
    return ct_lerpf(a, b, v);
}

/* ==========================================================================
 * Perlin gradient noise
 * ========================================================================== */

f32 perlin2(f32 x, f32 y) {
    int xi = fastfloor(x), yi = fastfloor(y);
    int X = xi & 255, Y = yi & 255;
    f32 xf = x - (f32)xi, yf = y - (f32)yi;
    f32 u = fade(xf), v = fade(yf);

    /* Hash the four cell corners. */
    int aa = pget(pget(X)     + Y);
    int ab = pget(pget(X)     + Y + 1);
    int ba = pget(pget(X + 1) + Y);
    int bb = pget(pget(X + 1) + Y + 1);

    /* Dot each corner gradient with the vector from that corner to (x,y). */
    f32 x1 = ct_lerpf(grad2(aa, xf,        yf),
                      grad2(ba, xf - 1.0f, yf),        u);
    f32 x2 = ct_lerpf(grad2(ab, xf,        yf - 1.0f),
                      grad2(bb, xf - 1.0f, yf - 1.0f), u);
    f32 val = ct_lerpf(x1, x2, v);

    val *= 1.41421356f;                     /* sqrt(2): fill [-1,1] */
    return ct_clampf(val, -1.0f, 1.0f);
}

f32 perlin3(f32 x, f32 y, f32 z) {
    int xi = fastfloor(x), yi = fastfloor(y), zi = fastfloor(z);
    int X = xi & 255, Y = yi & 255, Z = zi & 255;
    f32 xf = x - (f32)xi, yf = y - (f32)yi, zf = z - (f32)zi;
    f32 u = fade(xf), v = fade(yf), w = fade(zf);

    int A  = pget(X)     + Y;
    int AA = pget(A)     + Z;
    int AB = pget(A + 1) + Z;
    int B  = pget(X + 1) + Y;
    int BA = pget(B)     + Z;
    int BB = pget(B + 1) + Z;

    f32 res = ct_lerpf(
        ct_lerpf(
            ct_lerpf(grad3(pget(AA),     xf,        yf,        zf),
                     grad3(pget(BA),     xf - 1.0f, yf,        zf),        u),
            ct_lerpf(grad3(pget(AB),     xf,        yf - 1.0f, zf),
                     grad3(pget(BB),     xf - 1.0f, yf - 1.0f, zf),        u),
            v),
        ct_lerpf(
            ct_lerpf(grad3(pget(AA + 1), xf,        yf,        zf - 1.0f),
                     grad3(pget(BA + 1), xf - 1.0f, yf,        zf - 1.0f), u),
            ct_lerpf(grad3(pget(AB + 1), xf,        yf - 1.0f, zf - 1.0f),
                     grad3(pget(BB + 1), xf - 1.0f, yf - 1.0f, zf - 1.0f), u),
            v),
        w);

    /* Gradients have magnitude sqrt(2); Perlin-3D theoretical max is then
     * sqrt(2)*sqrt(3)/2 = sqrt(6)/2, so 1/(sqrt(6)/2) = 0.8165 fills [-1,1]. */
    res *= 0.81649658f;
    return ct_clampf(res, -1.0f, 1.0f);
}

/* ==========================================================================
 * 2D simplex noise (Gustavson-style, skewed triangular lattice)
 * ========================================================================== */

f32 simplex2(f32 xin, f32 yin) {
    /* Skew/unskew factors for the 2D simplex grid. */
    const f32 F2 = 0.36602540378443864676f;   /* 0.5*(sqrt(3)-1)  */
    const f32 G2 = 0.21132486540518711775f;   /* (3-sqrt(3))/6    */

    /* Skew input space to determine which simplex cell we're in. */
    f32 s = (xin + yin) * F2;
    int i = fastfloor(xin + s);
    int j = fastfloor(yin + s);

    /* Unskew the cell origin back to (x,y) space. */
    f32 t = (f32)(i + j) * G2;
    f32 X0 = (f32)i - t;
    f32 Y0 = (f32)j - t;
    f32 x0 = xin - X0;                 /* distance from cell origin */
    f32 y0 = yin - Y0;

    /* Determine which of the two triangles of the cell we're in and the
     * offsets to the middle corner. */
    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; }   /* lower triangle */
    else         { i1 = 0; j1 = 1; }   /* upper triangle */

    f32 x1 = x0 - (f32)i1 + G2;
    f32 y1 = y0 - (f32)j1 + G2;
    f32 x2 = x0 - 1.0f + 2.0f * G2;
    f32 y2 = y0 - 1.0f + 2.0f * G2;

    /* Hashed gradient indices for the three corners. */
    int ii = i & 255, jj = j & 255;
    int gi0 = pget(ii      + pget(jj))      % 12;
    int gi1 = pget(ii + i1 + pget(jj + j1)) % 12;
    int gi2 = pget(ii + 1  + pget(jj + 1))  % 12;

    /* Radially-symmetric attenuation (max(0, 0.5 - r^2)^4) times gradient. */
    f32 n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;
    f32 tt;

    tt = 0.5f - x0 * x0 - y0 * y0;
    if (tt > 0.0f) { tt *= tt; n0 = tt * tt * grad3(gi0, x0, y0, 0.0f); }

    tt = 0.5f - x1 * x1 - y1 * y1;
    if (tt > 0.0f) { tt *= tt; n1 = tt * tt * grad3(gi1, x1, y1, 0.0f); }

    tt = 0.5f - x2 * x2 - y2 * y2;
    if (tt > 0.0f) { tt *= tt; n2 = tt * tt * grad3(gi2, x2, y2, 0.0f); }

    /* 70 is the standard normalization that maps this construction to ~[-1,1]. */
    f32 val = 70.0f * (n0 + n1 + n2);
    return ct_clampf(val, -1.0f, 1.0f);
}

/* ==========================================================================
 * Fractal sums
 * ========================================================================== */

/* Fractional Brownian motion: sum of Perlin octaves with each octave scaled
 * up in frequency (lacunarity) and down in amplitude (gain). Normalizing by
 * the total amplitude keeps the result in roughly [-1,1] for any octave count. */
f32 fbm2(f32 x, f32 y, int octaves, f32 lac, f32 gain) {
    f32 sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    if (octaves < 1) return 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * perlin2(x * freq, y * freq);
        norm += amp;
        freq *= lac;
        amp  *= gain;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

f32 fbm3(f32 x, f32 y, f32 z, int octaves, f32 lac, f32 gain) {
    f32 sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    if (octaves < 1) return 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * perlin3(x * freq, y * freq, z * freq);
        norm += amp;
        freq *= lac;
        amp  *= gain;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

/* Ridged multifractal: 1 - |noise| turns zero-crossings into sharp ridges;
 * squaring sharpens them further. Output normalized to ~[0,1]. */
f32 ridged2(f32 x, f32 y, int octaves, f32 lac, f32 gain) {
    f32 sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    if (octaves < 1) return 0.0f;
    for (int o = 0; o < octaves; ++o) {
        f32 n = 1.0f - fabsf(perlin2(x * freq, y * freq));
        n = n * n;                       /* sharpen ridge crests */
        sum  += amp * n;
        norm += amp;
        freq *= lac;
        amp  *= gain;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

/* ==========================================================================
 * Worley / cellular noise.
 * Each integer lattice cell contains one jittered feature point. For a sample
 * point we scan the 3x3 neighborhood of cells, find the nearest (F1) and
 * second-nearest (F2) feature points, and return F1 or F2-F1. A cheap integer
 * hash places the feature point deterministically inside its cell.
 * ========================================================================== */
static inline u32 whash(int xi, int yi, u32 salt) {
    u32 h = (u32)xi * 0x8DA6B343u ^ (u32)yi * 0xD8163841u ^ salt * 0x2545F491u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
static inline f32 whash01(int xi, int yi, u32 salt) {
    return (f32)(whash(xi, yi, salt) >> 8) * (1.0f / 16777216.0f);
}

static void worley_dist(f32 x, f32 y, f32 *out_f1, f32 *out_f2) {
    int xi = (int)floorf(x), yi = (int)floorf(y);
    f32 f1 = 1e30f, f2 = 1e30f;
    for (int oy = -1; oy <= 1; ++oy)
        for (int ox = -1; ox <= 1; ++ox) {
            int cx = xi + ox, cy = yi + oy;
            /* feature point position within cell (cx,cy) */
            f32 fx = (f32)cx + whash01(cx, cy, 0x1234u);
            f32 fy = (f32)cy + whash01(cx, cy, 0x5678u);
            f32 dx = fx - x, dy = fy - y;
            f32 d = sqrtf(dx*dx + dy*dy);
            if (d < f1) { f2 = f1; f1 = d; }
            else if (d < f2) { f2 = d; }
        }
    *out_f1 = f1; *out_f2 = f2;
}

f32 worley2(f32 x, f32 y) {
    f32 f1, f2; worley_dist(x, y, &f1, &f2);
    return f1;                 /* distance to nearest feature point */
}
f32 worley2_f2f1(f32 x, f32 y) {
    f32 f1, f2; worley_dist(x, y, &f1, &f2);
    return f2 - f1;            /* ~0 at cell centers, ridges at cell borders */
}
