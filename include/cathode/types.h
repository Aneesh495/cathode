/* ==========================================================================
 * cathode/types.h — foundational types shared across every module.
 * FROZEN CONTRACT: do not change without updating all modules.
 * ========================================================================== */
#ifndef CATHODE_TYPES_H
#define CATHODE_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

/* Linear RGB color, 0..1 per channel (scene-referred, pre-CRT). */
typedef struct { f32 r, g, b; } Color3;

/* 2D vectors used by rasterizer / screen space. */
typedef struct { f32 x, y; } Vec2;

/* Small vector types are defined in vec.h; forward-usable names here. */
typedef struct { f32 x, y, z;    } Vec3;
typedef struct { f32 x, y, z, w; } Vec4;

/* Column-major 4x4, m[col*4+row]. Matches simd.h asm layout. */
typedef struct { f32 m[16]; } Mat4;

/* Quaternion (x,y,z,w), w scalar. */
typedef struct { f32 x, y, z, w; } Quat;

#define CT_PI      3.14159265358979323846f
#define CT_TAU     6.28318530717958647692f
#define CT_DEG2RAD (CT_PI / 180.0f)
#define CT_RAD2DEG (180.0f / CT_PI)

/* Color3 constructor lives with its type so framebuffer.h (which only pulls
 * in types.h) can use it; the arithmetic helpers stay in vec.h. */
static inline Color3 col3(f32 r, f32 g, f32 b){ Color3 c={r,g,b}; return c; }

static inline f32 ct_clampf(f32 x, f32 lo, f32 hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline f32 ct_lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
static inline f32 ct_minf(f32 a, f32 b) { return a < b ? a : b; }
static inline f32 ct_maxf(f32 a, f32 b) { return a > b ? a : b; }
static inline i32 ct_mini(i32 a, i32 b) { return a < b ? a : b; }
static inline i32 ct_maxi(i32 a, i32 b) { return a > b ? a : b; }

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_TYPES_H */
