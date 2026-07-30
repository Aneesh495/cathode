# CATHODE — changelog

A running log of major additions. CATHODE is a from-scratch, CPU-only graphics
and audio engine with software NTSC/CRT emulation, rendered to the terminal;
polyglot C / AArch64 NEON assembly / Rust / C++ over a frozen C ABI.

## Milestone: 50 scenes

The engine now ships **50 demo scenes**, **9 hand-written NEON kernels**, **11
Rust modules**, **7 C++ subsystems**, **4 physics engines**, and from-scratch
PNG / GIF89a / WAV encoders — ~24k lines, verified by a 115-point cross-language
suite (unit + property + golden-image + end-to-end integration), clean under
AddressSanitizer / UBSan / ThreadSanitizer, and built warning-free.

### Rendering
- CPU triangle rasterizer with **perspective-correct texture mapping**
  (`RasterCtx.texfn` hook; NULL = untextured, behavior-preserving).
- SDF sphere-tracer, BVH Monte-Carlo path tracer (multithreaded), marching
  cubes, volumetric ray-march.
- **CSG modeler** (C++): boolean tree of SDF primitives (sphere/box/cylinder/
  torus) with smooth-min/max blends → scalar field → marching cubes.
- **Quaternion Julia** 4D fractal (distance-estimated sphere trace, threaded).

### Physics
- Barnes–Hut N-body, Stam stable-fluids, SPH, and a new **2D impulse-based
  rigid-body solver** (SAT collision, sequential impulses, energy-stable
  stacking — position correction instead of velocity bias).

### Audio (complete from-scratch stack — see docs/AUDIO.md)
- Polyphonic C++ synth (ADSR, filter, Schroeder reverb).
- **Pattern tracker** (pure C): patterns × channels × rows, tick-level timing,
  effects column (arpeggio / note-cut / note-delay).
- **WAV encoder** (RIFF/PCM16, one-shot + streaming with size backfill).
- **Rust radix-2 FFT** wired into the synth spectrum (14–190× vs the DFT it
  replaced; numerically transparent — golden hashes unchanged).
- `capture --song out.wav` renders a composed chiptune with no audio library.

### Text & UI
- **5×7 bitmap font** (`text.h`): glyphs drawn into the framebuffer so they flow
  through the CRT chain. Powers the demoscene sine-scroller, retro bootscreen,
  perspective credits crawl, and the live tracker (chiptune) UI.

### Procedural (Rust)
- **Maze** generator + BFS flood-fill solver (recursive-backtracker).
- **L-system** turtle-graphics plant generator (fractal plant, Koch, dragon,
  Sierpinski, bushy tree).

### Math-viz
- **Hyperbolic {p,q} tilings** in the Poincaré disk (reflection-group folding,
  animated Möbius translation, threaded).
- Rotating parametric surfaces (Möbius / Klein / trefoil), procedurally-textured
  planet (fbm continents + ice caps).

### Build & testing
- Added **header-dependency tracking** (`-MMD -MP`) — a changed header now
  recompiles every dependent TU (fixed a latent stale-object crash).
- Added an **end-to-end integration test** validating the emitted PNG/GIF/WAV
  bytes through the full scene→CRT→encode pipeline.
- Honestly documented where hand-written assembly *ties or loses* to the
  compiler's auto-vectorizer (memory-bound kernels); reverted an unused kernel
  that lost rather than ship dead code.

## Foundations (earlier)

- CPU→terminal renderer via Unicode half-blocks + diff presenter.
- Software NTSC composite encode/decode (QAM subcarrier, comb filter) + CRT
  display model (phosphor, bloom, scanlines, shadow mask, barrel, vignette).
- Hand-written AArch64 NEON kernels (mat4, transcendentals, ray/AABB, blur,
  fractal escape-time, batched projection), each bit-validated vs a C reference.
- Rust compute core (colorimetry, reaction-diffusion, cloth, attractors,
  terrain, DLA, WFC) and C++ subsystems (path tracer, synth, scene graph).
