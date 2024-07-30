# CATHODE — Architecture

This document is the map of the system. Read it before touching the code; it
gives you the whole picture in one place so you don't have to reverse-engineer
it from the source.

## One-sentence summary

CATHODE renders 3D graphics, physics simulations, and demoscene effects
entirely on the CPU, pipes each frame through a software emulation of an analog
NTSC composite-video signal and a CRT tube, and displays the result in a
terminal using truecolor Unicode half-blocks — with the hot loops written in
hand-tuned AArch64 NEON assembly and heavy subsystems split across C, Rust, and
C++.

## The frame pipeline

Everything is organized around one data flow, executed once per frame:

```
   Scene (C / C++ / Rust-backed)
        │  writes linear-RGB, scene-referred, HDR (values may exceed 1.0)
        ▼
   Framebuffer  (include/cathode/framebuffer.h — the central shared surface)
        │
        ▼
   CRT signal chain  (src/render/crt.c, uses NEON DSP in src/asm/dsp_neon.s)
        │  RGB→YIQ → QAM composite modulation → channel noise/ringing
        │  → comb-filter decode (chroma bleed & dot-crawl emerge) → YIQ→RGB
        │  → phosphor persistence (temporal IIR) → bloom
        │  → barrel distortion, scanlines, interlaced fields, shadow mask, vignette
        ▼
   Display Framebuffer (linear RGB)
        │
        ▼
   TUI presenter  (src/tui/tui.c)
        │  Reinhard+sRGB tonemap → per-cell fg/bg + U+2580 half-block
        │  diff renderer: only emits escape codes for changed cells
        ▼
   Terminal
```

The interactive front-end (`src/app/main.c`) drives this loop, handles input,
paces to ~60 FPS, and — importantly — **re-queries the terminal size every
frame and rebuilds all buffers on change**, so the demo fills the window and
survives resizes. The headless front-end (`src/app/capture.c`) runs the same
pipeline without a terminal and writes PNGs, which is how the engine is
visually verified.

## Languages and why

| Language | Where | Why |
|----------|-------|-----|
| **C11** | core, renderers, physics, TUI, app, scenes | the backbone; the shared ABI is C |
| **AArch64 asm** | `src/asm/*.s` | hand-tuned NEON for the hottest kernels, each validated bit-for-bit vs a C reference |
| **Rust** | `rustsrc/` (staticlib) | memory-safe compute: colorimetry, reaction-diffusion, cloth, attractors, terrain |
| **C++20** | `src/cpp/*.cpp` | subsystems that benefit from templates/RAII/STL: BVH path tracer, audio synth, scene graph |

The polyglot boundary is always a **C ABI**. Rust exposes
`#[no_mangle] pub extern "C"` symbols (contract: `include/cathode/rustcore.h`);
C++ exposes `extern "C"` functions with opaque handles (contract:
`include/cathode/cppcore.h`). No Rust or C++ types ever cross the boundary —
only C-layout POD and opaque pointers. See `docs/ABI.md` for the ownership and
safety rules.

## Module map

```
include/cathode/     FROZEN interface contracts (headers). Never break these.
  types.h            base types (u8..f64, Vec3/Vec4/Mat4/Quat/Color3), constants
  vec.h              inline vector/matrix/quaternion math (header-only)
  simd.h             NEON SIMD kernel contract (mat4, dot, rsqrt, saxpy…)
  dsp.h              NEON DSP contract (YIQ, FIR, IIR, scale/bias/clamp)
  fastmath.h         NEON transcendental approximations (sin/cos/exp)
  raykernel.h        NEON batched ray-sphere / ray-AABB intersection
  framebuffer.h      the central shared linear-RGB surface + depth buffer
  crt.h              NTSC/CRT signal-chain config + state
  raster.h           CPU triangle rasterizer + procedural meshes
  sdf.h              signed-distance-field ray marcher
  physics.h          N-body (Barnes–Hut) + fluid (stable Navier–Stokes)
  noise.h            PRNG + value/Perlin/simplex/fbm/ridged noise
  image.h            framebuffer → PPM/PNG + animated GIF89a (self-contained)
  wav.h              f32 samples → RIFF/WAVE PCM16 (self-contained encoder)
  text.h             5x7 bitmap font → framebuffer (goes through the CRT chain)
  tui.h              terminal presenter + input + timing
  scene.h            the Scene vtable + registry
  rustcore.h         C ABI implemented by the Rust staticlib
  cppcore.h          C ABI implemented by the C++ subsystems

src/asm/             hand-written AArch64 NEON assembly (+ C references in core/)
src/core/            framebuffer, C references, noise, PNG, post-FX
src/render/          rasterizer, meshes, SDF marcher, CRT chain
src/physics/         N-body, fluid
src/tui/             terminal presenter
src/app/             registry, thread pool, main loop, headless capture
src/scenes/          the demo scenes (one Scene vtable each)
src/cpp/             C++20 subsystems (path tracer, synth, scene graph)
rustsrc/             Rust compute core (Cargo staticlib)
test/                per-module unit / integration / property tests
docs/                this file and the subsystem docs
```

## Invariants you must not break

1. **Headers in `include/` are frozen contracts.** Changing a signature means
   updating every implementation and caller. Add new headers rather than
   mutating shared ones mid-flight.
2. **Framebuffer is linear-RGB, HDR.** Never tonemap into it; tonemapping
   happens only at the final output stage (image.c / tui.c / rust_tonemap_frame).
3. **Column-major 4×4 matrices**, element (row,col) at `m[col*4+row]`. The NEON
   asm depends on this layout.
4. **NEON asm obeys the Apple ABI:** args in x0–x7 / v0–v7 / s0–s7 (GP and FP
   are *separate banks*), float return in s0, v8–v15 are callee-saved (low 64
   bits), save LR before `bl`. Every asm kernel has a C reference and a test
   that proves bit-for-bit agreement. See `docs/NEON.md`.
5. **The FFI boundary never unwinds.** Rust is built `panic = "abort"`; C++
   must not let exceptions escape an `extern "C"` function.

## Verification strategy

- `make test` builds and runs every C unit/integration test (each prints
  `ALL PASS` / a failure list; the suite reports `passed=N failed=M`).
- `cargo test --release` (or `make rust-test`) runs the Rust `#[test]` suite.
- The C++ subsystems each have a `test/test_*.cpp` compiled and run standalone.
- `make bench` reports NEON-vs-C speedups.
- The whole engine is run under AddressSanitizer + UBSan across all scenes.
- `make capture` renders every scene to a PNG for visual inspection.

See `docs/BUILD.md` for the build system and `docs/TESTING.md` for the full
testing philosophy.
