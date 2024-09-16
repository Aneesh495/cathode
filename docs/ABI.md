# CATHODE  -  The polyglot ABI boundary

CATHODE is C, Rust, and C++ linked into one binary. They talk over a **C ABI**.
This document is the contract's rulebook: follow it and cross-language calls are
safe; violate it and you get silent corruption or UB.

## The golden rule

**Only C-layout POD and opaque pointers cross the boundary.** No Rust enums with
data, no `String`/`Vec`/`std::vector`/`std::string`, no C++ classes by value, no
exceptions, no Rust panics. The boundary types are exactly those in
`include/cathode/types.h` (`u8..f64`, `Vec3`, `Vec4`, `Mat4`, `Color3`) plus raw
pointers and opaque handle structs.

## Where the contracts live

| Boundary | Header | Implemented by |
|----------|--------|----------------|
| C ↔ Rust | `include/cathode/rustcore.h` | `rustsrc/src/*.rs` |
| C ↔ C++  | `include/cathode/cppcore.h`  | `src/cpp/*.cpp` |
| C ↔ asm  | `simd.h`, `dsp.h`, `fastmath.h`, `raykernel.h` | `src/asm/*.s` |

Each side must match the header **exactly**: name, parameter types, order, and
return type. An ABI-version handshake (`rust_core_abi_version()`,
`cpp_core_abi_version()`) lets the host assert at startup that the linked
library matches the header it was compiled against.

## Rust side rules

- Crate is `crate-type = ["staticlib"]`, built `panic = "abort"` (a panic
  crossing into C is UB; aborting is the safe failure mode).
- Every exported function is `#[no_mangle] pub extern "C"`.
- Mirror C structs with `#[repr(C)]` (see `Vec3`, `Color3` in `lib.rs`).
- Functions taking raw pointers are `unsafe` internally; validate lengths, treat
  input pointers as **borrowed for the call only** (never store them), and
  null-check.
- Ownership: anything returned by a `*_create()` is Rust-owned; free it only via
  the matching `*_destroy()`. Never `free()` it from C, never `malloc` something
  in C and free it in Rust.

## C++ side rules

- Every boundary function is `extern "C"` and returns POD / opaque `T*`.
- Use classes, templates, RAII, and the STL freely *inside*; none of it leaks
  through the boundary.
- **No exception may escape an `extern "C"` function.** Wrap bodies that can
  throw and convert to an error return / null.
- Opaque handles: `struct CppTracer;` in the header is a forward declaration;
  the real class is defined in the `.cpp`. `create` does `new`, `destroy` does
  `delete`.
- Link the final binary with the C++ driver (`clang++`) so the C++ runtime and
  `libc++` are pulled in.

## Assembly side rules

See `docs/NEON.md` for the full AArch64 Apple ABI notes. The short version:
integer/pointer args in x0-x7, float/SIMD in v0-v7 (**separate banks**  -  a
`size_t n` after three `float` params is still the next *integer* register), a
float result in s0, v8-v15 callee-saved, save the link register before `bl`.
Every asm routine has a C reference and a test proving bit-for-bit agreement.

## Memory ownership cheat-sheet

| Pattern | Who allocates | Who frees |
|---------|---------------|-----------|
| `X *x = foo_create(...)` | Rust/C++ | `foo_destroy(x)` (same side) |
| `void f(const T *in, u64 n)` | caller (C) | caller; callee only borrows |
| `void f(T *out, u64 n)` | caller (C) | caller; callee only writes |

When in doubt: the side that created a resource frees it, and buffers passed by
pointer are owned by the caller for the whole call and not retained.
