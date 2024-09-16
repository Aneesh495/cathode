/* test_image.c  -  verifies PPM round-trip and PNG validity.
 *
 * (a) Build an 8x8 HDR gradient framebuffer, write P6 PPM, re-read it, check
 *     the header dims and that every byte equals image_tonemap_srgb output.
 * (b) Write PNG and validate it with a real Python inflater: signature, chunk
 *     structure, zlib.decompress of concatenated IDAT, per-row filter stripping,
 *     and byte count == h*(1 + w*3). The Python exit code gates the test.
 */
#include "cathode/image.h"
#include "cathode/framebuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 8
#define H 8

static int failures = 0;
static int checks   = 0;

static void chk(const char *name, int cond) {
    checks++;
    if (cond) { printf("  ok   %s\n", name); }
    else      { printf("  FAIL %s\n", name); failures++; }
}

/* Populate an HDR gradient: R ramps across x, G across y, B pushes into HDR
 * (>1.0) so the Reinhard compression path is exercised, plus one exact-0 and
 * one very small value to hit the sRGB linear segment. */
static void fill_gradient(Framebuffer *fb) {
    for (i32 y = 0; y < H; ++y) {
        for (i32 x = 0; x < W; ++x) {
            f32 r = (f32)x / (f32)(W - 1);          /* 0 .. 1 */
            f32 g = (f32)y / (f32)(H - 1) * 3.0f;   /* 0 .. 3 (HDR) */
            f32 b = (f32)(x + y) / 4.0f;            /* 0 .. up to ~3.5 */
            if (x == 0 && y == 0) { r = 0.0f; g = 0.0f; b = 0.0f; }
            if (x == 1 && y == 0) { r = 0.001f; }   /* sRGB linear segment */
            fb_set(fb, x, y, col3(r, g, b));
        }
    }
}

/* Read a whitespace/newline terminated token from a binary PPM stream. */
static int read_token(FILE *f, char *buf, int cap) {
    int c, n = 0;
    do { c = fgetc(f); } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    if (c == EOF) return -1;
    while (c != EOF && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
        if (n < cap - 1) buf[n++] = (char)c;
        c = fgetc(f);
    }
    buf[n] = 0;
    return n;
}

static int test_ppm(Framebuffer *fb, const u8 *expect) {
    const char *path = "build/_test_image.ppm";
    int rc = image_write_ppm(fb, path);
    chk("image_write_ppm returns 0", rc == 0);
    if (rc != 0) return -1;

    FILE *f = fopen(path, "rb");
    chk("PPM reopens", f != NULL);
    if (!f) return -1;

    char tok[64];
    read_token(f, tok, sizeof tok);
    chk("PPM magic P6", strcmp(tok, "P6") == 0);
    read_token(f, tok, sizeof tok);
    int fw = atoi(tok);
    read_token(f, tok, sizeof tok);
    int fh = atoi(tok);
    read_token(f, tok, sizeof tok);
    int maxv = atoi(tok);
    chk("PPM width == 8",  fw == W);
    chk("PPM height == 8", fh == H);
    chk("PPM maxval 255",  maxv == 255);
    /* read_token already consumed the single whitespace after maxval, so the
     * binary payload starts at the current stream position. */

    size_t n = (size_t)W * H * 3;
    u8 *got = (u8 *)malloc(n);
    size_t rd = fread(got, 1, n, f);
    chk("PPM payload length", rd == n);
    /* Ensure there are no trailing bytes beyond the pixel payload. */
    int extra = fgetc(f);
    chk("PPM no trailing bytes", extra == EOF);
    fclose(f);

    int match = (memcmp(got, expect, n) == 0);
    chk("PPM bytes == image_tonemap_srgb", match);
    if (!match) {
        for (size_t i = 0; i < n && i < 24; ++i)
            printf("    [%zu] got %3u exp %3u\n", i, got[i], expect[i]);
    }
    free(got);
    return match ? 0 : -1;
}

int main(void) {
    printf("== CATHODE image module ==\n");

    Framebuffer *fb = fb_create(W, H);
    if (!fb) { printf("FAIL fb_create\n"); return 1; }
    fill_gradient(fb);

    /* Reference tonemap output used by both PPM check and sanity. */
    u8 expect[W * H * 3];
    image_tonemap_srgb(fb, expect);

    /* Sanity: the (0,0) black pixel must map to exactly 0. */
    chk("black -> 0", expect[0] == 0 && expect[1] == 0 && expect[2] == 0);

    test_ppm(fb, expect);

    /* PNG */
    const char *png = "build/_test_image.png";
    int rc = image_write_png(fb, png);
    chk("image_write_png returns 0", rc == 0);

    fb_destroy(fb);

    /* Validate the PNG with Python's real inflater. We invoke `python3 -`
     * so the heredoc is the script and PATH/W/H become sys.argv[1..3]. */
    int py = -1;
    if (rc == 0) {
        const char *body =
            "import sys, struct, zlib\n"
            "path, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])\n"
            "d = open(path,'rb').read()\n"
            "sig = bytes([137,80,78,71,13,10,26,10])\n"
            "assert d[:8]==sig, 'bad signature'\n"
            "off=8; chunks=[]; idat=b''; ihdr=None\n"
            "while off < len(d):\n"
            "    (ln,) = struct.unpack('>I', d[off:off+4]); off+=4\n"
            "    ctype = d[off:off+4]; off+=4\n"
            "    data = d[off:off+ln]; off+=ln\n"
            "    (crc,) = struct.unpack('>I', d[off:off+4]); off+=4\n"
            "    calc = zlib.crc32(ctype+data) & 0xffffffff\n"
            "    assert crc==calc, 'bad CRC on '+ctype.decode('latin1')\n"
            "    chunks.append(ctype)\n"
            "    if ctype==b'IHDR': ihdr=data\n"
            "    if ctype==b'IDAT': idat+=data\n"
            "assert chunks[0]==b'IHDR', 'first chunk not IHDR'\n"
            "assert chunks[-1]==b'IEND', 'last chunk not IEND'\n"
            "iw,ih,bd,ct,cm,fm,il = struct.unpack('>IIBBBBB', ihdr)\n"
            "assert (iw,ih)==(W,H), 'IHDR dims %r != %r'%((iw,ih),(W,H))\n"
            "assert bd==8 and ct==2, 'not 8-bit RGB'\n"
            "raw = zlib.decompress(idat)\n"
            "assert len(raw)==H*(1+W*3), 'raw len %d != %d'%(len(raw),H*(1+W*3))\n"
            "stride=1+W*3; pix=bytearray()\n"
            "for y in range(H):\n"
            "    row=raw[y*stride:(y+1)*stride]\n"
            "    assert row[0]==0, 'filter byte != 0'\n"
            "    pix += row[1:]\n"
            "assert len(pix)==W*H*3\n"
            "print('PY PASS: png valid, %dx%d, decompressed %d bytes'%(iw,ih,len(raw)))\n";
        char cmd[4096];
        snprintf(cmd, sizeof cmd,
                 "python3 - %s %d %d <<'PYEOF'\n%sPYEOF\n", png, W, H, body);
        py = system(cmd);
    }
    chk("PNG validated by Python", rc == 0 && py == 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
