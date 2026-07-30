/* postfx_ref.c — portable C reference for the NEON post-FX kernels. */
#include "app/postfx.h"

void postfx_accumulate_ref(float *dst, const float *src, float scale, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) dst[i] += src[i] * scale;
}

void postfx_screen_ref(float *dst, const float *src, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) {
        float s = 1.0f - (1.0f - dst[i]) * (1.0f - src[i]);
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
        dst[i] = s;
    }
}

void postfx_brightpass_ref(float *out, const float *in, float threshold, float knee, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i) {
        float v = in[i] - threshold;
        if (v < 0.0f) v = 0.0f;
        out[i] = v * knee;
    }
}

void postfx_blur5_ref(float *out, const float *in, unsigned long n) {
    if (n == 0) return;
    for (unsigned long i = 0; i < n; ++i) {
        long im2 = (long)i - 2, im1 = (long)i - 1, ip1 = (long)i + 1, ip2 = (long)i + 2;
        if (im2 < 0) im2 = 0;
        if (im1 < 0) im1 = 0;
        if (ip1 > (long)n - 1) ip1 = (long)n - 1;
        if (ip2 > (long)n - 1) ip2 = (long)n - 1;
        out[i] = (in[im2] + 4.0f*in[im1] + 6.0f*in[i] + 4.0f*in[ip1] + in[ip2]) * (1.0f/16.0f);
    }
}

float postfx_sum_ref(const float *in, unsigned long n) {
    float s = 0.0f;
    for (unsigned long i = 0; i < n; ++i) s += in[i];
    return s;
}

float postfx_max_ref(const float *in, unsigned long n) {
    if (n == 0) return 0.0f;
    float m = in[0];
    for (unsigned long i = 1; i < n; ++i) if (in[i] > m) m = in[i];
    return m;
}
