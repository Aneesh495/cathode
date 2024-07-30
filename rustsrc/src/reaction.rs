//! Gray-Scott reaction-diffusion.
//!
//! Two virtual chemicals `U` and `V` diffuse across a toroidal (wrap-around)
//! grid while reacting via the autocatalytic rule `U + 2V -> 3V`. Depending on
//! the feed/kill rates the system self-organizes into spots, stripes, mazes,
//! and mitosis-like patterns — the classic Pearson parameter zoo.
//!
//! Discrete update per cell, with `dt = 1.0` and `substeps` iterations per
//! [`rust_rd_step`] call:
//!
//! ```text
//! u' = u + (Du * lap(U) - u*v*v + feed*(1 - u)) * dt
//! v' = v + (Dv * lap(V) + u*v*v - (feed + kill)*v) * dt
//! ```
//!
//! The Laplacian uses the standard 9-point stencil (orthogonal weight 0.2,
//! diagonal weight 0.05, center -1; the weights sum to zero so a flat field
//! has zero Laplacian) with toroidal wrapping at every edge.
//!
//! This module exposes the `rust_rd_*` C ABI declared in
//! `include/cathode/rustcore.h`. `RustReactionDiffusion` is opaque to C: it is
//! heap-allocated by [`rust_rd_create`] (via `Box`) and must be released with
//! [`rust_rd_destroy`]. No Rust types cross the FFI boundary.

use core::slice;

/// Fixed integration time-step. The Gray-Scott parameter presets and the
/// diffusion weights below are all tuned for `dt = 1.0`.
const DT: f32 = 1.0;

/// Orthogonal (edge) neighbor weight for the 9-point Laplacian stencil.
const W_ORTHO: f32 = 0.2;
/// Diagonal (corner) neighbor weight for the 9-point Laplacian stencil.
const W_DIAG: f32 = 0.05;

/// A minimal `splitmix64` PRNG. Deterministic, dependency-free, excellent
/// avalanche — ideal for seeding reproducible random blobs. Same seed always
/// produces the same stream, which the determinism test relies on.
struct SplitMix64 {
    state: u64,
}

impl SplitMix64 {
    #[inline]
    fn new(seed: u64) -> Self {
        Self { state: seed }
    }

    /// Advance the generator and return the next 64-bit value.
    #[inline]
    fn next_u64(&mut self) -> u64 {
        // Golden-ratio odd increment, then the canonical splitmix64 finalizer.
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    /// Uniform `f32` in `[0, 1)` using the top 24 bits (the f32 mantissa width).
    #[inline]
    fn next_f32(&mut self) -> f32 {
        (self.next_u64() >> 40) as f32 / (1u32 << 24) as f32
    }

    /// Uniform integer in `[0, n)` for `n > 0` (simple modulo; bias is
    /// negligible for the small grid dimensions used here).
    #[inline]
    fn next_usize(&mut self, n: usize) -> usize {
        (self.next_u64() % n as u64) as usize
    }
}

/// Opaque Gray-Scott simulation state. Holds the two chemical fields plus a
/// pair of double-buffer scratch fields written during a step and then swapped
/// in (this avoids reading half-updated data mid-sweep).
pub struct RustReactionDiffusion {
    w: usize,
    h: usize,
    /// Chemical U concentration, row-major `w*h`. Default 1.0 everywhere.
    u: Vec<f32>,
    /// Chemical V concentration, row-major `w*h`. Default 0.0 everywhere.
    v: Vec<f32>,
    /// Scratch destination for U during a step (swapped with `u` after).
    u_next: Vec<f32>,
    /// Scratch destination for V during a step (swapped with `v` after).
    v_next: Vec<f32>,
    feed: f32,
    kill: f32,
    du: f32,
    dv: f32,
}

impl RustReactionDiffusion {
    /// Allocate a grid initialized to the resting state (U = 1, V = 0).
    fn new(w: usize, h: usize, feed: f32, kill: f32, du: f32, dv: f32) -> Self {
        let n = w * h;
        Self {
            w,
            h,
            u: vec![1.0; n],
            v: vec![0.0; n],
            u_next: vec![1.0; n],
            v_next: vec![0.0; n],
            feed,
            kill,
            du,
            dv,
        }
    }

    /// Reset both fields to the resting state (U = 1, V = 0) everywhere.
    fn reset(&mut self) {
        for x in self.u.iter_mut() {
            *x = 1.0;
        }
        for x in self.v.iter_mut() {
            *x = 0.0;
        }
    }

    /// Stamp a filled disk of radius `r` centered at `(cx, cy)`, setting V = 1
    /// and U = 0 inside it. Coordinates wrap toroidally so a seed near an edge
    /// still produces a whole disk.
    fn stamp_disk(&mut self, cx: isize, cy: isize, r: isize) {
        if r < 0 {
            return;
        }
        let w = self.w as isize;
        let h = self.h as isize;
        let r2 = r * r;
        for dy in -r..=r {
            for dx in -r..=r {
                if dx * dx + dy * dy > r2 {
                    continue; // outside the disk
                }
                // rem_euclid gives a non-negative wrapped index.
                let x = (cx + dx).rem_euclid(w) as usize;
                let y = (cy + dy).rem_euclid(h) as usize;
                let idx = y * self.w + x;
                self.v[idx] = 1.0;
                self.u[idx] = 0.0;
            }
        }
    }

    /// Fill V with pseudo-random blobs. `density` in `[0, 1]` scales how much of
    /// the grid gets seeded. Deterministic for a given `seed`.
    fn seed_random(&mut self, seed: u64, density: f32) {
        self.reset();
        let density = density.clamp(0.0, 1.0);
        if density <= 0.0 {
            return;
        }
        let area = self.w * self.h;
        // Each blob covers on the order of a few dozen cells; choose a blob
        // count so total coverage tracks `density`. Bounded so the loop is
        // always finite regardless of grid size.
        let n_blobs = (((area as f32) * density) / 24.0).round() as usize;
        let n_blobs = n_blobs.clamp(1, area.max(1)).min(1 << 16);
        let mut rng = SplitMix64::new(seed);
        for _ in 0..n_blobs {
            let cx = rng.next_usize(self.w) as isize;
            let cy = rng.next_usize(self.h) as isize;
            // Radius 1..=3 gives compact seeds that then grow via the reaction.
            let r = 1 + (rng.next_f32() * 3.0) as isize;
            self.stamp_disk(cx, cy, r);
        }
    }

    /// Advance the simulation by a single time-step using the double buffer.
    fn step_once(&mut self) {
        let w = self.w;
        let h = self.h;
        // Disjoint field borrows: read `u`/`v`, write `u_next`/`v_next`.
        let u = &self.u;
        let v = &self.v;
        let un = &mut self.u_next;
        let vn = &mut self.v_next;
        let (feed, kill, du, dv) = (self.feed, self.kill, self.du, self.dv);

        for y in 0..h {
            // Wrapped row offsets (toroidal top/bottom edges).
            let ym = if y == 0 { h - 1 } else { y - 1 };
            let yp = if y == h - 1 { 0 } else { y + 1 };
            let row = y * w;
            let row_m = ym * w;
            let row_p = yp * w;
            for x in 0..w {
                // Wrapped column indices (toroidal left/right edges).
                let xm = if x == 0 { w - 1 } else { x - 1 };
                let xp = if x == w - 1 { 0 } else { x + 1 };
                let c = row + x;
                let uc = u[c];
                let vc = v[c];

                // 9-point Laplacian: orthogonal + diagonal - center.
                let lap_u = W_ORTHO * (u[row + xm] + u[row + xp] + u[row_m + x] + u[row_p + x])
                    + W_DIAG * (u[row_m + xm] + u[row_m + xp] + u[row_p + xm] + u[row_p + xp])
                    - uc;
                let lap_v = W_ORTHO * (v[row + xm] + v[row + xp] + v[row_m + x] + v[row_p + x])
                    + W_DIAG * (v[row_m + xm] + v[row_m + xp] + v[row_p + xm] + v[row_p + xp])
                    - vc;

                // Autocatalytic reaction term U + 2V -> 3V.
                let uvv = uc * vc * vc;
                un[c] = uc + (du * lap_u - uvv + feed * (1.0 - uc)) * DT;
                vn[c] = vc + (dv * lap_v + uvv - (feed + kill) * vc) * DT;
            }
        }

        // Promote the freshly computed buffers to current.
        core::mem::swap(&mut self.u, &mut self.u_next);
        core::mem::swap(&mut self.v, &mut self.v_next);
    }

    /// Advance `substeps` steps (values <= 0 are a no-op).
    fn step(&mut self, substeps: i32) {
        let n = substeps.max(0) as usize;
        for _ in 0..n {
            self.step_once();
        }
    }
}

// ============================ C ABI surface ============================
//
// Thin `unsafe extern "C"` wrappers over the safe methods above. Each validates
// its pointer, then delegates. `create` hands ownership to C via `Box::into_raw`;
// `destroy` reclaims it. All other calls borrow through the raw pointer for the
// duration of the call only.

/// Create a `w*h` grid with the given parameters, initialized to U = 1, V = 0.
/// Returns null for non-positive dimensions.
#[no_mangle]
pub extern "C" fn rust_rd_create(
    w: i32,
    h: i32,
    feed: f32,
    kill: f32,
    du: f32,
    dv: f32,
) -> *mut RustReactionDiffusion {
    if w <= 0 || h <= 0 {
        return core::ptr::null_mut();
    }
    let rd = RustReactionDiffusion::new(w as usize, h as usize, feed, kill, du, dv);
    Box::into_raw(Box::new(rd))
}

/// Free a grid created by [`rust_rd_create`]. Null is ignored.
#[no_mangle]
pub unsafe extern "C" fn rust_rd_destroy(rd: *mut RustReactionDiffusion) {
    if !rd.is_null() {
        drop(Box::from_raw(rd));
    }
}

/// Seed V with deterministic random blobs (see [`RustReactionDiffusion::seed_random`]).
#[no_mangle]
pub unsafe extern "C" fn rust_rd_seed_random(rd: *mut RustReactionDiffusion, seed: u64, density: f32) {
    if rd.is_null() {
        return;
    }
    (*rd).seed_random(seed, density);
}

/// Seed a single disk of V = 1, U = 0 at `(x, y)` with the given `radius`.
#[no_mangle]
pub unsafe extern "C" fn rust_rd_seed_point(rd: *mut RustReactionDiffusion, x: i32, y: i32, radius: i32) {
    if rd.is_null() {
        return;
    }
    (*rd).stamp_disk(x as isize, y as isize, radius as isize);
}

/// Update the reaction/diffusion parameters in place.
#[no_mangle]
pub unsafe extern "C" fn rust_rd_set_params(
    rd: *mut RustReactionDiffusion,
    feed: f32,
    kill: f32,
    du: f32,
    dv: f32,
) {
    if rd.is_null() {
        return;
    }
    let rd = &mut *rd;
    rd.feed = feed;
    rd.kill = kill;
    rd.du = du;
    rd.dv = dv;
}

/// Advance the simulation `substeps` iterations.
#[no_mangle]
pub unsafe extern "C" fn rust_rd_step(rd: *mut RustReactionDiffusion, substeps: i32) {
    if rd.is_null() {
        return;
    }
    (*rd).step(substeps);
}

/// Write the V field, clamped to `[0, 1]`, into `out_v` (`w*h` floats).
#[no_mangle]
pub unsafe extern "C" fn rust_rd_field(rd: *const RustReactionDiffusion, out_v: *mut f32) {
    if rd.is_null() || out_v.is_null() {
        return;
    }
    let rd = &*rd;
    let n = rd.w * rd.h;
    let dst = slice::from_raw_parts_mut(out_v, n);
    for i in 0..n {
        dst[i] = rd.v[i].clamp(0.0, 1.0);
    }
}

/// Grid width in cells (0 for a null handle).
#[no_mangle]
pub unsafe extern "C" fn rust_rd_width(rd: *const RustReactionDiffusion) -> i32 {
    if rd.is_null() {
        return 0;
    }
    (*rd).w as i32
}

/// Grid height in cells (0 for a null handle).
#[no_mangle]
pub unsafe extern "C" fn rust_rd_height(rd: *const RustReactionDiffusion) -> i32 {
    if rd.is_null() {
        return 0;
    }
    (*rd).h as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Classic Pearson "coral/mitosis" preset — grows stable structures.
    const FEED: f32 = 0.055;
    const KILL: f32 = 0.062;
    const DU: f32 = 0.16;
    const DV: f32 = 0.08;

    /// Helper: build a grid, returning the raw pointer for FFI-style testing.
    fn make(w: i32, h: i32) -> *mut RustReactionDiffusion {
        rust_rd_create(w, h, FEED, KILL, DU, DV)
    }

    /// Sum of the whole V field (via the clamped C accessor).
    fn v_sum(rd: *const RustReactionDiffusion, w: usize, h: usize) -> f64 {
        let mut buf = vec![0.0f32; w * h];
        unsafe { rust_rd_field(rd, buf.as_mut_ptr()) };
        buf.iter().map(|&x| x as f64).sum()
    }

    #[test]
    fn create_reports_dimensions_and_rest_state() {
        let rd = make(40, 30);
        assert!(!rd.is_null());
        unsafe {
            assert_eq!(rust_rd_width(rd), 40);
            assert_eq!(rust_rd_height(rd), 30);
        }
        // Fresh grid: V is zero everywhere.
        assert!(v_sum(rd, 40, 30) < 1e-6);
        unsafe { rust_rd_destroy(rd) };
        // Non-positive dimensions -> null handle.
        assert!(rust_rd_create(0, 10, FEED, KILL, DU, DV).is_null());
        assert!(rust_rd_create(10, -1, FEED, KILL, DU, DV).is_null());
    }

    #[test]
    fn seed_point_then_step_spreads_pattern() {
        let (w, h) = (64usize, 64usize);
        let rd = make(w as i32, h as i32);
        unsafe { rust_rd_seed_point(rd, 32, 32, 4) };

        // A cell well outside the seed disk starts empty.
        let far = (32 - 12) * w + 32; // 12 rows above center
        let mut before = vec![0.0f32; w * h];
        unsafe { rust_rd_field(rd, before.as_mut_ptr()) };
        assert_eq!(before[far], 0.0, "far cell should start empty");

        let seed_sum = v_sum(rd, w, h);
        assert!(seed_sum > 0.0, "seeding must deposit some V");

        // Diffusion must carry V outward and change the total.
        unsafe { rust_rd_step(rd, 120) };
        let after_sum = v_sum(rd, w, h);
        assert!(
            (after_sum - seed_sum).abs() > 1e-3,
            "total V should change: {seed_sum} -> {after_sum}"
        );

        // Count cells that became nonzero beyond the initial disk footprint.
        let mut after = vec![0.0f32; w * h];
        unsafe { rust_rd_field(rd, after.as_mut_ptr()) };
        let spread = after
            .iter()
            .zip(before.iter())
            .filter(|(a, b)| **a > 1e-4 && **b == 0.0)
            .count();
        assert!(spread > 0, "pattern should spread into previously empty cells");

        unsafe { rust_rd_destroy(rd) };
    }

    #[test]
    fn fields_stay_finite_and_bounded_over_many_steps() {
        let (w, h) = (48usize, 48usize);
        let rd = make(w as i32, h as i32);
        unsafe { rust_rd_seed_random(rd, 0xC0FFEE, 0.3) };
        unsafe { rust_rd_step(rd, 800) };

        // Inspect the raw (unclamped) U and V fields directly.
        let inner = unsafe { &*rd };
        for &val in inner.u.iter().chain(inner.v.iter()) {
            assert!(val.is_finite(), "field value must stay finite, got {val}");
            // Gray-Scott concentrations live in [0,1]; allow a small numerical
            // margin. A blow-up (instability) would fail this immediately.
            assert!(
                (-0.5..=2.0).contains(&val),
                "field value out of reasonable bounds: {val}"
            );
        }
        unsafe { rust_rd_destroy(rd) };
    }

    #[test]
    fn set_params_changes_the_dynamics() {
        let (w, h) = (48usize, 48usize);
        // Two identically seeded grids...
        let a = make(w as i32, h as i32);
        let b = make(w as i32, h as i32);
        unsafe {
            rust_rd_seed_point(a, 24, 24, 5);
            rust_rd_seed_point(b, 24, 24, 5);
            // ...but B gets very different feed/kill/diffusion.
            rust_rd_set_params(b, 0.014, 0.054, 0.20, 0.10);
            rust_rd_step(a, 60);
            rust_rd_step(b, 60);
        }
        let sum_a = v_sum(a, w, h);
        let sum_b = v_sum(b, w, h);
        assert!(
            (sum_a - sum_b).abs() > 1e-3,
            "different params must yield different fields: {sum_a} vs {sum_b}"
        );
        unsafe {
            rust_rd_destroy(a);
            rust_rd_destroy(b);
        }
    }

    #[test]
    fn seed_random_is_deterministic_for_fixed_seed() {
        let (w, h) = (50usize, 40usize);
        let a = make(w as i32, h as i32);
        let b = make(w as i32, h as i32);
        let c = make(w as i32, h as i32);
        unsafe {
            rust_rd_seed_random(a, 12345, 0.25);
            rust_rd_seed_random(b, 12345, 0.25); // same seed -> identical
            rust_rd_seed_random(c, 99999, 0.25); // different seed -> different
        }
        let mut fa = vec![0.0f32; w * h];
        let mut fb = vec![0.0f32; w * h];
        let mut fc = vec![0.0f32; w * h];
        unsafe {
            rust_rd_field(a, fa.as_mut_ptr());
            rust_rd_field(b, fb.as_mut_ptr());
            rust_rd_field(c, fc.as_mut_ptr());
        }
        assert_eq!(fa, fb, "same seed must reproduce the same field");
        assert!(fa.iter().any(|&x| x > 0.0), "seeding should deposit V");
        assert_ne!(fa, fc, "different seed should give a different field");

        // Determinism must also hold after stepping.
        unsafe {
            rust_rd_step(a, 40);
            rust_rd_step(b, 40);
            rust_rd_field(a, fa.as_mut_ptr());
            rust_rd_field(b, fb.as_mut_ptr());
        }
        assert_eq!(fa, fb, "stepping must be deterministic too");
        unsafe {
            rust_rd_destroy(a);
            rust_rd_destroy(b);
            rust_rd_destroy(c);
        }
    }
}
