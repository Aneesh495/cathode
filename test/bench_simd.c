/* ==========================================================================
 * bench_simd.c  -  measure the hand-written NEON asm speedup over the C
 * reference for the hot kernels. Prints throughput + speedup ratios.
 * ========================================================================== */
#include "cathode/simd.h"
#include "cathode/dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

#define BENCH(label, iters, stmt) do {                                  \
    /* warmup */                                                        \
    for (int _i = 0; _i < (iters)/10 + 1; ++_i) { stmt; }               \
    double _t0 = now_s();                                               \
    for (long _i = 0; _i < (iters); ++_i) { stmt; }                     \
    double _t1 = now_s();                                               \
    printf("  %-26s %10.2f Mops/s  (%.3f s / %ld iters)\n",             \
           label, (double)(iters) / (_t1 - _t0) / 1e6, _t1 - _t0,       \
           (long)(iters));                                              \
    last_time = _t1 - _t0;                                              \
} while (0)

int main(void) {
    double last_time = 0, tn = 0, tr = 0;
    printf("== CATHODE NEON vs C-reference microbenchmarks ==\n");
    volatile float sink = 0;

    /* ---- mat4_mul ---- */
    {
        float a[16], b[16], o[16];
        for (int i = 0; i < 16; ++i) { a[i] = (float)(i+1)*0.1f; b[i] = (float)(16-i)*0.07f; }
        const long N = 50000000;
        printf("mat4_mul (4x4 * 4x4):\n");
        BENCH("NEON asm", N, mat4_mul_neon(o, a, b)); tn = last_time; sink += o[0];
        BENCH("C reference", N, mat4_mul_ref(o, a, b)); tr = last_time; sink += o[0];
        printf("  >> speedup: %.2fx\n", tr / tn);
    }

    /* ---- saxpy over a big array (memory bound, shows vector width) ---- */
    {
        const unsigned long M = 1u << 16;
        float *x = malloc(M*sizeof(float)), *y = malloc(M*sizeof(float));
        for (unsigned long i = 0; i < M; ++i) { x[i] = (float)i*1e-3f; y[i] = 1.0f; }
        const long N = 20000;
        printf("saxpy (n=%lu):\n", M);
        BENCH("NEON asm", N, saxpy_neon(y, x, 1.0001f, M)); tn = last_time; sink += y[0];
        for (unsigned long i = 0; i < M; ++i) y[i] = 1.0f;
        BENCH("C reference", N, saxpy_ref(y, x, 1.0001f, M)); tr = last_time; sink += y[0];
        printf("  >> speedup: %.2fx\n", tr / tn);
        free(x); free(y);
    }

    /* ---- dsp rgb->yiq over a scanline-ish buffer ---- */
    {
        const unsigned long P = 4096;              /* pixels */
        float *rgb = malloc(P*3*sizeof(float));
        float *out = malloc(P*3*sizeof(float));
        for (unsigned long i = 0; i < P*3; ++i) rgb[i] = (float)((i*2654435761u) & 0xffff) / 65535.0f;
        const long N = 100000;
        printf("dsp_rgb2yiq (n=%lu px):\n", P);
        BENCH("NEON asm", N, dsp_rgb2yiq_neon(out, rgb, P)); tn = last_time; sink += out[0];
        BENCH("C reference", N, dsp_rgb2yiq_ref(out, rgb, P)); tr = last_time; sink += out[0];
        printf("  >> speedup: %.2fx\n", tr / tn);
        free(rgb); free(out);
    }

    printf("(sink=%g)\n", (double)sink);
    return 0;
}
