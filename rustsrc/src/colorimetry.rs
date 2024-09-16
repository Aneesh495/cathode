//! High-precision NTSC colorimetry and tone mapping.
//!
//! These `f64` reference implementations are the ground truth the `f32` NEON
//! DSP kernels are validated against, and they generate the exact same YIQ
//! matrix the C reference (`src/core/dsp_ref.c`) uses so all three agree.
//!
//! Layout: `rgb`/`yiq` are interleaved, 3 components per pixel, `n` pixels.

use core::slice;

// FCC/NTSC RGB<->YIQ matrices (same constants as src/core/dsp_ref.c).
const RGB2YIQ: [[f64; 3]; 3] = [
    [0.299_000, 0.587_000, 0.114_000],   // Y
    [0.595_716, -0.274_453, -0.321_263], // I
    [0.211_456, -0.522_591, 0.311_135],  // Q
];
const YIQ2RGB: [[f64; 3]; 3] = [
    [1.0, 0.956_000, 0.621_000],  // R
    [1.0, -0.272_000, -0.647_000], // G
    [1.0, -1.107_000, 1.704_600], // B
];

#[inline]
fn matmul3(m: &[[f64; 3]; 3], a: f64, b: f64, c: f64) -> (f64, f64, f64) {
    (
        m[0][0] * a + m[0][1] * b + m[0][2] * c,
        m[1][0] * a + m[1][1] * b + m[1][2] * c,
        m[2][0] * a + m[2][1] * b + m[2][2] * c,
    )
}

/// RGB -> YIQ, f64, interleaved, `n` pixels.
#[no_mangle]
pub unsafe extern "C" fn rust_rgb2yiq_f64(out: *mut f64, rgb: *const f64, n: u64) {
    if out.is_null() || rgb.is_null() || n == 0 {
        return;
    }
    let n = n as usize;
    let src = slice::from_raw_parts(rgb, n * 3);
    let dst = slice::from_raw_parts_mut(out, n * 3);
    for i in 0..n {
        let (y, iq, q) = matmul3(&RGB2YIQ, src[3 * i], src[3 * i + 1], src[3 * i + 2]);
        dst[3 * i] = y;
        dst[3 * i + 1] = iq;
        dst[3 * i + 2] = q;
    }
}

/// YIQ -> RGB, f64, interleaved, `n` pixels.
#[no_mangle]
pub unsafe extern "C" fn rust_yiq2rgb_f64(out: *mut f64, yiq: *const f64, n: u64) {
    if out.is_null() || yiq.is_null() || n == 0 {
        return;
    }
    let n = n as usize;
    let src = slice::from_raw_parts(yiq, n * 3);
    let dst = slice::from_raw_parts_mut(out, n * 3);
    for i in 0..n {
        let (r, g, b) = matmul3(&YIQ2RGB, src[3 * i], src[3 * i + 1], src[3 * i + 2]);
        dst[3 * i] = r;
        dst[3 * i + 1] = g;
        dst[3 * i + 2] = b;
    }
}

/// sRGB electro-optical transfer (gamma-encoded -> linear), single channel.
#[inline]
fn srgb_to_linear_scalar(c: f32) -> f32 {
    if c <= 0.040_45 {
        c / 12.92
    } else {
        ((c + 0.055) / 1.055).powf(2.4)
    }
}
#[inline]
fn linear_to_srgb_scalar(c: f32) -> f32 {
    if c <= 0.003_130_8 {
        12.92 * c
    } else {
        1.055 * c.powf(1.0 / 2.4) - 0.055
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_srgb_to_linear(out: *mut f32, inp: *const f32, n: u64) {
    if out.is_null() || inp.is_null() || n == 0 {
        return;
    }
    let n = n as usize;
    let s = slice::from_raw_parts(inp, n);
    let d = slice::from_raw_parts_mut(out, n);
    for i in 0..n {
        d[i] = srgb_to_linear_scalar(s[i]);
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_linear_to_srgb(out: *mut f32, inp: *const f32, n: u64) {
    if out.is_null() || inp.is_null() || n == 0 {
        return;
    }
    let n = n as usize;
    let s = slice::from_raw_parts(inp, n);
    let d = slice::from_raw_parts_mut(out, n);
    for i in 0..n {
        d[i] = linear_to_srgb_scalar(s[i]);
    }
}

/// Reinhard-Jodie tonemap + sRGB encode of a whole frame, linear RGB -> u8.
/// Reinhard-Jodie preserves saturation better than per-channel Reinhard by
/// mixing a luminance-based and a per-channel Reinhard result.
#[no_mangle]
pub unsafe extern "C" fn rust_tonemap_frame(out8: *mut u8, lin_rgb: *const f32, npx: u64, exposure: f32) {
    if out8.is_null() || lin_rgb.is_null() || npx == 0 {
        return;
    }
    let n = npx as usize;
    let src = slice::from_raw_parts(lin_rgb, n * 3);
    let dst = slice::from_raw_parts_mut(out8, n * 3);
    for i in 0..n {
        let r = (src[3 * i] * exposure).max(0.0);
        let g = (src[3 * i + 1] * exposure).max(0.0);
        let b = (src[3 * i + 2] * exposure).max(0.0);
        let l = 0.2126 * r + 0.7152 * g + 0.0722 * b;
        let ld = l / (1.0 + l);
        // per-channel reinhard
        let (rc, gc, bc) = (r / (1.0 + r), g / (1.0 + g), b / (1.0 + b));
        // Jodie mix
        let mix = |c: f32, cc: f32| -> f32 {
            let base = if l > 1e-6 { c * ld / l } else { 0.0 };
            base * (1.0 - ld) + cc * ld
        };
        let rr = linear_to_srgb_scalar(mix(r, rc).clamp(0.0, 1.0));
        let gg = linear_to_srgb_scalar(mix(g, gc).clamp(0.0, 1.0));
        let bb = linear_to_srgb_scalar(mix(b, bc).clamp(0.0, 1.0));
        dst[3 * i] = (rr * 255.0 + 0.5) as u8;
        dst[3 * i + 1] = (gg * 255.0 + 0.5) as u8;
        dst[3 * i + 2] = (bb * 255.0 + 0.5) as u8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn yiq_roundtrip_within_published_constant_error() {
        // The canonical NTSC RGB2YIQ / YIQ2RGB matrices are the standard
        // *rounded published* constants (matching src/core/dsp_ref.c and the
        // NEON path), not exact numerical inverses. So a roundtrip carries a
        // small, bounded error (~1e-3). We assert that bound rather than exact
        // identity  -  the whole point is that all three impls share these exact
        // constants and therefore agree with each other.
        let rgb = [0.2f64, 0.5, 0.8, 0.9, 0.1, 0.3];
        let mut yiq = [0.0f64; 6];
        let mut back = [0.0f64; 6];
        unsafe {
            rust_rgb2yiq_f64(yiq.as_mut_ptr(), rgb.as_ptr(), 2);
            rust_yiq2rgb_f64(back.as_mut_ptr(), yiq.as_ptr(), 2);
        }
        for i in 0..6 {
            assert!((rgb[i] - back[i]).abs() < 2e-3, "roundtrip {} {} {}", i, rgb[i], back[i]);
        }
    }

    #[test]
    fn srgb_transfer_roundtrip() {
        let inp: Vec<f32> = (0..=100).map(|i| i as f32 / 100.0).collect();
        let mut lin = vec![0.0f32; inp.len()];
        let mut back = vec![0.0f32; inp.len()];
        unsafe {
            rust_srgb_to_linear(lin.as_mut_ptr(), inp.as_ptr(), inp.len() as u64);
            rust_linear_to_srgb(back.as_mut_ptr(), lin.as_ptr(), lin.len() as u64);
        }
        for i in 0..inp.len() {
            assert!((inp[i] - back[i]).abs() < 1e-4, "srgb roundtrip at {}", i);
        }
    }

    #[test]
    fn known_yiq_white() {
        // pure white RGB(1,1,1) -> Y=1, I=0, Q=0
        let rgb = [1.0f64, 1.0, 1.0];
        let mut yiq = [0.0f64; 3];
        unsafe { rust_rgb2yiq_f64(yiq.as_mut_ptr(), rgb.as_ptr(), 1); }
        assert!((yiq[0] - 1.0).abs() < 1e-9);
        assert!(yiq[1].abs() < 1e-9);
        assert!(yiq[2].abs() < 1e-9);
    }
}
