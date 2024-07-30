// ==========================================================================
// test_scenegraph.cpp — unit tests for the retained-mode wireframe scene graph
// (cpp_sg_* in cppcore.h). Headless, deterministic, terminates in ms.
//
// Coverage:
//   (a) a single box at identity -> 12 segments with the expected extent;
//   (b) compose correctness: a child node translated by T yields the parent's
//       geometry offset by T (verified on a known endpoint);
//   (c) a grid of n lines/axis -> 2*n segments;
//   (d) flatten honors the max_segs cap.
// Prints PASS/FAIL per check and an ALL PASS / SOME FAILED summary.
// ==========================================================================
#include "cathode/cppcore.h"
#include "cathode/vec.h"

#include <cstdio>
#include <cmath>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

// Record and print one boolean check.
static void check(const char *name, bool cond) {
    ++g_checks;
    if (cond) {
        std::printf("  PASS  %s\n", name);
    } else {
        std::printf("  FAIL  %s\n", name);
        ++g_failures;
    }
}

// Approximate float / vector equality (segments go through fp transforms).
static bool approx(f32 a, f32 b, f32 eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}
static bool approx_v3(Vec3 a, Vec3 b, f32 eps = 1e-4f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

// Compute the axis-aligned bounds over every endpoint of `n` segments.
static void seg_bounds(const CppSegment *s, i32 n, Vec3 &lo, Vec3 &hi) {
    lo = v3( 1e30f,  1e30f,  1e30f);
    hi = v3(-1e30f, -1e30f, -1e30f);
    for (i32 i = 0; i < n; ++i) {
        lo = v3_min(lo, s[i].a); hi = v3_max(hi, s[i].a);
        lo = v3_min(lo, s[i].b); hi = v3_max(hi, s[i].b);
    }
}

// Does any emitted segment have an endpoint (a or b) approximately equal to p?
static bool has_endpoint(const CppSegment *s, i32 n, Vec3 p) {
    for (i32 i = 0; i < n; ++i) {
        if (approx_v3(s[i].a, p) || approx_v3(s[i].b, p)) return true;
    }
    return false;
}

int main(void) {
    std::printf("== CATHODE scene graph ==\n");

    const Color3 wire = col3(0.1f, 0.9f, 0.4f);
    const Mat4 I = mat4_identity();
    std::vector<CppSegment> buf(4096);

    // ----------------------------------------------------------------------
    // (a) Single box at identity -> exactly 12 segments spanning [-h, +h].
    // ----------------------------------------------------------------------
    {
        CppSceneGraph *g = cpp_sg_create();
        i32 node = cpp_sg_add_node(g, -1);
        Vec3 h = v3(2.0f, 3.0f, 4.0f);
        cpp_sg_add_box(g, node, h, wire);

        i32 n = cpp_sg_flatten(g, I, buf.data(), (i32)buf.size());
        check("box -> 12 segments", n == 12);

        Vec3 lo, hi;
        seg_bounds(buf.data(), n, lo, hi);
        check("box extent lo == -half", approx_v3(lo, v3_neg(h)));
        check("box extent hi == +half", approx_v3(hi, h));

        // Colors propagate to every emitted segment.
        bool color_ok = true;
        for (i32 i = 0; i < n; ++i) {
            if (!approx(buf[i].color.r, wire.r) ||
                !approx(buf[i].color.g, wire.g) ||
                !approx(buf[i].color.b, wire.b)) { color_ok = false; break; }
        }
        check("box color propagated", color_ok);

        // Every one of the 8 corners must appear as some endpoint.
        bool corners_ok = true;
        for (int sx = -1; sx <= 1 && corners_ok; sx += 2)
        for (int sy = -1; sy <= 1 && corners_ok; sy += 2)
        for (int sz = -1; sz <= 1 && corners_ok; sz += 2) {
            Vec3 c = v3(sx * h.x, sy * h.y, sz * h.z);
            if (!has_endpoint(buf.data(), n, c)) corners_ok = false;
        }
        check("box all 8 corners present", corners_ok);

        cpp_sg_destroy(g);
    }

    // ----------------------------------------------------------------------
    // (b) Compose correctness. A parent translated by Tp holds a box; a child
    //     (translated by Tc relative to the parent) holds an identical box.
    //     world[child] = I * translate(Tp) * translate(Tc) = translate(Tp+Tc),
    //     so the child's geometry is the parent's, offset by (Tp+Tc).
    // ----------------------------------------------------------------------
    {
        CppSceneGraph *g = cpp_sg_create();
        Vec3 Tp = v3(10.0f, 0.0f, 0.0f);
        Vec3 Tc = v3(0.0f, 5.0f, -3.0f);
        Vec3 h  = v3(1.0f, 1.0f, 1.0f);

        i32 parent = cpp_sg_add_node(g, -1);
        cpp_sg_set_transform(g, parent, mat4_translate(Tp));
        cpp_sg_add_box(g, parent, h, wire);

        i32 child = cpp_sg_add_node(g, parent);
        cpp_sg_set_transform(g, child, mat4_translate(Tc));
        cpp_sg_add_box(g, child, h, wire);

        i32 n = cpp_sg_flatten(g, I, buf.data(), (i32)buf.size());
        check("two boxes -> 24 segments", n == 24);

        // Parent box corner (+h) lands at Tp + h.
        Vec3 parent_corner = v3_add(Tp, h);
        check("parent corner at Tp+h", has_endpoint(buf.data(), n, parent_corner));

        // Child box corner (+h) lands at Tp + Tc + h  (chained locals).
        Vec3 child_offset = v3_add(Tp, Tc);
        Vec3 child_corner = v3_add(child_offset, h);
        check("child corner offset by Tp+Tc", has_endpoint(buf.data(), n, child_corner));

        // And the negative corner too, to pin the whole box, not one point.
        Vec3 child_neg = v3_add(child_offset, v3_neg(h));
        check("child -corner offset by Tp+Tc", has_endpoint(buf.data(), n, child_neg));

        // A nonzero root transform must also compose in: shift everything by R.
        Vec3 R = v3(-2.0f, 7.0f, 1.0f);
        i32 n2 = cpp_sg_flatten(g, mat4_translate(R), buf.data(), (i32)buf.size());
        Vec3 child_corner_R = v3_add(v3_add(R, child_offset), h);
        check("root transform composes",
              n2 == 24 && has_endpoint(buf.data(), n2, child_corner_R));

        cpp_sg_destroy(g);
    }

    // ----------------------------------------------------------------------
    // (c) Grid of n lines per axis -> 2*n segments, spanning [-size/2,size/2].
    // ----------------------------------------------------------------------
    {
        CppSceneGraph *g = cpp_sg_create();
        i32 node = cpp_sg_add_node(g, -1);
        i32 nlines = 8;
        f32 size = 6.0f;
        cpp_sg_add_grid(g, node, nlines, size, wire);

        i32 n = cpp_sg_flatten(g, I, buf.data(), (i32)buf.size());
        check("grid -> 2*n segments", n == 2 * nlines);

        // Grid lies in the y=0 plane and spans exactly [-size/2, +size/2] in xz.
        Vec3 lo, hi;
        seg_bounds(buf.data(), n, lo, hi);
        f32 half = size * 0.5f;
        check("grid extent x", approx(lo.x, -half) && approx(hi.x, half));
        check("grid extent z", approx(lo.z, -half) && approx(hi.z, half));
        check("grid flat in y", approx(lo.y, 0.0f) && approx(hi.y, 0.0f));

        cpp_sg_destroy(g);
    }

    // ----------------------------------------------------------------------
    // (d) flatten respects the max_segs cap: box has 12 segments; ask for 5.
    // ----------------------------------------------------------------------
    {
        CppSceneGraph *g = cpp_sg_create();
        i32 node = cpp_sg_add_node(g, -1);
        cpp_sg_add_box(g, node, v3(1, 1, 1), wire);

        i32 capped = cpp_sg_flatten(g, I, buf.data(), 5);
        check("max_segs cap returns 5", capped == 5);

        i32 zero = cpp_sg_flatten(g, I, buf.data(), 0);
        check("max_segs 0 returns 0", zero == 0);

        i32 full = cpp_sg_flatten(g, I, buf.data(), 100);
        check("uncapped returns all 12", full == 12);

        cpp_sg_destroy(g);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("SOME FAILED\n");
    return 1;
}
