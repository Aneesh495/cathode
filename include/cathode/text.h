/* ==========================================================================
 * cathode/text.h  -  bitmap text rendering into the linear-RGB framebuffer.
 *
 * A classic 5x7 ASCII font (printable range 0x20..0x7E) rasterized directly
 * into a Framebuffer, so all drawn text flows through the full NTSC/CRT signal
 * chain downstream  -  glyphs bloom, bleed chroma, and pick up scanlines exactly
 * like real broadcast titling. This is the substrate for boot screens,
 * demoscene sine-scrollers, HUD text, and credits.
 *
 * The font is stored ROW-major: each glyph is 7 bytes (one per row, top to
 * bottom). Within a byte, bit 4 is the LEFT column and bit 0 the RIGHT column
 * (5 columns used; the upper 3 bits are ignored). Row-major makes the glyph
 * table legible in source  -  each byte reads as the pixels of that row.
 * ========================================================================== */
#ifndef CATHODE_TEXT_H
#define CATHODE_TEXT_H

#include "cathode/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_W 5   /* glyph cell width  in source pixels */
#define FONT_H 7   /* glyph cell height in source pixels */

/* Pointer to the 7 row-bytes for character c. Any character outside the
 * printable range maps to the blank (space) glyph. Never returns NULL. */
const u8 *font5x7_glyph(char c);

/* Pixel width of string s drawn at integer `scale` with `spacing` blank
 * source-columns between glyphs (typical spacing = 1). */
i32 text_width(const char *s, i32 scale, i32 spacing);

/* Draw one glyph with its top-left at (x,y), each source pixel expanded to a
 * scale×scale block. If `additive` the color is added (glows through bloom);
 * otherwise it overwrites. Bounds-checked per pixel. */
void text_char(Framebuffer *fb, i32 x, i32 y, char c, Color3 col,
               i32 scale, int additive);

/* Draw a NUL-terminated string left-to-right starting at (x,y). Advances by
 * (FONT_W + spacing)*scale per character. `\n` moves to a new line. */
void text_draw(Framebuffer *fb, i32 x, i32 y, const char *s, Color3 col,
               i32 scale, i32 spacing);
void text_draw_add(Framebuffer *fb, i32 x, i32 y, const char *s, Color3 col,
                   i32 scale, i32 spacing);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_TEXT_H */
