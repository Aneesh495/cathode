# CATHODE  -  Testing philosophy

Every non-trivial piece of CATHODE is verified three complementary ways.
Nothing ships on "it compiles."

## 1. Equivalence tests (asm ⇔ C reference)

Every hand-written NEON kernel has a portable C reference implementing the same
math, and a test that feeds thousands of random + edge-case inputs to both and
asserts they agree (bit-for-bit where the math is exact, within a stated
tolerance for approximations). This is how we trust the assembly.

| kernel module | test | tolerance |
|---------------|------|-----------|
| simd (mat4, dot, rsqrt, saxpy) | test_simd | 1e-5 |
| dsp (YIQ, FIR, IIR) | test_dsp | 1e-5 |
| postfx (blend, blur, reduce) | test_postfx | 1e-5 |
| fastmath (sin/cos/exp) | test_fastmath | 1e-3 (approximation) |
| raykernel (ray-sphere/AABB) | test_raykernel | 1e-4 |
| fractalkernel (escape-time) | test_fractalkernel | exact* |
| blur (separable Gaussian) | test_blur | 1e-4 |

\* Escape-time fractals are chaotic  -  the kernel and reference must round
identically, so this test is built `-fno-fast-math -ffp-contract=off`. See
`docs/NEON.md`.

## 2. Functional / structural tests

Modules without an "obvious reference" are checked against their defining
behavior:

- **framebuffer / image**  -  write a known gradient, read it back, verify the
  bytes; the from-scratch PNG and GIF encoders are re-parsed by Python (PIL and
  a structural fallback) to prove they're spec-valid.
- **rasterizer**  -  occlusion is order-independent; a near triangle wins over a
  far one; generated meshes have correct counts and unit normals.
- **sdf**  -  the marcher's reported hit distance matches a known analytic plane;
  scenes are non-uniform (surface vs sky).
- **crt**  -  vignette darkens corners; phosphor leaves a trail on a black frame;
  chroma bleeds across a colour edge; presets differ.
- **nbody**  -  a two-body orbit stays bounded; Barnes-Hut acceleration matches
  direct O(N²) within a few percent.
- **fluid**  -  stable under divergent forcing; dye roughly conserved.
- **tui**  -  the diff renderer emits far fewer bytes on an unchanged frame.
- **marching cubes**  -  a sphere field yields vertices on the sphere with
  consistently-oriented unit normals; an empty field yields zero triangles.

## 3. Property-based tests (`test_properties.c`)

Assert *mathematical invariants* over ~20 000 randomized trials each  -  the
gold standard for catching subtle bugs example tests miss:

- dot commutes; cross ⟂ both operands; normalize ⇒ unit length
- identity matrix/quaternion are neutral; `(AB)v == A(Bv)`
- rotations & unit quaternions preserve length
- `mat4_mul_neon == mat4_mul` (cross-implementation agreement)
- YIQ round-trip bounded; clamp output stays in `[0,hi]`
- Perlin bounded in `[-1,1]` and Lipschitz-continuous; fbm finite
- N-body total momentum conserved per step (Barnes-Hut drift < 5 %)

Rust modules carry their own `#[test]` suite (26 tests: colorimetry
round-trips, cloth stays bounded, DLA cluster contiguity + determinism,
attractor boundedness, terrain seamlessness). C++ subsystems each have a
`test_*.cpp`.

## 4. Golden-image regression (`test_golden.c`)

Every scene is rendered deterministically (fixed 96×72 buffer, 24 frames, the
same dt/t sequence the capture tool uses) through the full CRT chain, the
tonemapped output is FNV-1a hashed, and the hash is compared to a stored
baseline in `test/golden.txt`. Any change that alters a scene's pixels  -  a real
regression, or an intended tweak  -  shows up as a mismatch. `make golden` checks;
`make golden-update` rewrites baselines after an intentional visual change. All
all 43 scenes render bit-identically across runs (verified), so the harness has zero
false positives. This is the safety net that lets the engine keep growing
without silently breaking a scene.

## 5. End-to-end integration (`test_integration.c`)

Where the golden test checks each scene's *pixels*, the integration test checks
that the whole headless *pipeline wires together*: it links the entire engine
(all scenes + Rust + C++), runs a sample of scenes through
init→update→render→CRT for several frames asserting every frame is finite and
non-negative, then drives the three encoders and re-parses their output bytes  - 
a structurally valid PNG (signature, IHDR geometry, IEND), a GIF89a
(header + `0x3B` trailer), and a RIFF/WAVE header with the right data size. This
is the test that catches a regression in how the pieces connect (buffer sizes,
byte order, chunk framing) even when every unit still passes alone. `make
integration`.

## 6. Sanitizers

The whole engine is built with AddressSanitizer + UBSan and every scene is run
through `capture --all`. This caught a real use-after-free in the N-body octree
(a `realloc` invalidating a held node pointer). Threaded scenes (raymarch,
mandelbrot, hyperbolic) are additionally checked under ThreadSanitizer  -  the
disjoint-band render pattern is race-free. Re-run after any change that touches
memory ownership  -  see `docs/BUILD.md`.

## Running everything

```sh
make test         # C + NEON: equivalence, functional, property (25 suites)
make rust-test    # 33 Rust #[test]s
make cpp-test     # 4 C++ subsystem tests
make golden       # 46-scene deterministic-render regression
make integration  # end-to-end pipeline + encoder-output validation
make test-all     # all of the above
```

Current tally: **25 C/NEON + 33 Rust + 4 C++ unit suites + 46-scene golden
regression + the end-to-end integration test**, plus the ASan+UBSan sweep, the
ThreadSanitizer check on the render pool, and the visual PNG/GIF captures.
