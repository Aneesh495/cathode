//! Strange attractors  -  chaotic ODE integrators emitting 3D point streams.
//!
//! Implements `rust_attractor_generate` from `include/cathode/rustcore.h`. We
//! integrate one of four classic autonomous chaotic systems (Lorenz, Aizawa,
//! Thomas, Halvorsen) with a fixed-step 4th-order Runge-Kutta (RK4) scheme and
//! write the resulting trajectory as a stream of [`Vec3`] points.
//!
//! Precision note: the ODE state is carried in `f64` throughout the RK4 steps
//! and only down-cast to the `f32` fields of [`Vec3`] on store. The extra
//! headroom keeps long trajectories (thousands of steps) from accumulating
//! visible drift/blow-up that a pure-`f32` integrator would suffer near the
//! stiff regions of these systems.

use crate::Vec3;

/// Which chaotic system to integrate. Mirrors the C enum `RustAttractor`
/// (`ATTR_LORENZ=0`, `ATTR_AIZAWA=1`, `ATTR_THOMAS=2`, `ATTR_HALVORSEN=3`).
///
/// We map from the raw `i32` the C ABI hands us rather than declaring a
/// `#[repr(i32)]` enum directly at the boundary: constructing a Rust enum from
/// an out-of-range discriminant is undefined behavior, so we validate first and
/// fall back to Lorenz for any unrecognized value.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    Lorenz,
    Aizawa,
    Thomas,
    Halvorsen,
}

impl Kind {
    #[inline]
    fn from_raw(k: i32) -> Kind {
        match k {
            1 => Kind::Aizawa,
            2 => Kind::Thomas,
            3 => Kind::Halvorsen,
            _ => Kind::Lorenz, // 0 and anything unexpected
        }
    }
}

/// Evaluate the system's velocity field  (dx/dt, dy/dt, dz/dt)  at state
/// `(x, y, z)`. All constants are the canonical published parameters for each
/// attractor, matching the specification in the ABI header.
#[inline]
fn derivative(kind: Kind, x: f64, y: f64, z: f64) -> (f64, f64, f64) {
    match kind {
        // Lorenz '63 with the classic chaotic parameters
        // sigma = 10, rho = 28, beta = 8/3.
        Kind::Lorenz => {
            let dx = 10.0 * (y - x);
            let dy = x * (28.0 - z) - y;
            let dz = x * y - (8.0 / 3.0) * z;
            (dx, dy, dz)
        }
        // Aizawa attractor with the standard parameter set
        // a=0.95, b=0.7, c=0.6, d=3.5, e=0.25, f=0.1.
        Kind::Aizawa => {
            let (a, b, c, d, e, f) = (0.95, 0.7, 0.6, 3.5, 0.25, 0.1);
            let dx = (z - b) * x - d * y;
            let dy = d * x + (z - b) * y;
            let dz =
                c + a * z - (z * z * z) / 3.0 - (x * x + y * y) * (1.0 + e * z) + f * z * x * x * x;
            (dx, dy, dz)
        }
        // Thomas' cyclically-symmetric attractor, dissipation b = 0.208.
        Kind::Thomas => {
            let b = 0.208;
            let dx = y.sin() - b * x;
            let dy = z.sin() - b * y;
            let dz = x.sin() - b * z;
            (dx, dy, dz)
        }
        // Halvorsen's cyclically-symmetric attractor, a = 1.89.
        Kind::Halvorsen => {
            let a = 1.89;
            let dx = -a * x - 4.0 * y - 4.0 * z - y * y;
            let dy = -a * y - 4.0 * z - 4.0 * x - z * z;
            let dz = -a * z - 4.0 * x - 4.0 * y - x * x;
            (dx, dy, dz)
        }
    }
}

/// Advance the state one step of size `dt` using classic RK4. RK4 has local
/// error O(dt^5) / global O(dt^4), which keeps these chaotic orbits on their
/// attracting manifolds far more faithfully than Euler at the same step size.
#[inline]
fn rk4_step(kind: Kind, s: (f64, f64, f64), dt: f64) -> (f64, f64, f64) {
    let (x, y, z) = s;
    let k1 = derivative(kind, x, y, z);
    let k2 = derivative(
        kind,
        x + 0.5 * dt * k1.0,
        y + 0.5 * dt * k1.1,
        z + 0.5 * dt * k1.2,
    );
    let k3 = derivative(
        kind,
        x + 0.5 * dt * k2.0,
        y + 0.5 * dt * k2.1,
        z + 0.5 * dt * k2.2,
    );
    let k4 = derivative(kind, x + dt * k3.0, y + dt * k3.1, z + dt * k3.2);
    (
        x + (dt / 6.0) * (k1.0 + 2.0 * k2.0 + 2.0 * k3.0 + k4.0),
        y + (dt / 6.0) * (k1.1 + 2.0 * k2.1 + 2.0 * k3.1 + k4.1),
        z + (dt / 6.0) * (k1.2 + 2.0 * k2.2 + 2.0 * k3.2 + k4.2),
    )
}

/// Integrate the selected attractor from `start` for up to `count` samples,
/// writing successive trajectory points into `out_pts` (capacity `max_pts`).
///
/// The first written point is `start` itself; each subsequent point is one RK4
/// step of size `dt` later. Returns the number of points written, which is
/// `min(count, max_pts)` clamped to be non-negative. Returns 0 for a null
/// output pointer or non-positive `count`/`max_pts`.
///
/// # Safety
/// `out_pts` must point to storage for at least `max_pts` [`Vec3`] values.
#[no_mangle]
pub unsafe extern "C" fn rust_attractor_generate(
    kind: i32,
    start: Vec3,
    dt: f32,
    count: i32,
    out_pts: *mut Vec3,
    max_pts: i32,
) -> i32 {
    if out_pts.is_null() || count <= 0 || max_pts <= 0 {
        return 0;
    }
    let kind = Kind::from_raw(kind);
    let n = count.min(max_pts) as usize;
    let dt = dt as f64;

    let dst = core::slice::from_raw_parts_mut(out_pts, n);

    // Carry the integrator state in f64; store each sample down-cast to f32.
    let mut state = (start.x as f64, start.y as f64, start.z as f64);
    for slot in dst.iter_mut() {
        *slot = Vec3 {
            x: state.0 as f32,
            y: state.1 as f32,
            z: state.2 as f32,
        };
        state = rk4_step(kind, state, dt);
    }
    n as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The Lorenz orbit must stay bounded (no NaN/inf, no blow-up) over a long
    /// run and must actually wander across a region  -  its hallmark is the
    /// two-lobed butterfly, so the x-coordinate should span a wide range.
    #[test]
    fn lorenz_bounded_and_explores() {
        let n = 5000usize;
        let mut pts = vec![Vec3::default(); n];
        let start = Vec3 {
            x: 0.1,
            y: 0.0,
            z: 0.0,
        };
        let written = unsafe {
            rust_attractor_generate(
                0, // ATTR_LORENZ
                start,
                0.005,
                n as i32,
                pts.as_mut_ptr(),
                n as i32,
            )
        };
        assert_eq!(written, n as i32);

        let (mut xmin, mut xmax) = (f32::INFINITY, f32::NEG_INFINITY);
        for p in &pts {
            assert!(p.x.is_finite() && p.y.is_finite() && p.z.is_finite(), "non-finite point");
            // The classic Lorenz attractor lives comfortably within |coord|<100.
            assert!(p.x.abs() < 100.0 && p.y.abs() < 100.0 && p.z.abs() < 100.0, "escaped bounds");
            xmin = xmin.min(p.x);
            xmax = xmax.max(p.x);
        }
        // Wandering across both lobes gives an x-spread well over 20 units.
        assert!(xmax - xmin > 20.0, "x spread too small: {}", xmax - xmin);
    }

    /// `generate` returns exactly `count` when it fits, and clamps to `max_pts`
    /// when asked for more than the buffer holds.
    #[test]
    fn returns_count_and_clamps() {
        let mut pts = vec![Vec3::default(); 64];
        let start = Vec3 { x: 1.0, y: 1.0, z: 1.0 };

        // count <= max_pts: returns count.
        let got = unsafe {
            rust_attractor_generate(0, start, 0.01, 40, pts.as_mut_ptr(), 64)
        };
        assert_eq!(got, 40);
        // First point is exactly the start state.
        assert_eq!(pts[0].x, 1.0);

        // count > max_pts: clamps to max_pts.
        let got = unsafe {
            rust_attractor_generate(0, start, 0.01, 100, pts.as_mut_ptr(), 64)
        };
        assert_eq!(got, 64);

        // Degenerate inputs return 0.
        let got = unsafe { rust_attractor_generate(0, start, 0.01, 0, pts.as_mut_ptr(), 64) };
        assert_eq!(got, 0);
        let got =
            unsafe { rust_attractor_generate(0, start, 0.01, 10, core::ptr::null_mut(), 64) };
        assert_eq!(got, 0);
    }

    /// Every attractor variant must produce finite, bounded output  -  a smoke
    /// test that the parameter sets and cyclic equations are wired correctly.
    #[test]
    fn all_variants_finite() {
        let starts = [
            Vec3 { x: 0.1, y: 0.0, z: 0.0 },   // Lorenz
            Vec3 { x: 0.1, y: 0.0, z: 0.0 },   // Aizawa
            Vec3 { x: 0.1, y: 0.1, z: 0.1 },   // Thomas
            Vec3 { x: -1.0, y: 0.0, z: 0.0 },  // Halvorsen
        ];
        for kind in 0..4i32 {
            let n = 2000usize;
            let mut pts = vec![Vec3::default(); n];
            let got = unsafe {
                rust_attractor_generate(kind, starts[kind as usize], 0.01, n as i32, pts.as_mut_ptr(), n as i32)
            };
            assert_eq!(got, n as i32, "variant {}", kind);
            for p in &pts {
                assert!(
                    p.x.is_finite() && p.y.is_finite() && p.z.is_finite(),
                    "variant {} produced non-finite output",
                    kind
                );
                assert!(p.x.abs() < 1e4 && p.y.abs() < 1e4 && p.z.abs() < 1e4, "variant {} blew up", kind);
            }
        }
    }
}
