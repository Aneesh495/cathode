/* framebuffer.c  -  allocation, clear, and bilinear splat for the shared surface. */
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <math.h>

Framebuffer *fb_create(i32 w, i32 h) {
    Framebuffer *fb = (Framebuffer *)calloc(1, sizeof(Framebuffer));
    if (!fb) return NULL;
    fb->w = w; fb->h = h;
    fb->px    = (f32 *)calloc((size_t)w * h * 3, sizeof(f32));
    fb->depth = (f32 *)malloc((size_t)w * h * sizeof(f32));
    if (!fb->px || !fb->depth) { fb_destroy(fb); return NULL; }
    fb_clear_depth(fb, 1e30f);
    return fb;
}

void fb_destroy(Framebuffer *fb) {
    if (!fb) return;
    free(fb->px);
    free(fb->depth);
    free(fb);
}

void fb_clear(Framebuffer *fb, Color3 c) {
    const i32 n = fb->w * fb->h;
    f32 *p = fb->px;
    for (i32 i = 0; i < n; ++i) { p[0]=c.r; p[1]=c.g; p[2]=c.b; p += 3; }
}

void fb_clear_depth(Framebuffer *fb, f32 value) {
    const i32 n = fb->w * fb->h;
    for (i32 i = 0; i < n; ++i) fb->depth[i] = value;
}

void fb_splat(Framebuffer *fb, f32 x, f32 y, Color3 c) {
    i32 x0 = (i32)floorf(x), y0 = (i32)floorf(y);
    f32 fx = x - (f32)x0, fy = y - (f32)y0;
    f32 w00 = (1-fx)*(1-fy), w10 = fx*(1-fy), w01 = (1-fx)*fy, w11 = fx*fy;
    fb_add(fb, x0,   y0,   col_scale(c, w00));
    fb_add(fb, x0+1, y0,   col_scale(c, w10));
    fb_add(fb, x0,   y0+1, col_scale(c, w01));
    fb_add(fb, x0+1, y0+1, col_scale(c, w11));
}
