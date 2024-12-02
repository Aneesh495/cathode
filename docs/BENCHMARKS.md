# CATHODE - Benchmarks

Hand-written NEON kernels vs portable C references, both at `-O3 -ffast-math`
(so C is auto-vectorized - a fair baseline). Measured on Apple Silicon (arm64).
Reproduce with `make bench`.

## Primary performance gates

These are the verified engineering metrics for the engine. They reflect measured
results on Apple Silicon hardware (arm64, Apple Clang -O3 -ffast-math).

| Metric | Gate / Baseline | Measured | Harness | Context & Notes |
| --- | --- | --- | --- | --- |
| Rasterizer NEON speedup | **~4×** | **~6.7×** (640 vs 92 Mops/s) | `mat4_mul` NEON vs C reference | Compute-dense transform kernel on the CPU rasterizer path |
| NTSC composite throughput | **≥114 MS/s** | **~330 MS/s** | RGB→YIQ + **I/Q QAM** encode/decode | Sustained sample rate on single-threaded DSP hot path |
| DSP numerical fidelity | **<0.5% NRMSE** | **<0.01% NRMSE** | ≥**13,000** randomized + edge vectors | Neon↔ref equivalence & encode→decode round-trips (`test_dsp`) |
| Golden image regression | 100% match | 56/56 scenes match | `test_golden` (96×72, 24 frames) | Deterministic headless pixel regression across full scene roster |

Terminal interactive resolution is decoupled from headless capture: the TUI maps
into Unicode half-blocks; high-resolution captures use the headless framebuffer path.

## Kernel microbenchmarks

| kernel | NEON | C reference | speedup | notes |
|--------|-----:|------------:|--------:|-------|
| `mat4_mul` (4×4×4×4) | ~640 Mops/s | ~92 Mops/s | **~6.7×** | Rasterizer / N-body per-vertex win; core SIMD speedup |
| `fm_sin4` (sin approx) | ~2990 Mops/s | ~500 Mops/s | **~6.0×** | libm is the C baseline here |
| `fm_exp4` (exp approx) | ~2470 Mops/s | ~585 Mops/s | **~4.2×** | |
| `fm_log4` (log approx) | ~1980 Mops/s | ~470 Mops/s | **~4.2×** | |
| `rk_ray4_spheres` | ~1290 Mops/s | ~415 Mops/s | **~3.1×** | |
| `fk_mandel4` (256 it) | ~3.4 Mpx/s | ~1.3 Mpx/s | **~2.6×** | |
| `blur_v` (256², r4) | ~720 Mops/s | ~300 Mops/s | **~2.4×** | |
| `dsp_rgb2yiq` | ~3300 Mops/s | ~3250 Mops/s | ~1.0× | Memory-bound; streaming bandwidth tie |
| Composite I/Q path | **~330 MS/s** | ≥114 MS/s floor | - | Encode/decode sample throughput gate |
| `grav_accum` | ~1135 Mpair/s | ~1690 Mpair/s | **~0.67×** | Honest loss; see below |

### Where hand assembly loses (`grav_accum`)

The hand-written NEON gravity kernel can lose to `-O3 -ffast-math` C: clang
auto-vectorizes well and the hand kernel may over-refine `rsqrt`. Kept as a
validated reference, not as a speed claim.

## Frame rendering throughput and threading context

The CPU graphics engine and analog CRT emulator execute **strictly single-threaded on the CPU** (no GPU dispatch or shaders).

### Measured frame times across resolutions (Apple Silicon M-series)

| Resolution | Pixels | Scene Render (`solids`) | CRT Emulation (`crt_process`) | Total Frame Time | Effective Rate |
|---|---|---|---|---|---|
| **96×72** (golden test / baseline) | 6.9k | 0.25 ms | 0.20 ms | **0.45 ms** | >2,200 fps |
| **320×240** (retro NTSC / capture) | 76.8k | 0.83 ms | 2.13 ms | **2.95 ms** | ~340 fps |
| **640×480** (standard VGA / SD) | 307.2k | 2.59 ms | 9.03 ms | **11.62 ms** | ~86 fps |
| **1280×720** (720p HD) | 921.6k | 5.70 ms | 34.53 ms | **40.23 ms** | ~25 fps |
| **2560×1440** (1440p QHD) | 3.69M | 21.07 ms | 150.50 ms | **171.57 ms** | ~5.8 fps |

### Reconciling throughput and frame time

For scale, at 1440p (3.69 million pixels):
- At the **≥114 MS/s** DSP throughput floor, a single flat pass over 3.69M samples takes ~32.3 ms.
- At our measured **~330 MS/s** sustained composite DSP throughput, a single 1D modulation pass takes ~11.2 ms.
- The complete physical CRT pipeline (`crt_process`) is multi-stage: RGB→YIQ, 1D QAM subcarrier modulation, low-pass notch filtering, YIQ→RGB decode, temporal phosphor persistence IIR blend, two-pass separable Gaussian bloom blur (horizontal + vertical), scanlines, aperture-grille shadow mask, and non-linear barrel distortion with bilinear coordinate mapping. Across 3.69 million pixels on a single CPU thread, the full chain takes ~150 ms.
- For interactive playback in the terminal TUI, cathode targets standard terminal dimensions (~2,000 half-block cells, ~80×50 px). In this domain, per-frame render compute is under 1 ms, easily saturating the terminal's refresh rate. High-resolution 1440p rendering is supported in headless mode for offline asset rendering, contact sheets, and deterministic golden-image captures.

## Reading these honestly

- **Compute-bound kernels** (including `mat4_mul`) show multi-× wins; the
  documented rasterizer transform speedup is the **~4–7×** `mat4_mul` result.
- **Memory-bound color conversion** ties asm and auto-vec C. Guaranteed LD3/ST3
  layout still matters for the CRT chain, but it is not marketed as a multi-× win.
- Multi-× wins are concentrated in compute-bound kernels. Frame rendering scales linearly with framebuffer pixel count; interactive terminal rendering runs at sub-millisecond times, while 1440p offline captures take ~170 ms single-threaded.

## NTSC DSP: throughput and NRMSE

The CRT chain (`src/render/crt.c`) performs real composite DSP:

```
RGB -> YIQ -> c = Y + I·cos(φ) + Q·sin(φ)  (I/Q QAM on the color subcarrier)
    -> channel (noise / ringing)
    -> Y' / I' / Q' demodulation via FIR -> RGB
```

Gates:

1. **Throughput ≥114 MS/s** (measured **~330 MS/s**) - sustained samples/second through the modulate /
   demodulate path used by `crt_process` (NEON helpers in `dsp_neon.s`).
2. **NRMSE <0.5%** (measured **<0.01%**) - root-mean-square error normalized by signal energy, over
   **≥13,000** vectors (random rows, edge widths, subcarrier phases, and
   neon↔ref pairs). Failures fail `make bench` / DSP tests.

## Cross-language: Rust FFT vs reference DFT

| bins | DFT (ref) | Rust FFT | speedup |
|-----:|----------:|---------:|--------:|
| 64   | 564 µs    | 40.0 µs  | **14×** |
| 256  | 2334 µs   | 25.8 µs  | **90×** |
| 512  | 4826 µs   | 25.4 µs  | **190×** |

Golden-image hash for `audioviz` remains unchanged after the FFT swap when
bin alignment matches (see `docs/AUDIO.md`).

## Interactive vs headless

At typical terminal cell counts, demoscene effects exceed 60 fps including CRT.
Headless benchmarking and captures are used for deterministic regression and offline asset export, decoupled from terminal emulator TTY I/O throughput.

## Method

`bench_all` / DSP harnesses time enough repetitions for a stable interval,
self-warm in-loop, and use production flags for both NEON and C references.
`fk_mandel4` correctness builds with strict FP; production timing may use
`-ffast-math` (see `docs/NEON.md`).
