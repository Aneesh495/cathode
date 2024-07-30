//! Verlet cloth / soft-body.
//!
//! A grid of `nx * ny` point masses laid out in the XY plane and connected by
//! distance constraints. We integrate motion with **position-based Verlet**
//! (velocity is implicit in the delta between the current and previous
//! position) and then satisfy the distance constraints with a fixed number of
//! Gauss-Seidel relaxation passes. This is the classic Jakobsen "Advanced
//! Character Physics" cloth: cheap, unconditionally stable enough for a waving
//! flag under gravity and wind, and requires no velocity state or matrices.
//!
//! Constraint set per interior quad:
//!   * structural  — horizontal `(ix,iy)-(ix+1,iy)` and vertical
//!                    `(ix,iy)-(ix,iy+1)` neighbours (rest = `spacing`).
//!   * shear       — the two diagonals of the quad (rest = `spacing*sqrt(2)`),
//!                    which stop the sheet from collapsing/folding flat.
//!
//! Layout convention: node `(ix,iy)` sits at world `(ix*spacing, iy*spacing, 0)`
//! and is stored at linear index `iy*nx + ix`. Larger `iy` is physically higher
//! (top of the cloth); gravity pulls toward -Y, so free nodes fall (y
//! decreases) while pinned nodes are held.
//!
//! This mirrors the frozen C ABI in `include/cathode/rustcore.h`. Every entry
//! point is `#[no_mangle] pub extern "C"`, takes/returns only C-layout POD or
//! the opaque `RustCloth` pointer, validates its arguments, and never unwinds.

use crate::Vec3;
use core::slice;

/// A single distance constraint between two nodes with a target rest length.
struct Constraint {
    a: usize,
    b: usize,
    rest: f32,
}

/// Opaque cloth state. The C side only ever holds a `*mut RustCloth`; the
/// layout is entirely private to Rust. Owned by Rust, freed by
/// `rust_cloth_destroy`.
pub struct RustCloth {
    nx: usize,
    ny: usize,
    /// Current node positions (index = iy*nx + ix).
    pos: Vec<Vec3>,
    /// Previous node positions; `pos - prev` encodes the implicit velocity.
    prev: Vec<Vec3>,
    /// Per-node pin flag; pinned nodes are never integrated nor moved by
    /// constraint relaxation.
    pinned: Vec<bool>,
    /// Precomputed distance constraints (structural + shear).
    constraints: Vec<Constraint>,
    /// Wind acceleration (applied every step, added to gravity).
    wind: Vec3,
    /// Gravity magnitude; applied as acceleration `(0, -gravity, 0)`.
    gravity: f32,
    /// Velocity retention per step in `[0,1]`; `<1` bleeds energy for stability.
    damping: f32,
}

// ---- small Vec3 helpers (Vec3 is a plain repr(C) POD with no arithmetic) ----

#[inline]
fn v_sub(a: Vec3, b: Vec3) -> Vec3 {
    Vec3 { x: a.x - b.x, y: a.y - b.y, z: a.z - b.z }
}
#[inline]
fn v_len(a: Vec3) -> f32 {
    (a.x * a.x + a.y * a.y + a.z * a.z).sqrt()
}

impl RustCloth {
    /// Build the grid and its constraint list.
    fn new(nx: usize, ny: usize, spacing: f32) -> RustCloth {
        let count = nx * ny;
        let mut pos = Vec::with_capacity(count);
        // Lay nodes on the XY plane. z stays 0 so the sheet starts flat.
        for iy in 0..ny {
            for ix in 0..nx {
                pos.push(Vec3 {
                    x: ix as f32 * spacing,
                    y: iy as f32 * spacing,
                    z: 0.0,
                });
            }
        }
        // prev == pos means zero initial velocity.
        let prev = pos.clone();
        let pinned = vec![false; count];

        // Build constraints. `idx(ix,iy)` = iy*nx + ix.
        let idx = |ix: usize, iy: usize| -> usize { iy * nx + ix };
        let diag = spacing * core::f32::consts::SQRT_2;
        let mut constraints = Vec::new();
        for iy in 0..ny {
            for ix in 0..nx {
                // structural horizontal
                if ix + 1 < nx {
                    constraints.push(Constraint { a: idx(ix, iy), b: idx(ix + 1, iy), rest: spacing });
                }
                // structural vertical
                if iy + 1 < ny {
                    constraints.push(Constraint { a: idx(ix, iy), b: idx(ix, iy + 1), rest: spacing });
                }
                // shear diagonals of the quad whose lower-left corner is (ix,iy)
                if ix + 1 < nx && iy + 1 < ny {
                    constraints.push(Constraint { a: idx(ix, iy), b: idx(ix + 1, iy + 1), rest: diag });
                    constraints.push(Constraint { a: idx(ix + 1, iy), b: idx(ix, iy + 1), rest: diag });
                }
            }
        }

        RustCloth {
            nx,
            ny,
            pos,
            prev,
            pinned,
            constraints,
            wind: Vec3::default(),
            gravity: 9.81,
            damping: 0.99,
        }
    }

    /// One Verlet integration + constraint-relaxation step.
    fn step(&mut self, dt: f32, relax_iters: i32) {
        // Total acceleration: gravity toward -Y plus the stored wind force.
        let ax = self.wind.x;
        let ay = -self.gravity + self.wind.y;
        let az = self.wind.z;
        let dt2 = dt * dt;
        let d = self.damping;

        // --- integrate unpinned nodes ---
        for i in 0..self.pos.len() {
            if self.pinned[i] {
                // Held nodes never move; keep prev == pos so they contribute no
                // implicit velocity if later unpinned.
                self.prev[i] = self.pos[i];
                continue;
            }
            let cur = self.pos[i];
            let pv = self.prev[i];
            // new = pos + (pos - prev)*damping + accel*dt^2
            let nx_ = cur.x + (cur.x - pv.x) * d + ax * dt2;
            let ny_ = cur.y + (cur.y - pv.y) * d + ay * dt2;
            let nz_ = cur.z + (cur.z - pv.z) * d + az * dt2;
            self.prev[i] = cur;
            self.pos[i] = Vec3 { x: nx_, y: ny_, z: nz_ };
        }

        // --- satisfy distance constraints (Gauss-Seidel relaxation) ---
        let iters = if relax_iters < 0 { 0 } else { relax_iters };
        for _ in 0..iters {
            for c in &self.constraints {
                let pa = self.pos[c.a];
                let pb = self.pos[c.b];
                let delta = v_sub(pb, pa);
                let dist = v_len(delta);
                // Degenerate (coincident) nodes have no defined direction; skip
                // to avoid dividing by zero.
                if dist <= 1e-8 {
                    continue;
                }
                // Fraction of the separation vector that must be removed to
                // restore the rest length.
                let diff = (dist - c.rest) / dist;
                // Distribute the correction by inverse "mass": pinned endpoints
                // are infinitely heavy (weight 0) and stay put.
                let wa = if self.pinned[c.a] { 0.0 } else { 1.0 };
                let wb = if self.pinned[c.b] { 0.0 } else { 1.0 };
                let total = wa + wb;
                if total == 0.0 {
                    continue; // both endpoints pinned — constraint is inert
                }
                // corr = full closing vector; split between endpoints by weight.
                let corr_x = delta.x * diff;
                let corr_y = delta.y * diff;
                let corr_z = delta.z * diff;
                let fa = wa / total;
                let fb = wb / total;
                // a moves toward b, b moves toward a.
                if wa != 0.0 {
                    let p = &mut self.pos[c.a];
                    p.x += corr_x * fa;
                    p.y += corr_y * fa;
                    p.z += corr_z * fa;
                }
                if wb != 0.0 {
                    let p = &mut self.pos[c.b];
                    p.x -= corr_x * fb;
                    p.y -= corr_y * fb;
                    p.z -= corr_z * fb;
                }
            }
        }
    }
}

// ============================ C ABI surface ============================

/// Create an `nx * ny` cloth grid spaced by `spacing` in the XY plane.
/// Returns null for a degenerate (`<=0`) grid dimension.
#[no_mangle]
pub extern "C" fn rust_cloth_create(nx: i32, ny: i32, spacing: f32) -> *mut RustCloth {
    if nx <= 0 || ny <= 0 {
        return core::ptr::null_mut();
    }
    let cloth = RustCloth::new(nx as usize, ny as usize, spacing);
    // Hand ownership to C; reclaimed by rust_cloth_destroy.
    Box::into_raw(Box::new(cloth))
}

/// Destroy a cloth created by `rust_cloth_create`. Safe to call with null.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_destroy(c: *mut RustCloth) {
    if !c.is_null() {
        drop(Box::from_raw(c));
    }
}

/// Pin (fix in place) the node at grid coordinate `(ix,iy)`. Out-of-range
/// coordinates are ignored.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_pin(c: *mut RustCloth, ix: i32, iy: i32) {
    let Some(cloth) = c.as_mut() else { return };
    if ix < 0 || iy < 0 {
        return;
    }
    let (ix, iy) = (ix as usize, iy as usize);
    if ix < cloth.nx && iy < cloth.ny {
        cloth.pinned[iy * cloth.nx + ix] = true;
    }
}

/// Set the constant wind acceleration applied every step.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_set_wind(c: *mut RustCloth, wx: f32, wy: f32, wz: f32) {
    if let Some(cloth) = c.as_mut() {
        cloth.wind = Vec3 { x: wx, y: wy, z: wz };
    }
}

/// Set the gravity magnitude (applied as acceleration toward -Y).
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_set_gravity(c: *mut RustCloth, g: f32) {
    if let Some(cloth) = c.as_mut() {
        cloth.gravity = g;
    }
}

/// Advance the simulation by `dt` seconds, then run `relax_iters` constraint
/// relaxation passes.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_step(c: *mut RustCloth, dt: f32, relax_iters: i32) {
    if let Some(cloth) = c.as_mut() {
        cloth.step(dt, relax_iters);
    }
}

/// Number of nodes = `nx * ny`.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_node_count(c: *const RustCloth) -> i32 {
    match c.as_ref() {
        Some(cloth) => (cloth.nx * cloth.ny) as i32,
        None => 0,
    }
}

/// Copy node positions into `out_xyz`, which must hold `node_count * 3` floats.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_positions(c: *const RustCloth, out_xyz: *mut f32) {
    let Some(cloth) = c.as_ref() else { return };
    if out_xyz.is_null() {
        return;
    }
    let n = cloth.pos.len();
    let dst = slice::from_raw_parts_mut(out_xyz, n * 3);
    for (i, p) in cloth.pos.iter().enumerate() {
        dst[3 * i] = p.x;
        dst[3 * i + 1] = p.y;
        dst[3 * i + 2] = p.z;
    }
}

/// Emit triangle indices (2 triangles per quad, consistent CCW winding) into
/// `out_idx`. Writes at most `max_tris` triangles (`max_tris*3` indices) and
/// returns the number of triangles actually written.
#[no_mangle]
pub unsafe extern "C" fn rust_cloth_indices(c: *const RustCloth, out_idx: *mut u32, max_tris: i32) -> i32 {
    let Some(cloth) = c.as_ref() else { return 0 };
    if out_idx.is_null() || max_tris <= 0 {
        return 0;
    }
    let max = max_tris as usize;
    let dst = slice::from_raw_parts_mut(out_idx, max * 3);
    let nx = cloth.nx;
    let ny = cloth.ny;
    let mut tri: usize = 0;
    // Each interior quad (lower-left corner ix,iy) becomes two triangles:
    //   (a,b,c) and (b,d,c) where
    //   a=(ix,iy) b=(ix+1,iy) c=(ix,iy+1) d=(ix+1,iy+1)
    'outer: for iy in 0..ny.saturating_sub(1) {
        for ix in 0..nx.saturating_sub(1) {
            let a = (iy * nx + ix) as u32;
            let b = (iy * nx + ix + 1) as u32;
            let cc = ((iy + 1) * nx + ix) as u32;
            let dd = ((iy + 1) * nx + ix + 1) as u32;
            if tri >= max {
                break 'outer;
            }
            dst[tri * 3] = a;
            dst[tri * 3 + 1] = b;
            dst[tri * 3 + 2] = cc;
            tri += 1;
            if tri >= max {
                break 'outer;
            }
            dst[tri * 3] = b;
            dst[tri * 3 + 1] = dd;
            dst[tri * 3 + 2] = cc;
            tri += 1;
        }
    }
    tri as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: read every node position out through the C ABI.
    fn positions(c: *const RustCloth) -> Vec<Vec3> {
        let n = unsafe { rust_cloth_node_count(c) } as usize;
        let mut buf = vec![0.0f32; n * 3];
        unsafe { rust_cloth_positions(c, buf.as_mut_ptr()) };
        (0..n)
            .map(|i| Vec3 { x: buf[3 * i], y: buf[3 * i + 1], z: buf[3 * i + 2] })
            .collect()
    }

    #[test]
    fn cloth_pins_hold_and_free_nodes_fall() {
        // (a) Cloth pinned at the two TOP corners falls under gravity: bottom
        // nodes drop (y decreases) while the pinned corners stay exactly put.
        let nx = 10i32;
        let ny = 10i32;
        let c = rust_cloth_create(nx, ny, 1.0);
        assert!(!c.is_null());
        // Top row is iy = ny-1 (larger y is higher). Pin its two corners.
        unsafe {
            rust_cloth_pin(c, 0, ny - 1);
            rust_cloth_pin(c, nx - 1, ny - 1);
            rust_cloth_set_gravity(c, 9.81);
        }
        let idx = |ix: i32, iy: i32| (iy * nx + ix) as usize;
        let start = positions(c);
        // Simulate to equilibrium (~2s at 60fps). We use a moderate relaxation
        // count: a sheet pinned only at its two top corners and triangulated
        // with shear diagonals is a very stiff in-plane truss, so it settles to
        // a small but unambiguous downward sag rather than draping far.
        for _ in 0..120 {
            unsafe { rust_cloth_step(c, 1.0 / 60.0, 12) };
        }
        let end = positions(c);

        // Pinned corners must be byte-for-byte unchanged.
        for &corner in &[idx(0, ny - 1), idx(nx - 1, ny - 1)] {
            assert_eq!(start[corner].x, end[corner].x, "pinned x moved");
            assert_eq!(start[corner].y, end[corner].y, "pinned y moved");
            assert_eq!(start[corner].z, end[corner].z, "pinned z moved");
        }
        // Every free bottom-row node must have fallen (y strictly decreased).
        for ix in 0..nx {
            let bottom = idx(ix, 0);
            assert!(
                end[bottom].y < start[bottom].y,
                "bottom node {} did not fall: {} -> {}",
                ix, start[bottom].y, end[bottom].y
            );
        }
        // The mid-bottom node's sag must exceed a clear margin (equilibrium sag
        // here is ~1.4e-2; assert well above float noise).
        let mid_bottom = idx(nx / 2, 0);
        assert!(
            end[mid_bottom].y < start[mid_bottom].y - 4e-3,
            "bottom node barely moved: {} -> {}",
            start[mid_bottom].y,
            end[mid_bottom].y
        );
        unsafe { rust_cloth_destroy(c) };
    }

    #[test]
    fn cloth_constraints_stay_near_rest_no_explosion() {
        // (b) Over 200 steps the neighbour distances stay near the rest length:
        // the sim does not explode. We pin the whole top row (a hanging sheet)
        // and measure the worst structural stretch ratio.
        let nx = 12usize;
        let ny = 12usize;
        let spacing = 0.5f32;
        let c = rust_cloth_create(nx as i32, ny as i32, spacing);
        assert!(!c.is_null());
        unsafe {
            for ix in 0..nx as i32 {
                rust_cloth_pin(c, ix, ny as i32 - 1);
            }
            rust_cloth_set_gravity(c, 9.81);
            rust_cloth_set_wind(c, 2.0, 0.0, 1.0);
            for _ in 0..200 {
                rust_cloth_step(c, 1.0 / 60.0, 25);
            }
        }
        let p = positions(c);
        // Worst structural (horizontal/vertical) neighbour stretch ratio.
        let mut max_ratio = 0.0f32;
        let at = |ix: usize, iy: usize| p[iy * nx + ix];
        for iy in 0..ny {
            for ix in 0..nx {
                if ix + 1 < nx {
                    let d = v_len(v_sub(at(ix + 1, iy), at(ix, iy)));
                    max_ratio = max_ratio.max(d / spacing);
                }
                if iy + 1 < ny {
                    let d = v_len(v_sub(at(ix, iy + 1), at(ix, iy)));
                    max_ratio = max_ratio.max(d / spacing);
                }
            }
        }
        // Bounded stretch => stable. A well-behaved PBD cloth stays well under
        // 1.5x; we assert a comfortable ceiling that still rules out blow-up.
        assert!(max_ratio.is_finite(), "stretch ratio not finite");
        assert!(max_ratio < 1.5, "cloth exploded: max stretch ratio {}", max_ratio);
        unsafe { rust_cloth_destroy(c) };
    }

    #[test]
    fn cloth_counts_are_correct() {
        // (c) node_count == nx*ny, and indices == 2*(nx-1)*(ny-1) triangles,
        // all indices in range.
        let nx = 7i32;
        let ny = 5i32;
        let c = rust_cloth_create(nx, ny, 1.0);
        assert!(!c.is_null());
        let n = unsafe { rust_cloth_node_count(c) };
        assert_eq!(n, nx * ny);

        let expected_tris = 2 * (nx - 1) * (ny - 1);
        let max_tris = expected_tris; // give exactly enough room
        let mut idx = vec![0u32; (max_tris * 3) as usize];
        let got = unsafe { rust_cloth_indices(c, idx.as_mut_ptr(), max_tris) };
        assert_eq!(got, expected_tris, "triangle count wrong");
        // Every emitted index must reference a real node.
        for &v in &idx {
            assert!((v as i32) < n, "index {} out of range", v);
        }
        // Respect the max_tris cap: asking for fewer must write fewer.
        let capped = unsafe { rust_cloth_indices(c, idx.as_mut_ptr(), 3) };
        assert_eq!(capped, 3, "cap not honoured");
        unsafe { rust_cloth_destroy(c) };
    }

    #[test]
    fn cloth_positions_stay_finite() {
        // (d) All positions remain finite after simulation, even with pins,
        // gravity, and wind together.
        let nx = 8i32;
        let ny = 8i32;
        let c = rust_cloth_create(nx, ny, 1.0);
        assert!(!c.is_null());
        unsafe {
            rust_cloth_pin(c, 0, ny - 1);
            rust_cloth_pin(c, nx - 1, ny - 1);
            rust_cloth_set_gravity(c, 9.81);
            rust_cloth_set_wind(c, 5.0, -1.0, 3.0);
            for _ in 0..300 {
                rust_cloth_step(c, 1.0 / 60.0, 15);
            }
        }
        for (i, v) in positions(c).iter().enumerate() {
            assert!(v.x.is_finite() && v.y.is_finite() && v.z.is_finite(), "node {} not finite: {:?}", i, v);
        }
        unsafe { rust_cloth_destroy(c) };
    }

    #[test]
    fn cloth_create_rejects_degenerate() {
        // Defensive: non-positive dimensions must not allocate.
        assert!(rust_cloth_create(0, 5, 1.0).is_null());
        assert!(rust_cloth_create(5, -1, 1.0).is_null());
    }
}
