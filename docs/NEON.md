# CATHODE — Hand-written AArch64 NEON assembly

The hot kernels are hand-written NEON. This document captures the conventions
and the traps, so the next person writing a kernel gets it right the first time.

## Files

| File | Kernels | C reference | Test |
|------|---------|-------------|------|
| `src/asm/simd_neon.s` | mat4 mul/transform, vec4 dot, rsqrt, vec3 normalize, saxpy | `src/core/simd_ref.c` | `test/test_simd.c` |
| `src/asm/project_neon.s` | batched 3D point → clip → perspective-divide → screen (+visibility) | `src/core/simd_ref.c` | `test/test_project.c` |
| `src/asm/dsp_neon.s` | RGB↔YIQ (LD3/ST3), symmetric FIR, IIR blend, scale/bias/clamp | `src/core/dsp_ref.c` | `test/test_dsp.c` |
| `src/asm/postfx_neon.s` | accumulate, screen blend, bright-pass, 5-tap blur, sum/max reduce | `src/core/postfx_ref.c` | `test/test_postfx.c` |
| `src/asm/fastmath_neon.s` | vectorized sin/cos/exp approximations | `src/core/fastmath_ref.c` | `test/test_fastmath.c` |
| `src/asm/raykernel_neon.s` | 4-wide ray-sphere and ray-AABB intersection | `src/core/raykernel_ref.c` | `test/test_raykernel.c` |
| `src/asm/fractalkernel_neon.s` | 4-lane escape-time Mandelbrot/Julia iteration | `src/core/fractalkernel*.c` | `test/test_fractalkernel.c` |
| `src/asm/blur_neon.s` | separable Gaussian blur (horizontal & vertical passes) | `src/core/blur_ref.c` | `test/test_blur.c` |

**Every kernel is validated bit-for-bit (or within a stated tolerance for the
approximations) against its C reference.** That is the non-negotiable rule: no
asm ships without a passing equivalence test.

## The Apple / Darwin AArch64 ABI (what the kernels rely on)

- **Symbol names get a leading underscore.** `void foo(...)` in C is `_foo` in
  asm: `.globl _foo`.
- **Argument registers, two separate banks:**
  - integers/pointers: `x0..x7`
  - floats/SIMD: `v0..v7` (`s0..s7` for scalars)
  - The banks are counted independently. For
    `f(float*, const float*, float g, float b, float hi, size_t n)`: the two
    pointers take `x0,x1`; `g,b,hi` take `s0,s1,s2`; and `n` is the next
    **integer** arg → `x2`, *not* `x3`. Miscounting this is a classic SIGBUS.
- **Return:** float in `s0`, integer in `x0`.
- **Callee-saved SIMD:** the low 64 bits of `v8..v15` must be preserved across a
  call. If a leaf/helper scribbles `v8..v15` without saving them, the *caller's*
  live floats get corrupted — a bug that shows up as unrelated wrong values, not
  a crash. Use `v0..v7` and `v16..v31` (all caller-saved) in leaf routines.
- **Link register:** `bl` clobbers `x30`. Save/restore it in any routine that
  calls a helper: `str x30,[sp,#-16]!` … `ldr x30,[sp],#16`.
- **Alignment:** `.p2align 2` for 4-byte instruction alignment; `.p2align 4` for
  16-byte data (literal pools).

## Idioms used throughout

- `ld1 {v0.4s},[x0]` / `st1 {v0.4s},[x0],#16` — load/store 4 floats, optional
  post-increment.
- `ld3 {v0.4s,v1.4s,v2.4s},[x]` — de-interleave 3-channel (RGB) data on load;
  `st3` re-interleaves on store. This is why the color-space conversions are
  clean 4-wide SIMD.
- `fmla v0.4s, v1.4s, v2.s[0]` — fused multiply-accumulate, optionally by a
  broadcast lane (used for the matrix-column combine).
- `faddp` / `fmaxp` — pairwise reduce for horizontal sum/max.
- `frsqrte` + `frsqrts` — reciprocal-sqrt estimate + Newton-Raphson refinement.
- Every loop is 4-wide with a scalar tail for `n` not divisible by 4.

## Measured wins (`make bench`, Apple M3 Pro)

`mat4_mul` (compute-dense) runs **~6× faster** than the `-O3` C reference.
Streaming kernels (saxpy, color conversion) are memory-bandwidth-bound, so the
compiler's auto-vectorization already ties hand asm — expected, and documented
so nobody "optimizes" a memory-bound kernel expecting a 6× that isn't physically
available.

## Adding a new kernel — the checklist

1. Declare it in a header (extern C), with a `_ref` C sibling.
2. Write the C reference first (it's the spec).
3. Write the asm, obeying every ABI rule above.
4. Write a test that feeds random + edge-case inputs to both and asserts
   agreement across many sizes including 1,2,3 and non-multiples of 4.
5. Build with `clang -Iinclude test.c kernel.s ref.c -o t -lm` and iterate until
   it passes. Only then wire it into the Makefile and callers.
