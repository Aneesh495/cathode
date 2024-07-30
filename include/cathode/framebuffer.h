/* ==========================================================================
 * cathode/framebuffer.h — the central shared surface.
 *
 * Everything that produces pixels writes into a Framebuffer of linear RGB
 * f32 (scene-referred, HDR-capable, values may exceed 1.0). The CRT signal
 * chain reads this, and the TUI presents the CRT output.
 *
 * Layout: planar-interleaved RGB, row-major. px[(y*w + x)*3 + {0,1,2}].
 * A separate f32 depth buffer supports the rasterizer / z-test.
 * ========================================================================== */
#ifndef CATHODE_FRAMEBUFFER_H
#define CATHODE_FRAMEBUFFER_H

#include "cathode/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i32  w, h;
    f32 *px;    /* w*h*3 linear RGB */
    f32 *depth; /* w*h, +inf = empty; smaller = nearer */
} Framebuffer;

Framebuffer *fb_create(i32 w, i32 h);
void         fb_destroy(Framebuffer *fb);
void         fb_clear(Framebuffer *fb, Color3 c);
void         fb_clear_depth(Framebuffer *fb, f32 value);

/* Bounds-checked accessors (no-op on out-of-range). */
static inline void fb_set(Framebuffer *fb, i32 x, i32 y, Color3 c) {
    if ((unsigned)x >= (unsigned)fb->w || (unsigned)y >= (unsigned)fb->h) return;
    f32 *p = &fb->px[(y * fb->w + x) * 3];
    p[0] = c.r; p[1] = c.g; p[2] = c.b;
}
static inline Color3 fb_get(const Framebuffer *fb, i32 x, i32 y) {
    if ((unsigned)x >= (unsigned)fb->w || (unsigned)y >= (unsigned)fb->h) return col3(0,0,0);
    const f32 *p = &fb->px[(y * fb->w + x) * 3];
    return col3(p[0], p[1], p[2]);
}
static inline void fb_add(Framebuffer *fb, i32 x, i32 y, Color3 c) {
    if ((unsigned)x >= (unsigned)fb->w || (unsigned)y >= (unsigned)fb->h) return;
    f32 *p = &fb->px[(y * fb->w + x) * 3];
    p[0] += c.r; p[1] += c.g; p[2] += c.b;
}
/* Additive splat with bilinear weights, for particle/nbody rendering. */
void fb_splat(Framebuffer *fb, f32 x, f32 y, Color3 c);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_FRAMEBUFFER_H */
