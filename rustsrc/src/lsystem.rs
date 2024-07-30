//! lsystem.rs — Lindenmayer-system turtle-graphics generator, over the C ABI.
//!
//! An L-system rewrites a start string (the *axiom*) by simultaneously applying
//! production rules for a number of iterations, then interprets the resulting
//! string as **turtle graphics** to produce 2D line segments. This is the
//! classic procedural-botany technique: a handful of rules generate plants,
//! bushes, and space-filling fractal curves.
//!
//! Turtle alphabet:
//!   F, G  move forward one step, drawing a segment
//!   f     move forward one step WITHOUT drawing
//!   +     turn left by the system angle
//!   -     turn right by the system angle
//!   [     push turtle state (position, heading, depth)
//!   ]     pop turtle state
//!   |     reverse direction (turn 180°)
//! Other letters are treated as no-ops that only expand via rules (common for
//! auxiliary symbols like X in the classic fractal plant).
//!
//! We ship a set of built-in presets (fractal plant, Koch curve, dragon,
//! Sierpinski, a bushy tree) selected by index, each with its own axiom, rules,
//! angle, and iteration count. Output is a flat array of segments; each segment
//! is (x0,y0,x1,y1, depth) so the host can color by branch depth. Coordinates
//! are normalized to roughly [0,1]×[0,1] (the host scales to the screen).

use core::slice;

/// One drawn segment plus the branching depth it was emitted at.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LSeg {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    pub depth: i32,
}

pub struct LSystem {
    segs: Vec<LSeg>,
    max_depth: i32,
}

struct Preset {
    axiom: &'static str,
    // rules as (symbol, replacement) pairs
    rules: &'static [(char, &'static str)],
    angle_deg: f32,
    iters: u32,
}

const PRESETS: &[Preset] = &[
    // 0: classic fractal plant (Prusinkiewicz), grows upward, bushy
    Preset {
        axiom: "X",
        rules: &[('X', "F+[[X]-X]-F[-FX]+X"), ('F', "FF")],
        angle_deg: 25.0,
        iters: 6,
    },
    // 1: Koch snowflake curve
    Preset {
        axiom: "F--F--F",
        rules: &[('F', "F+F--F+F")],
        angle_deg: 60.0,
        iters: 4,
    },
    // 2: dragon curve
    Preset {
        axiom: "FX",
        rules: &[('X', "X+YF+"), ('Y', "-FX-Y")],
        angle_deg: 90.0,
        iters: 12,
    },
    // 3: Sierpinski arrowhead
    Preset {
        axiom: "F",
        rules: &[('F', "G-F-G"), ('G', "F+G+F")],
        angle_deg: 60.0,
        iters: 7,
    },
    // 4: bushy symmetric tree
    Preset {
        axiom: "F",
        rules: &[('F', "FF+[+F-F-F]-[-F+F+F]")],
        angle_deg: 22.5,
        iters: 4,
    },
];

fn expand(preset: &Preset) -> String {
    let mut s = String::from(preset.axiom);
    for _ in 0..preset.iters {
        let mut next = String::with_capacity(s.len() * 3);
        for c in s.chars() {
            let mut replaced = false;
            for &(sym, rep) in preset.rules {
                if sym == c {
                    next.push_str(rep);
                    replaced = true;
                    break;
                }
            }
            if !replaced {
                next.push(c);
            }
            // guard against runaway strings on high iteration counts
            if next.len() > 4_000_000 {
                return next;
            }
        }
        s = next;
    }
    s
}

struct Turtle {
    x: f32,
    y: f32,
    ang: f32, // radians, 0 = +x, grows CCW
    depth: i32,
}

fn interpret(s: &str, angle_deg: f32) -> (Vec<LSeg>, i32) {
    let a = angle_deg.to_radians();
    let step = 1.0f32;
    let mut t = Turtle { x: 0.0, y: 0.0, ang: std::f32::consts::FRAC_PI_2, depth: 0 };
    let mut stack: Vec<Turtle> = Vec::new();
    let mut segs: Vec<LSeg> = Vec::new();
    let mut max_depth = 0;
    for c in s.chars() {
        match c {
            'F' | 'G' => {
                let nx = t.x + step * t.ang.cos();
                let ny = t.y + step * t.ang.sin();
                segs.push(LSeg { x0: t.x, y0: t.y, x1: nx, y1: ny, depth: t.depth });
                t.x = nx;
                t.y = ny;
            }
            'f' => {
                t.x += step * t.ang.cos();
                t.y += step * t.ang.sin();
            }
            '+' => t.ang += a,
            '-' => t.ang -= a,
            '|' => t.ang += std::f32::consts::PI,
            '[' => {
                stack.push(Turtle { x: t.x, y: t.y, ang: t.ang, depth: t.depth });
                t.depth += 1;
                if t.depth > max_depth {
                    max_depth = t.depth;
                }
            }
            ']' => {
                if let Some(p) = stack.pop() {
                    t = p;
                }
            }
            _ => {}
        }
    }
    (segs, max_depth)
}

// Normalize segment coords into [0,1]^2 preserving aspect (fit + center).
fn normalize(segs: &mut [LSeg]) {
    if segs.is_empty() {
        return;
    }
    let mut minx = f32::INFINITY;
    let mut miny = f32::INFINITY;
    let mut maxx = f32::NEG_INFINITY;
    let mut maxy = f32::NEG_INFINITY;
    for s in segs.iter() {
        minx = minx.min(s.x0).min(s.x1);
        miny = miny.min(s.y0).min(s.y1);
        maxx = maxx.max(s.x0).max(s.x1);
        maxy = maxy.max(s.y0).max(s.y1);
    }
    let w = (maxx - minx).max(1e-6);
    let h = (maxy - miny).max(1e-6);
    let scale = 1.0 / w.max(h);
    // center within the unit square
    let ox = (1.0 - w * scale) * 0.5;
    let oy = (1.0 - h * scale) * 0.5;
    for s in segs.iter_mut() {
        s.x0 = (s.x0 - minx) * scale + ox;
        s.y0 = (s.y0 - miny) * scale + oy;
        s.x1 = (s.x1 - minx) * scale + ox;
        s.y1 = (s.y1 - miny) * scale + oy;
    }
}

impl LSystem {
    fn new(preset_idx: i32) -> Box<LSystem> {
        let idx = if preset_idx < 0 || preset_idx as usize >= PRESETS.len() {
            0
        } else {
            preset_idx as usize
        };
        let p = &PRESETS[idx];
        let s = expand(p);
        let (mut segs, max_depth) = interpret(&s, p.angle_deg);
        normalize(&mut segs);
        Box::new(LSystem { segs, max_depth })
    }
}

#[no_mangle]
pub extern "C" fn rust_lsystem_create(preset: i32) -> *mut LSystem {
    Box::into_raw(LSystem::new(preset))
}

#[no_mangle]
pub unsafe extern "C" fn rust_lsystem_destroy(l: *mut LSystem) {
    if !l.is_null() {
        drop(Box::from_raw(l));
    }
}

#[no_mangle]
pub extern "C" fn rust_lsystem_preset_count() -> i32 {
    PRESETS.len() as i32
}

#[no_mangle]
pub unsafe extern "C" fn rust_lsystem_nsegs(l: *const LSystem) -> i32 {
    if l.is_null() { 0 } else { (&*l).segs.len() as i32 }
}

#[no_mangle]
pub unsafe extern "C" fn rust_lsystem_max_depth(l: *const LSystem) -> i32 {
    if l.is_null() { 0 } else { (&*l).max_depth }
}

/// Copy up to `max` segments into `out`; returns the number copied.
#[no_mangle]
pub unsafe extern "C" fn rust_lsystem_segs(l: *const LSystem, out: *mut LSeg, max: i32) -> i32 {
    if l.is_null() || out.is_null() || max <= 0 {
        return 0;
    }
    let l = &*l;
    let n = l.segs.len().min(max as usize);
    let o = slice::from_raw_parts_mut(out, n);
    o.copy_from_slice(&l.segs[..n]);
    n as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn presets_produce_segments() {
        for i in 0..PRESETS.len() as i32 {
            let l = LSystem::new(i);
            assert!(!l.segs.is_empty(), "preset {} produced no segments", i);
        }
    }

    #[test]
    fn segments_are_normalized() {
        let l = LSystem::new(0);
        for s in &l.segs {
            assert!(s.x0 >= -0.01 && s.x0 <= 1.01, "x0 out of unit box: {}", s.x0);
            assert!(s.y0 >= -0.01 && s.y0 <= 1.01, "y0 out of unit box: {}", s.y0);
            assert!(s.x1 >= -0.01 && s.x1 <= 1.01);
            assert!(s.y1 >= -0.01 && s.y1 <= 1.01);
        }
    }

    #[test]
    fn fractal_plant_has_branches() {
        // the plant preset uses [ ] so it must reach depth > 0
        let l = LSystem::new(0);
        assert!(l.max_depth > 0, "fractal plant should branch");
    }

    #[test]
    fn koch_is_a_single_path() {
        // the Koch curve has no brackets => depth stays 0
        let l = LSystem::new(1);
        assert_eq!(l.max_depth, 0, "Koch curve should not branch");
    }

    #[test]
    fn out_of_range_preset_falls_back() {
        let l = LSystem::new(999);
        assert!(!l.segs.is_empty());
    }

    #[test]
    fn copy_respects_capacity() {
        let l = LSystem::new(2); // dragon
        let total = l.segs.len();
        let cap = (total / 2).max(1);
        let mut buf = vec![
            LSeg { x0: 0.0, y0: 0.0, x1: 0.0, y1: 0.0, depth: 0 };
            cap
        ];
        let n = unsafe { rust_lsystem_segs(&*l as *const LSystem, buf.as_mut_ptr(), cap as i32) };
        assert_eq!(n as usize, cap, "must copy exactly capacity when truncating");
    }
}
