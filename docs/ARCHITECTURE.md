# CATHODE - Architecture

Map of the system. Read this before changing hot paths.

## One-sentence summary

CATHODE is an **AArch64 NEON CPU graphics engine**: scenes render into a
linear-RGB framebuffer (including **1440p** headless), pass through a software
**NTSC composite DSP** path with **I/Q (QAM) modulation**, and present either
in a truecolor terminal or via PNG/GIF/WAV capture - with C, Rust, and C++
linked over a frozen C ABI.

## The frame pipeline

```
   Scene (C / C++ / Rust-backed)
        |  linear-RGB, scene-referred, HDR (may exceed 1.0)
        |  interactive: terminal-sized FB
        |  headless / bench: up to 2560x1440 (1440p), target <2 ms/frame
        v
   Framebuffer  (include/cathode/framebuffer.h)
        |
        v
   CPU renderers
        |  triangle rasterizer (NEON mat4 / project) - ~4x vs C on mat4_mul
        |  SDF marcher, N-body, fluids, demoscene kernels, ...
        v
   CRT / NTSC signal chain  (src/render/crt.c + src/asm/dsp_neon.s)
        |  RGB->YIQ
        |  I/Q QAM composite: c = Y + I cos(phi) + Q sin(phi)
        |  channel noise / ringing / dot-crawl
        |  comb/notch demodulate -> YIQ->RGB
        |  phosphor IIR, bloom, geometry (scanlines, mask, barrel, vignette)
        |  DSP gates: >=114 MS/s, <0.5% NRMSE over >=13K vectors
        v
   Display Framebuffer
        |
        +--> TUI (Reinhard+sRGB, U+2580 half-blocks, diff presenter)
        +--> image.c / capture (PNG, GIF89a) / wav.h
```

Interactive main (`src/app/main.c`) paces input and rebuilds buffers on resize.
Headless capture (`src/app/capture.c`) runs the same pipeline without a TTY -
this is how **1440p timing** and golden images are produced.

## Languages and why

| Language | Where | Why |
|----------|-------|-----|
| **C11** | core, renderers, physics, TUI, app, scenes | Shared ABI backbone |
| **AArch64 asm** | `src/asm/*.s` | Hand-tuned NEON; bit-level / NRMSE-checked vs C refs |
| **Rust** | `rustsrc/` | Memory-safe compute (RD, cloth, DLA, WFC, FFT, ...) |
| **C++20** | `src/cpp/*.cpp` | BVH path tracer, synth, CSG, softbody, ... |

FFI is always a **C ABI** (`rustcore.h` / `cppcore.h`). No Rust/C++ types cross
the boundary. See `docs/ABI.md`.

## Module map

```
include/cathode/     frozen contracts (do not break casually)
  simd.h             NEON mat4 / project / saxpy - rasterizer hot path
  dsp.h              RGB<->YIQ, FIR, IIR - NTSC I/Q helpers
  crt.h              NTSC/CRT chain config + process
  raster.h           CPU triangle rasterizer
  ...
src/asm/             NEON implementations (+ C refs under src/core/)
src/render/          rasterizer, SDF, crt.c (composite I/Q encode/decode)
src/scenes/          50+ demo scenes
test/                equivalence, NRMSE, golden, integration, benches
```

## Invariants

1. **`include/` headers are contracts.** Prefer additive APIs.
2. **Framebuffer is linear-RGB HDR.** Tonemap only at TUI / image output.
3. **Column-major 4×4.** Required by NEON layout.
4. **Apple AArch64 ABI** for asm (separate GP/FP banks; v8-v15 low 64 callee-saved).
5. **FFI never unwinds** (`panic=abort`; no C++ exceptions across `extern "C"`).

## Verification strategy

- Unit / property / golden / integration: `docs/TESTING.md`
- NEON equivalence: `docs/NEON.md`
- Throughput, 1440p frame time, DSP MS/s + NRMSE: `docs/BENCHMARKS.md`
- Sanitizers across scenes: `docs/BUILD.md`
