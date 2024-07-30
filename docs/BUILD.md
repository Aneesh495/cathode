# CATHODE — Build system

Polyglot build: C + hand-written AArch64 assembly + a Rust staticlib + C++20,
all linked into two binaries. Driven by a single `Makefile`.

## Prerequisites

- Apple clang / clang++ (arm64-darwin) — the reference toolchain.
- `cargo` / rustc (the Rust core is a `staticlib`).
- A truecolor terminal for the interactive binary. Everything else (tests,
  PNG/GIF capture) is headless.
- No third-party libraries. The PNG and GIF encoders, the DSP, the physics —
  all from scratch. Only libc / libm / libc++ / pthreads and the Rust std.

## Targets

| command | what it does |
|---------|--------------|
| `make` / `make all` | build the Rust staticlib, then `bin/cathode` (interactive) and `bin/capture` (headless) |
| `make test` | build & run the C + NEON unit/property test suite |
| `make rust-test` | `cargo test --release` for the Rust core |
| `make cpp-test` | compile & run the C++ subsystem tests |
| `make test-all` | all three suites |
| `make bench` | NEON-vs-C microbenchmarks |
| `make run` | build & launch the interactive demo |
| `make capture` | render every scene to a PNG contact sheet in `assets/` |
| `make clean` | remove `build/` |

## How the languages link

```
   rustsrc/  --cargo-->  libcathode_rustcore.a  (staticlib, panic=abort)
   src/*.c   --clang-->  build/**/*.o
   src/*.s   --clang-->  build/asm/*.o           (integrated assembler)
   src/cpp/  --clang++-> build/cpp/*.o
                                   |
   final link (clang++):  all .o  +  libcathode_rustcore.a  ->  bin/{cathode,capture}
```

The final link uses `clang++` so the C++ runtime and `libc++` are pulled in.
The Rust staticlib is only rebuilt when a `rustsrc/src/*.rs` file or `Cargo.toml`
changes (a Make prerequisite on the `.a`).

## Flags

- C: `-O3 -std=c11 -ffast-math -fno-math-errno` plus include paths and the
  `-DCATHODE_HAVE_POLYGLOT_SCENES=1` define that enables the Rust/C++-backed
  scenes.
- C++: `-O3 -std=c++20 -ffast-math`.
- Assembly: assembled by clang with `-Iinclude` (no separate assembler).
- **Exception:** `test_fractalkernel` builds with `-fno-fast-math
  -ffp-contract=off`. Escape-time fractals are chaotic; the NEON kernel and its
  C reference must round identically or boundary pixels' iteration counts
  diverge. See `docs/NEON.md`.

## Adding a source file

- A new `src/**/*.c` in one of the source groups (`CORE_SRC`, `REND_SRC`, …) is
  picked up automatically; `src/asm/*.s` and `src/cpp/*.cpp` are globbed.
- A new **test** goes in the `TESTS` list with a `test_<name>_OBJ` variable
  naming its dependencies; add `test_<name>_CFLAGS` if it needs special flags.
- A new **Rust module**: add `pub mod <name>;` to `rustsrc/src/lib.rs`, bump
  `CATHODE_RUST_ABI` in `rustcore.h` and the matching constant in `lib.rs` if
  you changed the ABI surface.

## Verification you should run before declaring done

1. `make clean && make all` — clean polyglot build, no errors.
2. `make test-all` — every suite green.
3. Sanitizers: build `capture` with `-fsanitize=address,undefined` and run
   `--all` over every scene (this is how the octree use-after-free was caught).
4. `make capture` and eyeball the PNGs — the ultimate "does it actually look
   right" check.
