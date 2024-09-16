/* fractalkernel_ref.c  -  C reference for the NEON escape-time fractal kernels.
 *
 * Escape-time fractals are chaotic: a single differing rounding near the
 * bailout boundary flips a point's iteration count. The NEON kernel uses
 * strict (non-fused) multiply/add, so this reference must too  -  otherwise the
 * compiler may contract `2*zr*zi + ci` into an FMA and the two diverge. Pin it
 * off here so the equivalence holds under any build flags. */
#include "cathode/fractalkernel.h"
#include <math.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize ("no-fast-math")
#endif

static float smooth_escape(float zr, float zi, float cr, float ci,
                           int max_iter, float bail2){
    float zr2=zr*zr, zi2=zi*zi;
    int n=0;
    while (n<max_iter && (zr2+zi2) <= bail2){
        zi = 2.0f*zr*zi + ci;
        zr = zr2 - zi2 + cr;
        zr2 = zr*zr; zi2 = zi*zi;
        ++n;
    }
    if (n>=max_iter) return (float)max_iter;
    /* smooth iteration count: n + 1 - log2(log2|z|) */
    float mag2 = zr2 + zi2;
    float nu = logf(logf(mag2)*0.5f)/logf(2.0f);   /* = log2( 0.5*log|z|^2 ) */
    return (float)n + 1.0f - nu;
}

void fk_mandel4_ref(float *out4, const float *cre, const float *cim,
                    int max_iter, float bailout2){
    for (int i=0;i<4;++i)
        out4[i] = smooth_escape(0.0f, 0.0f, cre[i], cim[i], max_iter, bailout2);
}

void fk_julia4_ref(float *out4, const float *zre, const float *zim,
                   float jcre, float jcim, int max_iter, float bailout2){
    for (int i=0;i<4;++i)
        out4[i] = smooth_escape(zre[i], zim[i], jcre, jcim, max_iter, bailout2);
}
