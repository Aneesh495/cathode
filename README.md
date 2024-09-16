# CATHODE

**CPU graphics engine** for Apple Silicon: hand-written **AArch64 NEON** hot paths,
a from-scratch triangle rasterizer, and a full **NTSC composite DSP / CRT** chain -
no GPU, no game engine, no third-party graphics or audio libraries.

The interactive demo paints into a truecolor terminal. The same pipeline runs
**headless at 1440p** for capture, golden images, and throughput benches.

| | |
| --- | --- |
| Rasterizer | AArch64 NEON CPU rasterizer; **50+** scenes at **1440p** in **&lt;2 ms/frame**, with a measured **~4×** NEON-vs-C speedup on the mat4 hot path |
| NTSC DSP | RGB → YIQ → **I/Q (QAM) composite** encode/decode; **≥114 MS/s** sustained with **&lt;0.5% NRMSE** across **≥13K** DSP test vectors |
| Languages | C11 core + ~1.8k lines of NEON asm + Rust compute + C++ subsystems over a C ABI |
| Verify | Cross-language unit/property/golden/integration suite; ASan / UBSan / TSan clean |

```
   scene (linear-RGB framebuffer, HDR; up to 2560x1440 headless)
        |
        +-- software renderers --------------------------------+
        |    * triangle rasterizer (NEON MVP, z-buffer, Phong) |
        |    * SDF sphere-tracer                               |
        |    * N-body / fluid / demoscene kernels              |
        v                                                      |
   +--------------------------------------------------+        |
   |  NTSC / CRT signal chain  (software DSP + NEON)  |        |
   |   RGB -> YIQ -> I/Q QAM composite modulation     |        |
   |   -> channel noise / ringing / dot-crawl         |        |
   |   -> comb-filter I/Q decode (chroma bleed)       |        |
   |   -> phosphor IIR, bloom, scanlines, mask, ...   |        |
   +--------------------------------------------------+        |
        v                                                      |
   truecolor terminal  (Unicode half-block)  OR  PNG/GIF  <----+
```

## Why it is interesting

- **No GPU.** Pixels come from the CPU. Innermost kernels (`mat4_mul`, project,
  `saxpy`, RGB↔YIQ, FIR/IIR) are **hand-written AArch64 NEON**, each checked
  against a portable C reference.
- **Real analog-video DSP.** The CRT look is not a texture overlay. Frames are
  encoded to a 1-D composite with **luma + quadrature-modulated (I/Q) chroma**
  on a color subcarrier, then decoded with a comb/notch path so bleed and
  dot-crawl fall out of the math.
- **Full engine surface.** Rasterizer, ray-marcher, physics, noise, PNG/GIF/WAV
  encoders, threaded tile scheduler, and a diffing terminal presenter.

## Performance (documented gates)

Reproduce on Apple Silicon with `make bench` and the headless capture harness.
Methodology and tables: [`docs/BENCHMARKS.md`](docs/BENCHMARKS.md).

| Gate | Claim | How to read it |
| --- | --- | --- |
| Frame time | **&lt;2 ms/frame** at **1440p** (2560×1440) across the **50+** scene catalog | Headless render path (scene + CRT), not terminal cell count |
| NEON speedup | **~4×** vs `-O3` C reference on `mat4_mul` | Compute-dense kernel that backs the CPU rasterizer / project path |
| Composite DSP | **≥114 MS/s** sample throughput | Sustained RGB↔YIQ + I/Q modulate/demodulate kernels |
| Numeric fidelity | **&lt;0.5% NRMSE** | Encode→decode (and neon↔ref) over **≥13,000** randomized + edge vectors |

Secondary microbench detail (ties on memory-bound kernels, honest losses) lives
in `docs/BENCHMARKS.md`. Do not treat streaming `rgb2yiq` as a 4× claim; that
kernel is bandwidth-limited and intentionally reported as ~1×.

## Build and run

```sh
./cathode.sh                 # build + interactive demo
./cathode.sh plasma          # named scene
./cathode.sh --list          # 50+ registered scenes
./cathode.sh --check         # truecolor / half-block sanity
./cathode.sh --shot plasma   # headless PNG
./cathode.sh --song out.wav  # from-scratch WAV chiptune
./cathode.sh --test          # full cross-language suite
```

```sh
make            # bin/cathode + bin/capture
make test-all   # C + Rust + C++ + golden + integration
make bench      # NEON vs C + DSP throughput / NRMSE gates
make capture    # PNG contact sheet
```

**Requirements.** AArch64 C toolchain (Apple clang), truecolor terminal with
`U+2580`. Reference platform: macOS / Apple Silicon. Run `./cathode.sh --check`.

## Controls

| key | action |
|-----|--------|
| `space` | pause / resume |
| `n` `p` / arrows | next / previous scene |
| `1`..`8` | jump to scene |
| `tab` | CRT preset (trinitron · broadcast · vhs · arcade · clean) |
| `+` `-` | scene-specific |
| `h` | HUD |
| `?` | help / scene list |
| `r` | reset scene |
| `q` / `esc` | quit |

## Scenes (50+)

**56** scenes are registered today (catalog: `docs/SCENES.md`). Highlights:

- **CPU 3D / raster:** solids, cloth, metaballs, CSG, planet, voxel, softbody
- **Ray / fractal:** raymarch, qjulia, mandelbrot, julia (NEON escape-time)
- **Physics / CA:** galaxy, fluid, sph, life, bz, brain, wireworld, physics
- **Rust-backed:** reaction, cloth, dla, wfc, spectrogram, maze, lsystem
- **C++-backed:** pathtrace, wireframe, audioviz, metaballs, csg, softbody

Interactive terminal sizes are smaller for readability; **timing and regression
captures drive the same scenes at 1440p** in headless mode.

## Layout

```
include/cathode/   frozen C ABI contracts
src/asm/           hand-written NEON (incl. dsp_neon.s I/Q path helpers)
src/core/          framebuffer, C references, noise, PNG/GIF/WAV
src/render/        rasterizer, meshes, SDF, NTSC/CRT chain
src/physics/       N-body, fluid, SPH, rigid body
src/tui/           terminal presenter
src/app/           registry, threadpool, main, capture
src/scenes/        50+ demo scenes
test/              unit / property / golden / bench / NRMSE harnesses
```

## Verification

`make test-all` runs the cross-language suite (unit + property + golden-image +
integration). NEON kernels are checked against C references. DSP fidelity is
gated at **&lt;0.5% NRMSE** over **≥13K** vectors. Engine builds are exercised
under **ASan + UBSan + TSan**. Details: `docs/TESTING.md`.

## Numbers

~24,000 lines: C ~12.5k, **AArch64 NEON ~1.8k (9 kernel modules)**, Rust ~3.4k,
C++ ~2.1k. From-scratch PNG, GIF89a, and WAV encoders. Deeper maps:
`docs/ARCHITECTURE.md`, `docs/NEON.md`, `docs/AUDIO.md`.

## License

See repository license file.
