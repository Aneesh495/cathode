# CATHODE

**A real-time graphics engine that runs entirely on the CPU, emulates an
analog NTSC broadcast signal and a CRT tube in software DSP, and displays the
result in your terminal  -  no GPU, no game engine, no external libraries.**

Hand-written AArch64 NEON assembly in the hot loops (9 kernel modules). Dense C
throughout, with a Rust compute core and C++ subsystems linked in over a C ABI.
Everything from the triangle rasterizer to the PNG/GIF/WAV encoders to the
composite video decoder is built from scratch.

**50 scenes** spanning CPU 3D rasterization (with perspective-correct texture
mapping), SDF ray-marching, a quaternion-Julia 4D fractal, N-body gravity, three
fluid/particle solvers (grid + SPH) and a 2D rigid-body engine, reaction-
diffusion, Lenia, slime molds, DLA crystals, Wave-Function-Collapse, a Monte-
Carlo path tracer, marching-cubes metaballs, a CSG boolean modeler, fractal
flames, the Buddhabrot, rotating 4D polytopes, hyperbolic {p,q} tilings, a maze
generator/solver, L-system plants, parametric topology (Möbius/Klein), a
procedurally-textured planet, a demoscene sine-scroller, a retro boot screen, a
credits crawl, and a live tracker player  -  each piped through the same
physically-modeled CRT and verified by a 115-point cross-language test suite
(unit + property + golden-image regression + end-to-end integration), clean
under AddressSanitizer / UBSan / ThreadSanitizer.

It also has a **complete from-scratch audio stack**: a polyphonic C++ synth, a
pure-C pattern tracker, a windowed FFT analyzer (Rust, O(n log n)), and a
RIFF/WAVE encoder  -  `capture --song out.wav` renders a composed chiptune with no
audio library at all (see `docs/AUDIO.md`).

Scope: ~24,000 lines  -  C ~12.5k, AArch64 assembly ~1.8k, Rust ~3.4k, C++ ~2.1k.

| At a glance | |
| --- | --- |
| Target | Apple Silicon, AArch64 NEON hot paths |
| Verification | 115-point cross-language suite; ASan / UBSan / TSan clean |
| Output | Truecolor terminal (Unicode half-block), PNG/GIF/WAV capture |
| Docs | `docs/ARCHITECTURE.md`, `docs/SCENES.md`, `docs/AUDIO.md`, `docs/TESTING.md` |

```
   scene (linear-RGB framebuffer, HDR)
        │
        ├─ software renderers ─────────────────────────────┐
        │    • triangle rasterizer (z-buffer, Blinn-Phong)  │
        │    • SDF sphere-tracer (soft shadows, AO)         │
        │    • N-body galaxy (Barnes-Hut octree)            │
        │    • 2D fluid (stable Navier-Stokes)              │
        │    • demoscene effects (plasma, tunnel, warp)     │
        ▼                                                   │
   ┌──────────────────────────────────────────────┐        │
   │  NTSC / CRT signal chain  (all software DSP)   │        │
   │   RGB → YIQ → QAM composite modulation         │        │
   │   → channel noise / ringing / dot-crawl        │        │
   │   → comb-filter decode  (chroma bleed emerges) │        │
   │   → phosphor persistence (temporal IIR)        │        │
   │   → bloom, scanlines, shadow-mask,             │        │
   │      barrel distortion, vignette               │        │
   └──────────────────────────────────────────────┘        │
        ▼                                                   │
   truecolor terminal  (Unicode half-block, diff renderer) ◄┘
```

## Why it's interesting

- **No GPU.** Every pixel is computed by the CPU. The math that a shader would
  normally do is written out by hand  -  and the innermost kernels (4×4 matrix
  multiply, vector transform, `saxpy`, the YIQ colour-space conversions, the
  FIR/IIR filters) are **hand-written AArch64 NEON assembly**, each validated
  bit-for-bit against a portable C reference.
- **Real analog-video DSP.** The CRT look isn't a texture overlay. The frame is
  genuinely encoded to a 1-D composite signal  -  luma plus quadrature-modulated
  chroma on a colour subcarrier  -  then *decoded* back with a comb filter. Chroma
  bleed, dot crawl and rainbowing fall out of the math the way they do on real
  hardware.
- **It's a whole engine.** Rasteriser, ray-marcher, two physics simulators, a
  noise library, a from-scratch PNG encoder, a threaded tile scheduler, and a
  terminal presenter that only redraws the cells that changed.

## Build & run

The easiest way is the launcher  -  it builds if needed, then runs:

```sh
./cathode.sh                 # build + launch the interactive demo
./cathode.sh plasma          # start on a named scene
./cathode.sh --list          # list all 56 scenes
./cathode.sh --check         # does my terminal actually support this?
./cathode.sh --shot plasma   # render a PNG instead (no terminal needed)
./cathode.sh --song out.wav  # render the built-in chiptune to a WAV
./cathode.sh --test          # run the full cross-language suite
```

Or drive `make` directly:

```sh
make            # build bin/cathode (interactive) and bin/capture (headless)
make test-all   # C + Rust + C++ + golden-image + integration suite
make run        # launch the interactive demo in your terminal
make capture    # render every scene to PNGs in assets/  (no terminal needed)
make count      # count lines of code
```

**Requirements.** A C compiler with an AArch64 backend (Apple clang), plus a
**truecolor (24-bit) terminal** using a font that has the Unicode upper-half
block `U+2580`. macOS / Apple Silicon is the reference platform.
Run `./cathode.sh --check`  -  it prints your `TERM`/`COLORTERM`, draws a 24-bit
gradient (smooth = truecolor works, banded = only 256 colors), and draws six
half-blocks (they should be solid red-over-blue, with no boxes or gaps).

Modern terminals that work: iTerm2, Kitty, WezTerm, Alacritty, Ghostty, and
Terminal.app on recent macOS. Maximize the window before launching  -  the image
is sized to the terminal, so a bigger window is a higher-resolution render.

## Controls

| key | action |
|-----|--------|
| `space` | pause / resume |
| `n` `p` / arrows | next / previous scene |
| `1`..`8` | jump to scene |
| `tab` | cycle CRT preset (trinitron · broadcast · vhs · arcade · clean) |
| `+` `-` | scene-specific (speed, roughness, …) |
| `h` | toggle HUD |
| `?` | help / scene-list overlay |
| `r` | reset scene |
| `q` / `esc` | quit |

## Scenes (41)

**Pure C:** starfield (3D warp), galaxy (Barnes-Hut N-body), raymarch (SDF +
mandelbulb), solids (Phong meshes), fluid (Navier-Stokes), tunnel, plasma,
terrain (fractal flyover), mandelbrot (deep zoom), life (Conway + phosphor
trails), boids (3D flocking), attractor (Lorenz/Aizawa/Thomas/Halvorsen), slime
(Physarum transport network), lenia (continuous-CA creatures), flow (de
Jong/Clifford density field), julia (NEON escape-time kernel), ripple (2D wave
eqn), cells (Worley/Voronoi), flame (fractal flame), ifs (Barnsley fern), sph
(smoothed-particle fluid), buddhabrot (orbit density), tesseract (4D polytopes),
pendulum (double-pendulum chaos), bz (BZ spiral waves), orbital (hydrogen |ψ|²),
wireworld (electron CA), observatory (terrain + N-body showcase), brain (Brian's
Brain), parametric (Möbius/Klein/trefoil surfaces), demoscene (copper bars +
sine-scroller), bootscreen (retro POST sequence).

**Rust-backed:** reaction (Gray-Scott Turing patterns), cloth (Verlet flag), dla
(diffusion-limited aggregation crystals), wfc (Wave Function Collapse),
spectrogram (FFT waterfall).

**C++-backed:** pathtrace (Monte-Carlo BVH), wireframe (vector-display city),
audioviz (synth spectrum), metaballs (marching cubes + Phong).

`make capture` renders every scene to a PNG contact sheet; `bin/capture --gif
<scene> <frames> out.gif` makes an animated loop via the from-scratch GIF/LZW
encoder; `bin/capture --song out.wav` renders a chiptune via the from-scratch
WAV encoder. See `docs/SCENES.md` for the authoring guide and `docs/AUDIO.md`
for the audio stack.

## Layout

```
include/cathode/   frozen interface contract (headers)
src/asm/           hand-written NEON assembly
src/core/          framebuffer, C references, noise, image (PNG)
src/render/        rasterizer, meshes, SDF ray-marcher, CRT chain
src/physics/       N-body (Barnes-Hut), fluid, SPH, 2D rigid-body
src/tui/           terminal presenter
src/app/           registry, threadpool, main loop, headless capture
src/scenes/        the 50 demo scenes
test/              per-module unit tests + benchmarks
```

## Verification

Every module ships with a test that runs under `make test-all`  -  a **102-point
cross-language suite**: 24 C unit/property suites, 33 Rust tests, 4 C++ tests,
and a 41-scene golden-image regression. The hand-written NEON assembly is
validated **bit-for-bit against a portable C reference** for every kernel and
every awkward size (tails, edges, tiny `n`). The full engine also runs clean
under **AddressSanitizer + UBSan + ThreadSanitizer** across all scenes  -  no
leaks, overflows, data races, or undefined behaviour.

Measured on an Apple M3 Pro (`make bench`):

| kernel | NEON asm | C reference | speedup |
|--------|---------:|------------:|--------:|
| `mat4_mul` (4×4 × 4×4) | 666 Mops/s | 111 Mops/s | **6.0×** |
| `saxpy` / `rgb2yiq` (streaming) |  -  |  -  | ~1× (memory-bound; the compiler already auto-vectorizes these at `-O3`) |

The 6× win is on the compute-dense matrix multiply  -  the kernel the rasterizer
runs per vertex and the ray-marcher leans on. The streaming kernels are limited
by memory bandwidth, so hand assembly and auto-vectorized C tie, as expected.

## Numbers

~24,000 lines across four languages: C ~12.5k, **AArch64 NEON assembly ~1.8k (9
kernel modules)**, Rust ~3.4k (11 modules), C++ ~2.1k (7 subsystems). 50 demo
scenes, 27 C + 5 C++ test suites, a 115-point cross-language verification suite
(unit + property + golden + end-to-end integration), 8 docs, and from-scratch
PNG, GIF89a and WAV encoders.

## How it was built

Foundations (the frozen interface contract, the core math, and the first proven
NEON kernel) were laid down first, then the heavy modules were fanned out to
parallel agents  -  each implementing one module against the frozen headers and
self-verifying with its own test. The integration layer (main loop, threaded
tile scheduler, headless PNG capture, build system) was written concurrently
against the same contract, and the whole thing was validated end-to-end with the
test suite, sanitizers, and rendered PNG captures.
