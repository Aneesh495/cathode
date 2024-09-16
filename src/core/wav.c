/* ==========================================================================
 * wav.c  -  RIFF/WAVE PCM16 encoder, written to the spec by hand.
 *
 * WAV layout we emit (canonical 44-byte header + PCM data):
 *
 *   offset  size  field                value
 *   0       4     ChunkID              "RIFF"
 *   4       4     ChunkSize            36 + data_bytes         (little-endian)
 *   8       4     Format               "WAVE"
 *   12      4     Subchunk1ID          "fmt "
 *   16      4     Subchunk1Size        16                      (PCM)
 *   20      2     AudioFormat          1                       (PCM, no compr.)
 *   22      2     NumChannels          channels
 *   24      4     SampleRate           sample_rate
 *   28      4     ByteRate             sr * channels * 2
 *   32      2     BlockAlign           channels * 2
 *   34      2     BitsPerSample        16
 *   36      4     Subchunk2ID          "data"
 *   40      4     Subchunk2Size        data_bytes
 *   44      ...   PCM samples          int16 LE, interleaved
 *
 * All multi-byte integer fields are little-endian (WAV is a little-endian
 * format, unlike PNG's big-endian network order  -  a nice contrast to the
 * image.c encoder). Samples are f32 in [-1,1], hard-clamped then rounded to
 * signed 16-bit.
 * ========================================================================== */
#include "cathode/wav.h"
#include <stdlib.h>
#include <string.h>

/* ---- little-endian field writers ---- */
static void w_u32le(FILE *f, u32 v) {
    u8 b[4] = { (u8)(v), (u8)(v >> 8), (u8)(v >> 16), (u8)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void w_u16le(FILE *f, u16 v) {
    u8 b[2] = { (u8)(v), (u8)(v >> 8) };
    fwrite(b, 1, 2, f);
}
static void w_tag(FILE *f, const char *t) { fwrite(t, 1, 4, f); }

/* Quantize one f32 sample in [-1,1] to int16 with hard clipping + rounding. */
static i16 quantize(f32 s) {
    /* Guard against NaN/Inf (e.g. a diverged synth filter) before the cast  - 
     * a NaN cast to int is UB. We test the IEEE-754 bit pattern rather than
     * `s==s`/isnan, because the file is built with -ffast-math, under which the
     * compiler assumes finiteness and folds those checks away. Exponent all-ones
     * (0xFF) means Inf or NaN; clamp those to the finite range below via 0. */
    u32 bits; memcpy(&bits, &s, sizeof(bits));
    if (((bits >> 23) & 0xFFu) == 0xFFu) {
        /* Inf keeps its sign so +Inf -> +full, -Inf -> -full; NaN -> silence. */
        u32 mant = bits & 0x7FFFFFu;
        if (mant != 0) return 0;                 /* NaN */
        s = (bits & 0x80000000u) ? -1.0f : 1.0f; /* +/- Inf */
    }
    if (s >  1.0f) s =  1.0f;
    if (s < -1.0f) s = -1.0f;
    /* scale to full int16 range; 32767 keeps +1.0 in range and symmetric-ish */
    f32 v = s * 32767.0f;
    /* round to nearest, away from zero */
    i32 iv = (i32)(v >= 0.0f ? v + 0.5f : v - 0.5f);
    if (iv >  32767) iv =  32767;
    if (iv < -32768) iv = -32768;
    return (i16)iv;
}

/* Write the 44-byte header. `data_bytes` may be a provisional 0 that a
 * streaming writer patches on close. */
static void write_header(FILE *f, u32 channels, u32 sample_rate, u32 data_bytes) {
    const u32 byte_rate  = sample_rate * channels * 2u;
    const u16 block_align = (u16)(channels * 2u);
    w_tag(f, "RIFF");
    w_u32le(f, 36u + data_bytes);
    w_tag(f, "WAVE");
    w_tag(f, "fmt ");
    w_u32le(f, 16u);              /* PCM fmt chunk size */
    w_u16le(f, 1u);              /* AudioFormat = PCM */
    w_u16le(f, (u16)channels);
    w_u32le(f, sample_rate);
    w_u32le(f, byte_rate);
    w_u16le(f, block_align);
    w_u16le(f, 16u);             /* bits per sample */
    w_tag(f, "data");
    w_u32le(f, data_bytes);
}

/* Convert & write a run of f32 samples as int16 LE. Buffers in small chunks so
 * we don't allocate the whole stream. Returns 0 on success. */
static int write_samples(FILE *f, const f32 *samples, size_t count) {
    enum { CHUNK = 1024 };
    u8 buf[CHUNK * 2];
    size_t i = 0;
    while (i < count) {
        size_t n = count - i; if (n > CHUNK) n = CHUNK;
        for (size_t k = 0; k < n; ++k) {
            i16 q = quantize(samples[i + k]);
            buf[2 * k + 0] = (u8)(q & 0xFF);
            buf[2 * k + 1] = (u8)((u16)q >> 8);
        }
        if (fwrite(buf, 2, n, f) != n) return 1;
        i += n;
    }
    return 0;
}

int wav_write_pcm16(const char *path, const f32 *samples,
                    u32 nframes, u32 channels, u32 sample_rate) {
    if (!path || !samples || channels == 0 || sample_rate == 0) return 1;
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    const size_t count = (size_t)nframes * channels;
    const u32 data_bytes = (u32)(count * 2u);
    write_header(f, channels, sample_rate, data_bytes);
    int rc = write_samples(f, samples, count);
    if (ferror(f)) rc = 1;         /* catch header/data write failures (e.g. full disk) */
    if (fclose(f) != 0) rc = 1;
    return rc;
}

/* ---- streaming writer ---- */
struct WavWriter {
    FILE *f;
    u32   channels;
    u32   sample_rate;
    u32   frames_written;   /* running frame count for the size backfill */
};

WavWriter *wav_begin(const char *path, u32 channels, u32 sample_rate) {
    if (!path || channels == 0 || sample_rate == 0) return NULL;
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    WavWriter *w = (WavWriter *)calloc(1, sizeof(WavWriter));
    if (!w) { fclose(f); return NULL; }
    w->f = f; w->channels = channels; w->sample_rate = sample_rate;
    w->frames_written = 0;
    /* header with provisional 0 sizes; patched in wav_end */
    write_header(f, channels, sample_rate, 0u);
    return w;
}

int wav_write_frames(WavWriter *w, const f32 *frames, u32 nframes) {
    if (!w || !w->f || (!frames && nframes)) return 1;
    if (nframes == 0) return 0;
    size_t count = (size_t)nframes * w->channels;
    if (write_samples(w->f, frames, count) != 0) return 1;
    w->frames_written += nframes;
    return 0;
}

int wav_end(WavWriter *w) {
    if (!w) return 1;
    int rc = 0;
    if (w->f) {
        const u32 data_bytes = w->frames_written * w->channels * 2u;
        /* backfill ChunkSize @ offset 4 and Subchunk2Size @ offset 40 */
        if (fseek(w->f, 4, SEEK_SET) == 0) {
            u8 b[4]; u32 v = 36u + data_bytes;
            b[0]=(u8)v; b[1]=(u8)(v>>8); b[2]=(u8)(v>>16); b[3]=(u8)(v>>24);
            if (fwrite(b,1,4,w->f) != 4) rc = 1;
        } else rc = 1;
        if (fseek(w->f, 40, SEEK_SET) == 0) {
            u8 b[4]; u32 v = data_bytes;
            b[0]=(u8)v; b[1]=(u8)(v>>8); b[2]=(u8)(v>>16); b[3]=(u8)(v>>24);
            if (fwrite(b,1,4,w->f) != 4) rc = 1;
        } else rc = 1;
        if (fclose(w->f) != 0) rc = 1;
    }
    free(w);
    return rc;
}
