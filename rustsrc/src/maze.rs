//! maze.rs  -  perfect-maze generation + BFS flood-fill solve, over the C ABI.
//!
//! A "perfect" maze (exactly one path between any two cells, no loops) is grown
//! with the **recursive-backtracker** algorithm: from a start cell, repeatedly
//! carve into a random unvisited neighbour, backtracking when boxed in. The
//! result is a spanning tree of the grid, stored as per-cell wall bits.
//!
//! It is then solved from the top-left to the bottom-right with a **breadth-
//! first search** that records, for every reachable cell, the BFS distance
//! (its "discovery time") and a parent pointer. The host can therefore animate
//! the flood filling the maze outward from the start, and highlight the unique
//! shortest solution path by walking parents back from the goal.
//!
//! Grid layout: `w` × `h` cells, row-major, index = y*w + x. Wall bits per cell:
//! bit0=N, bit1=E, bit2=S, bit3=W set when that wall is OPEN (carved).
//! No Rust types cross the boundary; we hand out plain arrays through pointers.

use core::slice;

const N: u8 = 1;
const E: u8 = 2;
const S: u8 = 4;
const W: u8 = 8;

/// Deterministic xorshift64* PRNG so a given seed reproduces a maze exactly.
struct Rng(u64);
impl Rng {
    fn new(seed: u64) -> Self {
        Rng(if seed == 0 { 0x9E3779B97F4A7C15 } else { seed })
    }
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }
    fn below(&mut self, n: u32) -> u32 {
        (self.next() % n as u64) as u32
    }
}

pub struct Maze {
    w: i32,
    h: i32,
    open: Vec<u8>,     // wall-open bitmask per cell
    dist: Vec<i32>,    // BFS distance from start (-1 = unreached)
    parent: Vec<i32>,  // BFS parent cell index (-1 = none)
    max_dist: i32,     // largest finite dist (for coloring / animation length)
    goal: i32,         // goal cell index (bottom-right)
}

impl Maze {
    fn new(w: i32, h: i32, seed: u64) -> Box<Maze> {
        let n = (w * h) as usize;
        let mut m = Maze {
            w,
            h,
            open: vec![0u8; n],
            dist: vec![-1; n],
            parent: vec![-1; n],
            max_dist: 0,
            goal: (n as i32) - 1,
        };
        m.generate(seed);
        m.solve();
        Box::new(m)
    }

    #[inline]
    fn idx(&self, x: i32, y: i32) -> usize {
        (y * self.w + x) as usize
    }

    fn generate(&mut self, seed: u64) {
        for v in self.open.iter_mut() {
            *v = 0;
        }
        let mut rng = Rng::new(seed);
        let n = (self.w * self.h) as usize;
        let mut visited = vec![false; n];
        // explicit stack of cell indices (avoids deep recursion for big mazes)
        let mut stack: Vec<i32> = Vec::with_capacity(n);
        stack.push(0);
        visited[0] = true;
        while let Some(&cur) = stack.last() {
            let cx = cur % self.w;
            let cy = cur / self.w;
            // gather unvisited neighbours
            let mut cand: [(i32, u8, u8); 4] = [(0, 0, 0); 4];
            let mut nc = 0;
            // N
            if cy > 0 && !visited[self.idx(cx, cy - 1)] {
                cand[nc] = (self.idx(cx, cy - 1) as i32, N, S);
                nc += 1;
            }
            // E
            if cx < self.w - 1 && !visited[self.idx(cx + 1, cy)] {
                cand[nc] = (self.idx(cx + 1, cy) as i32, E, W);
                nc += 1;
            }
            // S
            if cy < self.h - 1 && !visited[self.idx(cx, cy + 1)] {
                cand[nc] = (self.idx(cx, cy + 1) as i32, S, N);
                nc += 1;
            }
            // W
            if cx > 0 && !visited[self.idx(cx - 1, cy)] {
                cand[nc] = (self.idx(cx - 1, cy) as i32, W, E);
                nc += 1;
            }
            if nc == 0 {
                stack.pop();
                continue;
            }
            let pick = cand[rng.below(nc as u32) as usize];
            let (nb, my_bit, their_bit) = pick;
            self.open[cur as usize] |= my_bit;
            self.open[nb as usize] |= their_bit;
            visited[nb as usize] = true;
            stack.push(nb);
        }
    }

    fn solve(&mut self) {
        let n = (self.w * self.h) as usize;
        for d in self.dist.iter_mut() {
            *d = -1;
        }
        for p in self.parent.iter_mut() {
            *p = -1;
        }
        let mut q: std::collections::VecDeque<i32> = std::collections::VecDeque::new();
        self.dist[0] = 0;
        q.push_back(0);
        let mut maxd = 0;
        while let Some(cur) = q.pop_front() {
            let cx = cur % self.w;
            let cy = cur / self.w;
            let ob = self.open[cur as usize];
            let d = self.dist[cur as usize];
            if d > maxd {
                maxd = d;
            }
            // walk each open wall to its neighbour
            let step = |nx: i32, ny: i32, this: &mut Maze, qq: &mut std::collections::VecDeque<i32>| {
                let ni = (ny * this.w + nx) as usize;
                if this.dist[ni] < 0 {
                    this.dist[ni] = d + 1;
                    this.parent[ni] = cur;
                    qq.push_back(ni as i32);
                }
            };
            if ob & N != 0 && cy > 0 {
                step(cx, cy - 1, self, &mut q);
            }
            if ob & E != 0 && cx < self.w - 1 {
                step(cx + 1, cy, self, &mut q);
            }
            if ob & S != 0 && cy < self.h - 1 {
                step(cx, cy + 1, self, &mut q);
            }
            if ob & W != 0 && cx > 0 {
                step(cx - 1, cy, self, &mut q);
            }
        }
        self.max_dist = maxd;
        let _ = n;
    }
}

#[no_mangle]
pub extern "C" fn rust_maze_create(w: i32, h: i32, seed: u64) -> *mut Maze {
    if w < 2 || h < 2 || w > 4096 || h > 4096 {
        return core::ptr::null_mut();
    }
    Box::into_raw(Maze::new(w, h, seed))
}

#[no_mangle]
pub unsafe extern "C" fn rust_maze_destroy(m: *mut Maze) {
    if !m.is_null() {
        drop(Box::from_raw(m));
    }
}

/// Regenerate + resolve in place with a new seed (dimensions unchanged).
#[no_mangle]
pub unsafe extern "C" fn rust_maze_reset(m: *mut Maze, seed: u64) {
    if m.is_null() {
        return;
    }
    let m = &mut *m;
    m.generate(seed);
    m.solve();
}

#[no_mangle]
pub unsafe extern "C" fn rust_maze_width(m: *const Maze) -> i32 {
    if m.is_null() { 0 } else { (&*m).w }
}
#[no_mangle]
pub unsafe extern "C" fn rust_maze_height(m: *const Maze) -> i32 {
    if m.is_null() { 0 } else { (&*m).h }
}
#[no_mangle]
pub unsafe extern "C" fn rust_maze_max_dist(m: *const Maze) -> i32 {
    if m.is_null() { 0 } else { (&*m).max_dist }
}

/// Copy the per-cell open-wall bitmasks (w*h bytes) into `out`.
#[no_mangle]
pub unsafe extern "C" fn rust_maze_walls(m: *const Maze, out: *mut u8) {
    if m.is_null() || out.is_null() {
        return;
    }
    let m = &*m;
    let n = (m.w * m.h) as usize;
    slice::from_raw_parts_mut(out, n).copy_from_slice(&m.open);
}

/// Copy the per-cell BFS distances (w*h i32s) into `out` (-1 = unreached).
#[no_mangle]
pub unsafe extern "C" fn rust_maze_dist(m: *const Maze, out: *mut i32) {
    if m.is_null() || out.is_null() {
        return;
    }
    let m = &*m;
    let n = (m.w * m.h) as usize;
    slice::from_raw_parts_mut(out, n).copy_from_slice(&m.dist);
}

/// Write the shortest start→goal path as cell indices into `out` (capacity
/// `max`), returning the path length. The path is ordered start..goal.
#[no_mangle]
pub unsafe extern "C" fn rust_maze_path(m: *const Maze, out: *mut i32, max: i32) -> i32 {
    if m.is_null() || out.is_null() || max <= 0 {
        return 0;
    }
    let m = &*m;
    // walk parents back from goal, then reverse
    let mut rev: Vec<i32> = Vec::new();
    let mut c = m.goal;
    if m.dist[c as usize] < 0 {
        return 0; // unreachable (shouldn't happen for a perfect maze)
    }
    while c != -1 {
        rev.push(c);
        c = m.parent[c as usize];
    }
    let o = slice::from_raw_parts_mut(out, max as usize);
    let n = rev.len().min(max as usize);
    for i in 0..n {
        o[i] = rev[rev.len() - 1 - i]; // reverse into start..goal order
    }
    n as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn maze_is_connected_and_perfect() {
        let m = Maze::new(20, 15, 12345);
        let n = (m.w * m.h) as usize;
        // BFS reached every cell => fully connected
        for i in 0..n {
            assert!(m.dist[i] >= 0, "cell {} unreached", i);
        }
        // A perfect maze on N cells has exactly N-1 carved passages. Count
        // open walls (each passage is shared by 2 cells => counted twice).
        let mut open_edges = 0;
        for &b in &m.open {
            open_edges += (b & N != 0) as i32
                + (b & E != 0) as i32
                + (b & S != 0) as i32
                + (b & W != 0) as i32;
        }
        assert_eq!(open_edges / 2, (n as i32) - 1, "not a spanning tree");
    }

    #[test]
    fn wall_reciprocity() {
        // if cell A has its E wall open, its east neighbour must have W open
        let m = Maze::new(16, 16, 999);
        for y in 0..m.h {
            for x in 0..m.w {
                let c = m.open[(y * m.w + x) as usize];
                if x < m.w - 1 {
                    let e = m.open[(y * m.w + x + 1) as usize];
                    assert_eq!((c & E != 0), (e & W != 0), "E/W mismatch at {},{}", x, y);
                }
                if y < m.h - 1 {
                    let s = m.open[((y + 1) * m.w + x) as usize];
                    assert_eq!((c & S != 0), (s & N != 0), "S/N mismatch at {},{}", x, y);
                }
            }
        }
    }

    #[test]
    fn path_runs_start_to_goal() {
        let m = Maze::new(24, 18, 7);
        let mut buf = vec![0i32; (m.w * m.h) as usize];
        let len = unsafe { rust_maze_path(&*m as *const Maze, buf.as_mut_ptr(), buf.len() as i32) };
        assert!(len >= 2);
        assert_eq!(buf[0], 0, "path must start at cell 0");
        assert_eq!(buf[(len - 1) as usize], m.goal, "path must end at goal");
        // consecutive cells must be grid neighbours
        for i in 1..len as usize {
            let a = buf[i - 1];
            let b = buf[i];
            let (ax, ay) = (a % m.w, a / m.w);
            let (bx, by) = (b % m.w, b / m.w);
            assert_eq!((ax - bx).abs() + (ay - by).abs(), 1, "path step not adjacent");
        }
        // path length must equal goal distance + 1
        assert_eq!(len, m.dist[m.goal as usize] + 1);
    }

    #[test]
    fn reset_changes_maze() {
        let mut m = Maze::new(20, 20, 1);
        let before = m.open.clone();
        m.generate(2);
        m.solve();
        assert!(m.open != before, "different seed should give a different maze");
    }

    #[test]
    fn probe_edge_cases() {
        // minimum-size maze
        let m = Maze::new(2, 2, 0);
        let n = (m.w*m.h) as usize;
        for i in 0..n { assert!(m.dist[i] >= 0); }
        // truncated path: max=1 should write exactly the start cell, return 1
        let mut buf = [ -7i32; 8];
        let len = unsafe { rust_maze_path(&*m as *const Maze, buf.as_mut_ptr(), 1) };
        assert_eq!(len, 1, "truncated len");
        assert_eq!(buf[0], 0, "truncated must be start cell 0");
        assert_eq!(buf[1], -7, "must not write past max");
        // full path on 2x2
        let mut buf2 = [0i32; 4];
        let l2 = unsafe { rust_maze_path(&*m as *const Maze, buf2.as_mut_ptr(), 4) };
        assert_eq!(buf2[0], 0);
        assert_eq!(buf2[(l2-1) as usize], m.goal);
        // truncated max=2 -> writes first 2 cells of start..goal order
        let mut buf3 = [-9i32;4];
        let l3 = unsafe { rust_maze_path(&*m as *const Maze, buf3.as_mut_ptr(), 2) };
        assert_eq!(l3, 2);
        assert_eq!(buf3[0], 0);
        assert_eq!(buf3[2], -9, "no write past max");
        // null handling must not panic
        unsafe {
            assert_eq!(rust_maze_width(core::ptr::null()), 0);
            assert_eq!(rust_maze_path(core::ptr::null(), buf3.as_mut_ptr(), 4), 0);
            rust_maze_reset(core::ptr::null_mut(), 5);
        }
        // largest allowed dims don't overflow i32 index math
        let big = Maze::new(4096, 4096, 1);
        assert_eq!((big.w as i64)*(big.h as i64), big.open.len() as i64);
    }

}
