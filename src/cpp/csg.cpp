// ==========================================================================
// csg.cpp — constructive solid geometry: a boolean tree of signed-distance
// primitives, evaluated into a scalar field for marching cubes.
//
// Each node is either a primitive (sphere/box/cylinder/torus) or a boolean
// operator (union / intersection / subtract) combining two child nodes, with an
// optional smooth-blend radius k for the rounded "metaball-like" joins that CSG
// modelers use. We evaluate the tree's signed distance at every grid cell:
// SDF < 0 inside, > 0 outside. The field we hand back is NEGATED (inside
// positive) so the marching-cubes convention (extract where field crosses a
// small positive iso) yields the solid's surface.
//
// The classic SDF formulas (Inigo Quilez) are used for the primitives, and the
// smooth-min/smooth-max polynomial blends for the soft boolean ops.
// ==========================================================================
#include "cathode/cppcore.h"
#include <cmath>
#include <algorithm>
#include <vector>

namespace {

struct V3 { float x, y, z; };
static inline V3 sub(V3 a, V3 b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline float len(V3 a) { return std::sqrt(a.x*a.x + a.y*a.y + a.z*a.z); }

// --- primitive signed distances (negative inside) ---
static float sd_sphere(V3 p, V3 c, float r) {
    return len(sub(p, c)) - r;
}
static float sd_box(V3 p, V3 c, V3 h) {
    float qx = std::fabs(p.x-c.x) - h.x;
    float qy = std::fabs(p.y-c.y) - h.y;
    float qz = std::fabs(p.z-c.z) - h.z;
    float ax = std::max(qx, 0.0f), ay = std::max(qy, 0.0f), az = std::max(qz, 0.0f);
    float outside = std::sqrt(ax*ax + ay*ay + az*az);
    float inside = std::min(std::max(qx, std::max(qy, qz)), 0.0f);
    return outside + inside;
}
static float sd_cylinder(V3 p, V3 c, float r, float halfh) {
    // axis-aligned along y
    float dx = p.x - c.x, dz = p.z - c.z;
    float radial = std::sqrt(dx*dx + dz*dz) - r;
    float axial = std::fabs(p.y - c.y) - halfh;
    float outside = std::sqrt(std::max(radial,0.0f)*std::max(radial,0.0f)
                            + std::max(axial,0.0f)*std::max(axial,0.0f));
    float inside = std::min(std::max(radial, axial), 0.0f);
    return outside + inside;
}
static float sd_torus(V3 p, V3 c, float R, float r) {
    // torus in the xz-plane
    float dx = p.x - c.x, dz = p.z - c.z, dy = p.y - c.y;
    float q = std::sqrt(dx*dx + dz*dz) - R;
    return std::sqrt(q*q + dy*dy) - r;
}

// --- smooth boolean blends (k>0) and hard versions (k<=0) ---
static float smin(float a, float b, float k) {
    if (k <= 0.0f) return std::min(a, b);
    float hh = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::min(a, b) - hh*hh*k*0.25f;
}
static float smax(float a, float b, float k) {
    if (k <= 0.0f) return std::max(a, b);
    float hh = std::max(k - std::fabs(a - b), 0.0f) / k;
    return std::max(a, b) + hh*hh*k*0.25f;
}

// Recursive evaluation with a depth guard (protects against malformed trees).
static float eval_node(const CsgNode* nodes, int n, int idx, V3 p, int depth, bool* ok) {
    if (idx < 0 || idx >= n || depth > 64) { *ok = false; return 1e9f; }
    const CsgNode& nd = nodes[idx];
    switch (nd.kind) {
        case CSG_SPHERE:   return sd_sphere(p, {nd.center.x,nd.center.y,nd.center.z}, nd.size.x);
        case CSG_BOX:      return sd_box(p, {nd.center.x,nd.center.y,nd.center.z},
                                         {nd.size.x,nd.size.y,nd.size.z});
        case CSG_CYLINDER: return sd_cylinder(p, {nd.center.x,nd.center.y,nd.center.z},
                                              nd.size.x, nd.size.y);
        case CSG_TORUS:    return sd_torus(p, {nd.center.x,nd.center.y,nd.center.z},
                                           nd.size.x, nd.size.y);
        case CSG_UNION: {
            float a = eval_node(nodes, n, nd.a, p, depth+1, ok);
            float b = eval_node(nodes, n, nd.b, p, depth+1, ok);
            return smin(a, b, nd.k);
        }
        case CSG_INTERSECT: {
            float a = eval_node(nodes, n, nd.a, p, depth+1, ok);
            float b = eval_node(nodes, n, nd.b, p, depth+1, ok);
            return smax(a, b, nd.k);
        }
        case CSG_SUBTRACT: {
            // A minus B = intersection of A and complement(B) = max(A, -B)
            float a = eval_node(nodes, n, nd.a, p, depth+1, ok);
            float b = eval_node(nodes, n, nd.b, p, depth+1, ok);
            return smax(a, -b, nd.k);
        }
        default: *ok = false; return 1e9f;
    }
}

// Validate the tree ONCE up front: every operator's children must be in range
// and the graph must be acyclic (a self- or mutually-referential operator would
// otherwise drive eval_node into ~2^depth recursion — an effective hang). We do
// a DFS coloring nodes white/grey/black; a grey node reached again is a back
// edge (cycle). Returns true iff the tree rooted at `root` is well formed.
static bool validate(const CsgNode* nodes, int n, int idx, char* color) {
    if (idx < 0 || idx >= n) return false;
    if (color[idx] == 1) return false;   // grey => cycle
    if (color[idx] == 2) return true;    // black => already proven acyclic
    color[idx] = 1;                      // grey (on the current DFS path)
    const CsgNode& nd = nodes[idx];
    bool ok = true;
    if (nd.kind == CSG_UNION || nd.kind == CSG_INTERSECT || nd.kind == CSG_SUBTRACT) {
        ok = validate(nodes, n, nd.a, color) && validate(nodes, n, nd.b, color);
    } else if (nd.kind != CSG_SPHERE && nd.kind != CSG_BOX &&
               nd.kind != CSG_CYLINDER && nd.kind != CSG_TORUS) {
        ok = false;                      // unknown kind
    }
    color[idx] = 2;                      // black (fully explored)
    return ok;
}

} // namespace

extern "C" i32 cpp_csg_eval(f32* field, i32 nx, i32 ny, i32 nz,
                            const CsgNode* nodes, i32 nnodes, i32 root) {
    if (!field || !nodes || nx <= 0 || ny <= 0 || nz <= 0 ||
        nnodes <= 0 || root < 0 || root >= nnodes) return 1;
    // Reject malformed/cyclic trees before touching a single voxel, so eval_node
    // can never be driven into runaway recursion.
    {
        std::vector<char> color((size_t)nnodes, 0);
        if (!validate(nodes, nnodes, root, color.data())) return 2;
    }
    bool ok = true;
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x) {
                V3 p = { (float)x, (float)y, (float)z };
                float d = eval_node(nodes, nnodes, root, p, 0, &ok);
                // negate so inside (d<0) becomes positive field, matching the
                // marching-cubes "extract above iso" convention.
                field[(size_t)(z*ny + y)*nx + x] = -d;
            }
    return ok ? 0 : 2;
}
