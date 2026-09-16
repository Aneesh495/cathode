# CATHODE  -  Benchmarks

Hand-written NEON kernels vs their portable C references, both built at
`-O3 -ffast-math` (so the C side is auto-vectorized by clang  -  a fair, honest
comparison, not asm-vs-scalar). Measured on Apple silicon (arm64). Reproduce
with `make bench` (mat4/dsp) and the `bench_all` harness.

| kernel | NEON | C reference | speedup | notes |
|--------|-----:|------------:|--------:|-------|
| `mat4_mul` (4×4×4×4) | ~450 Mops/s | ~112 Mops/s | **~4.0×** | compute-dense; the rasterizer/N-body per-vertex win |
| `fm_sin4` (sin approx) | ~3760 Mops/s | ~666 Mops/s | **~5.6×** | libm is the "C" baseline here |
| `fm_exp4` (exp approx) | ~3150 Mops/s | ~834 Mops/s | **~3.8×** | |
| `fm_log4` (log approx) | ~2530 Mops/s | ~646 Mops/s | **~3.9×** | IEEE exp-extract + Mercator series |
| `rk_ray4_spheres` | ~1730 Mops/s | ~563 Mops/s | **~3.1×** | 4 ray-sphere tests at once |
| `fk_mandel4` (256 it) | ~4.6 Mpx/s | ~1.7 Mpx/s | **~2.7×** | escape-time, per 4-pixel block |
| `blur_v` (256², r4) | ~870 Mops/s | ~465 Mops/s | **~1.9×** | separable Gaussian, vertical pass |
| `dsp_rgb2yiq` | ~3735 Mops/s | ~3638 Mops/s | ~1.0× | **memory-bound**  -  asm and auto-vec C tie |
| `grav_accum` (N-body force) | ~1135 Mpair/s | ~1690 Mpair/s | **~0.67×** | **asm LOSES**  -  see below |

### Where hand assembly loses (`grav_accum`)

Reported honestly: the hand-written NEON gravity kernel is *slower* than its
`-O3 -ffast-math` C reference. clang auto-vectorizes the reference to 4-wide,
lowers `1/sqrtf` to the same `frsqrte`+refine hardware path, and schedules it
better than the hand code  -  and the reference's single Newton-Raphson step is
enough at `-ffast-math` tolerance, while the hand kernel does two. The lesson
mirrors the memory-bound case: a modern autovectorizer is a strong baseline, and
hand assembly only wins where it can do something the compiler won't (guaranteed
LD3/ST3 de-interleave, custom range reduction, fused lane tricks). The kernel is
kept as a *validated reference* implementation, not because it's faster.

## Reading these honestly

- **Compute-bound kernels win big** (2.7-5.6×): the work is arithmetic in
  registers, so hand-scheduling FMAs and using the full 128-bit lanes pays off.
- **Memory-bound kernels tie** (`dsp_rgb2yiq`): throughput is limited by how
  fast we can stream the pixel arrays from memory, and clang's auto-vectorizer
  already saturates that. Hand assembly cannot beat physics; we report the tie
  rather than cherry-picking. The value of the hand-written version there is
  the *guaranteed* vectorization (LD3/ST3 de-interleave) independent of compiler
  mood, and it's the reference the CRT chain is validated against.
- These are single-threaded kernel microbenchmarks. On top of them, the SDF
  ray-marcher scene fans scanlines across all cores via the work-stealing
  thread pool (~4-5× additional throughput, measured at ~466 % CPU), and is
  ThreadSanitizer-clean.

## Cross-language: Rust FFT vs the reference C++ DFT (synth spectrum)

The C++ audio synth's magnitude-spectrum analyzer originally evaluated a
windowed **DFT** directly at each output bin  -  O(bins × window). Wiring in the
Rust radix-2 FFT (`rust_fft_mag`/`rust_fft_complex`, one O(N log N) transform of
the zero-padded 2048-point Hann frame, then read the aligned bins) is a large,
scale-independent win. Measured on this machine (2000 calls, note A4 sounding,
window = 2048 samples):

| bins | DFT (ref) | Rust FFT | speedup |
|-----:|----------:|---------:|--------:|
| 64   | 564 µs    | 40.0 µs  | **14×** |
| 256  | 2334 µs   | 25.8 µs  | **90×** |
| 512  | 4826 µs   | 25.4 µs  | **190×** |

The FFT cost is dominated by the fixed-size transform, so it's essentially flat
in `bins` while the DFT grows linearly. Crucially the swap is **numerically
transparent**: the FFT bin `j = round(k·N / 2·nbins)` coincides with the DFT's
`ω_k = π·k/nbins` (exact when `N = 2·nbins`), reusing the identical Hann window
and `2/wnorm` coherent-gain normalization  -  so the `audioviz` golden-image hash
is *unchanged* after the swap (verified by `make golden`). The fast path is
guarded by `-DCATHODE_HAVE_RUST_FFT` (engine only); the standalone `test_synth`
compiles without the Rust staticlib and falls back to the reference DFT, so its
"dominant bin matches pitch" assertions still validate the math independently.

## Rendering throughput (end-to-end, incl. CRT chain)

At 240×180 the multithreaded `raymarch` scene runs ~30 frames in ~0.7 s wall
(≈42 fps) including the full NTSC/CRT signal chain; the pure-effect scenes
(plasma, tunnel, cells) run comfortably above 60 fps at typical terminal sizes.

## Method

`bench_all.c` times each kernel over enough repetitions to run for a
meaningful interval, warms nothing special (the loops self-warm), and computes
ops/s from the known work per call. The C references are compiled with the same
optimization flags as production. `fk_mandel4` is timed at the production
`-ffast-math`; its *correctness* test is built strict-FP (see `docs/NEON.md`).
