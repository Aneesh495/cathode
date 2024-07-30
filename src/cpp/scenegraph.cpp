// ==========================================================================
// scenegraph.cpp — retained-mode wireframe scene graph (cpp_sg_* in cppcore.h)
//
// A tree of transformed nodes. Each node owns:
//   * a parent index (-1 == a direct child of the passed-in `root`),
//   * a local Mat4 transform (column-major, m[col*4+row], per types.h),
//   * a list of wireframe primitives (boxes / grids), each a bundle of
//     line-segment endpoints in the node's LOCAL space plus a color.
//
// Flatten walks the tree, composes each node's world matrix as
//     world[node] = world[parent] * local[node]      (world[-1] == root)
// transforms every primitive's line endpoints into world space, and emits
// CppSegment{a,b,color} records, up to `max_segs`.
//
// The C++ side uses STL/RAII internally; the boundary stays extern "C" POD.
// ==========================================================================
#include "cathode/cppcore.h"
#include "cathode/vec.h"

#include <vector>
#include <cmath>

// --------------------------------------------------------------------------
// Internal representation
// --------------------------------------------------------------------------

// One wireframe primitive: a flat list of line-segment endpoints stored in the
// owning node's local coordinate frame. Endpoints come in pairs, so segment i
// spans pts[2*i] -> pts[2*i+1]. Keeping raw endpoint pairs (rather than a
// vertex+index scheme) makes flattening a trivial transform-and-emit loop.
struct Prim {
    std::vector<Vec3> pts;   // even length; consecutive pairs form segments
    Color3            color; // per-primitive wire color
};

// A node in the hierarchy.
struct Node {
    i32               parent; // index into g->nodes, or -1 for a root child
    Mat4              local;  // local transform relative to parent frame
    std::vector<Prim> prims;  // geometry attached directly to this node
};

// Opaque handle exposed to C: just a flat node table. Because cpp_sg_add_node
// returns a fresh id and a child can only reference an already-created parent,
// a node's parent index is always strictly less than its own index. That
// guarantees a valid forward topological order for world-matrix composition.
struct CppSceneGraph {
    std::vector<Node> nodes;
};

// --------------------------------------------------------------------------
// Geometry generators (fill a Prim with local-space segment endpoints)
// --------------------------------------------------------------------------

// Axis-aligned box centered at the local origin with the given half extents.
// Produces the canonical 12 edges of a cuboid (4 bottom + 4 top + 4 vertical).
static void build_box(Prim& p, Vec3 h) {
    // The 8 corners, indexed so bit0=x sign, bit1=y sign, bit2=z sign.
    const Vec3 c[8] = {
        v3(-h.x, -h.y, -h.z), // 0
        v3(+h.x, -h.y, -h.z), // 1
        v3(-h.x, +h.y, -h.z), // 2
        v3(+h.x, +h.y, -h.z), // 3
        v3(-h.x, -h.y, +h.z), // 4
        v3(+h.x, -h.y, +h.z), // 5
        v3(-h.x, +h.y, +h.z), // 6
        v3(+h.x, +h.y, +h.z), // 7
    };
    // 12 edges as corner-index pairs.
    static const int e[12][2] = {
        {0,1}, {1,3}, {3,2}, {2,0}, // bottom face (z = -h.z)
        {4,5}, {5,7}, {7,6}, {6,4}, // top face    (z = +h.z)
        {0,4}, {1,5}, {2,6}, {3,7}, // vertical struts
    };
    p.pts.reserve(24);
    for (int i = 0; i < 12; ++i) {
        p.pts.push_back(c[e[i][0]]);
        p.pts.push_back(c[e[i][1]]);
    }
}

// A flat wireframe grid in the local XZ plane (y = 0), centered on the origin
// and spanning [-size/2, +size/2] on each axis. `n` is the number of grid
// lines PER axis, so the result has exactly 2*n line segments: n lines running
// parallel to X (each at a fixed z) and n lines running parallel to Z (each at
// a fixed x). n<=0 yields no geometry; n==1 places a single centered line per
// axis to avoid a divide-by-zero in the spacing.
static void build_grid(Prim& p, i32 n, f32 size) {
    if (n <= 0) return;
    const f32 half = size * 0.5f;
    // Spacing between adjacent lines: for n lines the two outermost sit on the
    // edges, so there are (n-1) gaps across the full `size`.
    const f32 step = (n > 1) ? (size / (f32)(n - 1)) : 0.0f;
    p.pts.reserve((size_t)n * 4);
    for (i32 i = 0; i < n; ++i) {
        // For n==1, coord collapses to 0 (a centered line).
        const f32 t = (n > 1) ? (-half + step * (f32)i) : 0.0f;
        // Line parallel to X at z = t: spans x in [-half, +half].
        p.pts.push_back(v3(-half, 0.0f, t));
        p.pts.push_back(v3(+half, 0.0f, t));
        // Line parallel to Z at x = t: spans z in [-half, +half].
        p.pts.push_back(v3(t, 0.0f, -half));
        p.pts.push_back(v3(t, 0.0f, +half));
    }
}

// --------------------------------------------------------------------------
// C ABI
// --------------------------------------------------------------------------
extern "C" {

CppSceneGraph *cpp_sg_create(void) {
    return new CppSceneGraph();
}

void cpp_sg_destroy(CppSceneGraph *g) {
    delete g; // ~vector cascades and frees every node + prim (RAII)
}

// Create a node under `parent` (-1 == root child). Returns the new node id, or
// -1 on a bad handle / out-of-range parent (defensive; normal callers pass a
// previously returned id or -1).
i32 cpp_sg_add_node(CppSceneGraph *g, i32 parent) {
    if (!g) return -1;
    if (parent < -1 || parent >= (i32)g->nodes.size()) return -1;
    Node nd;
    nd.parent = parent;
    nd.local  = mat4_identity(); // sane default until set_transform
    g->nodes.push_back(std::move(nd));
    return (i32)g->nodes.size() - 1;
}

// Replace a node's local transform. No-op on a bad handle / id.
void cpp_sg_set_transform(CppSceneGraph *g, i32 node, Mat4 local) {
    if (!g || node < 0 || node >= (i32)g->nodes.size()) return;
    g->nodes[(size_t)node].local = local;
}

// Attach a wireframe box (12 edges) with half-extents `half` and color `c`.
void cpp_sg_add_box(CppSceneGraph *g, i32 node, Vec3 half, Color3 c) {
    if (!g || node < 0 || node >= (i32)g->nodes.size()) return;
    Prim p;
    p.color = c;
    build_box(p, half);
    g->nodes[(size_t)node].prims.push_back(std::move(p));
}

// Attach a wireframe grid (n lines per axis, 2*n segments) with color `c`.
void cpp_sg_add_grid(CppSceneGraph *g, i32 node, i32 n, f32 size, Color3 c) {
    if (!g || node < 0 || node >= (i32)g->nodes.size()) return;
    Prim p;
    p.color = c;
    build_grid(p, n, size);
    g->nodes[(size_t)node].prims.push_back(std::move(p));
}

// Flatten the whole graph into world-space line segments.
//
// Composition: world[node] = world[parent] * local[node], with the caller's
// `root` standing in for world[-1]. Because parent indices are always smaller
// than child indices (see CppSceneGraph note), a single forward pass computes
// every world matrix with each parent already resolved.
//
// Each primitive endpoint p (local) becomes root..*local * (p,1); we drop w
// (affine transforms keep it 1). Writes at most `max_segs` segments into `out`
// and returns the number actually written.
i32 cpp_sg_flatten(CppSceneGraph *g, Mat4 root, CppSegment *out, i32 max_segs) {
    if (!g || !out || max_segs <= 0) return 0;

    const size_t count = g->nodes.size();

    // Pass 1: resolve every node's world matrix in index (topological) order.
    std::vector<Mat4> world(count);
    for (size_t i = 0; i < count; ++i) {
        const Node& nd = g->nodes[i];
        const Mat4 parent_world =
            (nd.parent < 0) ? root : world[(size_t)nd.parent];
        world[i] = mat4_mul(parent_world, nd.local); // parent * local
    }

    // Pass 2: transform each primitive's local endpoints to world space and
    // emit segments until we hit the cap.
    i32 written = 0;
    for (size_t i = 0; i < count && written < max_segs; ++i) {
        const Node& nd = g->nodes[i];
        const Mat4& W = world[i];
        for (size_t pi = 0; pi < nd.prims.size() && written < max_segs; ++pi) {
            const Prim& pr = nd.prims[pi];
            const size_t nseg = pr.pts.size() / 2; // endpoints come in pairs
            for (size_t s = 0; s < nseg && written < max_segs; ++s) {
                const Vec3& la = pr.pts[2 * s];
                const Vec3& lb = pr.pts[2 * s + 1];
                // Homogeneous transform (w=1), then take the xyz part.
                const Vec4 wa = mat4_mul_v4(W, v4_from_v3(la, 1.0f));
                const Vec4 wb = mat4_mul_v4(W, v4_from_v3(lb, 1.0f));
                out[written].a     = v4_xyz(wa);
                out[written].b     = v4_xyz(wb);
                out[written].color = pr.color;
                ++written;
            }
        }
    }
    return written;
}

} // extern "C"
