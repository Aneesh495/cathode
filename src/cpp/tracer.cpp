// ==========================================================================
// tracer.cpp  -  BVH Monte-Carlo path tracer (implements cpp_tracer_* in
// cathode/cppcore.h).
//
// This translation unit uses C++20 internally (RAII containers, templates,
// STL algorithms, std::thread) but exposes ONLY the frozen extern "C" POD
// surface declared in the header. All C++ state lives behind the opaque
// `CppTracer` handle.
//
// Pipeline overview
// -----------------
//   * Geometry: spheres (accelerated by a bounding-volume hierarchy) and
//     planes (tested linearly  -  usually few of them, and they have infinite
//     extent so they do not fit an AABB well).
//   * Materials: Lambert (cosine-weighted diffuse), Metal (mirror + fuzz),
//     Dielectric (Schlick-fresnel refract/reflect), Emissive (light source).
//   * Camera: thin-lens (aperture -> depth of field) with a vertical FOV.
//   * Integrator: unidirectional path tracing, up to `max_bounces` bounces,
//     with optional Russian-roulette path termination after a few bounces.
//   * Accumulation: radiance sums land in an internal HDR buffer; successive
//     render() calls refine progressively; resolve() averages into the
//     shared Framebuffer as linear HDR.
//
// Every routine here is finite and bounded: sampling uses analytic (loop-free)
// transforms rather than rejection sampling, the bounce loop is capped by
// `max_bounces`, and BVH traversal uses a fixed-size explicit stack.
// ==========================================================================
#include "cathode/cppcore.h"
#include "cathode/vec.h"

#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <thread>
#include <atomic>

namespace {

// ------------------------------------------------------------------ helpers
// Colors are stored/manipulated as Vec3 internally so we can reuse the rich
// vec.h arithmetic; convert to/from the ABI Color3 at the boundary.
static inline Vec3   c2v(Color3 c) { return v3(c.r, c.g, c.b); }

static inline f32 maxf3(Vec3 v) { return ct_maxf(v.x, ct_maxf(v.y, v.z)); }

// A single geometric ray. Direction is kept unit-length by construction.
struct Ray { Vec3 o, d; };

// ------------------------------------------------------------------ PRNG
// PCG32  -  small, fast, statistically excellent. One instance per pixel keeps
// worker threads independent (no shared RNG state, so no data races).
struct Pcg {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc   = 0xda3e39cb94b95bdbULL;

    inline uint32_t next() {
        uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = (uint32_t)(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((-(int)rot) & 31));
    }
    // Seed from a state/sequence pair (RTIOW/PCG convention).
    inline void seed(uint64_t s, uint64_t seq) {
        state = 0u;
        inc = (seq << 1u) | 1u;
        next();
        state += s;
        next();
    }
    // Uniform float in [0,1) with 24 bits of mantissa entropy.
    inline f32 f() { return (f32)(next() >> 8) * (1.0f / 16777216.0f); }
};

// splitmix64  -  mixes integer seeds so adjacent pixels/frames decorrelate.
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// Uniform point on the unit sphere (loop-free inverse-CDF construction).
static inline Vec3 random_unit_vector(Pcg &rng) {
    f32 z = 1.0f - 2.0f * rng.f();
    f32 r = sqrtf(ct_maxf(0.0f, 1.0f - z * z));
    f32 phi = CT_TAU * rng.f();
    return v3(r * cosf(phi), r * sinf(phi), z);
}
// Uniform point inside the unit ball: direction * radius^(1/3).
static inline Vec3 random_in_unit_sphere(Pcg &rng) {
    Vec3 dir = random_unit_vector(rng);
    f32 rad = cbrtf(rng.f());
    return v3_scale(dir, rad);
}
// Uniform point inside the unit disk (for thin-lens aperture sampling).
static inline Vec3 random_in_unit_disk(Pcg &rng) {
    f32 r = sqrtf(rng.f());
    f32 phi = CT_TAU * rng.f();
    return v3(r * cosf(phi), r * sinf(phi), 0.0f);
}

// ------------------------------------------------------------------ materials
struct Material {
    CppMatKind kind = CPPMAT_LAMBERT;
    Vec3       albedo{0.8f, 0.8f, 0.8f}; // diffuse/reflection tint
    f32        param = 0.0f;             // fuzz (metal) | IOR (dielectric)
    Vec3       emission{0.0f, 0.0f, 0.0f}; // radiance emitted (emissive only)
};

struct Sphere { Vec3 center; f32 radius; Material mat; };
struct Plane  { Vec3 point;  Vec3 normal; Material mat; };

// What a ray hit: parametric distance, surface point, shading normal (already
// oriented against the incoming ray), whether we struck the outward face, and
// the material.
struct Hit {
    f32 t;
    Vec3 p;
    Vec3 n;
    bool front;
    const Material *mat;
};

// ------------------------------------------------------------------ AABB / BVH
struct AABB {
    Vec3 lo{ 1e30f,  1e30f,  1e30f};
    Vec3 hi{-1e30f, -1e30f, -1e30f};
    inline void expand(Vec3 p) { lo = v3_min(lo, p); hi = v3_max(hi, p); }
    inline void merge(const AABB &b) { lo = v3_min(lo, b.lo); hi = v3_max(hi, b.hi); }
    inline Vec3 center() const { return v3_scale(v3_add(lo, hi), 0.5f); }
};

// Slab test. Narrows [tmin,tmax] against each axis; returns whether the ray
// interval still overlaps the box. Branch-lean; handles axis-parallel rays via
// the +/-inf that 1/0 produces (the min/max then discard them correctly).
static inline bool aabb_hit(const AABB &b, const Ray &r, f32 tmin, f32 tmax) {
    for (int a = 0; a < 3; ++a) {
        f32 o = (&r.o.x)[a];
        f32 d = (&r.d.x)[a];
        f32 invD = 1.0f / d;
        f32 t0 = ((&b.lo.x)[a] - o) * invD;
        f32 t1 = ((&b.hi.x)[a] - o) * invD;
        if (invD < 0.0f) { f32 tmp = t0; t0 = t1; t1 = tmp; }
        tmin = t0 > tmin ? t0 : tmin;
        tmax = t1 < tmax ? t1 : tmax;
        if (tmax <= tmin) return false;
    }
    return true;
}

// A BVH node is either an interior node (left/right children) or a leaf that
// references a contiguous [start, start+count) run of the reordered index list.
struct BVHNode {
    AABB box;
    i32  left  = -1;
    i32  right = -1;
    i32  start = 0;
    i32  count = 0; // >0 => leaf
};

// ------------------------------------------------------------------ tracer state
struct Tracer {
    i32 w = 0, h = 0;

    std::vector<Sphere> spheres;
    std::vector<Plane>  planes;

    // BVH over spheres.
    std::vector<i32>     order; // sphere indices, reordered by the builder
    std::vector<BVHNode> nodes;

    // Camera (thin lens). Basis vectors + viewport are derived per render().
    Vec3 eye{0, 0, 5}, target{0, 0, 0};
    f32  fov = 60.0f * CT_DEG2RAD;
    f32  aperture = 0.0f;

    // Sky gradient endpoints (blended by ray-direction elevation on a miss).
    Vec3 sky_top{0.5f, 0.7f, 1.0f};
    Vec3 sky_bot{1.0f, 1.0f, 1.0f};

    // HDR accumulation: running radiance sum + count of samples folded in.
    std::vector<Vec3> accum;
    i64  sample_count = 0;
    u32  frame = 0; // advanced each render() so refinement draws fresh samples

    std::atomic<int64_t> rays{0};
};

// ---- ray/primitive intersection -----------------------------------------

// Ray vs sphere. Solves |o + t d - c|^2 = r^2 with the "half-b" reduction.
// Returns the nearest root within (tmin,tmax) and fills the hit record.
static inline bool hit_sphere(const Sphere &s, const Ray &r,
                              f32 tmin, f32 tmax, Hit &rec) {
    Vec3 oc = v3_sub(r.o, s.center);
    f32 a = v3_dot(r.d, r.d);          // == 1 for unit dir, kept general
    f32 half_b = v3_dot(oc, r.d);
    f32 c = v3_dot(oc, oc) - s.radius * s.radius;
    f32 disc = half_b * half_b - a * c;
    if (disc < 0.0f) return false;
    f32 sq = sqrtf(disc);
    f32 root = (-half_b - sq) / a;
    if (root <= tmin || root >= tmax) {
        root = (-half_b + sq) / a;
        if (root <= tmin || root >= tmax) return false;
    }
    rec.t = root;
    rec.p = v3_add(r.o, v3_scale(r.d, root));
    Vec3 outward = v3_scale(v3_sub(rec.p, s.center), 1.0f / s.radius);
    rec.front = v3_dot(r.d, outward) < 0.0f;
    rec.n = rec.front ? outward : v3_neg(outward);
    rec.mat = &s.mat;
    return true;
}

// Ray vs infinite plane. Skips near-parallel rays to avoid huge/NaN t.
static inline bool hit_plane(const Plane &pl, const Ray &r,
                             f32 tmin, f32 tmax, Hit &rec) {
    f32 denom = v3_dot(pl.normal, r.d);
    if (fabsf(denom) < 1e-8f) return false;
    f32 t = v3_dot(v3_sub(pl.point, r.o), pl.normal) / denom;
    if (t <= tmin || t >= tmax) return false;
    rec.t = t;
    rec.p = v3_add(r.o, v3_scale(r.d, t));
    rec.front = denom < 0.0f;
    rec.n = rec.front ? pl.normal : v3_neg(pl.normal);
    rec.mat = &pl.mat;
    return true;
}

// Traverse the sphere BVH with a fixed-size explicit stack (no recursion, so
// bounded stack usage). Returns the closest sphere hit within (tmin,tmax).
static bool hit_bvh(const Tracer *t, const Ray &r, f32 tmin, f32 tmax, Hit &rec) {
    if (t->nodes.empty()) return false;
    bool hit = false;
    f32 closest = tmax;

    // Depth is O(log N) for a median-split BVH; 64 covers astronomically many
    // primitives. Overflow is impossible for any scene we can hold in memory.
    i32 stack[64];
    int sp = 0;
    stack[sp++] = 0; // root

    while (sp > 0) {
        const BVHNode &node = t->nodes[(size_t)stack[--sp]];
        if (!aabb_hit(node.box, r, tmin, closest)) continue;
        if (node.count > 0) {
            // Leaf: test each referenced sphere.
            for (i32 i = 0; i < node.count; ++i) {
                i32 si = t->order[(size_t)(node.start + i)];
                Hit tmp;
                if (hit_sphere(t->spheres[(size_t)si], r, tmin, closest, tmp)) {
                    closest = tmp.t;
                    rec = tmp;
                    hit = true;
                }
            }
        } else {
            // Interior: descend both children (order-independent; the AABB
            // culling above prunes work either way).
            if (sp + 2 <= (int)(sizeof(stack) / sizeof(stack[0]))) {
                stack[sp++] = node.left;
                stack[sp++] = node.right;
            }
        }
    }
    return hit;
}

// Nearest hit across the whole scene = BVH (spheres) unioned with the linear
// plane list, keeping whichever is closer.
static bool hit_scene(const Tracer *t, const Ray &r, f32 tmin, f32 tmax, Hit &rec) {
    bool hit = false;
    f32 closest = tmax;
    if (hit_bvh(t, r, tmin, closest, rec)) { hit = true; closest = rec.t; }
    for (const Plane &pl : t->planes) {
        Hit tmp;
        if (hit_plane(pl, r, tmin, closest, tmp)) {
            closest = tmp.t;
            rec = tmp;
            hit = true;
        }
    }
    return hit;
}

// ---- BVH construction (median split on the widest centroid axis) ----------

static AABB sphere_box(const Sphere &s) {
    AABB b;
    b.lo = v3_sub(s.center, v3(s.radius, s.radius, s.radius));
    b.hi = v3_add(s.center, v3(s.radius, s.radius, s.radius));
    return b;
}

// Recursively build over order[start, start+count). Recursion depth is
// O(log count) because each split roughly halves the range; degenerate cases
// (all centroids coincident, or tiny counts) fall back to a leaf, which caps
// the depth and guarantees termination.
static i32 bvh_build(Tracer *t, i32 start, i32 count) {
    const i32 kLeaf = 2; // small leaves keep traversal cheap for our scenes

    i32 idx = (i32)t->nodes.size();
    t->nodes.push_back(BVHNode{}); // reserve our slot before recursing

    // Bounds over this range, and the bounds of just the centroids (used to
    // pick the split axis).
    AABB box, centroid_box;
    for (i32 i = 0; i < count; ++i) {
        const Sphere &s = t->spheres[(size_t)t->order[(size_t)(start + i)]];
        box.merge(sphere_box(s));
        centroid_box.expand(s.center);
    }

    auto make_leaf = [&]() {
        BVHNode n;
        n.box = box; n.start = start; n.count = count; n.left = n.right = -1;
        t->nodes[(size_t)idx] = n;
        return idx;
    };

    if (count <= kLeaf) return make_leaf();

    Vec3 ext = v3_sub(centroid_box.hi, centroid_box.lo);
    int axis = 0;
    if (ext.y > ext.x) axis = 1;
    if ((&ext.x)[2] > (&ext.x)[axis]) axis = 2;
    if ((&ext.x)[axis] < 1e-12f) return make_leaf(); // no spread => leaf

    // Median split: partition so the lower half has the smaller centroid
    // coordinate along `axis`. nth_element is average O(count).
    i32 mid = count / 2;
    auto begin = t->order.begin() + start;
    std::nth_element(begin, begin + mid, begin + count,
        [&](i32 lhs, i32 rhs) {
            f32 cl = (&t->spheres[(size_t)lhs].center.x)[axis];
            f32 cr = (&t->spheres[(size_t)rhs].center.x)[axis];
            return cl < cr;
        });

    i32 l = bvh_build(t, start, mid);
    i32 r = bvh_build(t, start + mid, count - mid);

    BVHNode n;
    n.box = box; n.start = -1; n.count = 0; n.left = l; n.right = r;
    t->nodes[(size_t)idx] = n; // safe: children already fully built
    return idx;
}

// ---- shading / BSDF sampling ---------------------------------------------

// Schlick's polynomial approximation of Fresnel reflectance for dielectrics.
static inline f32 schlick(f32 cosine, f32 ref_idx) {
    f32 r0 = (1.0f - ref_idx) / (1.0f + ref_idx);
    r0 = r0 * r0;
    f32 m = 1.0f - cosine;
    return r0 + (1.0f - r0) * m * m * m * m * m;
}

// Snell refraction of a unit incident vector `uv` across normal `n` with the
// given relative index ratio (eta_in/eta_out).
static inline Vec3 refract(Vec3 uv, Vec3 n, f32 ratio) {
    f32 cos_theta = ct_minf(v3_dot(v3_neg(uv), n), 1.0f);
    Vec3 perp = v3_scale(v3_add(uv, v3_scale(n, cos_theta)), ratio);
    f32 k = 1.0f - v3_dot(perp, perp);
    Vec3 parallel = v3_scale(n, -sqrtf(fabsf(k)));
    return v3_add(perp, parallel);
}

// Given an incoming ray and a hit, produce the scattered ray + attenuation.
// Returns false for a fully absorbed interaction (path ends). Emissive
// surfaces never "scatter"  -  they are handled in the integrator (add & stop).
static bool scatter(const Ray &in, const Hit &rec, Pcg &rng,
                    Vec3 &attenuation, Ray &scattered) {
    switch (rec.mat->kind) {
    case CPPMAT_LAMBERT: {
        // Cosine-weighted hemisphere sample: normal + point-on-unit-sphere.
        Vec3 dir = v3_add(rec.n, random_unit_vector(rng));
        if (v3_len2(dir) < 1e-12f) dir = rec.n; // guard degenerate direction
        attenuation = rec.mat->albedo;
        scattered = Ray{ rec.p, v3_norm(dir) };
        return true;
    }
    case CPPMAT_METAL: {
        Vec3 reflected = v3_reflect(v3_norm(in.d), rec.n);
        f32 fuzz = ct_clampf(rec.mat->param, 0.0f, 1.0f);
        Vec3 dir = v3_add(reflected, v3_scale(random_in_unit_sphere(rng), fuzz));
        attenuation = rec.mat->albedo;
        scattered = Ray{ rec.p, v3_norm(dir) };
        // Fuzz can scatter below the surface; treat that as absorption.
        return v3_dot(scattered.d, rec.n) > 0.0f;
    }
    case CPPMAT_DIELECTRIC: {
        attenuation = rec.mat->albedo; // usually white glass; albedo tints it
        f32 ior = rec.mat->param > 1e-4f ? rec.mat->param : 1.5f;
        f32 ratio = rec.front ? (1.0f / ior) : ior;
        Vec3 unit = v3_norm(in.d);
        f32 cos_theta = ct_minf(v3_dot(v3_neg(unit), rec.n), 1.0f);
        f32 sin_theta = sqrtf(ct_maxf(0.0f, 1.0f - cos_theta * cos_theta));
        bool cannot_refract = ratio * sin_theta > 1.0f; // total internal refl.
        Vec3 dir;
        if (cannot_refract || schlick(cos_theta, ratio) > rng.f())
            dir = v3_reflect(unit, rec.n);
        else
            dir = refract(unit, rec.n, ratio);
        scattered = Ray{ rec.p, v3_norm(dir) };
        return true;
    }
    case CPPMAT_EMISSIVE:
    default:
        return false; // lights do not scatter
    }
}

// Sky background: blend bottom->top by the ray's vertical direction.
static inline Vec3 sky_color(const Tracer *t, Vec3 dir) {
    f32 a = 0.5f * (v3_norm(dir).y + 1.0f); // 0 at nadir, 1 at zenith
    return v3_lerp(t->sky_bot, t->sky_top, a);
}

// Iterative path integrator. Accumulates emitted radiance weighted by the
// running path throughput; bounded by `max_bounces` and Russian roulette.
static Vec3 radiance(const Tracer *t, Ray r, i32 max_bounces, Pcg &rng,
                     int64_t &ray_counter) {
    Vec3 L{0, 0, 0};    // accumulated radiance
    Vec3 beta{1, 1, 1}; // path throughput (product of attenuations)

    for (i32 bounce = 0; bounce < max_bounces; ++bounce) {
        ++ray_counter;
        Hit rec;
        // tmin 1e-3 pushes past the surface to avoid shadow/self-intersection
        // acne from floating-point error.
        if (!hit_scene(t, r, 1e-3f, 1e30f, rec)) {
            L = v3_add(L, v3_mul(beta, sky_color(t, r.d)));
            break;
        }
        if (rec.mat->kind == CPPMAT_EMISSIVE) {
            // Direct light: add emission through the current throughput, stop.
            L = v3_add(L, v3_mul(beta, rec.mat->emission));
            break;
        }
        Vec3 atten;
        Ray scattered;
        if (!scatter(r, rec, rng, atten, scattered)) break; // absorbed
        beta = v3_mul(beta, atten);
        r = scattered;

        // Russian roulette after a few bounces: probabilistically kill dim
        // paths, compensating survivors so the estimator stays unbiased.
        if (bounce >= 3) {
            f32 p = ct_minf(0.95f, maxf3(beta));
            if (p <= 0.0f || rng.f() >= p) break;
            beta = v3_scale(beta, 1.0f / p);
        }
    }
    return L;
}

// ---- camera ---------------------------------------------------------------

// Orthonormal thin-lens camera basis + viewport, derived from eye/target/fov/
// aperture and the current aspect ratio.
struct Camera {
    Vec3 origin, lower_left, horizontal, vertical, right, up;
    f32  lens_radius;
};

static Camera make_camera(const Tracer *t) {
    Camera c;
    Vec3 forward = v3_norm(v3_sub(t->target, t->eye));
    Vec3 world_up = v3(0, 1, 0);
    if (fabsf(v3_dot(forward, world_up)) > 0.999f) world_up = v3(1, 0, 0);
    c.right = v3_norm(v3_cross(forward, world_up));
    c.up = v3_cross(c.right, forward);

    f32 focus = v3_len(v3_sub(t->target, t->eye));
    if (focus < 1e-4f) focus = 1.0f;
    f32 aspect = (f32)t->w / (f32)ct_maxi(t->h, 1);
    f32 half_h = tanf(t->fov * 0.5f);
    f32 vp_h = 2.0f * half_h * focus;
    f32 vp_w = aspect * vp_h;

    c.origin = t->eye;
    c.horizontal = v3_scale(c.right, vp_w);
    c.vertical = v3_scale(c.up, vp_h);
    c.lower_left = v3_sub(v3_sub(v3_add(c.origin, v3_scale(forward, focus)),
                                 v3_scale(c.horizontal, 0.5f)),
                          v3_scale(c.vertical, 0.5f));
    c.lens_radius = t->aperture * 0.5f;
    return c;
}

// Generate a primary ray for normalized screen coords (s,t) in [0,1], with
// aperture jitter for depth of field.
static inline Ray camera_ray(const Camera &c, f32 s, f32 tt, Pcg &rng) {
    Vec3 origin = c.origin;
    if (c.lens_radius > 0.0f) {
        Vec3 rd = v3_scale(random_in_unit_disk(rng), c.lens_radius);
        Vec3 offset = v3_add(v3_scale(c.right, rd.x), v3_scale(c.up, rd.y));
        origin = v3_add(origin, offset);
    }
    Vec3 target = v3_add(v3_add(c.lower_left, v3_scale(c.horizontal, s)),
                         v3_scale(c.vertical, tt));
    return Ray{ origin, v3_norm(v3_sub(target, origin)) };
}

} // anonymous namespace

// The opaque handle the C ABI hands around is just our Tracer.
struct CppTracer { Tracer impl; };

// ==========================================================================
// extern "C" ABI surface (matches cathode/cppcore.h exactly)
// ==========================================================================
extern "C" {

CppTracer *cpp_tracer_create(i32 w, i32 h) {
    CppTracer *t = new (std::nothrow) CppTracer();
    if (!t) return nullptr;
    t->impl.w = w > 0 ? w : 1;
    t->impl.h = h > 0 ? h : 1;
    t->impl.accum.assign((size_t)t->impl.w * (size_t)t->impl.h, Vec3{0, 0, 0});
    t->impl.sample_count = 0;
    return t;
}

void cpp_tracer_destroy(CppTracer *t) { delete t; }

void cpp_tracer_clear_scene(CppTracer *t) {
    if (!t) return;
    t->impl.spheres.clear();
    t->impl.planes.clear();
    t->impl.order.clear();
    t->impl.nodes.clear();
}

// Build a Material from the ABI (albedo + a single `param` slot whose meaning
// depends on the material kind; emissive uses param as an emission scale).
static Material make_material(CppMatKind mat, Color3 albedo, f32 param) {
    Material m;
    m.kind = mat;
    m.albedo = c2v(albedo);
    m.param = param;
    if (mat == CPPMAT_EMISSIVE)
        m.emission = v3_scale(c2v(albedo), param > 0.0f ? param : 1.0f);
    return m;
}

void cpp_tracer_add_sphere(CppTracer *t, Vec3 center, f32 radius,
                           CppMatKind mat, Color3 albedo, f32 param) {
    if (!t) return;
    Sphere s;
    s.center = center;
    s.radius = radius;
    s.mat = make_material(mat, albedo, param);
    t->impl.spheres.push_back(s);
}

void cpp_tracer_add_plane(CppTracer *t, Vec3 point, Vec3 normal,
                          CppMatKind mat, Color3 albedo, f32 param) {
    if (!t) return;
    Plane p;
    p.point = point;
    p.normal = v3_norm(normal);
    p.mat = make_material(mat, albedo, param);
    t->impl.planes.push_back(p);
}

void cpp_tracer_set_camera(CppTracer *t, Vec3 eye, Vec3 target,
                           f32 fov_rad, f32 aperture) {
    if (!t) return;
    t->impl.eye = eye;
    t->impl.target = target;
    t->impl.fov = fov_rad > 1e-3f ? fov_rad : (60.0f * CT_DEG2RAD);
    t->impl.aperture = aperture > 0.0f ? aperture : 0.0f;
}

void cpp_tracer_set_sky(CppTracer *t, Color3 top, Color3 bottom) {
    if (!t) return;
    t->impl.sky_top = c2v(top);
    t->impl.sky_bot = c2v(bottom);
}

void cpp_tracer_build(CppTracer *t) {
    if (!t) return;
    Tracer &tr = t->impl;
    tr.nodes.clear();
    tr.order.resize(tr.spheres.size());
    std::iota(tr.order.begin(), tr.order.end(), 0);
    if (!tr.spheres.empty())
        bvh_build(&tr, 0, (i32)tr.spheres.size());
}

void cpp_tracer_reset_accum(CppTracer *t) {
    if (!t) return;
    std::fill(t->impl.accum.begin(), t->impl.accum.end(), Vec3{0, 0, 0});
    t->impl.sample_count = 0;
    t->impl.frame = 0;
}

void cpp_tracer_render(CppTracer *t, i32 spp, i32 max_bounces) {
    if (!t || spp <= 0) return;
    Tracer &tr = t->impl;
    if (max_bounces < 1) max_bounces = 1;
    // Lazily build the BVH if geometry was added without an explicit build().
    if (tr.nodes.empty() && !tr.spheres.empty()) cpp_tracer_build(t);

    ++tr.frame; // decorrelates this frame's samples from previous frames
    const Camera cam = make_camera(&tr);
    const i32 W = tr.w, H = tr.h;
    const u32 frame = tr.frame;

    // Render disjoint horizontal bands on worker threads. Each band owns its
    // own accum rows (no shared writes) and its own ray counter, folded into
    // the atomic once at the end  -  so results are deterministic regardless of
    // thread count.
    auto worker = [&](i32 y0, i32 y1) {
        int64_t local_rays = 0;
        Pcg rng;
        for (i32 y = y0; y < y1; ++y) {
            for (i32 x = 0; x < W; ++x) {
                // Per-pixel seed folds in the frame index so progressive
                // frames advance to fresh, decorrelated sample streams.
                uint64_t pix = (uint64_t)y * (uint64_t)W + (uint64_t)x;
                rng.seed(splitmix64(pix * 0x9E3779B1u + (uint64_t)frame * 0xD1B54A33u),
                         splitmix64((uint64_t)frame * 0x1000193u + pix + 1u));
                Vec3 sum{0, 0, 0};
                for (i32 s = 0; s < spp; ++s) {
                    // Jittered sample position within the pixel (antialiasing).
                    f32 su = ((f32)x + rng.f()) / (f32)W;
                    // Flip vertically: y=0 is the top row, which should map to
                    // the top of the viewport (upward v direction).
                    f32 sv = 1.0f - ((f32)y + rng.f()) / (f32)H;
                    Ray r = camera_ray(cam, su, sv, rng);
                    sum = v3_add(sum, radiance(&tr, r, max_bounces, rng, local_rays));
                }
                size_t idx = (size_t)y * (size_t)W + (size_t)x;
                tr.accum[idx] = v3_add(tr.accum[idx], sum);
            }
        }
        tr.rays.fetch_add(local_rays, std::memory_order_relaxed);
    };

    unsigned hw = std::thread::hardware_concurrency();
    int nthreads = (int)(hw == 0 ? 1u : hw);
    nthreads = std::min(nthreads, 16);
    nthreads = std::max(1, std::min(nthreads, H));

    if (nthreads == 1) {
        worker(0, H);
    } else {
        std::vector<std::thread> pool;
        pool.reserve((size_t)nthreads);
        i32 rows = (H + nthreads - 1) / nthreads;
        for (int i = 0; i < nthreads; ++i) {
            i32 y0 = i * rows;
            i32 y1 = std::min(H, y0 + rows);
            if (y0 >= y1) break;
            pool.emplace_back(worker, y0, y1);
        }
        for (auto &th : pool) th.join();
    }

    tr.sample_count += spp; // every pixel received `spp` more samples
}

void cpp_tracer_resolve(CppTracer *t, Framebuffer *fb) {
    if (!t || !fb) return;
    const Tracer &tr = t->impl;
    f32 inv = tr.sample_count > 0 ? 1.0f / (f32)tr.sample_count : 0.0f;
    i32 W = ct_mini(tr.w, fb->w);
    i32 H = ct_mini(tr.h, fb->h);
    for (i32 y = 0; y < H; ++y) {
        for (i32 x = 0; x < W; ++x) {
            Vec3 v = v3_scale(tr.accum[(size_t)y * (size_t)tr.w + (size_t)x], inv);
            f32 *p = &fb->px[((size_t)y * (size_t)fb->w + (size_t)x) * 3];
            p[0] = v.x; p[1] = v.y; p[2] = v.z;
        }
    }
}

i64 cpp_tracer_rays_cast(const CppTracer *t) {
    if (!t) return 0;
    return (i64)t->impl.rays.load(std::memory_order_relaxed);
}

} // extern "C"
