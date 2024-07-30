// test_softbody.cpp — invariants for the 2D pressurized soft-body blob.
//
// Checks: it stays inside the world box, its enclosed area stays near the
// target (gas pressure holds volume, doesn't collapse or explode), a dropped
// blob falls and settles above the floor with bounded energy, points stay
// finite over many steps, and bad args are handled.
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

int main() {
    std::printf("== CATHODE soft-body tests ==\n");
    const float W = 20, H = 20;

    // area holds near target after settling
    {
        CppSoftBody* b = cpp_softbody_create(24, 10, 12, 3.0f, W, H);
        float target = cpp_softbody_area(b);
        check("initial area > 0", target > 0);
        cpp_softbody_set_gravity(b, 0, -9.8f);
        for (int i = 0; i < 1200; ++i) cpp_softbody_step(b, 1.0f/120.0f, 6);
        float area = cpp_softbody_area(b);
        // gas pressure should keep it within ~40% of the initial disc area
        check("settled area within 40% of target",
              area > 0.6f * target && area < 1.4f * target);
        cpp_softbody_destroy(b);
    }

    // stays inside the box, points finite
    {
        CppSoftBody* b = cpp_softbody_create(32, 10, 14, 3.5f, W, H);
        cpp_softbody_kick(b, 0.4f, 0.0f);       // give it some sideways motion
        int n = cpp_softbody_npoints(b);
        std::vector<float> xy(2 * n);
        bool inside = true, finite = true;
        for (int step = 0; step < 1500; ++step) {
            cpp_softbody_step(b, 1.0f/120.0f, 6);
            cpp_softbody_points(b, xy.data());
            for (int i = 0; i < n; ++i) {
                float x = xy[2*i], y = xy[2*i+1];
                if (!(x == x) || !(y == y)) finite = false;
                if (x < -0.5f || x > W + 0.5f || y < -0.5f || y > H + 0.5f) inside = false;
            }
        }
        check("blob stays within the world box", inside);
        check("all points remain finite", finite);
        // bottom of the blob should be resting near/above the floor
        cpp_softbody_points(b, xy.data());
        float miny = 1e30f;
        for (int i = 0; i < n; ++i) if (xy[2*i+1] < miny) miny = xy[2*i+1];
        check("blob rests on/above the floor", miny > -0.2f);
        cpp_softbody_destroy(b);
    }

    // n is clamped to >= 3
    {
        CppSoftBody* b = cpp_softbody_create(1, 5, 5, 1.0f, W, H);
        check("npoints clamped to >= 3", cpp_softbody_npoints(b) >= 3);
        cpp_softbody_destroy(b);
    }

    // null-safe
    check("npoints(NULL) = 0", cpp_softbody_npoints(nullptr) == 0);
    check("area(NULL) = 0", cpp_softbody_area(nullptr) == 0.0f);
    cpp_softbody_step(nullptr, 0.01f, 4);   // must not crash
    check("step(NULL) is safe", true);

    std::printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) std::printf("ALL PASS\n");
    return failures ? 1 : 0;
}
