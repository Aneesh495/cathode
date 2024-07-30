//! Radix-2 iterative Cooley–Tukey FFT (in-place), plus a real->magnitude helper.
//!
//! The transform is the standard decimation-in-time algorithm:
//!   1. bit-reversal permutation of the input,
//!   2. log2(n) stages of butterflies with twiddle factors e^{-2πi k/m}.
//! Iterative (no recursion) so it's allocation-free on the complex buffer and
//! fast. `n` must be a power of two; non-powers are treated as no-ops.
//!
//! Exposed over the C ABI (see rustcore.h): `rust_fft_mag` for the real audio
//! path (Hann-windowed, returns n/2 magnitudes) and `rust_fft_complex` for a
//! general in-place complex transform (forward/inverse).

use core::slice;

const TAU: f32 = core::f32::consts::TAU;

#[inline]
fn is_pow2(n: u32) -> bool {
    n >= 2 && (n & (n - 1)) == 0
}

/// In-place complex FFT on interleaved (re, im) data of length `n` complex
/// values (2*n floats). `inverse` flips the twiddle sign and applies 1/n.
fn fft_inplace(re: &mut [f32], im: &mut [f32], inverse: bool) {
    let n = re.len();
    if n < 2 || (n & (n - 1)) != 0 {
        return;
    }
    // bit-reversal permutation
    let mut j = 0usize;
    for i in 1..n {
        let mut bit = n >> 1;
        while j & bit != 0 {
            j ^= bit;
            bit >>= 1;
        }
        j ^= bit;
        if i < j {
            re.swap(i, j);
            im.swap(i, j);
        }
    }
    // butterflies
    let sign = if inverse { 1.0f32 } else { -1.0f32 };
    let mut len = 2usize;
    while len <= n {
        let ang = sign * TAU / (len as f32);
        let (wr_step, wi_step) = (ang.cos(), ang.sin());
        let half = len / 2;
        let mut i = 0usize;
        while i < n {
            // twiddle starts at 1+0i, multiplied by w each sub-step
            let mut cwr = 1.0f32;
            let mut cwi = 0.0f32;
            for k in 0..half {
                let a = i + k;
                let b = i + k + half;
                let tr = cwr * re[b] - cwi * im[b];
                let ti = cwr * im[b] + cwi * re[b];
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                // advance twiddle: cw *= w_step
                let ncwr = cwr * wr_step - cwi * wi_step;
                cwi = cwr * wi_step + cwi * wr_step;
                cwr = ncwr;
                let _ = k;
            }
            i += len;
        }
        len <<= 1;
    }
    if inverse {
        let inv = 1.0f32 / (n as f32);
        for x in re.iter_mut() {
            *x *= inv;
        }
        for x in im.iter_mut() {
            *x *= inv;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_fft_mag(out_mag: *mut f32, input: *const f32, n: u32) {
    if out_mag.is_null() || input.is_null() || !is_pow2(n) {
        return;
    }
    let nn = n as usize;
    let src = slice::from_raw_parts(input, nn);
    let mut re = vec![0.0f32; nn];
    let mut im = vec![0.0f32; nn];
    // Hann window to cut spectral leakage, matching the old DFT path.
    for i in 0..nn {
        let w = 0.5 - 0.5 * ((TAU * i as f32) / (nn as f32 - 1.0)).cos();
        re[i] = src[i] * w;
    }
    fft_inplace(&mut re, &mut im, false);
    let out = slice::from_raw_parts_mut(out_mag, nn / 2);
    for k in 0..nn / 2 {
        out[k] = (re[k] * re[k] + im[k] * im[k]).sqrt();
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_fft_complex(data: *mut f32, n: u32, inverse: i32) {
    if data.is_null() || !is_pow2(n) {
        return;
    }
    let nn = n as usize;
    let d = slice::from_raw_parts_mut(data, nn * 2);
    // de-interleave, transform, re-interleave (keeps the ABI simple/portable)
    let mut re = vec![0.0f32; nn];
    let mut im = vec![0.0f32; nn];
    for i in 0..nn {
        re[i] = d[2 * i];
        im[i] = d[2 * i + 1];
    }
    fft_inplace(&mut re, &mut im, inverse != 0);
    for i in 0..nn {
        d[2 * i] = re[i];
        d[2 * i + 1] = im[i];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // naive DFT magnitude for cross-checking
    fn dft_mag(input: &[f32]) -> Vec<f32> {
        let n = input.len();
        let mut out = vec![0.0f32; n / 2];
        for k in 0..n / 2 {
            let (mut sr, mut si) = (0.0f32, 0.0f32);
            for (t, &x) in input.iter().enumerate() {
                let w = 0.5 - 0.5 * ((TAU * t as f32) / (n as f32 - 1.0)).cos();
                let ang = -TAU * (k * t) as f32 / n as f32;
                sr += x * w * ang.cos();
                si += x * w * ang.sin();
            }
            out[k] = (sr * sr + si * si).sqrt();
        }
        out
    }

    #[test]
    fn fft_matches_dft() {
        let n = 256usize;
        let mut input = vec![0.0f32; n];
        // a couple of sinusoids
        for i in 0..n {
            let t = i as f32;
            input[i] = (TAU * 8.0 * t / n as f32).sin() + 0.5 * (TAU * 20.0 * t / n as f32).sin();
        }
        let mut fft = vec![0.0f32; n / 2];
        unsafe { rust_fft_mag(fft.as_mut_ptr(), input.as_ptr(), n as u32); }
        let dft = dft_mag(&input);
        let mut worst = 0.0f32;
        for k in 0..n / 2 {
            let d = (fft[k] - dft[k]).abs();
            if d > worst { worst = d; }
        }
        assert!(worst < 1e-2, "FFT vs DFT worst abs diff {worst}");
    }

    #[test]
    fn fft_peak_at_input_frequency() {
        // a pure tone at bin 10 should peak the magnitude spectrum at bin 10
        let n = 512usize;
        let mut input = vec![0.0f32; n];
        for i in 0..n {
            input[i] = (TAU * 10.0 * i as f32 / n as f32).sin();
        }
        let mut fft = vec![0.0f32; n / 2];
        unsafe { rust_fft_mag(fft.as_mut_ptr(), input.as_ptr(), n as u32); }
        let mut best = 0usize;
        for k in 1..n / 2 {
            if fft[k] > fft[best] { best = k; }
        }
        assert!((best as i32 - 10).abs() <= 1, "peak at bin {best}, expected ~10");
    }

    #[test]
    fn forward_inverse_roundtrip() {
        let n = 128usize;
        let mut data = vec![0.0f32; n * 2];
        let orig: Vec<f32> = (0..n * 2).map(|i| ((i * 7 % 13) as f32) - 6.0).collect();
        data.copy_from_slice(&orig);
        unsafe {
            rust_fft_complex(data.as_mut_ptr(), n as u32, 0);
            rust_fft_complex(data.as_mut_ptr(), n as u32, 1);
        }
        for i in 0..n * 2 {
            assert!((data[i] - orig[i]).abs() < 1e-3, "roundtrip at {i}: {} vs {}", data[i], orig[i]);
        }
    }

    #[test]
    fn rejects_non_power_of_two() {
        // n=100 is not a power of two -> no-op, out stays zero
        let input = vec![1.0f32; 100];
        let mut out = vec![7.0f32; 50];
        unsafe { rust_fft_mag(out.as_mut_ptr(), input.as_ptr(), 100); }
        assert!(out.iter().all(|&v| v == 7.0), "non-pow2 must be a no-op");
    }
}
