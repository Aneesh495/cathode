/* ==========================================================================
 * test_integration.c  -  end-to-end pipeline integration test.
 *
 * Unlike the per-module unit tests, this exercises the whole headless chain as
 * one flow and validates the artifacts it emits:
 *
 *   1. register scenes; for a representative sample, run init → update → render
 *      → crt_process for several frames at a real resolution;
 *   2. write the CRT-processed frame to PNG and check the emitted bytes form a
 *      structurally valid PNG (signature, IHDR geometry, IEND terminator);
 *   3. encode a multi-frame animation to GIF and check GIF89a header + trailer;
 *   4. render synth audio to WAV and re-parse the RIFF header;
 *   5. assert every frame the pipeline produced is finite and non-negative.
 *
 * This is the test that would catch a regression in how the pieces are wired
 * together (buffer sizes, byte order, chunk framing) even when each unit still
 * passes in isolation.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/framebuffer.h"
#include "cathode/crt.h"
#include "cathode/image.h"
#include "cathode/wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0, checks = 0;
static void check(const char *n, int ok) {
    checks++;
    if (ok) printf("  ok   %s\n", n);
    else { printf("  FAIL %s\n", n); failures++; }
}

static long slurp(const char *path, u8 **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f); *out = b; return n;
}
static u32 be32(const u8 *p){ return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }

/* run one scene through init/update/render/crt for `frames`, return finiteness */
static int run_scene(Scene *sc, CrtState *crt, Framebuffer *scene_fb,
                     Framebuffer *disp_fb, int frames) {
    int ok = 1;
    if (sc->init) sc->init(sc, scene_fb->w, scene_fb->h);
    for (int f = 0; f < frames; ++f) {
        f32 t = (f32)f / 30.0f;
        if (sc->update) sc->update(sc, 1.0f/30.0f, t);
        if (sc->render) sc->render(sc, scene_fb);
        crt_process(crt, scene_fb, disp_fb);
        for (int i = 0; i < disp_fb->w*disp_fb->h*3 && ok; ++i) {
            f32 v = disp_fb->px[i];
            if (!(v == v) || v < -1e-3f || v > 1e5f) ok = 0;
        }
    }
    return ok;
}

int main(void) {
    printf("== CATHODE end-to-end integration test ==\n");
    scenes_register_all();

    const i32 W = 96, H = 72;
    int nsc = scene_count();
    check("scenes registered (>= 30)", nsc >= 30);

    Framebuffer *scene_fb = fb_create(W, H);
    Framebuffer *disp_fb  = fb_create(W, H);
    CrtConfig cfg = crt_config_preset("trinitron");
    CrtState *crt = crt_create(W, H, &cfg);

    /* --- 1+5. run a representative sample of scenes end-to-end --- */
    const char *sample[] = { "solids", "plasma", "mandelbrot", "demoscene",
                             "hyperbolic", "credits", "bootscreen", "life" };
    int all_finite = 1, ran = 0;
    for (unsigned s = 0; s < sizeof(sample)/sizeof(sample[0]); ++s) {
        /* find the scene by name */
        int idx = -1;
        for (int i = 0; i < nsc; ++i)
            if (strcmp(scene_name_at(i), sample[s]) == 0) { idx = i; break; }
        if (idx < 0) continue;   /* scene not present in this build */
        Scene *sc = scene_create(idx);
        if (!sc) continue;
        if (!run_scene(sc, crt, scene_fb, disp_fb, 6)) all_finite = 0;
        if (sc->destroy) sc->destroy(sc);
        ran++;
    }
    check("ran a sample of scenes end-to-end", ran >= 5);
    check("every pipeline frame finite & non-negative", all_finite);

    /* --- 2. PNG: render 'solids' one more frame, write PNG, validate bytes --- */
    {
        int idx = -1;
        for (int i = 0; i < nsc; ++i) if (!strcmp(scene_name_at(i),"solids")) idx=i;
        if (idx < 0) idx = 0;
        Scene *sc = scene_create(idx);
        if (sc->init) sc->init(sc, W, H);
        if (sc->update) sc->update(sc, 0.033f, 0.5f);
        if (sc->render) sc->render(sc, scene_fb);
        crt_process(crt, scene_fb, disp_fb);
        const char *pp = "/tmp/cathode_integ.png";
        int rc = image_write_png(disp_fb, pp);
        check("image_write_png returns 0", rc == 0);
        u8 *b = NULL; long len = slurp(pp, &b);
        check("PNG file non-empty", len > 8);
        if (len > 8) {
            static const u8 SIG[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
            check("PNG signature correct", memcmp(b, SIG, 8) == 0);
            /* first chunk must be IHDR with our width/height */
            check("first chunk is IHDR", memcmp(b+12, "IHDR", 4) == 0);
            check("IHDR width matches", (i32)be32(b+16) == W);
            check("IHDR height matches", (i32)be32(b+20) == H);
            /* file must end with an IEND chunk (type at len-8) */
            check("ends with IEND", len >= 12 && memcmp(b+len-8, "IEND", 4) == 0);
            free(b);
        }
        if (sc->destroy) sc->destroy(sc);
    }

    /* --- 3. GIF: encode a few frames, validate GIF89a header + trailer --- */
    {
        const char *gp = "/tmp/cathode_integ.gif";
        GifWriter *g = gif_begin(gp, W, H, 6, 1);
        check("gif_begin returns writer", g != NULL);
        int gok = (g != NULL);
        if (g) {
            int idx = -1;
            for (int i = 0; i < nsc; ++i) if (!strcmp(scene_name_at(i),"plasma")) idx=i;
            if (idx < 0) idx = 0;
            Scene *sc = scene_create(idx);
            if (sc->init) sc->init(sc, W, H);
            for (int f = 0; f < 4 && gok; ++f) {
                if (sc->update) sc->update(sc, 0.033f, f*0.1f);
                if (sc->render) sc->render(sc, scene_fb);
                crt_process(crt, scene_fb, disp_fb);
                if (gif_add_frame(g, disp_fb) != 0) gok = 0;
            }
            if (gif_end(g) != 0) gok = 0;
            if (sc->destroy) sc->destroy(sc);
        }
        check("gif encode succeeded", gok);
        u8 *b = NULL; long len = slurp(gp, &b);
        check("GIF file non-empty", len > 6);
        if (len > 6) {
            check("GIF89a header", memcmp(b, "GIF89a", 6) == 0);
            check("GIF trailer 0x3B", b[len-1] == 0x3B);
            free(b);
        }
    }

    /* --- 4. WAV via the tracker-independent one-shot path --- */
    {
        const char *wp = "/tmp/cathode_integ.wav";
        const u32 sr = 22050, N = 4096;
        f32 *s = (f32 *)malloc(sizeof(f32)*N);
        for (u32 i = 0; i < N; ++i) s[i] = 0.4f * sinf((float)i * 0.06f);
        int rc = wav_write_pcm16(wp, s, N, 1, sr);
        check("wav_write_pcm16 returns 0", rc == 0);
        free(s);
        u8 *b = NULL; long len = slurp(wp, &b);
        check("WAV file non-empty", len > 44);
        if (len > 44) {
            check("RIFF/WAVE header", memcmp(b,"RIFF",4)==0 && memcmp(b+8,"WAVE",4)==0);
            check("WAV data size matches", len == 44 + (long)N*2);
            free(b);
        }
    }

    crt_destroy(crt);
    fb_destroy(scene_fb);
    fb_destroy(disp_fb);

    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
