//! CATHODE Rust compute core.
//!
//! This crate is compiled as a `staticlib` and linked into the C host. Every
//! public symbol is `#[no_mangle] pub extern "C"` and matches a declaration in
//! `include/cathode/rustcore.h` (the frozen ABI contract). No Rust types cross
//! the boundary — only C-layout POD and opaque pointers.
//!
//! Safety model: functions taking raw pointers are `unsafe` internally but
//! present a safe C ABI. We validate lengths, never retain borrowed input
//! buffers, and abort (never unwind) on panic — see `panic = "abort"` in
//! Cargo.toml. A panic crossing into C would be UB.

#![allow(clippy::missing_safety_doc)]

/// C-layout Vec3 mirroring `cathode/types.h`'s `Vec3`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// C-layout Color3 mirroring `cathode/types.h`'s `Color3`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Color3 {
    pub r: f32,
    pub g: f32,
    pub b: f32,
}

pub mod colorimetry;
pub mod reaction;
pub mod cloth;
pub mod attractor;
pub mod terrain;
pub mod dla;
pub mod wfc;
pub mod fft;
pub mod maze;
pub mod lsystem;

/// ABI version handshake. The C host asserts this equals `CATHODE_RUST_ABI`.
#[no_mangle]
pub extern "C" fn rust_core_abi_version() -> u32 {
    7
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn abi_version_matches_header() {
        assert_eq!(rust_core_abi_version(), 7);
    }
}
