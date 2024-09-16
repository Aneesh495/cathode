/* ==========================================================================
 * cathode/wav.h  -  a from-scratch RIFF/WAVE (PCM) audio encoder.
 *
 * Companion to the from-scratch PNG and GIF89a image encoders: writes a
 * canonical 44-byte-header WAV file with 16-bit signed little-endian PCM
 * samples. This is what turns the C++ synth's f32 sample stream into a real,
 * playable audio artifact  -  no libsndfile, no dependencies, just the bytes.
 *
 * Two interfaces:
 *   - one-shot: hand it a whole float buffer, it clamps/quantizes and writes.
 *   - streaming: begin -> write chunks -> end, patching the length fields at
 *     the end (so you can encode audio you generate block-by-block without
 *     buffering the whole thing).
 *
 * Input samples are f32 nominally in [-1, 1]; out-of-range values are clamped
 * (hard-limited) before quantization to 16-bit.
 * ========================================================================== */
#ifndef CATHODE_WAV_H
#define CATHODE_WAV_H

#include "cathode/types.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot: write `nframes` frames of `channels`-interleaved f32 samples at
 * `sample_rate` Hz to `path` as 16-bit PCM. Returns 0 on success, non-zero on
 * error. `samples` length must be nframes*channels. */
int wav_write_pcm16(const char *path, const f32 *samples,
                    u32 nframes, u32 channels, u32 sample_rate);

/* Streaming writer. Open with wav_begin (channels=1 mono, 2 stereo, etc.),
 * push interleaved f32 frames with wav_write_frames any number of times, then
 * wav_end to backfill the RIFF/data sizes and close. */
typedef struct WavWriter WavWriter;
WavWriter *wav_begin(const char *path, u32 channels, u32 sample_rate);
/* write `nframes` interleaved frames (nframes*channels floats). Returns 0 ok. */
int        wav_write_frames(WavWriter *w, const f32 *frames, u32 nframes);
/* finalize (patch header sizes) and close; frees the writer. 0 ok. */
int        wav_end(WavWriter *w);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_WAV_H */
