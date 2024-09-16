# CATHODE  -  the audio subsystem

CATHODE grew a full from-scratch audio stack alongside its graphics: a
polyphonic software synth, a pattern-based music tracker, a windowed FFT
analyzer, a RIFF/WAVE encoder, and (optional) real device output. Nothing
depends on an external audio library  -  like the rest of the engine, it's built
from the sample up.

```
   tracker (C)                  synth (C++)                encoders / sinks
   ───────────                  ───────────                ────────────────
   TrackerCell[]  ── events ──▶ cpp_synth_note_on/off  ┌─▶ wav.c  → .wav file
   order list       (sample-    voices + ADSR + filter │   (RIFF/PCM16)
   speed/tempo      accurate)   + Schroeder reverb ─────┤
                                        │               └─▶ AudioQueue (macOS,
                                        ▼                    opt-in AUDIO=1)
                                 rust_fft_mag  ──▶ audioviz / spectrogram scenes
                                 (O(n log n))       (bars + waterfall + scope)
```

## The synth (`src/cpp/synth.cpp`, ABI in `cppcore.h`)

A polyphonic subtractive synth. Each `note_on` allocates a voice (with voice
stealing when full); a voice has a phase oscillator (sine / saw / square /
triangle), a linear ADSR envelope, and feeds a shared one-pole-ish resonant
filter and a Schroeder reverb (4 comb + 2 all-pass) on the master. It never
opens a device on its own  -  it renders mono `f32` sample blocks in `[-1,1]` via
`cpp_synth_render`, which keeps it headless-safe and deterministic for tests
and golden images.

`cpp_synth_spectrum` returns a Hann-windowed magnitude spectrum of the last
rendered audio. In the full engine this is computed by the **Rust radix-2 FFT**
(`rust_fft_mag`/`rust_fft_complex`)  -  14-190× faster than the reference DFT and
numerically transparent (see `BENCHMARKS.md`); the standalone `test_synth`
falls back to the DFT so it needs no Rust.

## The tracker (`src/core/tracker.c`, `tracker.h`)

A pure-C MOD/XM-style sequencer: a song is `npatterns` patterns of
`rows × channels` cells, played through an `order` list at a given `speed`
(ticks/row) and `tempo` (BPM). It makes no sound itself  -  `tracker_advance`
converts the song into **sample-accurate note on/off events** that a caller
feeds to the synth. Timing is integer-sample based (`row = speed × round(sr ×
2.5 / tempo)` samples) so playback is perfectly reproducible.

Note-column semantics: `NOTE_NONE` holds the channel, `NOTE_OFF` releases it,
and a fresh note auto-releases whatever the channel was playing (so you always
get a clean `OFF` before the replacing `ON` at the same sample). The
instrument column selects a synth waveform; the volume column (0-64) sets note
velocity.

The engine runs at **tick** resolution (a row is `speed` ticks), which drives an
`effect` column packed as `0xCP` (command nibble C, param nibble P), built with
the `TRK_FX(cmd,param)` macro:

- **`0x0P` arpeggio**  -  retriggers the channel through pitches `base`, `base+P`,
  `base+2P` semitones on successive ticks: the classic chiptune "chord from one
  voice" (`P=3` minor-ish, `P=4` major/augmented).
- **`0xCP` note cut**  -  releases the note `P` ticks into the row.
- **`0xDP` note delay**  -  defers the note-on until `P` ticks into the row.

Effects are emitted at the exact tick's absolute sample, so block-by-block
pulling still places them correctly. The built-in `--song` tune arpeggiates its
pad channel to show the effect in action.

## The WAV encoder (`src/core/wav.c`, `wav.h`)

A canonical 44-byte-header RIFF/WAVE PCM16 writer, little-endian (a nice
contrast to the big-endian PNG encoder in `image.c`). Two interfaces:

- **one-shot** `wav_write_pcm16(path, samples, nframes, channels, rate)`;
- **streaming** `wav_begin → wav_write_frames* → wav_end`, where `wav_end`
  seeks back and backfills the RIFF `ChunkSize` and `data` `Subchunk2Size`
  fields  -  so you can encode audio you generate block by block without
  buffering the whole song.

`f32` inputs in `[-1,1]` are hard-clipped and rounded to `int16`.

## Rendering audio headless (`capture`)

```
capture --audio <out.wav> [seconds] [rate] [waveform 0-3] [reverb 0-1]
    render the synth's generative sequencer to a WAV.

capture --song  <out.wav> [repeats] [rate] [reverb]
    render the built-in 3-channel chiptune (Am-C / F-G) through the tracker
    → synth → WAV chain. Proves the whole pipeline with real composed music.
```

Both produce standards-compliant files (verified with macOS `afinfo`; the demo
tune measures ~0.22 RMS, full-scale peaks, 100 % non-silent).

## Seeing it: the `chiptune` scene

The `chiptune` scene is the audio stack's capstone  -  it wires **all four
languages** into one view: the pure-C tracker plays a looping pattern, its note
events drive the C++ synth, the Rust FFT analyzes each rendered block, and the
5×7 font draws a live MOD-style pattern grid (note names + instrument per
channel, the playing row highlighted) beside a spectrum-analyzer strip that
dances to the audio. It's fully deterministic and headless, so it golden-tests
like any other scene  -  you can *watch* the tracker play without a sound card.

## Real-time output (opt-in)

`make AUDIO=1` links `src/cpp/audio_out.cpp` (macOS AudioQueue). The `audioviz`
scene then toggles live playback with **Enter**; the device callback owns
`cpp_synth_render` on its own thread, and the main thread skips re-rendering
while the device is running to avoid a data race. The default build stubs these
to no-ops so the engine stays headless-safe.

## Tests

- `test_synth` (C++): RMS/pitch/reverb/waveform sanity of the synth.
- `test_tracker` (C): timing math, event ordering, note replacement/off,
  order-list traversal, looping, streamed draining, the arpeggio/note-cut/
  note-delay effects at tick granularity, and arg validation (23 checks).
- `test_wav` (C): reads emitted bytes back and validates every header field,
  quantization, clipping, and streaming size backfill (18 checks).

All are part of `make test` / `make test-all`, and each subsystem is
ASan/UBSan-clean.
