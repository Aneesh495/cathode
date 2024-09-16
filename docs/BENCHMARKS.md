# CATHODE - Benchmarks

Hand-written NEON kernels vs portable C references, both at `-O3 -ffast-math`
(so C is auto-vectorized - a fair baseline). Measured on Apple Silicon (arm64).
Reproduce with `make bench`.

## Resume-facing gates

These are the numbers a reader should be able to cross-check against the
project README. They are **separate** measurements (do not conflate them).

| Gate | Target | Harness | Notes |
| --- | --- | --- | --- |
| 1440p frame time | **&lt;2 ms/frame** | Headless scene + CRT at **2560×1440** | Covers the **50+** scene catalog; median over warm repetitions |
| Rasterizer NEON speedup | **~4×** | `mat4_mul` NEON vs C reference | Compute-dense kernel on the CPU rasterizer / project path |
| NTSC composite throughput | **≥114 MS/s** | RGB→YIQ + **I/Q QAM** encode/decode (+ FIR helpers) | Sustained sample rate on the DSP hot path |
| DSP fidelity | **&lt;0.5% NRMSE** | ≥**13,000** randomized + edge-case vectors | Neon↔ref and encode→decode round-trips |

Terminal interactive resolution is unrelated to the 1440p gate: the TUI maps
into Unicode half-blocks; throughput claims use the headless framebuffer path.

## Kernel microbenchmarks

| kernel | NEON | C reference | speedup | notes |
|--------|-----:|------------:|--------:|-------|
| `mat4_mul` (4×4×4×4) | ~450 Mops/s | ~112 Mops/s | **~4.0×** | Rasterizer / N-body per-vertex win; **resume speedup** |
| `fm_sin4` (sin approx) | ~3760 Mops/s | ~666 Mops/s | **~5.6×** | libm is the C baseline here |
| `fm_exp4` (exp approx) | ~3150 Mops/s | ~834 Mops/s | **~3.8×** | |
| `fm_log4` (log approx) | ~2530 Mops/s | ~646 Mops/s | **~3.9×** | |
| `rk_ray4_spheres` | ~1730 Mops/s | ~563 Mops/s | **~3.1×** | |
| `fk_mandel4` (256 it) | ~4.6 Mpx/s | ~1.7 Mpx/s | **~2.7×** | |
| `blur_v` (256², r4) | ~870 Mops/s | ~465 Mops/s | **~1.9×** | |
| `dsp_rgb2yiq` | ~3735 Mops/s | ~3638 Mops/s | ~1.0× | Memory-bound; **not** the 4× claim |
| Composite I/Q path | **≥114 MS/s** | (ref within NRMSE) | - | Encode/decode sample throughput gate |
| `grav_accum` | ~1135 Mpair/s | ~1690 Mpair/s | **~0.67×** | Honest loss; see below |

### Where hand assembly loses (`grav_accum`)

The hand-written NEON gravity kernel can lose to `-O3 -ffast-math` C: clang
auto-vectorizes well and the hand kernel may over-refine `rsqrt`. Kept as a
validated reference, not as a speed claim.

## Reading these honestly

- **Compute-bound kernels** (including `mat4_mul`) show multi-× wins; the
  documented rasterizer speedup is the **~4×** `mat4_mul` result.
- **Memory-bound color conversion** ties asm and auto-vec C. Guaranteed LD3/ST3
  layout still matters for the CRT chain, but it is not marketed as 4×.
- **1440p &lt;2 ms/frame** is end-to-end scene+CRT timing at full HD+ height, not
  a single kernel microbench and not terminal fps.

## NTSC DSP: throughput and NRMSE

The CRT chain (`src/render/crt.c`) performs real composite DSP:

```
RGB -> YIQ -> c = Y + I·cos(φ) + Q·sin(φ)  (I/Q QAM on the color subcarrier)
    -> channel (noise / ringing)
    -> Y' / I' / Q' demodulation via FIR -> RGB
```

Gates:

1. **Throughput ≥114 MS/s** - sustained samples/second through the modulate /
   demodulate path used by `crt_process` (NEON helpers in `dsp_neon.s`).
2. **NRMSE &lt;0.5%** - root-mean-square error normalized by signal energy, over
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
The **1440p / &lt;2 ms** claim is the headless engineering gate used for resume
and CI-style timing, not the Unicode presenter.

## Method

`bench_all` / DSP harnesses time enough repetitions for a stable interval,
self-warm in-loop, and use production flags for both NEON and C references.
`fk_mandel4` correctness builds with strict FP; production timing may use
`-ffast-math` (see `docs/NEON.md`).
