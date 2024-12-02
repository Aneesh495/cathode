# CATHODE documentation

Start with the top-level [`../README.md`](../README.md) for the project overview.
The docs here go deeper, one concern per file:

| Doc | What it covers |
|-----|----------------|
| [ARCHITECTURE.md](ARCHITECTURE.md) | The layered design, the frozen-header contract, the module map, and the scene → framebuffer → CRT → terminal pipeline. |
| [ABI.md](ABI.md) | The C ABI boundaries: how Rust (`rustcore.h`) and C++ (`cppcore.h`) subsystems link into the C host, opaque handles, version handshakes. |
| [NEON.md](NEON.md) | The hand-written AArch64 assembly: the Apple/Mach-O ABI traps (callee-saved lanes, arg banks, `fmla` scalar forms), and why fractal kernels need strict FP. |
| [BENCHMARKS.md](BENCHMARKS.md) | Performance gates: ~6.7× `mat4_mul`, ~330 MS/s I/Q DSP, &lt;0.01% NRMSE / 13K+ vectors, resolution scaling; plus honest kernel ties/losses. |
| [SCENES.md](SCENES.md) | The 50+ scene catalog (56 registered) and authoring guide (vtable, checklist, CRT presets, threading). |
| [AUDIO.md](AUDIO.md) | The audio stack: synth → tracker (with effects) → WAV encoder → FFT, and the `chiptune` capstone scene. |
| [TESTING.md](TESTING.md) | Equivalence, DSP NRMSE, property, golden-image, integration, sanitizers, and regression suite. |
| [BUILD.md](BUILD.md) | How to build and run; the polyglot link; `AUDIO=1`; sanitizer builds. |
| [CHANGELOG.md](CHANGELOG.md) | A running log of the major additions. |
