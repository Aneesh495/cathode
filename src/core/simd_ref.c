/* ==========================================================================
 * simd_ref.c — portable C reference for every NEON asm routine.
 * These are the ground truth the assembly is validated against.
 * ========================================================================== */
#include "cathode/simd.h"
#include <math.h>

void mat4_mul_ref(float *out, const float *a, const float *b) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = s;
        }
    }
}

void mat4_transform_ref(float *out, const float *m, const float *v) {
    for (int row = 0; row < 4; ++row) {
        float s = 0.0f;
        for (int k = 0; k < 4; ++k)
            s += m[k * 4 + row] * v[k];
        out[row] = s;
    }
}

float vec4_dot_ref(const float *a, const float *b) {
    return a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
}

float fast_rsqrt_ref(float x) {
    return 1.0f / sqrtf(x);
}

void vec3_normalize_ref(float *out, const float *v) {
    float len2 = v[0]*v[0] + v[1]*v[1] + v[2]*v[2];
    float inv  = 1.0f / sqrtf(len2);
    out[0] = v[0]*inv; out[1] = v[1]*inv; out[2] = v[2]*inv; out[3] = v[3];
}

void saxpy_ref(float *y, const float *x, float a, unsigned long n) {
    for (unsigned long i = 0; i < n; ++i)
        y[i] += a * x[i];
}

/* Batched project: M * (x,y,z,1) -> perspective divide -> screen. Column-major
 * m[col*4+row]. Reference for the NEON project_points kernel. */
void project_points_ref(float *out_xyz, unsigned char *vis,
                        const float *pts, unsigned long n,
                        const float *m, float vw, float vh, float wclip) {
    for (unsigned long i = 0; i < n; ++i) {
        float x = pts[3*i+0], y = pts[3*i+1], z = pts[3*i+2];
        float cx = m[0]*x + m[4]*y + m[8]*z  + m[12];
        float cy = m[1]*x + m[5]*y + m[9]*z  + m[13];
        float cz = m[2]*x + m[6]*y + m[10]*z + m[14];
        float cw = m[3]*x + m[7]*y + m[11]*z + m[15];
        if (cw > wclip) {
            float iw = 1.0f / cw;
            out_xyz[3*i+0] = (cx*iw*0.5f + 0.5f) * vw;
            out_xyz[3*i+1] = (1.0f - (cy*iw*0.5f + 0.5f)) * vh;
            out_xyz[3*i+2] = cz * iw;
            vis[i] = 1;
        } else {
            out_xyz[3*i+0] = out_xyz[3*i+1] = out_xyz[3*i+2] = 0.0f;
            vis[i] = 0;
        }
    }
}
