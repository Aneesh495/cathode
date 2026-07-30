//! Diffusion-limited aggregation (DLA).
//!
//! Particles released at the cluster's frontier random-walk until they touch
//! an occupied cell, then stick — producing fractal dendrites (the structure
//! of mineral deposits, coral, frost, lightning Lichtenberg figures). We store
//! the *age* each cell was added so a scene can color the growth history.
//!
//! Implementation notes for performance and correctness:
//!   * Walkers spawn on a circle just outside the current cluster radius and
//!     are killed if they wander too far (a "kill radius" a bit beyond spawn),
//!     the standard trick to keep walks bounded — without it a walker can
//!     wander for an unbounded time.
//!   * 8-neighbour stickiness gives fuller, more natural dendrites than 4.
//!   * A self-contained xorshift RNG (no external crates) keeps runs
//!     deterministic for a given seed.

use core::slice;

pub struct Dla {
    w: i32,
    h: i32,
    /// age grid: -1 = empty, else the order the cell was added (0 = first).
    age: Vec<i32>,
    count: i32,
    next_age: i32,
    // cluster bounding radius from center, tracks the frontier
    cx: f32,
    cy: f32,
    radius: f32,
    rng: u64,
    seeded_line: bool,
    line_y: i32,
}

#[inline]
fn xorshift(s: &mut u64) -> u64 {
    let mut x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    x
}
#[inline]
fn rand_f32(s: &mut u64) -> f32 {
    (xorshift(s) >> 40) as f32 * (1.0 / 16_777_216.0)
}

impl Dla {
    fn new(w: i32, h: i32, seed: u64) -> Box<Dla> {
        Box::new(Dla {
            w,
            h,
            age: vec![-1; (w * h) as usize],
            count: 0,
            next_age: 0,
            cx: w as f32 * 0.5,
            cy: h as f32 * 0.5,
            radius: 2.0,
            rng: seed | 1,
            seeded_line: false,
            line_y: 0,
        })
    }

    #[inline]
    fn idx(&self, x: i32, y: i32) -> usize {
        (y * self.w + x) as usize
    }
    #[inline]
    fn occupied(&self, x: i32, y: i32) -> bool {
        if x < 0 || y < 0 || x >= self.w || y >= self.h {
            return false;
        }
        self.age[self.idx(x, y)] >= 0
    }

    fn add(&mut self, x: i32, y: i32) {
        let i = self.idx(x, y);
        if self.age[i] < 0 {
            self.age[i] = self.next_age;
            self.next_age += 1;
            self.count += 1;
            // expand cluster radius to cover this point
            let d = (((x as f32 - self.cx).powi(2)) + ((y as f32 - self.cy).powi(2))).sqrt();
            if d + 2.0 > self.radius {
                self.radius = d + 2.0;
            }
        }
    }

    fn has_occupied_neighbour(&self, x: i32, y: i32) -> bool {
        for dy in -1..=1 {
            for dx in -1..=1 {
                if dx == 0 && dy == 0 {
                    continue;
                }
                if self.occupied(x + dx, y + dy) {
                    return true;
                }
            }
        }
        false
    }

    /// Grow up to `walkers` particles. Returns the number that stuck.
    fn grow(&mut self, walkers: i32) -> i32 {
        let mut stuck = 0;
        // cap the spawn/kill radius to the grid so line-seeded frost also works
        let maxdim = self.w.max(self.h) as f32;
        for _ in 0..walkers {
            let spawn_r = (self.radius + 3.0).min(maxdim * 0.72);
            let kill_r2 = {
                let k = spawn_r + maxdim * 0.15;
                k * k
            };
            // spawn on a circle around the cluster center (or random x on top
            // edge for a frost line)
            let (mut px, mut py);
            if self.seeded_line {
                px = (rand_f32(&mut self.rng) * self.w as f32) as i32;
                py = self.h - 1 - (rand_f32(&mut self.rng) * 3.0) as i32;
            } else {
                let a = rand_f32(&mut self.rng) * core::f32::consts::TAU;
                px = (self.cx + a.cos() * spawn_r) as i32;
                py = (self.cy + a.sin() * spawn_r) as i32;
            }
            // random walk until stick or killed
            let mut steps = 0;
            let max_steps = 8000;
            loop {
                steps += 1;
                if steps > max_steps {
                    break;
                }
                // step in one of 8 directions
                let r = xorshift(&mut self.rng) & 7;
                px += [-1, 0, 1, -1, 1, -1, 0, 1][r as usize];
                py += [-1, -1, -1, 0, 0, 1, 1, 1][r as usize];
                // wrap horizontally for the frost-line case, clamp otherwise
                if self.seeded_line {
                    if px < 0 {
                        px += self.w;
                    } else if px >= self.w {
                        px -= self.w;
                    }
                    if py < 0 {
                        break;
                    }
                    if py >= self.h {
                        py = self.h - 1;
                    }
                } else {
                    let dx = px as f32 - self.cx;
                    let dy = py as f32 - self.cy;
                    if dx * dx + dy * dy > kill_r2 {
                        break; // wandered too far; respawn
                    }
                    if px < 0 || py < 0 || px >= self.w || py >= self.h {
                        break;
                    }
                }
                if self.has_occupied_neighbour(px, py) {
                    self.add(px, py);
                    stuck += 1;
                    break;
                }
            }
        }
        stuck
    }
}

// ---------------- C ABI ----------------

#[no_mangle]
pub extern "C" fn rust_dla_create(w: i32, h: i32, seed: u64) -> *mut Dla {
    if w < 4 || h < 4 {
        return core::ptr::null_mut();
    }
    Box::into_raw(Dla::new(w, h, seed))
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_destroy(d: *mut Dla) {
    if !d.is_null() {
        drop(Box::from_raw(d));
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_seed_center(d: *mut Dla) {
    if d.is_null() {
        return;
    }
    let d = &mut *d;
    let (cx, cy) = (d.w / 2, d.h / 2);
    d.add(cx, cy);
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_seed_line(d: *mut Dla, y: i32) {
    if d.is_null() {
        return;
    }
    let d = &mut *d;
    let yy = y.clamp(0, d.h - 1);
    for x in 0..d.w {
        d.add(x, yy);
    }
    d.seeded_line = true;
    d.line_y = yy;
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_grow(d: *mut Dla, walkers: i32) -> i32 {
    if d.is_null() || walkers <= 0 {
        return 0;
    }
    (&mut *d).grow(walkers)
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_field(d: *const Dla, out_age: *mut f32) {
    if d.is_null() || out_age.is_null() {
        return;
    }
    let d = &*d;
    let n = (d.w * d.h) as usize;
    let out = slice::from_raw_parts_mut(out_age, n);
    let maxa = if d.next_age > 1 { (d.next_age - 1) as f32 } else { 1.0 };
    for i in 0..n {
        let a = d.age[i];
        out[i] = if a < 0 { -1.0 } else { a as f32 / maxa };
    }
}

#[no_mangle]
pub unsafe extern "C" fn rust_dla_width(d: *const Dla) -> i32 {
    if d.is_null() { 0 } else { (&*d).w }
}
#[no_mangle]
pub unsafe extern "C" fn rust_dla_height(d: *const Dla) -> i32 {
    if d.is_null() { 0 } else { (&*d).h }
}
#[no_mangle]
pub unsafe extern "C" fn rust_dla_count(d: *const Dla) -> i32 {
    if d.is_null() { 0 } else { (&*d).count }
}

#[cfg(test)]
mod tests {
    use super::*;
    unsafe fn mk(w: i32, h: i32) -> *mut Dla {
        let d = rust_dla_create(w, h, 0x1234_5678);
        rust_dla_seed_center(d);
        d
    }

    #[test]
    fn grows_a_connected_cluster() {
        unsafe {
            let d = mk(120, 120);
            let before = rust_dla_count(d);
            let stuck = rust_dla_grow(d, 400);
            let after = rust_dla_count(d);
            assert_eq!(before, 1, "seed adds one cell");
            assert!(stuck > 0, "some walkers must stick");
            assert_eq!(after, before + stuck, "count tracks stuck particles");
            rust_dla_destroy(d);
        }
    }

    #[test]
    fn field_is_normalized_and_marks_empty() {
        unsafe {
            let d = mk(80, 80);
            rust_dla_grow(d, 300);
            let mut f = vec![0.0f32; 80 * 80];
            rust_dla_field(d, f.as_mut_ptr());
            let mut occ = 0;
            for &v in &f {
                assert!(v == -1.0 || (0.0..=1.0).contains(&v), "age in [0,1] or -1, got {v}");
                if v >= 0.0 {
                    occ += 1;
                }
            }
            assert!(occ > 1, "cluster should occupy several cells");
            rust_dla_destroy(d);
        }
    }

    #[test]
    fn cluster_is_contiguous() {
        // every occupied cell (except the seed) must have an occupied 8-neighbour
        unsafe {
            let d = mk(100, 100);
            rust_dla_grow(d, 500);
            let dd = &*d;
            let mut violations = 0;
            for y in 0..dd.h {
                for x in 0..dd.w {
                    if dd.occupied(x, y) && dd.age[dd.idx(x, y)] > 0 {
                        if !dd.has_occupied_neighbour(x, y) {
                            violations += 1;
                        }
                    }
                }
            }
            assert_eq!(violations, 0, "DLA cells must touch the cluster when they stick");
            rust_dla_destroy(d);
        }
    }

    #[test]
    fn determinism() {
        unsafe {
            let a = rust_dla_create(64, 64, 42);
            rust_dla_seed_center(a);
            rust_dla_grow(a, 200);
            let b = rust_dla_create(64, 64, 42);
            rust_dla_seed_center(b);
            rust_dla_grow(b, 200);
            assert_eq!(rust_dla_count(a), rust_dla_count(b), "same seed -> same growth");
            rust_dla_destroy(a);
            rust_dla_destroy(b);
        }
    }
}
