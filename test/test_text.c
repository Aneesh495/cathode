/* ==========================================================================
 * test_text.c  -  unit + property tests for the 5x7 bitmap font subsystem.
 *
 * Verifies: glyph lookup is total (never NULL, out-of-range -> blank), space is
 * empty and a solid glyph is non-empty, scaling multiplies lit-pixel count by
 * scale^2 exactly, text_width matches measured extents, drawing stays inside
 * the framebuffer (bounds-safe), additive vs overwrite semantics, and newline
 * advances to a fresh line. All deterministic  -  no RNG needed.
 * ========================================================================== */
#include "cathode/text.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <string.h>

static int failures = 0, checks = 0;
static void check(const char *n, int ok) {
    checks++;
    if (ok) printf("  ok   %s\n", n);
    else { printf("  FAIL %s\n", n); failures++; }
}

/* count lit (any channel > eps) pixels in fb */
static int lit_count(const Framebuffer *fb) {
    int n = 0;
    for (int i = 0; i < fb->w * fb->h; ++i) {
        const f32 *p = &fb->px[i * 3];
        if (p[0] + p[1] + p[2] > 1e-4f) n++;
    }
    return n;
}

/* number of set bits in a glyph's 7 rows (source pixels) */
static int glyph_bits(char c) {
    const u8 *g = font5x7_glyph(c);
    int n = 0;
    for (int r = 0; r < FONT_H; ++r)
        for (int b = 0; b < FONT_W; ++b)
            if (g[r] & (1u << (FONT_W - 1 - b))) n++;
    return n;
}

int main(void) {
    printf("== CATHODE bitmap-font tests ==\n");

    /* glyph lookup is total and never NULL, incl. out-of-range */
    {
        int ok = 1;
        for (int c = 0; c < 256; ++c)
            if (font5x7_glyph((char)c) == NULL) { ok = 0; break; }
        check("glyph lookup never NULL (all 256 byte values)", ok);
    }

    /* space is blank; 'A','#','@' are non-empty */
    check("space glyph is empty", glyph_bits(' ') == 0);
    check("'A' glyph is non-empty", glyph_bits('A') > 0);
    check("'#' glyph is non-empty", glyph_bits('#') > 0);
    check("out-of-range char -> blank glyph", glyph_bits((char)0x01) == 0);

    /* drawing one glyph lights exactly glyph_bits pixels at scale 1 */
    {
        Framebuffer *fb = fb_create(20, 20);
        fb_clear(fb, col3(0, 0, 0));
        text_char(fb, 2, 2, 'A', col3(1, 1, 1), 1, 0);
        int expect = glyph_bits('A');
        check("scale-1 'A' lights exactly its set-bit count", lit_count(fb) == expect);
        fb_destroy(fb);
    }

    /* scale^2 law: scale-3 lights 9x the pixels of scale-1 */
    {
        Framebuffer *a = fb_create(40, 40), *b = fb_create(40, 40);
        fb_clear(a, col3(0, 0, 0)); fb_clear(b, col3(0, 0, 0));
        text_char(a, 1, 1, 'R', col3(1, 1, 1), 1, 0);
        text_char(b, 1, 1, 'R', col3(1, 1, 1), 3, 0);
        check("scale-3 lights 9x scale-1 pixel count", lit_count(b) == 9 * lit_count(a));
        fb_destroy(a); fb_destroy(b);
    }

    /* text_width matches the measured horizontal extent of a drawn string */
    {
        const char *s = "HELLO";
        i32 scale = 2, spacing = 1;
        i32 w = text_width(s, scale, spacing);
        Framebuffer *fb = fb_create(200, 40);
        fb_clear(fb, col3(0, 0, 0));
        text_draw(fb, 5, 5, s, col3(1, 1, 1), scale, spacing);
        /* find max lit column */
        int maxx = -1, minx = 999;
        for (int y = 0; y < fb->h; ++y)
            for (int x = 0; x < fb->w; ++x) {
                const f32 *p = &fb->px[(y * fb->w + x) * 3];
                if (p[0] + p[1] + p[2] > 1e-4f) { if (x > maxx) maxx = x; if (x < minx) minx = x; }
            }
        /* extent = last lit col - start x + 1; must be <= reported width and
         * within one glyph-cell of it (trailing blank columns aren't lit) */
        int extent = maxx - 5 + 1;
        check("text_width upper-bounds the drawn extent", extent <= w);
        check("text_width within a cell of the extent", (w - extent) <= FONT_W * scale);
        check("string starts at the pen x", minx == 5);
        fb_destroy(fb);
    }

    /* text_width of a multi-line string returns the WIDEST line, not the sum */
    {
        i32 scale = 2, spacing = 1;
        /* "A\nBCDE": widest line is 4 glyphs, not 5 */
        i32 w = text_width("A\nBCDE", scale, spacing);
        i32 expect = (4 * FONT_W + 3 * spacing) * scale;
        check("text_width(multiline) = widest line", w == expect);
        /* single line unchanged */
        check("text_width(single line) unchanged",
              text_width("BCDE", scale, spacing) == expect);
        /* trailing newline doesn't inflate width */
        check("text_width ignores trailing newline",
              text_width("BCDE\n", scale, spacing) == expect);
    }

    /* additive doubles overlapping intensity; overwrite does not */
    {
        Framebuffer *fb = fb_create(20, 20);
        fb_clear(fb, col3(0, 0, 0));
        text_char(fb, 2, 2, 'A', col3(0.4f, 0, 0), 1, 1);
        text_char(fb, 2, 2, 'A', col3(0.4f, 0, 0), 1, 1);  /* add again */
        /* pick a known-lit pixel: top of 'A' is row0 col2 -> (2+2, 2+0) */
        Color3 p = fb_get(fb, 4, 2);
        check("additive text accumulates", p.r > 0.7f);
        fb_clear(fb, col3(0, 0, 0));
        text_char(fb, 2, 2, 'A', col3(0.4f, 0, 0), 1, 0);
        text_char(fb, 2, 2, 'A', col3(0.4f, 0, 0), 1, 0);  /* overwrite */
        p = fb_get(fb, 4, 2);
        check("overwrite text does not accumulate", p.r > 0.35f && p.r < 0.45f);
        fb_destroy(fb);
    }

    /* bounds safety: draw far outside the framebuffer, must not crash/corrupt */
    {
        Framebuffer *fb = fb_create(16, 16);
        fb_clear(fb, col3(0, 0, 0));
        text_draw(fb, -100, -100, "OFFSCREEN", col3(1, 1, 1), 4, 1);
        text_draw(fb, 1000, 1000, "OFFSCREEN", col3(1, 1, 1), 4, 1);
        text_draw(fb, -3, -3, "EDGE", col3(1, 1, 1), 2, 1); /* partially on */
        check("offscreen draws are bounds-safe (no crash)", 1);
        fb_destroy(fb);
    }

    /* newline moves to a fresh line: two-line string lights rows in two bands */
    {
        Framebuffer *fb = fb_create(60, 40);
        fb_clear(fb, col3(0, 0, 0));
        text_draw(fb, 2, 2, "AB\nCD", col3(1, 1, 1), 2, 1);
        /* top band (y in [2, 2+FONT_H*2)) and a lower band must both have lit px */
        int top = 0, bot = 0;
        for (int y = 0; y < fb->h; ++y)
            for (int x = 0; x < fb->w; ++x) {
                const f32 *p = &fb->px[(y * fb->w + x) * 3];
                if (p[0] + p[1] + p[2] > 1e-4f) {
                    if (y < 2 + FONT_H * 2) top++;
                    else bot++;
                }
            }
        check("newline produces a second line of pixels", top > 0 && bot > 0);
        fb_destroy(fb);
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
