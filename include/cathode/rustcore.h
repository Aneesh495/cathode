/* ==========================================================================
 * cathode/rustcore.h — C ABI exposed by the Rust compute core (rustsrc/).
 *
 * FROZEN CONTRACT. The Rust side (rustsrc) implements exactly these
 * symbols with #[no_mangle] pub extern "C". The C/C++ side calls them through
 * this header. All types are C-layout POD; no Rust types cross the boundary.
 *
 * Memory ownership rules (critical for a safe FFI boundary):
 *   * Any pointer returned by a *_create() is owned by Rust and must be freed
 *     by the matching *_destroy(). Never free() it from C.
 *   * Buffers passed IN (const T*) are borrowed for the duration of the call
 *     only; Rust must not retain them.
 *   * Buffers passed OUT (T* out) are owned by C; Rust writes into them and
 *     must respect the stated length.
 * ========================================================================== */
#ifndef CATHODE_RUSTCORE_H
#define CATHODE_RUSTCORE_H

#include "cathode/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sanity / version ---- */
u32  rust_core_abi_version(void);   /* must equal CATHODE_RUST_ABI below */
#define CATHODE_RUST_ABI 7u

/* ================= high-precision NTSC colorimetry ================= *
 * Reference f64 implementations used to validate the f32 NEON DSP path
 * and to generate lookup tables. rgb/yiq are interleaved, n pixels. */
void rust_rgb2yiq_f64(f64 *out, const f64 *rgb, u64 n);
void rust_yiq2rgb_f64(f64 *out, const f64 *yiq, u64 n);
/* sRGB<->linear transfer, single channel arrays. */
void rust_srgb_to_linear(f32 *out, const f32 *in, u64 n);
void rust_linear_to_srgb(f32 *out, const f32 *in, u64 n);
/* full-frame Reinhard-Jodie tonemap + sRGB, linear rgb -> u8 rgb. */
void rust_tonemap_frame(u8 *out8, const f32 *lin_rgb, u64 npx, f32 exposure);

/* ================= Gray-Scott reaction-diffusion ================= *
 * A classic two-chemical PDE that self-organizes into spots, stripes,
 * mitosis patterns. Grid is w*h; render maps chemical V to Color3. */
typedef struct RustReactionDiffusion RustReactionDiffusion;
RustReactionDiffusion *rust_rd_create(i32 w, i32 h, f32 feed, f32 kill, f32 du, f32 dv);
void rust_rd_destroy(RustReactionDiffusion *rd);
void rust_rd_seed_random(RustReactionDiffusion *rd, u64 seed, f32 density);
void rust_rd_seed_point(RustReactionDiffusion *rd, i32 x, i32 y, i32 radius);
void rust_rd_set_params(RustReactionDiffusion *rd, f32 feed, f32 kill, f32 du, f32 dv);
void rust_rd_step(RustReactionDiffusion *rd, i32 substeps);
/* write V field (0..1) into out (w*h floats) */
void rust_rd_field(const RustReactionDiffusion *rd, f32 *out_v);
i32  rust_rd_width(const RustReactionDiffusion *rd);
i32  rust_rd_height(const RustReactionDiffusion *rd);

/* ================= Verlet cloth / soft-body ================= *
 * A grid of point masses connected by distance constraints, integrated with
 * position-based Verlet + constraint relaxation. Great for a waving flag/cloth
 * under gravity and wind. Positions are Vec3 (x,y,z). */
typedef struct RustCloth RustCloth;
RustCloth *rust_cloth_create(i32 nx, i32 ny, f32 spacing);
void rust_cloth_destroy(RustCloth *c);
void rust_cloth_pin(RustCloth *c, i32 ix, i32 iy);            /* fix a node */
void rust_cloth_set_wind(RustCloth *c, f32 wx, f32 wy, f32 wz);
void rust_cloth_set_gravity(RustCloth *c, f32 g);
void rust_cloth_step(RustCloth *c, f32 dt, i32 relax_iters);
i32  rust_cloth_node_count(const RustCloth *c);
/* copy node positions into out (node_count * 3 floats) */
void rust_cloth_positions(const RustCloth *c, f32 *out_xyz);
/* copy triangle indices (2 tris per quad); returns triangle count, writes
 * up to max_tris*3 u32 indices into out_idx. */
i32  rust_cloth_indices(const RustCloth *c, u32 *out_idx, i32 max_tris);

/* ================= strange attractors ================= *
 * Integrate a chaotic ODE (Lorenz / Aizawa / Thomas / Halvorsen) and emit
 * a stream of 3D points tracing the attractor. Returns points written. */
typedef enum { ATTR_LORENZ=0, ATTR_AIZAWA=1, ATTR_THOMAS=2, ATTR_HALVORSEN=3 } RustAttractor;
i32  rust_attractor_generate(RustAttractor kind, Vec3 start, f32 dt, i32 count,
                             Vec3 *out_pts, i32 max_pts);

/* ================= Wang-tile / value-noise terrain field ================ *
 * Deterministic multi-fractal heightfield sampler (f64 internally for large
 * coordinates). Fills out (nx*ny) with heights for a tile at (ox,oy). */
void rust_terrain_tile(f32 *out, i32 nx, i32 ny, f64 ox, f64 oy, f64 scale,
                       i32 octaves, u64 seed);

/* ================= diffusion-limited aggregation (DLA) ================= *
 * Crystal / coral growth: random-walker particles stick when they touch the
 * growing cluster, producing fractal dendrites. The grid stores the "age" a
 * cell was added (0 = seed, increasing outward), which scenes map to a color
 * ramp so the growth history is visible. */
typedef struct RustDLA RustDLA;
RustDLA *rust_dla_create(i32 w, i32 h, u64 seed);
void     rust_dla_destroy(RustDLA *d);
void     rust_dla_seed_center(RustDLA *d);          /* single seed at center */
void     rust_dla_seed_line(RustDLA *d, i32 y);     /* seed a whole row (frost) */
/* Grow up to `walkers` more particles; returns how many actually stuck (may be
 * fewer if the cluster reached the border). Bounded work per call. */
i32      rust_dla_grow(RustDLA *d, i32 walkers);
/* Write the per-cell age field into out (w*h floats): -1 = empty, else the
 * normalized age in [0,1] (0 oldest .. 1 newest). */
void     rust_dla_field(const RustDLA *d, f32 *out_age);
i32      rust_dla_width(const RustDLA *d);
i32      rust_dla_height(const RustDLA *d);
i32      rust_dla_count(const RustDLA *d);           /* particles in cluster */

/* ================= Wave Function Collapse (WFC) ================= *
 * Constraint-based procedural generation: a grid where each cell starts as a
 * superposition of all tile types, repeatedly collapsed (pick the lowest-
 * entropy cell, fix it to one tile weighted by frequency) with adjacency
 * constraints propagated outward. Produces coherent patterns (pipes, circuits,
 * mazes) from local rules. This uses a built-in "overlapping-edges" tile set
 * chosen by `ruleset`. Watch it solve cell-by-cell in real time.
 *
 * ruleset: 0 = pipes, 1 = circuit, 2 = maze. */
typedef struct RustWFC RustWFC;
RustWFC *rust_wfc_create(i32 w, i32 h, i32 ruleset, u64 seed);
void     rust_wfc_destroy(RustWFC *g);
void     rust_wfc_reset(RustWFC *g, u64 seed);
/* Advance the solver by up to `steps` collapses. Returns 1 if fully solved,
 * 0 if still working, -1 on contradiction (caller may reset). */
i32      rust_wfc_step(RustWFC *g, i32 steps);
/* Write the current per-cell tile id into out (w*h ints): -1 = not yet
 * collapsed, else 0..ntiles-1. */
void     rust_wfc_tiles(const RustWFC *g, i32 *out);
i32      rust_wfc_ntiles(const RustWFC *g);
i32      rust_wfc_width(const RustWFC *g);
i32      rust_wfc_height(const RustWFC *g);
/* Fill out_rgb (ntiles*3 floats) with a display color per tile id. */
void     rust_wfc_palette(const RustWFC *g, f32 *out_rgb);

/* ================= FFT (radix-2 Cooley–Tukey, in-place) ================= *
 * Power-of-two real->complex magnitude spectrum, computed with an iterative
 * radix-2 FFT (bit-reversal permutation + butterflies). `n` must be a power of
 * two. `in` is n real samples; `out_mag` receives the first n/2 magnitudes
 * (the useful half up to Nyquist). A Hann window is applied to reduce leakage.
 * Much faster and cleaner than an O(n^2) DFT for the audio visualizers. */
void rust_fft_mag(f32 *out_mag, const f32 *in, u32 n);
/* Full complex FFT in place (interleaved re,im pairs, 2*n floats). inverse!=0
 * computes the inverse transform (with 1/n scaling). n must be power of two. */
void rust_fft_complex(f32 *data, u32 n, i32 inverse);

/* ================= Maze (recursive-backtracker + BFS solve) ============= *
 * A "perfect" maze (a spanning tree of the w×h grid) generated by the
 * recursive-backtracker, then solved start(0)→goal(w*h-1) by BFS. The host can
 * read per-cell open-wall bitmasks (bit0=N,1=E,2=S,3=W set when carved open),
 * the BFS distance field (for an animated flood-fill), and the unique shortest
 * path. Opaque handle; all data crosses as plain arrays. */
typedef struct RustMaze RustMaze;
RustMaze *rust_maze_create(i32 w, i32 h, u64 seed);
void      rust_maze_destroy(RustMaze *m);
void      rust_maze_reset(RustMaze *m, u64 seed);      /* regen + resolve */
i32       rust_maze_width(const RustMaze *m);
i32       rust_maze_height(const RustMaze *m);
i32       rust_maze_max_dist(const RustMaze *m);        /* largest BFS distance */
void      rust_maze_walls(const RustMaze *m, u8 *out);  /* w*h bytes, wall bits */
void      rust_maze_dist(const RustMaze *m, i32 *out);  /* w*h ints, -1=unreached */
/* Shortest start→goal path as cell indices (start..goal) into out[max]; returns
 * the number written. */
i32       rust_maze_path(const RustMaze *m, i32 *out, i32 max);

/* ================= L-system (turtle-graphics plant generator) ========== *
 * A Lindenmayer system: rewrite an axiom by production rules for N iterations,
 * then interpret the string as turtle graphics into 2D line segments. Built-in
 * presets (fractal plant, Koch, dragon, Sierpinski, bushy tree) selected by
 * index. Segments are normalized to ~[0,1]^2; each carries the branch depth it
 * was drawn at so the host can color by depth. Opaque handle. */
typedef struct { f32 x0, y0, x1, y1; i32 depth; } RustLSeg;
typedef struct RustLSystem RustLSystem;
RustLSystem *rust_lsystem_create(i32 preset);
void         rust_lsystem_destroy(RustLSystem *l);
i32          rust_lsystem_preset_count(void);
i32          rust_lsystem_nsegs(const RustLSystem *l);
i32          rust_lsystem_max_depth(const RustLSystem *l);
/* Copy up to `max` segments into out[]; returns the count copied. */
i32          rust_lsystem_segs(const RustLSystem *l, RustLSeg *out, i32 max);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_RUSTCORE_H */
