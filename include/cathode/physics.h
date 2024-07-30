/* ==========================================================================
 * cathode/physics.h — N-body gravity (Barnes-Hut) + 2D fluid (Navier-Stokes).
 * ========================================================================== */
#ifndef CATHODE_PHYSICS_H
#define CATHODE_PHYSICS_H

#include "cathode/framebuffer.h"
#include "cathode/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- N-body (Barnes-Hut octree, O(N log N)) ---------------- */
typedef struct {
    Vec3  *pos, *vel;
    f32   *mass;
    Color3 *color;
    i32    n, cap;
    f32    theta;    /* Barnes-Hut opening angle (0.5 typical) */
    f32    softening;/* Plummer softening to avoid singularities */
    f32    g;        /* gravitational constant (scaled) */
} NBody;

NBody *nbody_create(i32 capacity);
void   nbody_destroy(NBody *nb);
void   nbody_add(NBody *nb, Vec3 pos, Vec3 vel, f32 mass, Color3 c);
void   nbody_seed_galaxy(NBody *nb, i32 count, Vec3 center, f32 radius, f32 central_mass);
void   nbody_seed_collision(NBody *nb, i32 per_galaxy);
/* Advance one leapfrog step of dt using a freshly built Barnes-Hut tree. */
void   nbody_step(NBody *nb, f32 dt);
/* Project particles to screen via view*proj and additively splat. */
void   nbody_render(const NBody *nb, Framebuffer *fb, Mat4 view, Mat4 proj);

/* ---------------- Fluid (stable semi-Lagrangian, Stam 1999) ------------- */
typedef struct FluidSim FluidSim;
FluidSim *fluid_create(i32 nx, i32 ny);
void      fluid_destroy(FluidSim *f);
void      fluid_add_density(FluidSim *f, i32 x, i32 y, f32 amount, Color3 c);
void      fluid_add_velocity(FluidSim *f, i32 x, i32 y, f32 vx, f32 vy);
void      fluid_step(FluidSim *f, f32 dt, f32 viscosity, f32 diffusion);
void      fluid_render(const FluidSim *f, Framebuffer *fb);
i32       fluid_nx(const FluidSim *f);
i32       fluid_ny(const FluidSim *f);

/* ---------------- SPH (Smoothed Particle Hydrodynamics), 2D ------------- *
 * Lagrangian (particle-based) fluid: each particle carries mass; density and
 * pressure are estimated by smoothing-kernel sums over neighbors, and forces
 * (pressure gradient + viscosity + gravity) integrate the particle motion.
 * Unlike the grid solver, this naturally produces droplets, splashes, and a
 * free surface. A uniform spatial-hash grid keeps neighbor search ~O(N).
 * Simulation domain is [0,w] x [0,h] (world units); particles bounce off it. */
typedef struct SphSim SphSim;
SphSim *sph_create(i32 capacity, f32 w, f32 h);
void    sph_destroy(SphSim *s);
void    sph_add(SphSim *s, f32 x, f32 y);
void    sph_add_block(SphSim *s, f32 x0, f32 y0, f32 x1, f32 y1, f32 spacing);
void    sph_set_gravity(SphSim *s, f32 gx, f32 gy);
void    sph_step(SphSim *s, f32 dt);
i32     sph_count(const SphSim *s);
/* copy particle positions (n*2 floats: x0,y0,x1,y1,...) and, if non-NULL,
 * per-particle speed (n floats) for coloring. */
void    sph_positions(const SphSim *s, f32 *out_xy, f32 *out_speed);
f32     sph_domain_w(const SphSim *s);
f32     sph_domain_h(const SphSim *s);

/* ---------------- 2D rigid-body dynamics (impulse-based) --------------- *
 * A small sequential-impulse rigid-body solver over convex polygons — the
 * classic Box2D-lite recipe: integrate velocities under gravity, detect
 * collisions with SAT (separating-axis test) between convex hulls + the static
 * floor/walls, then resolve each contact with iterated normal + friction
 * impulses (restitution for bounce) and a small positional bias to cure sink.
 * Bodies have linear + angular state; mass/inertia derive from the polygon.
 * Domain is [0,w] x [0,h] world units with a floor at y=0 and side walls.
 *
 * This is a genuine little physics engine: boxes tumble, stack, and settle. */
typedef struct RigidWorld RigidWorld;
RigidWorld *rb_create(f32 w, f32 h, i32 capacity);
void        rb_destroy(RigidWorld *rw);
void        rb_set_gravity(RigidWorld *rw, f32 gx, f32 gy);
/* Add a box of given half-extents at (x,y) with rotation `ang` (radians) and
 * density; returns the body index, or -1 if full. density<=0 makes it static. */
i32         rb_add_box(RigidWorld *rw, f32 x, f32 y, f32 hx, f32 hy,
                       f32 ang, f32 density);
/* Add a regular n-gon (3..8 sides) of circumradius r; returns index or -1. */
i32         rb_add_ngon(RigidWorld *rw, f32 x, f32 y, i32 sides, f32 r,
                        f32 ang, f32 density);
void        rb_step(RigidWorld *rw, f32 dt, i32 iterations);
i32         rb_count(const RigidWorld *rw);
/* Fetch body i's world-space polygon vertices into out_xy (2*out_n floats) and
 * report its vertex count via *out_n (<= maxv). Also returns center (cx,cy) and
 * angle if the pointers are non-NULL. */
void        rb_body_poly(const RigidWorld *rw, i32 i, f32 *out_xy, i32 maxv,
                         i32 *out_n, f32 *cx, f32 *cy, f32 *ang);
f32         rb_total_energy(const RigidWorld *rw);   /* KE (for tests) */
f32         rb_domain_w(const RigidWorld *rw);
f32         rb_domain_h(const RigidWorld *rw);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_PHYSICS_H */
