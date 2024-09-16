//! Wave Function Collapse  -  simple tiled model with edge-adjacency constraints.
//!
//! Each cell holds a bitmask of still-possible tiles (a "superposition"). We
//! repeatedly:
//!   1. pick the uncollapsed cell with the fewest remaining possibilities
//!      (lowest entropy), ties broken by a little noise,
//!   2. collapse it to one tile chosen by weight,
//!   3. propagate: a neighbour may only keep tiles whose touching edge code
//!      matches some still-possible tile in this cell; repeat until stable.
//! If a cell's possibility set empties, that's a contradiction (the caller can
//! reset with a new seed). This is the classic Gumin "Simple Tiled Model".
//!
//! Tiles are defined by 4 edge codes [N, E, S, W]; two horizontally-adjacent
//! tiles A (left) and B (right) are compatible iff A.east == B.west, etc. A
//! self-contained xorshift RNG keeps runs deterministic per seed.

use core::slice;

#[derive(Clone, Copy)]
struct Tile {
    edges: [u8; 4], // N, E, S, W  edge codes
    weight: f32,
    color: [f32; 3],
}

pub struct Wfc {
    w: i32,
    h: i32,
    tiles: Vec<Tile>,
    // per-cell possibility bitmask (u64 => up to 64 tiles) and collapsed id
    poss: Vec<u64>,
    collapsed: Vec<i32>, // -1 until fixed
    rng: u64,
    remaining: i32, // uncollapsed cells
    contradiction: bool,
}

#[inline]
fn xs(s: &mut u64) -> u64 {
    let mut x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    x
}
#[inline]
fn rf(s: &mut u64) -> f32 {
    (xs(s) >> 40) as f32 * (1.0 / 16_777_216.0)
}

// direction index: 0=N,1=E,2=S,3=W ; opposite = (d+2)%4
const DX: [i32; 4] = [0, 1, 0, -1];
const DY: [i32; 4] = [-1, 0, 1, 0];

/// Build one of the three built-in tile sets. Edge codes are small integers;
/// tiles are compatible across a shared border when the touching edge codes are
/// equal. Code 0 = "empty/wall", others = connection types.
fn make_tiles(ruleset: i32) -> Vec<Tile> {
    // helper edge patterns: connection present (1) or absent (0) per side.
    // We encode each side as 0 (no connection) or 1 (connection) and require
    // equality across borders, so a connection only meets a connection.
    let mut t = Vec::new();
    match ruleset {
        // ---- 0: PIPES (blank + straight + elbows + tee + cross) ----
        0 => {
            let c_pipe = [0.2f32, 0.8, 0.9];
            let c_bg = [0.03f32, 0.05, 0.08];
            // blank  -  LOW weight so pipes dominate and form long connected runs
            // (a high blank weight fragments the network into isolated dots).
            t.push(Tile { edges: [0, 0, 0, 0], weight: 0.25, color: c_bg });
            // straight N-S and E-W  -  favored, so pipes tend to run straight
            t.push(Tile { edges: [1, 0, 1, 0], weight: 2.4, color: c_pipe });
            t.push(Tile { edges: [0, 1, 0, 1], weight: 2.4, color: c_pipe });
            // four elbows
            t.push(Tile { edges: [1, 1, 0, 0], weight: 1.4, color: c_pipe });
            t.push(Tile { edges: [0, 1, 1, 0], weight: 1.4, color: c_pipe });
            t.push(Tile { edges: [0, 0, 1, 1], weight: 1.4, color: c_pipe });
            t.push(Tile { edges: [1, 0, 0, 1], weight: 1.4, color: c_pipe });
            // cross
            t.push(Tile { edges: [1, 1, 1, 1], weight: 0.8, color: [0.9, 0.5, 0.2] });
        }
        // ---- 1: CIRCUIT (wires on a substrate; connections must line up) ----
        1 => {
            let sub = [0.02f32, 0.12, 0.05];
            let wire = [0.4f32, 1.0, 0.5];
            let node = [1.0f32, 0.85, 0.2];
            t.push(Tile { edges: [0, 0, 0, 0], weight: 1.5, color: sub });
            t.push(Tile { edges: [2, 0, 2, 0], weight: 1.0, color: wire }); // vertical wire
            t.push(Tile { edges: [0, 2, 0, 2], weight: 1.0, color: wire }); // horizontal wire
            t.push(Tile { edges: [2, 2, 2, 2], weight: 0.4, color: node }); // junction
            t.push(Tile { edges: [2, 2, 0, 0], weight: 0.7, color: wire });
            t.push(Tile { edges: [0, 0, 2, 2], weight: 0.7, color: wire });
        }
        // ---- 2: MAZE (corridors) ----
        _ => {
            let wall = [0.05f32, 0.05, 0.1];
            let path = [0.85f32, 0.8, 0.6];
            t.push(Tile { edges: [0, 0, 0, 0], weight: 0.6, color: wall });
            t.push(Tile { edges: [3, 0, 3, 0], weight: 1.0, color: path });
            t.push(Tile { edges: [0, 3, 0, 3], weight: 1.0, color: path });
            t.push(Tile { edges: [3, 3, 0, 0], weight: 1.0, color: path });
            t.push(Tile { edges: [0, 3, 3, 0], weight: 1.0, color: path });
            t.push(Tile { edges: [0, 0, 3, 3], weight: 1.0, color: path });
            t.push(Tile { edges: [3, 0, 0, 3], weight: 1.0, color: path });
            t.push(Tile { edges: [3, 3, 3, 0], weight: 0.5, color: path });
            t.push(Tile { edges: [3, 3, 3, 3], weight: 0.3, color: [0.9, 0.6, 0.3] });
        }
    }
    t
}

impl Wfc {
    fn new(w: i32, h: i32, ruleset: i32, seed: u64) -> Box<Wfc> {
        let tiles = make_tiles(ruleset);
        let n = (w * h) as usize;
        let full: u64 = if tiles.len() >= 64 { u64::MAX } else { (1u64 << tiles.len()) - 1 };
        Box::new(Wfc {
            w,
            h,
            tiles,
            poss: vec![full; n],
            collapsed: vec![-1; n],
            rng: seed | 1,
            remaining: w * h,
            contradiction: false,
        })
    }

    fn reset(&mut self, seed: u64) {
        let full: u64 = if self.tiles.len() >= 64 { u64::MAX } else { (1u64 << self.tiles.len()) - 1 };
        for p in self.poss.iter_mut() {
            *p = full;
        }
        for c in self.collapsed.iter_mut() {
            *c = -1;
        }
        self.rng = seed | 1;
        self.remaining = self.w * self.h;
        self.contradiction = false;
    }

    #[inline]
    fn idx(&self, x: i32, y: i32) -> usize {
        (y * self.w + x) as usize
    }

    /// Does tile `a` on side `dir` allow tile `b` on the other side?
    #[inline]
    fn compatible(&self, a: usize, b: usize, dir: usize) -> bool {
        // a's edge on `dir` must equal b's edge on opposite side
        self.tiles[a].edges[dir] == self.tiles[b].edges[(dir + 2) % 4]
    }

    fn entropy_count(&self, i: usize) -> u32 {
        self.poss[i].count_ones()
    }

    /// Find the uncollapsed cell with the fewest possibilities (>1).
    fn lowest_entropy(&mut self) -> i32 {
        let mut best = -1i32;
        let mut best_e = u32::MAX;
        let n = (self.w * self.h) as usize;
        for i in 0..n {
            if self.collapsed[i] >= 0 {
                continue;
            }
            let e = self.entropy_count(i);
            if e == 0 {
                self.contradiction = true;
                return -1;
            }
            // add tiny noise so ties don't bias toward index order
            let noisy = e as f32 + rf(&mut self.rng) * 0.5;
            if (noisy as u32) < best_e || (best == -1) {
                if e < best_e || best == -1 {
                    best_e = e;
                    best = i as i32;
                }
            }
        }
        best
    }

    fn collapse_cell(&mut self, i: usize) {
        // choose a tile among the possibilities, weighted
        let mask = self.poss[i];
        let mut total = 0.0f32;
        for t in 0..self.tiles.len() {
            if mask & (1 << t) != 0 {
                total += self.tiles[t].weight;
            }
        }
        let mut r = rf(&mut self.rng) * total;
        let mut chosen = 0usize;
        for t in 0..self.tiles.len() {
            if mask & (1 << t) != 0 {
                r -= self.tiles[t].weight;
                if r <= 0.0 {
                    chosen = t;
                    break;
                }
                chosen = t;
            }
        }
        self.poss[i] = 1 << chosen;
        self.collapsed[i] = chosen as i32;
        self.remaining -= 1;
    }

    /// Propagate constraints from cell `start` outward until stable.
    fn propagate(&mut self, start: usize) {
        let mut stack = vec![start];
        while let Some(ci) = stack.pop() {
            let cx = (ci as i32) % self.w;
            let cy = (ci as i32) / self.w;
            let cur = self.poss[ci];
            for dir in 0..4usize {
                let nx = cx + DX[dir];
                let ny = cy + DY[dir];
                if nx < 0 || ny < 0 || nx >= self.w || ny >= self.h {
                    continue;
                }
                let ni = self.idx(nx, ny);
                if self.collapsed[ni] >= 0 {
                    continue;
                }
                // allowed tiles in neighbour = those compatible with SOME current tile
                let mut allowed: u64 = 0;
                let nposs = self.poss[ni];
                for b in 0..self.tiles.len() {
                    if nposs & (1 << b) == 0 {
                        continue;
                    }
                    // is there an a in cur that permits b across `dir`?
                    let mut ok = false;
                    for a in 0..self.tiles.len() {
                        if cur & (1 << a) != 0 && self.compatible(a, b, dir) {
                            ok = true;
                            break;
                        }
                    }
                    if ok {
                        allowed |= 1 << b;
                    }
                }
                if allowed != nposs {
                    self.poss[ni] = allowed;
                    if allowed == 0 {
                        self.contradiction = true;
                        return;
                    }
                    // if this collapsed the neighbour to one, mark it
                    if allowed.count_ones() == 1 && self.collapsed[ni] < 0 {
                        self.collapsed[ni] = allowed.trailing_zeros() as i32;
                        self.remaining -= 1;
                    }
                    stack.push(ni);
                }
            }
        }
    }

    fn step(&mut self, steps: i32) -> i32 {
        if self.contradiction {
            return -1;
        }
        for _ in 0..steps {
            if self.remaining <= 0 {
                return 1;
            }
            let cell = self.lowest_entropy();
            if self.contradiction || cell < 0 {
                return if self.remaining <= 0 { 1 } else { -1 };
            }
            let ci = cell as usize;
            self.collapse_cell(ci);
            self.propagate(ci);
            if self.contradiction {
                return -1;
            }
        }
        if self.remaining <= 0 {
            1
        } else {
            0
        }
    }
}

// ---------------- C ABI ----------------

#[no_mangle]
pub extern "C" fn rust_wfc_create(w: i32, h: i32, ruleset: i32, seed: u64) -> *mut Wfc {
    if w < 2 || h < 2 {
        return core::ptr::null_mut();
    }
    Box::into_raw(Wfc::new(w, h, ruleset, seed))
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_destroy(g: *mut Wfc) {
    if !g.is_null() {
        drop(Box::from_raw(g));
    }
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_reset(g: *mut Wfc, seed: u64) {
    if !g.is_null() {
        (&mut *g).reset(seed);
    }
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_step(g: *mut Wfc, steps: i32) -> i32 {
    if g.is_null() {
        return -1;
    }
    (&mut *g).step(steps.max(1))
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_tiles(g: *const Wfc, out: *mut i32) {
    if g.is_null() || out.is_null() {
        return;
    }
    let g = &*g;
    let n = (g.w * g.h) as usize;
    let o = slice::from_raw_parts_mut(out, n);
    o.copy_from_slice(&g.collapsed);
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_ntiles(g: *const Wfc) -> i32 {
    if g.is_null() { 0 } else { (&*g).tiles.len() as i32 }
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_width(g: *const Wfc) -> i32 {
    if g.is_null() { 0 } else { (&*g).w }
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_height(g: *const Wfc) -> i32 {
    if g.is_null() { 0 } else { (&*g).h }
}
#[no_mangle]
pub unsafe extern "C" fn rust_wfc_palette(g: *const Wfc, out_rgb: *mut f32) {
    if g.is_null() || out_rgb.is_null() {
        return;
    }
    let g = &*g;
    let o = slice::from_raw_parts_mut(out_rgb, g.tiles.len() * 3);
    for (t, tile) in g.tiles.iter().enumerate() {
        o[t * 3] = tile.color[0];
        o[t * 3 + 1] = tile.color[1];
        o[t * 3 + 2] = tile.color[2];
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    unsafe fn solve(rules: i32, w: i32, h: i32, seed: u64) -> (*mut Wfc, i32) {
        let g = rust_wfc_create(w, h, rules, seed);
        let mut status = 0;
        // bounded attempts: a solve may hit contradictions; retry a few seeds
        for attempt in 0..200 {
            status = rust_wfc_step(g, w * h * 4);
            if status == 1 {
                break;
            }
            if status == -1 {
                rust_wfc_reset(g, seed + attempt as u64 + 1);
            }
        }
        (g, status)
    }

    #[test]
    fn solves_to_completion() {
        unsafe {
            for rules in 0..3 {
                let (g, status) = solve(rules, 16, 16, 12345);
                assert_eq!(status, 1, "ruleset {rules} should solve");
                // every cell collapsed to a valid tile id
                let n = (16 * 16) as usize;
                let mut tiles = vec![0i32; n];
                rust_wfc_tiles(g, tiles.as_mut_ptr());
                let nt = rust_wfc_ntiles(g);
                for &t in &tiles {
                    assert!(t >= 0 && t < nt, "tile id {t} out of range");
                }
                rust_wfc_destroy(g);
            }
        }
    }

    #[test]
    fn solution_respects_adjacency() {
        unsafe {
            let (g, status) = solve(0, 20, 14, 999);
            assert_eq!(status, 1);
            let gg = &*g;
            let mut violations = 0;
            for y in 0..gg.h {
                for x in 0..gg.w {
                    let a = gg.collapsed[gg.idx(x, y)] as usize;
                    // check east neighbour
                    if x + 1 < gg.w {
                        let b = gg.collapsed[gg.idx(x + 1, y)] as usize;
                        if !gg.compatible(a, b, 1) {
                            violations += 1;
                        }
                    }
                    // check south neighbour
                    if y + 1 < gg.h {
                        let b = gg.collapsed[gg.idx(x, y + 1)] as usize;
                        if !gg.compatible(a, b, 2) {
                            violations += 1;
                        }
                    }
                }
            }
            assert_eq!(violations, 0, "adjacency constraints must hold in the solution");
            rust_wfc_destroy(g);
        }
    }

    #[test]
    fn determinism() {
        unsafe {
            let (a, sa) = solve(1, 12, 12, 7);
            let (b, sb) = solve(1, 12, 12, 7);
            assert_eq!(sa, sb);
            if sa == 1 {
                let n = 144;
                let mut ta = vec![0i32; n];
                let mut tb = vec![0i32; n];
                rust_wfc_tiles(a, ta.as_mut_ptr());
                rust_wfc_tiles(b, tb.as_mut_ptr());
                assert_eq!(ta, tb, "same seed -> same solution");
            }
            rust_wfc_destroy(a);
            rust_wfc_destroy(b);
        }
    }
}
