/* ==========================================================================
 * cathode/image.h — framebuffer -> PPM/PNG exporter (for headless capture &
 * visual verification). Applies Reinhard tonemap + sRGB gamma on the way out.
 * ========================================================================== */
#ifndef CATHODE_IMAGE_H
#define CATHODE_IMAGE_H

#include "cathode/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write linear-RGB framebuffer to an 8-bit sRGB PPM (P6). Returns 0 on ok. */
int image_write_ppm(const Framebuffer *fb, const char *path);

/* Same, but PNG via a minimal self-contained zlib/deflate encoder (no libpng). */
int image_write_png(const Framebuffer *fb, const char *path);

/* Tonemap+encode one framebuffer into an existing 8-bit RGB buffer (w*h*3). */
void image_tonemap_srgb(const Framebuffer *fb, u8 *rgb8);

/* ---- animated GIF89a encoder (from-scratch LZW, no libgif) ----
 * Open a writer, push frames (each tonemapped + median-cut quantized to a
 * 256-color palette), then finish. delay_cs = inter-frame delay in centiseconds
 * (e.g. 4 ≈ 25 fps). Returns NULL / non-zero on failure. */
typedef struct GifWriter GifWriter;
GifWriter *gif_begin(const char *path, i32 w, i32 h, i32 delay_cs, int loop);
int        gif_add_frame(GifWriter *g, const Framebuffer *fb);
int        gif_end(GifWriter *g);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_IMAGE_H */
