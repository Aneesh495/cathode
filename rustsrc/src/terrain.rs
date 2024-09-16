//! Terrain  -  deterministic multi-fractal value-noise heightfield sampler.
//!
//! Implements `rust_terrain_tile` from `include/cathode/rustcore.h`. Given a
//! tile origin `(ox, oy)`, a world-space `scale`, and a grid resolution
//! `nx * ny`, we sample a fractional-Brownian-motion (fBm) stack of value
//! noise at each cell's world coordinate and write the resulting heights.
//!
//! Design goals dictated by the contract:
//!   * **Deterministic**  -  height is a pure function of `(world_x, world_y,
//!     octaves, seed)`. Same inputs always yield the same bits.
//!   * **Seamless**  -  because sampling is keyed on absolute *world*
//!     coordinates (not tile-local indices), two adjacent tiles that share a
//!     world coordinate produce identical heights there. No seams.
//!   * **Continuous**  -  value noise uses a smoothstep (Hermite) fade, so the
//!     field and its first derivative are continuous across lattice cells.
//!   * **Large-coordinate safe**  -  all sampling math is `f64`; only the final
//!     height is down-cast to `f32`, so precision holds far from the origin.

use core::slice;

/// fBm spectral parameters fixed by the specification.
const LACUNARITY: f64 = 2.0; // frequency multiplier per octave
const GAIN: f64 = 0.5; // amplitude multiplier per octave

/// Upper bound on octaves. With `GAIN = 0.5` the amplitude of octave 24 is
/// 2^-24 (~6e-8), already negligible against an f32 mantissa, so clamping here
/// keeps the loop finite and bounded without visibly changing the result.
const MAX_OCTAVES: i32 = 24;

/// SplitMix64-style integer hash of a 2D integer lattice point plus a seed.
///
/// Combines the two lattice coordinates with large odd (prime-like) constants
/// so that neighbouring points decorrelate, then runs the SplitMix64 finalizer
/// for good avalanche. Returns a well-mixed 64-bit value.
#[inline]
fn hash2(ix: i64, iy: i64, seed: u64) -> u64 {
    // Odd constants derived from the golden ratio / fractional bits of primes.
    let mut h = seed;
    h ^= (ix as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    h = h.rotate_left(29);
    h ^= (iy as u64).wrapping_mul(0xC2B2_AE3D_27D4_EB4F);
    // SplitMix64 finalizer.
    h ^= h >> 30;
    h = h.wrapping_mul(0xBF58_476D_1CE4_E5B9);
    h ^= h >> 27;
    h = h.wrapping_mul(0x94D0_49BB_1331_11EB);
    h ^= h >> 31;
    h
}

/// Map a lattice point to a pseudo-random value in `[-1, 1]`.
///
/// Uses the top 53 bits of the hash to build a uniform `f64` in `[0, 1)` (full
/// mantissa precision), then rescales to `[-1, 1)`.
#[inline]
fn lattice_value(ix: i64, iy: i64, seed: u64) -> f64 {
    let h = hash2(ix, iy, seed);
    let unit = (h >> 11) as f64 / ((1u64 << 53) as f64); // [0, 1)
    unit * 2.0 - 1.0
}

/// Smoothstep / Hermite fade `t*t*(3 - 2t)`. Zero first derivative at both
/// endpoints, which is what makes the interpolated field C1-continuous across
/// lattice-cell boundaries (and hence seam- and crease-free).
#[inline]
fn smoothstep(t: f64) -> f64 {
    t * t * (3.0 - 2.0 * t)
}

/// A single octave of 2D value noise, output in `[-1, 1]`.
///
/// Floors to the containing lattice cell, hashes its four corners, and
/// bilinearly interpolates them with smoothstep-faded weights.
#[inline]
fn value_noise(x: f64, y: f64, seed: u64) -> f64 {
    // `floor` rounds toward -inf, so this is correct for negative coordinates.
    let x0 = x.floor();
    let y0 = y.floor();
    let ix = x0 as i64;
    let iy = y0 as i64;

    // Fractional position within the cell, both in [0, 1).
    let tx = x - x0;
    let ty = y - y0;

    // Four corner values.
    let v00 = lattice_value(ix, iy, seed);
    let v10 = lattice_value(ix + 1, iy, seed);
    let v01 = lattice_value(ix, iy + 1, seed);
    let v11 = lattice_value(ix + 1, iy + 1, seed);

    // Faded interpolation weights.
    let u = smoothstep(tx);
    let v = smoothstep(ty);

    // Bilinear blend: lerp along x on both rows, then lerp along y.
    let a = v00 + u * (v10 - v00);
    let b = v01 + u * (v11 - v01);
    a + v * (b - a)
}

/// Fractional-Brownian-motion height at a world coordinate.
///
/// Sums `octaves` of value noise with frequency scaled by `LACUNARITY` and
/// amplitude by `GAIN` per octave, then normalises by the total amplitude so
/// the result always lands in `[-1, 1]` regardless of octave count. Each octave
/// perturbs the seed so successive layers are statistically independent.
#[inline]
fn fbm(wx: f64, wy: f64, octaves: i32, seed: u64) -> f64 {
    let oct = octaves.clamp(0, MAX_OCTAVES);
    let mut freq = 1.0_f64;
    let mut amp = 1.0_f64;
    let mut sum = 0.0_f64;
    let mut norm = 0.0_f64;
    for o in 0..oct {
        // Decorrelate octaves deterministically by folding the octave index
        // into the seed with a large odd multiplier.
        let oseed = seed ^ (o as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
        sum += amp * value_noise(wx * freq, wy * freq, oseed);
        norm += amp;
        freq *= LACUNARITY;
        amp *= GAIN;
    }
    if norm > 0.0 {
        sum / norm
    } else {
        0.0 // octaves == 0: flat field
    }
}

/// Fill `out` (`nx * ny` heights, row-major with `i` fastest) with the terrain
/// tile whose top-left cell sits at world `(ox, oy)`. Cell `(i, j)` samples the
/// world coordinate `(ox + i*scale/nx, oy + j*scale/ny)`.
///
/// # Safety
/// `out` must point to storage for at least `nx * ny` `f32` values.
#[no_mangle]
pub unsafe extern "C" fn rust_terrain_tile(
    out: *mut f32,
    nx: i32,
    ny: i32,
    ox: f64,
    oy: f64,
    scale: f64,
    octaves: i32,
    seed: u64,
) {
    if out.is_null() || nx <= 0 || ny <= 0 {
        return;
    }
    let nxu = nx as usize;
    let nyu = ny as usize;
    let dst = slice::from_raw_parts_mut(out, nxu * nyu);

    // World-space distance between adjacent cells along each axis. Keeping this
    // as a single division per axis (rather than per cell) preserves the exact
    // `ox + i*(scale/nx)` mapping the header specifies and keeps tiles seamless.
    let dx = scale / nx as f64;
    let dy = scale / ny as f64;

    for j in 0..nyu {
        let wy = oy + (j as f64) * dy;
        let row = j * nxu;
        for i in 0..nxu {
            let wx = ox + (i as f64) * dx;
            dst[row + i] = fbm(wx, wy, octaves, seed) as f32;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Same seed + same coordinates must yield bit-identical heights.
    #[test]
    fn determinism() {
        let (nx, ny) = (32, 24);
        let mut a = vec![0.0f32; (nx * ny) as usize];
        let mut b = vec![0.0f32; (nx * ny) as usize];
        unsafe {
            rust_terrain_tile(a.as_mut_ptr(), nx, ny, 12.0, -7.0, 5.0, 5, 0xDEAD_BEEF);
            rust_terrain_tile(b.as_mut_ptr(), nx, ny, 12.0, -7.0, 5.0, 5, 0xDEAD_BEEF);
        }
        assert_eq!(a, b, "identical inputs must produce identical output");

        // A different seed should generally change the field.
        let mut c = vec![0.0f32; (nx * ny) as usize];
        unsafe {
            rust_terrain_tile(c.as_mut_ptr(), nx, ny, 12.0, -7.0, 5.0, 5, 0x1234_5678);
        }
        assert_ne!(a, c, "different seed should change the field");
    }

    /// All heights must be finite and within the normalised `[-1, 1]` range.
    #[test]
    fn finite_and_bounded() {
        let (nx, ny) = (48, 48);
        let mut h = vec![0.0f32; (nx * ny) as usize];
        unsafe {
            rust_terrain_tile(h.as_mut_ptr(), nx, ny, 100.0, 200.0, 16.0, 6, 42);
        }
        for &v in &h {
            assert!(v.is_finite(), "non-finite height");
            assert!(v.abs() <= 1.0 + 1e-6, "height out of range: {}", v);
        }
    }

    /// Continuity: with fine sampling, neighbouring cells differ by only a
    /// small, bounded amount (the field is C1, so no jumps).
    #[test]
    fn continuity() {
        // Fine spacing: scale/nx = 4/64 = 0.0625 world units per cell.
        let (nx, ny) = (64, 64);
        let mut h = vec![0.0f32; (nx * ny) as usize];
        let octaves = 4;
        unsafe {
            rust_terrain_tile(h.as_mut_ptr(), nx, ny, -3.0, 9.0, 4.0, octaves, 7);
        }
        let at = |i: usize, j: usize| h[j * nx as usize + i];
        let mut max_diff = 0.0f32;
        for j in 0..ny as usize {
            for i in 0..nx as usize {
                if i + 1 < nx as usize {
                    max_diff = max_diff.max((at(i, j) - at(i + 1, j)).abs());
                }
                if j + 1 < ny as usize {
                    max_diff = max_diff.max((at(i, j) - at(i, j + 1)).abs());
                }
            }
        }
        // Theoretical worst-case gradient for this config is well under this
        // bound; a real seam/discontinuity would blow past it.
        assert!(max_diff < 1.0, "field not continuous, max step {}", max_diff);
        assert!(max_diff > 0.0, "field is suspiciously flat");
    }

    /// Seamlessness: two tiles that overlap in world space must agree exactly
    /// on their shared coordinates. We give both tiles the same cell spacing
    /// (`scale/nx`) and offset tile B's origin by an integer number of cells,
    /// so B's left cells land on the same world points as A's right cells.
    #[test]
    fn adjacent_tiles_seamless() {
        let (nx, ny) = (8, 8);
        let scale = 8.0; // spacing = scale/nx = 1.0 world unit per cell
        let octaves = 5;
        let seed = 0xABCD_1234;

        let mut a = vec![0.0f32; (nx * ny) as usize];
        let mut b = vec![0.0f32; (nx * ny) as usize];
        unsafe {
            // Tile A covers world x in {0..7}.
            rust_terrain_tile(a.as_mut_ptr(), nx, ny, 0.0, 0.0, scale, octaves, seed);
            // Tile B is shifted +4 cells: covers world x in {4..11}.
            rust_terrain_tile(b.as_mut_ptr(), nx, ny, 4.0, 0.0, scale, octaves, seed);
        }
        let w = nx as usize;
        // Overlap: A columns 4..7 share world coords with B columns 0..3.
        for j in 0..ny as usize {
            for k in 0..4usize {
                let av = a[j * w + (4 + k)];
                let bv = b[j * w + k];
                assert_eq!(av, bv, "seam mismatch at row {} overlap col {}", j, k);
            }
        }
    }

    /// Degenerate inputs must be handled without panicking or writing.
    #[test]
    fn degenerate_inputs() {
        let mut h = vec![9.0f32; 4];
        unsafe {
            // nx <= 0: nothing written, buffer untouched.
            rust_terrain_tile(h.as_mut_ptr(), 0, 2, 0.0, 0.0, 1.0, 3, 1);
            rust_terrain_tile(core::ptr::null_mut(), 2, 2, 0.0, 0.0, 1.0, 3, 1);
        }
        assert_eq!(h, vec![9.0f32; 4]);

        // octaves == 0: flat (zero) field, still finite.
        let mut z = vec![7.0f32; 4];
        unsafe {
            rust_terrain_tile(z.as_mut_ptr(), 2, 2, 0.0, 0.0, 1.0, 0, 1);
        }
        for &v in &z {
            assert_eq!(v, 0.0);
        }
    }
}
