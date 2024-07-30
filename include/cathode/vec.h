/* ==========================================================================
 * cathode/vec.h — inline vector/matrix/quaternion math.
 * Header-only so every module gets it with zero link deps. The heavy
 * matrix multiply / transform have NEON asm fast paths (see simd.h); these
 * inline helpers are for glue code and setup, not inner loops.
 * ========================================================================== */
#ifndef CATHODE_VEC_H
#define CATHODE_VEC_H

#include "cathode/types.h"
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Vec3 ---- */
static inline Vec3 v3(f32 x, f32 y, f32 z) { Vec3 r = {x,y,z}; return r; }
static inline Vec3 v3_add(Vec3 a, Vec3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline Vec3 v3_sub(Vec3 a, Vec3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline Vec3 v3_mul(Vec3 a, Vec3 b) { return v3(a.x*b.x, a.y*b.y, a.z*b.z); }
static inline Vec3 v3_scale(Vec3 a, f32 s){ return v3(a.x*s, a.y*s, a.z*s); }
static inline f32  v3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3 v3_cross(Vec3 a, Vec3 b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static inline f32  v3_len2(Vec3 a) { return v3_dot(a, a); }
static inline f32  v3_len(Vec3 a)  { return sqrtf(v3_len2(a)); }
static inline Vec3 v3_norm(Vec3 a) {
    f32 l = v3_len(a);
    return l > 1e-20f ? v3_scale(a, 1.0f/l) : a;
}
static inline Vec3 v3_lerp(Vec3 a, Vec3 b, f32 t) {
    return v3(ct_lerpf(a.x,b.x,t), ct_lerpf(a.y,b.y,t), ct_lerpf(a.z,b.z,t));
}
static inline Vec3 v3_neg(Vec3 a) { return v3(-a.x,-a.y,-a.z); }
static inline Vec3 v3_reflect(Vec3 i, Vec3 n) {
    return v3_sub(i, v3_scale(n, 2.0f * v3_dot(i, n)));
}
static inline Vec3 v3_min(Vec3 a, Vec3 b){ return v3(ct_minf(a.x,b.x),ct_minf(a.y,b.y),ct_minf(a.z,b.z)); }
static inline Vec3 v3_max(Vec3 a, Vec3 b){ return v3(ct_maxf(a.x,b.x),ct_maxf(a.y,b.y),ct_maxf(a.z,b.z)); }
static inline Vec3 v3_abs(Vec3 a){ return v3(fabsf(a.x),fabsf(a.y),fabsf(a.z)); }

/* ---- Vec4 ---- */
static inline Vec4 v4(f32 x, f32 y, f32 z, f32 w) { Vec4 r = {x,y,z,w}; return r; }
static inline Vec4 v4_from_v3(Vec3 a, f32 w) { return v4(a.x,a.y,a.z,w); }
static inline Vec3 v4_xyz(Vec4 a) { return v3(a.x,a.y,a.z); }

/* ---- Mat4 (column-major) ---- */
static inline Mat4 mat4_identity(void) {
    Mat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static inline Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            f32 s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k*4+row] * b.m[c*4+k];
            r.m[c*4+row] = s;
        }
    return r;
}
static inline Vec4 mat4_mul_v4(Mat4 m, Vec4 v) {
    Vec4 r;
    r.x = m.m[0]*v.x + m.m[4]*v.y + m.m[8] *v.z + m.m[12]*v.w;
    r.y = m.m[1]*v.x + m.m[5]*v.y + m.m[9] *v.z + m.m[13]*v.w;
    r.z = m.m[2]*v.x + m.m[6]*v.y + m.m[10]*v.z + m.m[14]*v.w;
    r.w = m.m[3]*v.x + m.m[7]*v.y + m.m[11]*v.z + m.m[15]*v.w;
    return r;
}
static inline Mat4 mat4_translate(Vec3 t) {
    Mat4 r = mat4_identity();
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}
static inline Mat4 mat4_scale(Vec3 s) {
    Mat4 r = mat4_identity();
    r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z;
    return r;
}
static inline Mat4 mat4_rotate_x(f32 a) {
    Mat4 r = mat4_identity(); f32 c=cosf(a), s=sinf(a);
    r.m[5]=c; r.m[6]=s; r.m[9]=-s; r.m[10]=c; return r;
}
static inline Mat4 mat4_rotate_y(f32 a) {
    Mat4 r = mat4_identity(); f32 c=cosf(a), s=sinf(a);
    r.m[0]=c; r.m[2]=-s; r.m[8]=s; r.m[10]=c; return r;
}
static inline Mat4 mat4_rotate_z(f32 a) {
    Mat4 r = mat4_identity(); f32 c=cosf(a), s=sinf(a);
    r.m[0]=c; r.m[1]=s; r.m[4]=-s; r.m[5]=c; return r;
}
/* Right-handed perspective, maps z to [-1,1] (OpenGL clip). fov in radians. */
static inline Mat4 mat4_perspective(f32 fovy, f32 aspect, f32 znear, f32 zfar) {
    Mat4 r = {{0}};
    f32 f = 1.0f / tanf(fovy * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zfar + znear) / (znear - zfar);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zfar * znear) / (znear - zfar);
    return r;
}
/* Right-handed lookAt. */
static inline Mat4 mat4_look_at(Vec3 eye, Vec3 center, Vec3 up) {
    Vec3 f = v3_norm(v3_sub(center, eye));
    Vec3 s = v3_norm(v3_cross(f, up));
    Vec3 u = v3_cross(s, f);
    Mat4 r = mat4_identity();
    r.m[0]=s.x; r.m[4]=s.y; r.m[8]=s.z;
    r.m[1]=u.x; r.m[5]=u.y; r.m[9]=u.z;
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z;
    r.m[12]=-v3_dot(s,eye); r.m[13]=-v3_dot(u,eye); r.m[14]=v3_dot(f,eye);
    return r;
}

/* ---- Quaternion ---- */
static inline Quat quat_identity(void) { Quat q={0,0,0,1}; return q; }
static inline Quat quat_axis_angle(Vec3 axis, f32 angle) {
    Vec3 n = v3_norm(axis); f32 h = angle*0.5f, s = sinf(h);
    Quat q = { n.x*s, n.y*s, n.z*s, cosf(h) }; return q;
}
static inline Quat quat_mul(Quat a, Quat b) {
    Quat q;
    q.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    q.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    q.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    q.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return q;
}
static inline Mat4 quat_to_mat4(Quat q) {
    f32 x=q.x,y=q.y,z=q.z,w=q.w;
    f32 xx=x*x,yy=y*y,zz=z*z,xy=x*y,xz=x*z,yz=y*z,wx=w*x,wy=w*y,wz=w*z;
    Mat4 r = mat4_identity();
    r.m[0]=1-2*(yy+zz); r.m[1]=2*(xy+wz);   r.m[2]=2*(xz-wy);
    r.m[4]=2*(xy-wz);   r.m[5]=1-2*(xx+zz); r.m[6]=2*(yz+wx);
    r.m[8]=2*(xz+wy);   r.m[9]=2*(yz-wx);   r.m[10]=1-2*(xx+yy);
    return r;
}

/* ---- Color ---- (col3 constructor is in types.h) ---- */
static inline Color3 col_add(Color3 a, Color3 b){ return col3(a.r+b.r,a.g+b.g,a.b+b.b); }
static inline Color3 col_scale(Color3 a, f32 s){ return col3(a.r*s,a.g*s,a.b*s); }
static inline Color3 col_mul(Color3 a, Color3 b){ return col3(a.r*b.r,a.g*b.g,a.b*b.b); }
static inline Color3 col_lerp(Color3 a, Color3 b, f32 t){
    return col3(ct_lerpf(a.r,b.r,t),ct_lerpf(a.g,b.g,t),ct_lerpf(a.b,b.b,t));
}

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_VEC_H */
