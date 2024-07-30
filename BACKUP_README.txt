CATHODE — personal project backup
Exported 2026-07-29 from work laptop before handing it in.

What this is: a CPU-only real-time graphics engine that emulates an analog
NTSC composite signal + CRT tube in software DSP and renders to the terminal
via truecolor Unicode half-blocks. Polyglot: C11 + hand-written AArch64 NEON
assembly + Rust (staticlib) + C++20, linked over a frozen C ABI.

To build and run (macOS / Apple Silicon):
    cd cathode
    ./cathode.sh              # builds, then launches the interactive demo
    ./cathode.sh --list       # all 56 scenes
    ./cathode.sh --check      # verify your terminal supports truecolor
    make test-all             # full cross-language test suite

Requires: Apple clang, Rust toolchain (cargo), a truecolor terminal.
Build artifacts (build/, rustsrc/target/, assets/) were excluded to keep this
small; `make` regenerates them.

Docs are in cathode/docs/ — start with docs/README.md, then ARCHITECTURE.md.
