// test_csg.cpp  -  constructive solid geometry field evaluator.
//
// Checks the boolean semantics and field sign convention of cpp_csg_eval:
//   - a single sphere: field > 0 strictly inside, < 0 well outside;
//   - UNION contains points inside either operand;
//   - INTERSECT contains only points inside both;
//   - SUBTRACT (A - B) removes B's interior from A;
//   - malformed trees (bad root / child index) are rejected.
#include "cathode/cppcore.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int failures = 0, checks = 0;
static void check(const char* n, bool ok) {
    checks++;
    if (ok) std::printf("  ok   %s\n", n);
    else { std::printf("  FAIL %s\n", n); failures++; }
}

// sample the field at integer grid cell (x,y,z)
static float at(const std::vector<float>& f, int nx, int ny, int x, int y, int z) {
    return f[(size_t)(z*ny + y)*nx + x];
}

int main() {
    std::printf("== CATHODE CSG evaluator tests ==\n");
    const int N = 32;
    std::vector<float> field((size_t)N*N*N);
    Vec3 mid{ (float)N/2, (float)N/2, (float)N/2 };

    // --- single sphere radius 8 at center ---
    {
        CsgNode nodes[1];
        nodes[0] = {};
        nodes[0].kind = CSG_SPHERE; nodes[0].a = -1; nodes[0].b = -1;
        nodes[0].center = mid; nodes[0].size = { 8.0f, 0, 0 }; nodes[0].k = 0;
        int rc = cpp_csg_eval(field.data(), N, N, N, nodes, 1, 0);
        check("sphere eval returns 0", rc == 0);
        // center is inside => field > 0 (negated SDF); ~radius/2 in
        check("field positive at sphere center", at(field,N,N,N/2,N/2,N/2) > 0);
        // a far corner is outside => field < 0
        check("field negative outside sphere", at(field,N,N,1,1,1) < 0);
        // magnitude near center ~ +8 (distance to surface)
        check("center field magnitude ~ radius",
              std::fabs(at(field,N,N,N/2,N/2,N/2) - 8.0f) < 1.5f);
    }

    // --- UNION of two spheres: a point inside either is inside the union ---
    {
        CsgNode nodes[3];
        for (auto& n : nodes) n = {};
        nodes[0].kind=CSG_SPHERE; nodes[0].a=-1; nodes[0].b=-1;
        nodes[0].center={ (float)N/2-6, (float)N/2, (float)N/2 }; nodes[0].size={5,0,0};
        nodes[1].kind=CSG_SPHERE; nodes[1].a=-1; nodes[1].b=-1;
        nodes[1].center={ (float)N/2+6, (float)N/2, (float)N/2 }; nodes[1].size={5,0,0};
        nodes[2].kind=CSG_UNION; nodes[2].a=0; nodes[2].b=1; nodes[2].k=0;
        cpp_csg_eval(field.data(), N, N, N, nodes, 3, 2);
        check("union: inside left sphere", at(field,N,N,N/2-6,N/2,N/2) > 0);
        check("union: inside right sphere", at(field,N,N,N/2+6,N/2,N/2) > 0);
        // the midpoint gap (12 apart, r=5 each) is outside both => outside union
        check("union: gap between spheres is outside", at(field,N,N,N/2,N/2,N/2) < 0);
    }

    // --- INTERSECT of two overlapping spheres: only the lens is inside ---
    {
        CsgNode nodes[3];
        for (auto& n : nodes) n = {};
        nodes[0].kind=CSG_SPHERE; nodes[0].a=-1;nodes[0].b=-1;
        nodes[0].center={ (float)N/2-3, (float)N/2, (float)N/2 }; nodes[0].size={7,0,0};
        nodes[1].kind=CSG_SPHERE; nodes[1].a=-1;nodes[1].b=-1;
        nodes[1].center={ (float)N/2+3, (float)N/2, (float)N/2 }; nodes[1].size={7,0,0};
        nodes[2].kind=CSG_INTERSECT; nodes[2].a=0; nodes[2].b=1; nodes[2].k=0;
        cpp_csg_eval(field.data(), N, N, N, nodes, 3, 2);
        // center (in both) inside; far left (only in left sphere) outside intersection
        check("intersect: center in both is inside", at(field,N,N,N/2,N/2,N/2) > 0);
        check("intersect: left-only region is outside",
              at(field,N,N,N/2-9,N/2,N/2) < 0);
    }

    // --- SUBTRACT: big sphere minus a smaller one carves a cavity ---
    {
        CsgNode nodes[3];
        for (auto& n : nodes) n = {};
        nodes[0].kind=CSG_SPHERE; nodes[0].a=-1;nodes[0].b=-1;
        nodes[0].center=mid; nodes[0].size={10,0,0};
        nodes[1].kind=CSG_SPHERE; nodes[1].a=-1;nodes[1].b=-1;
        nodes[1].center=mid; nodes[1].size={5,0,0};
        nodes[2].kind=CSG_SUBTRACT; nodes[2].a=0; nodes[2].b=1; nodes[2].k=0;
        cpp_csg_eval(field.data(), N, N, N, nodes, 3, 2);
        // center is inside the carved-out inner sphere => now OUTSIDE the solid
        check("subtract: carved center is outside", at(field,N,N,N/2,N/2,N/2) < 0);
        // a shell point (r~7-8 from center, inside big, outside small) is inside
        check("subtract: shell region is inside", at(field,N,N,N/2+7,N/2,N/2) > 0);
    }

    // --- malformed trees rejected ---
    {
        CsgNode one[1]; one[0]={}; one[0].kind=CSG_SPHERE; one[0].a=-1; one[0].b=-1;
        one[0].center=mid; one[0].size={4,0,0};
        check("bad root index rejected",
              cpp_csg_eval(field.data(), N,N,N, one, 1, 5) != 0);
        // operator pointing at an out-of-range child => non-zero (field flagged)
        CsgNode bad[1]; bad[0]={}; bad[0].kind=CSG_UNION; bad[0].a=9; bad[0].b=9;
        check("bad child index flagged",
              cpp_csg_eval(field.data(), N,N,N, bad, 1, 0) != 0);
        check("null field rejected",
              cpp_csg_eval(nullptr, N,N,N, one, 1, 0) != 0);
        // a self-referential operator (a=0 points at itself) is a CYCLE: must be
        // rejected up front, not drive eval into runaway recursion.
        CsgNode loop[1]; loop[0]={}; loop[0].kind=CSG_UNION; loop[0].a=0; loop[0].b=0;
        check("self-loop node rejected (no infinite recursion)",
              cpp_csg_eval(field.data(), N,N,N, loop, 1, 0) != 0);
        // a two-node mutual cycle (0->1->0) must also be rejected.
        CsgNode cyc[2];
        cyc[0]={}; cyc[0].kind=CSG_UNION; cyc[0].a=1; cyc[0].b=1;
        cyc[1]={}; cyc[1].kind=CSG_UNION; cyc[1].a=0; cyc[1].b=0;
        check("mutual cycle rejected",
              cpp_csg_eval(field.data(), N,N,N, cyc, 2, 0) != 0);
        // a valid DAG where two operators SHARE a leaf child is NOT a cycle.
        CsgNode dag[3];
        dag[0]={}; dag[0].kind=CSG_SPHERE; dag[0].a=-1; dag[0].b=-1; dag[0].center=mid; dag[0].size={5,0,0};
        dag[1]={}; dag[1].kind=CSG_UNION; dag[1].a=0; dag[1].b=0;   /* shares leaf 0 */
        dag[2]={}; dag[2].kind=CSG_UNION; dag[2].a=1; dag[2].b=0;   /* also refs 0 */
        check("shared-leaf DAG accepted (not a cycle)",
              cpp_csg_eval(field.data(), N,N,N, dag, 3, 2) == 0);
    }

    std::printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) std::printf("ALL PASS\n");
    return failures ? 1 : 0;
}
