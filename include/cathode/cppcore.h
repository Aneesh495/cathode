/* ==========================================================================
 * cathode/cppcore.h — C ABI exposed by the C++20 subsystems (src/cpp/).
 *
 * FROZEN CONTRACT. The C++ side uses templates/RAII/STL internally but exposes
 * only these extern "C" POD functions. Opaque handles hide C++ objects.
 * ========================================================================== */
#ifndef CATHODE_CPPCORE_H
#define CATHODE_CPPCORE_H

#include "cathode/types.h"
#include "cathode/framebuffer.h"

#ifdef __cplusplus
extern "C" {
#endif

u32 cpp_core_abi_version(void);
#define CATHODE_CPP_ABI 6u

/* ================= BVH path tracer ================= *
 * A physically-based Monte-Carlo path tracer over a scene of spheres and
 * planes, accelerated by a bounding-volume hierarchy. Renders into a linear
 * RGB Framebuffer with progressive accumulation. */
typedef struct CppTracer CppTracer;

typedef enum { CPPMAT_LAMBERT=0, CPPMAT_METAL=1, CPPMAT_DIELECTRIC=2, CPPMAT_EMISSIVE=3 } CppMatKind;

CppTracer *cpp_tracer_create(i32 w, i32 h);
void cpp_tracer_destroy(CppTracer *t);
void cpp_tracer_clear_scene(CppTracer *t);
/* add a sphere; material params: albedo rgb, fuzz/ior in `param`, emission scale */
void cpp_tracer_add_sphere(CppTracer *t, Vec3 center, f32 radius,
                           CppMatKind mat, Color3 albedo, f32 param);
void cpp_tracer_add_plane(CppTracer *t, Vec3 point, Vec3 normal,
                          CppMatKind mat, Color3 albedo, f32 param);
void cpp_tracer_set_camera(CppTracer *t, Vec3 eye, Vec3 target, f32 fov_rad, f32 aperture);
void cpp_tracer_set_sky(CppTracer *t, Color3 top, Color3 bottom);
void cpp_tracer_build(CppTracer *t);   /* build BVH after adding geometry */
/* accumulate `spp` samples-per-pixel into the internal HDR buffer; call each
 * frame for progressive refinement, or reset first for a fresh image. */
void cpp_tracer_reset_accum(CppTracer *t);
void cpp_tracer_render(CppTracer *t, i32 spp, i32 max_bounces);
/* resolve accumulation into the framebuffer (averaged, still linear HDR). */
void cpp_tracer_resolve(CppTracer *t, Framebuffer *fb);
i64  cpp_tracer_rays_cast(const CppTracer *t);   /* stats */

/* ================= real-time audio synth (visual-coupled) ================= *
 * A polyphonic additive/subtractive synth. It does NOT open an audio device
 * (headless-safe); instead it renders sample buffers a scene can turn into a
 * visualizer, and exposes a spectrum for audio-reactive visuals. */
typedef struct CppSynth CppSynth;
CppSynth *cpp_synth_create(i32 sample_rate);
void cpp_synth_destroy(CppSynth *s);
void cpp_synth_note_on(CppSynth *s, i32 midi_note, f32 velocity);
void cpp_synth_note_off(CppSynth *s, i32 midi_note);
void cpp_synth_set_waveform(CppSynth *s, i32 wave); /* 0 sine 1 saw 2 square 3 tri */
void cpp_synth_set_filter(CppSynth *s, f32 cutoff_hz, f32 resonance);
/* render n stereo-interleaved... no: mono samples into out (n floats). */
void cpp_synth_render(CppSynth *s, f32 *out, i32 n);
/* magnitude spectrum (nbins) of the last rendered block, for visualizers. */
void cpp_synth_spectrum(CppSynth *s, f32 *out_mag, i32 nbins);
/* drive a simple generative melody from a seed (auto note on/off). */
void cpp_synth_sequencer_tick(CppSynth *s, f32 dt);

/* Schroeder reverb on the synth output (mono, in the render path). `wet` in
 * [0,1] mixes the reverberated signal; 0 disables it. `roomsize` in [0,1]
 * scales the comb-filter feedback (bigger = longer tail). */
void cpp_synth_set_reverb(CppSynth *s, f32 wet, f32 roomsize);

/* ---- real audio device output (macOS AudioQueue) ----
 * Start streaming a synth's output to the default audio device. The audio
 * callback pulls blocks via cpp_synth_render on a background thread, so the
 * synth must outlive the audio stream. Returns 1 on success, 0 if no device /
 * not built with audio (headless-safe: the visualizer still works silently).
 * cpp_audio_running() reports whether a stream is live. */
int  cpp_audio_start(CppSynth *s, i32 sample_rate);
void cpp_audio_stop(void);
int  cpp_audio_running(void);

/* ================= wireframe scene graph ================= *
 * Retained-mode hierarchy of transformed line-art nodes, flattened and
 * projected into 2D segments a scene can draw. Good for a neon "wireframe
 * city" / vector-display look. */
typedef struct CppSceneGraph CppSceneGraph;
CppSceneGraph *cpp_sg_create(void);
void cpp_sg_destroy(CppSceneGraph *g);
i32  cpp_sg_add_node(CppSceneGraph *g, i32 parent /* -1 root */);
void cpp_sg_set_transform(CppSceneGraph *g, i32 node, Mat4 local);
void cpp_sg_add_box(CppSceneGraph *g, i32 node, Vec3 half, Color3 c);
void cpp_sg_add_grid(CppSceneGraph *g, i32 node, i32 n, f32 size, Color3 c);
/* flatten to world-space line segments; each segment = 2 Vec3 endpoints +
 * a Color3. Writes up to max_segs; returns segment count. */
typedef struct { Vec3 a, b; Color3 color; } CppSegment;
i32  cpp_sg_flatten(CppSceneGraph *g, Mat4 root, CppSegment *out, i32 max_segs);

/* ================= marching cubes isosurface extractor ================= *
 * Turns a scalar field (e.g. a sum of metaballs, or a reaction/Lenia field)
 * into a triangle mesh at a given iso level, via the classic 256-case
 * marching-cubes tables. Output vertices are interpolated on cube edges for a
 * smooth surface, with per-vertex normals from the field gradient. The result
 * feeds straight into the C rasterizer.
 *
 * Field convention: a flat array of nx*ny*nz f32 samples, index
 * f[(z*ny + y)*nx + x], sampled on a unit-spaced grid; the caller places/scales
 * the resulting mesh. */
typedef struct {
    Vec3 pos;
    Vec3 normal;
} CppMcVertex;

/* Extract triangles from `field` at `iso`. Writes up to max_tris*3 vertices
 * (3 per triangle, already normal-shaded) into out_verts; returns the triangle
 * count. Deterministic; no allocation escapes. */
i32 cpp_marching_cubes(const f32 *field, i32 nx, i32 ny, i32 nz, f32 iso,
                       CppMcVertex *out_verts, i32 max_tris);

/* Convenience: fill `field` (nx*ny*nz) with a sum-of-metaballs scalar field for
 * `nballs` moving balls (centers in [0,nx]x[0,ny]x[0,nz], radii in `radii`).
 * Field value at a cell = sum_i radii[i]^2 / dist2(cell, center_i). */
void cpp_metaball_field(f32 *field, i32 nx, i32 ny, i32 nz,
                        const Vec3 *centers, const f32 *radii, i32 nballs);

/* ================= CSG (constructive solid geometry) =================== *
 * A boolean-tree evaluator over signed-distance primitives, sampled into a
 * scalar field that the marching-cubes subsystem turns into a mesh. Build a
 * tree of nodes (leaves are primitives, internal nodes are boolean ops with an
 * optional smooth-blend radius `k`), then evaluate it into a field. The field
 * is stored so that iso=0 is the surface (positive INSIDE the solid), matching
 * marching cubes' convention when you pass iso just above 0. */
typedef enum {
    CSG_SPHERE = 0, CSG_BOX = 1, CSG_CYLINDER = 2,   /* leaf primitives */
    CSG_TORUS = 3,
    CSG_UNION = 100, CSG_INTERSECT = 101, CSG_SUBTRACT = 102  /* operators */
} CsgKind;

typedef struct {
    i32 kind;        /* CsgKind */
    i32 a, b;        /* child node indices for operators (-1 for leaves) */
    Vec3 center;     /* leaf: primitive center (grid coords) */
    Vec3 size;       /* leaf: sphere->(r,_,_), box->half-extents, cyl->(r,h,_),
                      *       torus->(R,r,_) */
    f32  k;          /* operator: smooth-blend radius (0 = hard boolean) */
} CsgNode;

/* Evaluate a CSG tree (`nodes`[0..nnodes), root at index `root`) into `field`
 * (nx*ny*nz), signed so positive = inside. Returns 0 on success, non-zero on a
 * malformed tree (bad indices / cycle guard). */
i32 cpp_csg_eval(f32 *field, i32 nx, i32 ny, i32 nz,
                 const CsgNode *nodes, i32 nnodes, i32 root);

/* ================= pressurized soft-body (2D Verlet blob) ============== *
 * A closed loop of point masses connected by springs, with an ideal-gas
 * pressure force pushing the outline outward (∝ 1/area) — the classic
 * "pressurized soft body" (Matthias Müller). The result is a squishy 2D blob
 * that wobbles, squashes on impact, and holds its volume. Verlet integration
 * with distance-constraint relaxation keeps it stable; it collides with the
 * unit box floor/walls. Domain is [0,w] x [0,h] world units.
 *
 * A scene reads back the ring's world-space points to draw/fill the blob. */
typedef struct CppSoftBody CppSoftBody;
CppSoftBody *cpp_softbody_create(i32 npoints, f32 cx, f32 cy, f32 radius,
                                 f32 w, f32 h);
void  cpp_softbody_destroy(CppSoftBody *b);
void  cpp_softbody_set_gravity(CppSoftBody *b, f32 gx, f32 gy);
void  cpp_softbody_set_pressure(CppSoftBody *b, f32 p);   /* target gas amount */
/* Nudge the whole blob (e.g. to bounce it around). */
void  cpp_softbody_kick(CppSoftBody *b, f32 vx, f32 vy);
void  cpp_softbody_step(CppSoftBody *b, f32 dt, i32 iterations);
i32   cpp_softbody_npoints(const CppSoftBody *b);
/* Copy the ring's world points into out_xy (2*n floats: x0,y0,x1,y1,...). */
void  cpp_softbody_points(const CppSoftBody *b, f32 *out_xy);
f32   cpp_softbody_area(const CppSoftBody *b);   /* current enclosed area */

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_CPPCORE_H */
