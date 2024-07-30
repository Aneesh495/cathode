/* ==========================================================================
 * image.c — framebuffer -> PPM/PNG exporter.
 *
 * Pipeline for every channel:
 *   linear HDR f32  --Reinhard-->  [0,1)  --sRGB gamma-->  [0,1]  --> u8
 *
 * The PNG path is fully self-contained: no libpng, no zlib. We emit a valid
 * zlib stream whose DEFLATE payload uses only "stored" (uncompressed) blocks,
 * which any conformant inflater (e.g. Python's zlib) accepts. Every chunk
 * carries a correct CRC-32 and the zlib stream ends with a big-endian Adler-32.
 * ========================================================================== */
#include "cathode/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Tonemap + sRGB
 * ------------------------------------------------------------------------- */

/* Standard linear->sRGB electro-optical transfer (IEC 61966-2-1). */
static f32 srgb_encode(f32 c) {
    if (c <= 0.0031308f) return 12.92f * c;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

void image_tonemap_srgb(const Framebuffer *fb, u8 *rgb8) {
    const i32 n = fb->w * fb->h * 3;
    const f32 *src = fb->px;
    for (i32 i = 0; i < n; ++i) {
        f32 c = src[i];
        /* Clamp negatives (scene-referred noise can dip below 0); Reinhard
         * itself handles the HDR upper range by compressing [0,inf) -> [0,1). */
        if (c < 0.0f) c = 0.0f;
        f32 mapped = c / (1.0f + c);        /* Reinhard */
        f32 s = srgb_encode(mapped);        /* gamma */
        /* Round-to-nearest into 0..255, guarding the endpoints. */
        f32 q = s * 255.0f + 0.5f;
        if (q < 0.0f)   q = 0.0f;
        if (q > 255.0f) q = 255.0f;
        rgb8[i] = (u8)q;
    }
}

/* -------------------------------------------------------------------------
 * PPM (binary P6)
 * ------------------------------------------------------------------------- */

int image_write_ppm(const Framebuffer *fb, const char *path) {
    const size_t npix = (size_t)fb->w * (size_t)fb->h;
    u8 *rgb = (u8 *)malloc(npix * 3);
    if (!rgb) return -1;
    image_tonemap_srgb(fb, rgb);

    FILE *f = fopen(path, "wb");
    if (!f) { free(rgb); return -1; }
    /* P6 header: magic, width height, maxval, single whitespace, then binary. */
    fprintf(f, "P6\n%d %d\n255\n", fb->w, fb->h);
    size_t wrote = fwrite(rgb, 1, npix * 3, f);
    int ok = (wrote == npix * 3);
    if (fclose(f) != 0) ok = 0;
    free(rgb);
    return ok ? 0 : -1;
}

/* -------------------------------------------------------------------------
 * Growable byte buffer
 * ------------------------------------------------------------------------- */

typedef struct { u8 *data; size_t len, cap; int err; } ByteBuf;

static void bb_reserve(ByteBuf *b, size_t extra) {
    if (b->err) return;
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra) ncap *= 2;
    u8 *nd = (u8 *)realloc(b->data, ncap);
    if (!nd) { b->err = 1; return; }
    b->data = nd; b->cap = ncap;
}
static void bb_u8(ByteBuf *b, u8 v) {
    bb_reserve(b, 1);
    if (b->err) return;
    b->data[b->len++] = v;
}
static void bb_bytes(ByteBuf *b, const u8 *p, size_t n) {
    bb_reserve(b, n);
    if (b->err) return;
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void bb_u32be(ByteBuf *b, u32 v) {
    bb_u8(b, (u8)(v >> 24)); bb_u8(b, (u8)(v >> 16));
    bb_u8(b, (u8)(v >>  8)); bb_u8(b, (u8)(v));
}
static void bb_u16le(ByteBuf *b, u16 v) {
    bb_u8(b, (u8)(v & 0xFF)); bb_u8(b, (u8)(v >> 8));
}

/* -------------------------------------------------------------------------
 * CRC-32 (PNG polynomial 0xEDB88320) and Adler-32
 * ------------------------------------------------------------------------- */

static u32   crc_table[256];
static int   crc_ready = 0;

static void crc_init(void) {
    for (u32 n = 0; n < 256; ++n) {
        u32 c = n;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static u32 crc32_buf(const u8 *p, size_t n) {
    if (!crc_ready) crc_init();
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u32 adler32_buf(const u8 *p, size_t n) {
    const u32 MOD = 65521u;
    u32 a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % MOD;
        b = (b + a)    % MOD;
    }
    return (b << 16) | a;
}

/* Append a full PNG chunk: length(BE) | type | data | CRC(BE over type+data). */
static void png_chunk(ByteBuf *out, const char type[4], const u8 *data, size_t len) {
    bb_u32be(out, (u32)len);
    size_t crc_start = out->len;
    bb_bytes(out, (const u8 *)type, 4);
    if (len) bb_bytes(out, data, len);
    if (out->err) return;
    u32 crc = crc32_buf(out->data + crc_start, out->len - crc_start);
    bb_u32be(out, crc);
}

/* -------------------------------------------------------------------------
 * PNG writer
 * ------------------------------------------------------------------------- */

int image_write_png(const Framebuffer *fb, const char *path) {
    const i32 w = fb->w, h = fb->h;
    const size_t npix = (size_t)w * (size_t)h;

    /* 1) Tonemap to 8-bit RGB. */
    u8 *rgb = (u8 *)malloc(npix * 3);
    if (!rgb) return -1;
    image_tonemap_srgb(fb, rgb);

    /* 2) Build raw filtered scanlines: each row = filter byte 0 + w*3 bytes. */
    const size_t row_bytes = (size_t)w * 3;
    const size_t raw_len   = (size_t)h * (1 + row_bytes);
    u8 *raw = (u8 *)malloc(raw_len ? raw_len : 1);
    if (!raw) { free(rgb); return -1; }
    for (i32 y = 0; y < h; ++y) {
        u8 *dst = raw + (size_t)y * (1 + row_bytes);
        dst[0] = 0; /* filter type 0 (None) */
        memcpy(dst + 1, rgb + (size_t)y * row_bytes, row_bytes);
    }
    free(rgb);

    /* 3) Wrap raw bytes in a zlib stream using DEFLATE stored blocks. */
    ByteBuf zs = {0};
    bb_u8(&zs, 0x78);  /* CMF: CM=8 (deflate), CINFO=7 (32K window) */
    bb_u8(&zs, 0x01);  /* FLG: FCHECK so that (0x78*256+0x01)%31==0, no dict */

    /* Stored blocks: LEN capped at 0xFFFF each. Last block sets BFINAL. */
    size_t off = 0;
    if (raw_len == 0) {
        /* Empty image: still emit one final empty stored block. */
        bb_u8(&zs, 0x01);
        bb_u16le(&zs, 0);
        bb_u16le(&zs, 0xFFFF);
    } else {
        while (off < raw_len) {
            size_t remain = raw_len - off;
            size_t block  = remain > 0xFFFFu ? 0xFFFFu : remain;
            int final     = (off + block >= raw_len);
            bb_u8(&zs, final ? 0x01 : 0x00);   /* BFINAL | BTYPE=00 (stored) */
            bb_u16le(&zs, (u16)block);         /* LEN  */
            bb_u16le(&zs, (u16)(~block));      /* NLEN = ones-complement of LEN */
            bb_bytes(&zs, raw + off, block);
            off += block;
        }
    }
    /* zlib trailer: Adler-32 of the *uncompressed* data, big-endian. */
    bb_u32be(&zs, adler32_buf(raw, raw_len));
    free(raw);

    if (zs.err) { free(zs.data); return -1; }

    /* 4) Assemble the file: signature + IHDR + IDAT + IEND. */
    ByteBuf out = {0};
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    bb_bytes(&out, sig, 8);

    u8 ihdr[13];
    ihdr[0] = (u8)(w >> 24); ihdr[1] = (u8)(w >> 16);
    ihdr[2] = (u8)(w >>  8); ihdr[3] = (u8)(w);
    ihdr[4] = (u8)(h >> 24); ihdr[5] = (u8)(h >> 16);
    ihdr[6] = (u8)(h >>  8); ihdr[7] = (u8)(h);
    ihdr[8]  = 8;   /* bit depth */
    ihdr[9]  = 2;   /* color type 2 = truecolor RGB */
    ihdr[10] = 0;   /* compression: deflate */
    ihdr[11] = 0;   /* filter method 0 */
    ihdr[12] = 0;   /* no interlace */
    png_chunk(&out, "IHDR", ihdr, 13);

    png_chunk(&out, "IDAT", zs.data, zs.len);
    png_chunk(&out, "IEND", NULL, 0);
    free(zs.data);

    if (out.err) { free(out.data); return -1; }

    /* 5) Flush to disk. */
    FILE *f = fopen(path, "wb");
    if (!f) { free(out.data); return -1; }
    size_t wrote = fwrite(out.data, 1, out.len, f);
    int ok = (wrote == out.len);
    if (fclose(f) != 0) ok = 0;
    free(out.data);
    return ok ? 0 : -1;
}
