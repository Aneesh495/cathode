/* ==========================================================================
 * test_wav.c — verifies the from-scratch RIFF/WAVE PCM16 encoder.
 *
 * Writes files with both the one-shot and streaming APIs, then reads the raw
 * bytes back and checks every header field against the spec, the reported data
 * size against the sample count, and that a few known f32 inputs quantize to
 * the expected int16 little-endian values (incl. hard-clipping of out-of-range
 * input). Uses only stdio — no decoder dependency.
 * ========================================================================== */
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

static u32 rd_u32le(const u8 *p){ return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24); }
static u16 rd_u16le(const u8 *p){ return (u16)(p[0]|(p[1]<<8)); }
static i16 rd_i16le(const u8 *p){ return (i16)((u16)p[0]|((u16)p[1]<<8)); }

/* read a whole file into a malloc'd buffer; returns length (0 on error) */
static long slurp(const char *path, u8 **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f); *out = b; return n;
}

/* validate the canonical 44-byte header against known params */
static int check_header(const u8 *b, long len, u32 ch, u32 sr, u32 nframes) {
    if (len < 44) return 0;
    int ok = 1;
    ok &= (memcmp(b + 0, "RIFF", 4) == 0);
    ok &= (memcmp(b + 8, "WAVE", 4) == 0);
    ok &= (memcmp(b + 12, "fmt ", 4) == 0);
    ok &= (rd_u32le(b + 16) == 16u);          /* PCM fmt size */
    ok &= (rd_u16le(b + 20) == 1u);           /* PCM */
    ok &= (rd_u16le(b + 22) == (u16)ch);
    ok &= (rd_u32le(b + 24) == sr);
    ok &= (rd_u32le(b + 28) == sr * ch * 2u); /* byte rate */
    ok &= (rd_u16le(b + 32) == (u16)(ch * 2u));/* block align */
    ok &= (rd_u16le(b + 34) == 16u);          /* bits/sample */
    ok &= (memcmp(b + 36, "data", 4) == 0);
    u32 data_bytes = nframes * ch * 2u;
    ok &= (rd_u32le(b + 40) == data_bytes);
    ok &= (rd_u32le(b + 4)  == 36u + data_bytes);
    ok &= (len == 44 + (long)data_bytes);
    return ok;
}

int main(void) {
    printf("== CATHODE WAV encoder tests ==\n");
    const char *P1 = "/tmp/cathode_test_oneshot.wav";
    const char *P2 = "/tmp/cathode_test_stream.wav";

    /* --- one-shot mono: a 440 Hz-ish ramp of known samples --- */
    {
        const u32 sr = 44100, ch = 1, N = 1000;
        f32 *s = (f32 *)malloc(sizeof(f32) * N);
        for (u32 i = 0; i < N; ++i) s[i] = sinf((float)i * 0.05f) * 0.5f;
        int rc = wav_write_pcm16(P1, s, N, ch, sr);
        check("one-shot write returns 0", rc == 0);
        u8 *b = NULL; long len = slurp(P1, &b);
        check("one-shot file readable", len > 0);
        if (len > 0) {
            check("one-shot header valid", check_header(b, len, ch, sr, N));
            /* spot-check a sample round-trips through quantization */
            i16 got = rd_i16le(b + 44 + 2 * 10);
            i16 exp = (i16)(sinf(10 * 0.05f) * 0.5f * 32767.0f + 0.5f);
            check("one-shot sample[10] quantized correctly", abs(got - exp) <= 1);
            free(b);
        }
        free(s);
    }

    /* --- clipping: out-of-range inputs hard-limit to +/-32767..-32768 --- */
    {
        const u32 sr = 8000, ch = 1;
        f32 s[4] = { 2.0f, -2.0f, 1.0f, -1.0f };
        int rc = wav_write_pcm16(P1, s, 4, ch, sr);
        check("clip-test write returns 0", rc == 0);
        u8 *b = NULL; long len = slurp(P1, &b);
        if (len >= 44 + 8) {
            check("clip: +2.0 -> +32767",  rd_i16le(b + 44 + 0) == 32767);
            check("clip: -2.0 -> -32767..-32768", rd_i16le(b + 44 + 2) <= -32767);
            check("clip: +1.0 -> +32767",  rd_i16le(b + 44 + 4) == 32767);
            free(b);
        } else { check("clip: file long enough", 0); if(b) free(b); }
    }

    /* --- NaN/Inf inputs must not produce garbage samples (no UB cast) --- */
    {
        const u32 sr = 8000, ch = 1;
        f32 nan = 0.0f/0.0f, inf = 1.0f/0.0f;   /* diverged-filter style inputs */
        f32 s[4] = { nan, inf, -inf, 0.5f };
        int rc = wav_write_pcm16(P1, s, 4, ch, sr);
        check("NaN/Inf write returns 0", rc == 0);
        u8 *b = NULL; long len = slurp(P1, &b);
        if (len >= 44 + 8) {
            check("NaN -> 0 sample", rd_i16le(b + 44 + 0) == 0);
            check("+Inf clamped to +32767", rd_i16le(b + 44 + 2) == 32767);
            check("-Inf clamped to -32767..-32768", rd_i16le(b + 44 + 4) <= -32767);
            free(b);
        } else { check("NaN test: file long enough", 0); if(b) free(b); }
    }

    /* --- streaming stereo: write in 3 chunks, verify backfilled sizes --- */
    {
        const u32 sr = 22050, ch = 2;
        WavWriter *w = wav_begin(P2, ch, sr);
        check("wav_begin returns writer", w != NULL);
        u32 total = 0;
        if (w) {
            /* three interleaved LR chunks of varying length */
            f32 c1[] = {0.1f,-0.1f, 0.2f,-0.2f};      /* 2 frames */
            f32 c2[] = {0.3f,-0.3f};                  /* 1 frame  */
            f32 c3[] = {0.4f,-0.4f, 0.5f,-0.5f, 0.6f,-0.6f}; /* 3 frames */
            int rc = 0;
            rc |= wav_write_frames(w, c1, 2); total += 2;
            rc |= wav_write_frames(w, c2, 1); total += 1;
            rc |= wav_write_frames(w, c3, 3); total += 3;
            rc |= wav_write_frames(w, NULL, 0); /* no-op must be safe */
            check("streaming writes return 0", rc == 0);
            check("wav_end returns 0", wav_end(w) == 0);
        }
        u8 *b = NULL; long len = slurp(P2, &b);
        check("streaming file readable", len > 0);
        if (len > 0) {
            check("streaming header + backfilled sizes valid",
                  check_header(b, len, ch, sr, total));
            /* first interleaved sample L=0.1 -> ~3277 */
            i16 got = rd_i16le(b + 44);
            check("streaming first sample plausible", got > 3000 && got < 3600);
            free(b);
        }
    }

    /* --- error handling: bad args return non-zero / NULL --- */
    check("null path -> error", wav_write_pcm16(NULL, (f32*)"", 1, 1, 8000) != 0);
    check("zero channels -> error", wav_write_pcm16(P1, (f32*)"", 1, 0, 8000) != 0);
    check("wav_begin bad args -> NULL", wav_begin(P2, 0, 8000) == NULL);
    check("wav_end(NULL) -> error", wav_end(NULL) != 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL PASS\n");
    return failures ? 1 : 0;
}
