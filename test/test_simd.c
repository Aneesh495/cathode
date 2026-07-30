/* test_simd.c — proves hand-written NEON asm agrees with the C reference. */
#include "cathode/simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0;
static int checks   = 0;

static int close(float a, float b, float eps) {
    float d = fabsf(a - b);
    float m = fmaxf(fabsf(a), fabsf(b));
    return d <= eps * (1.0f + m);
}

static void chk_arr(const char *name, const float *got, const float *exp, int n, float eps) {
    checks++;
    for (int i = 0; i < n; ++i) {
        if (!close(got[i], exp[i], eps)) {
            printf("  FAIL %-20s [%d]: got %.7g  expected %.7g\n", name, i, got[i], exp[i]);
            failures++;
            return;
        }
    }
    printf("  ok   %-20s (%d elems)\n", name, n);
}

static void chk_scalar(const char *name, float got, float exp, float eps) {
    checks++;
    if (!close(got, exp, eps)) {
        printf("  FAIL %-20s: got %.7g  expected %.7g\n", name, got, exp);
        failures++;
    } else {
        printf("  ok   %-20s (%.7g)\n", name, got);
    }
}

static float frand(void) { return (float)rand() / (float)RAND_MAX * 2.0f - 1.0f; }

int main(void) {
    srand(0xC0FFEE);
    printf("== CATHODE NEON asm vs C reference ==\n");

    /* mat4_mul */
    for (int t = 0; t < 4; ++t) {
        float a[16], b[16], o1[16], o2[16];
        for (int i = 0; i < 16; ++i) { a[i] = frand(); b[i] = frand(); }
        mat4_mul_neon(o1, a, b);
        mat4_mul_ref (o2, a, b);
        chk_arr("mat4_mul", o1, o2, 16, 1e-5f);
    }

    /* mat4_transform */
    for (int t = 0; t < 4; ++t) {
        float m[16], v[4], o1[4], o2[4];
        for (int i = 0; i < 16; ++i) m[i] = frand();
        for (int i = 0; i < 4; ++i)  v[i] = frand();
        mat4_transform_neon(o1, m, v);
        mat4_transform_ref (o2, m, v);
        chk_arr("mat4_transform", o1, o2, 4, 1e-5f);
    }

    /* vec4_dot */
    for (int t = 0; t < 4; ++t) {
        float a[4], b[4];
        for (int i = 0; i < 4; ++i) { a[i] = frand(); b[i] = frand(); }
        chk_scalar("vec4_dot", vec4_dot_neon(a, b), vec4_dot_ref(a, b), 1e-5f);
    }

    /* fast_rsqrt */
    float xs[] = {1.0f, 2.0f, 0.5f, 100.0f, 0.001f, 42.42f};
    for (unsigned i = 0; i < sizeof(xs)/sizeof(xs[0]); ++i)
        chk_scalar("fast_rsqrt", fast_rsqrt_neon(xs[i]), fast_rsqrt_ref(xs[i]), 1e-3f);

    /* vec3_normalize */
    for (int t = 0; t < 4; ++t) {
        float v[4], o1[4], o2[4];
        for (int i = 0; i < 4; ++i) v[i] = frand() * 10.0f;
        vec3_normalize_neon(o1, v);
        vec3_normalize_ref (o2, v);
        chk_arr("vec3_normalize", o1, o2, 4, 1e-3f);
    }

    /* saxpy — exercises the vector loop + scalar tail (n not a multiple of 4) */
    for (int trial = 0; trial < 3; ++trial) {
        unsigned long n = 17 + trial * 100;
        float *x  = malloc(n * sizeof(float));
        float *y1 = malloc(n * sizeof(float));
        float *y2 = malloc(n * sizeof(float));
        float a = frand();
        for (unsigned long i = 0; i < n; ++i) { x[i] = frand(); y1[i] = y2[i] = frand(); }
        saxpy_neon(y1, x, a, n);
        saxpy_ref (y2, x, a, n);
        chk_arr("saxpy", y1, y2, (int)n, 1e-5f);
        free(x); free(y1); free(y2);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0) printf("ALL PASS \xE2\x9C\x93\n");
    return failures ? 1 : 0;
}
