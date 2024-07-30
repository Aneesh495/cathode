/* ==========================================================================
 * fractalkernel.c — public NEON fractal kernels.
 *
 * The heavy per-lane iteration runs in hand-written NEON (fk_iter4_neon in
 * src/asm/fractalkernel_neon.s); here we apply the O(1) smooth-escape log
 * correction so the result matches the C reference bit-closely. Splitting it
 * this way keeps the transcendental (logf) out of the vector inner loop while
 * still getting the SIMD speedup on the 99% of work that is the iteration.
 * ========================================================================== */
#include "cathode/fractalkernel.h"
#include <math.h>

/* implemented in fractalkernel_neon.s */
extern void fk_iter4_neon(float *niter, float *mag2,
                          const float *zr0, const float *zi0,
                          const float *cr, const float *ci,
                          int max_iter, float bail2);

static inline void finish(float *out4, const float *niter, const float *mag2,
                          int max_iter){
    const float inv_ln2 = 1.4426950408889634f;
    for (int i=0;i<4;++i){
        int n=(int)niter[i];
        if (n>=max_iter){ out4[i]=(float)max_iter; continue; }
        /* smooth count: n + 1 - log2( 0.5 * log|z|^2 ) */
        float nu = logf(logf(mag2[i])*0.5f) * inv_ln2;
        out4[i] = niter[i] + 1.0f - nu;
    }
}

void fk_mandel4_neon(float *out4, const float *cre, const float *cim,
                     int max_iter, float bailout2){
    float niter[4], mag2[4];
    const float z0[4]={0,0,0,0};
    fk_iter4_neon(niter, mag2, z0, z0, cre, cim, max_iter, bailout2);
    finish(out4, niter, mag2, max_iter);
}

void fk_julia4_neon(float *out4, const float *zre, const float *zim,
                    float jcre, float jcim, int max_iter, float bailout2){
    float niter[4], mag2[4];
    const float cr[4]={jcre,jcre,jcre,jcre};
    const float ci[4]={jcim,jcim,jcim,jcim};
    fk_iter4_neon(niter, mag2, zre, zim, cr, ci, max_iter, bailout2);
    finish(out4, niter, mag2, max_iter);
}
