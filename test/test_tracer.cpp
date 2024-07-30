// test_tracer.cpp — headless correctness test for the BVH path tracer.
//
// Builds tiny scenes, renders small images, and asserts physical/statistical
// properties of the output. Prints PASS/FAIL and returns non-zero on failure.
#include "cathode/cppcore.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"

#include <cstdio>
#include <cmath>

static int failures = 0, checks = 0;
static void ok(const char *name, bool cond) {
    ++checks;
    if (cond) std::printf("  ok   %s\n", name);
    else { std::printf("  FAIL %s\n", name); ++failures; }
}

// Perceptual-ish luminance of a linear-RGB pixel.
static float lum(const Framebuffer *fb, int x, int y) {
    Color3 c = fb_get(fb, x, y);
    return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

int main(void) {
    std::printf("== CATHODE path tracer ==\n");

    const int W = 48, H = 36;
    const int SPP = 8, BOUNCES = 6;

    // ---------------------------------------------------------------------
    // Scene A: a big ground sphere, a small diffuse sphere, and a bright
    // emissive light placed high so the upper-center of the frame points at
    // it while the lower corners point down at the (dim) ground.
    // ---------------------------------------------------------------------
    CppTracer *t = cpp_tracer_create(W, H);
    ok("create returns handle", t != nullptr);

    cpp_tracer_clear_scene(t);
    // Big Lambertian ground sphere (top surface at y=0).
    cpp_tracer_add_sphere(t, v3(0.0f, -1000.0f, 0.0f), 1000.0f,
                          CPPMAT_LAMBERT, col3(0.5f, 0.5f, 0.5f), 0.0f);
    // Small reddish diffuse sphere sitting on the ground.
    cpp_tracer_add_sphere(t, v3(0.0f, 0.5f, 0.0f), 0.5f,
                          CPPMAT_LAMBERT, col3(0.75f, 0.30f, 0.30f), 0.0f);
    // A metal sphere off to the side (exercises the reflect+fuzz path & BVH).
    cpp_tracer_add_sphere(t, v3(1.4f, 0.5f, 0.3f), 0.5f,
                          CPPMAT_METAL, col3(0.8f, 0.8f, 0.85f), 0.05f);
    // A glass sphere on the other side (dielectric path).
    cpp_tracer_add_sphere(t, v3(-1.4f, 0.5f, 0.3f), 0.5f,
                          CPPMAT_DIELECTRIC, col3(1.0f, 1.0f, 1.0f), 1.5f);
    // Big bright emissive light overhead-forward: spans the upper frame.
    cpp_tracer_add_sphere(t, v3(0.0f, 7.0f, -1.0f), 3.0f,
                          CPPMAT_EMISSIVE, col3(1.0f, 0.95f, 0.9f), 25.0f);

    // Dark sky so the emissive light dominates the lighting (clear contrast).
    cpp_tracer_set_sky(t, col3(0.02f, 0.02f, 0.04f), col3(0.0f, 0.0f, 0.0f));
    // Horizontal camera; +30deg top of frame points into the light.
    cpp_tracer_set_camera(t, v3(0.0f, 2.0f, 6.0f), v3(0.0f, 2.0f, 0.0f),
                          60.0f * CT_DEG2RAD, 0.0f);
    cpp_tracer_build(t);

    cpp_tracer_reset_accum(t);
    cpp_tracer_render(t, SPP, BOUNCES);

    Framebuffer *fb = fb_create(W, H);
    cpp_tracer_resolve(t, fb);

    // --- finite & non-negative -------------------------------------------
    bool finite = true, nonneg = true;
    for (int i = 0; i < W * H * 3; ++i) {
        float v = fb->px[i];
        if (!std::isfinite(v)) finite = false;
        if (v < 0.0f) nonneg = false;
    }
    ok("image finite", finite);
    ok("image non-negative", nonneg);

    // --- not all pixels equal --------------------------------------------
    float mn = 1e30f, mx = -1e30f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float l = lum(fb, x, y);
            if (l < mn) mn = l;
            if (l > mx) mx = l;
        }
    ok("pixels vary (min<max)", mx - mn > 1e-3f);

    // --- rays were cast --------------------------------------------------
    i64 rays = cpp_tracer_rays_cast(t);
    std::printf("  info rays_cast = %lld\n", (long long)rays);
    ok("rays_cast > 0", rays > 0);

    // --- pixel toward the light is brighter than a downward corner -------
    // Top-center points up into the emissive light; bottom-left points down
    // at the dimly-lit ground. Average a small neighborhood to reduce noise.
    auto avg_lum = [&](int cx, int cy, int rad) {
        float s = 0.0f; int n = 0;
        for (int dy = -rad; dy <= rad; ++dy)
            for (int dx = -rad; dx <= rad; ++dx) {
                int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                s += lum(fb, x, y); ++n;
            }
        return n ? s / (float)n : 0.0f;
    };
    float toward_light = avg_lum(W / 2, 1, 1);       // top-center
    float downward     = avg_lum(2, H - 2, 1);       // bottom-left corner
    std::printf("  info toward_light=%.4f downward=%.4f\n", toward_light, downward);
    ok("toward-light brighter than downward corner", toward_light > downward * 2.0f + 0.05f);

    fb_destroy(fb);
    cpp_tracer_destroy(t);

    // ---------------------------------------------------------------------
    // Scene B: a purely emissive sphere filling the center of the frame.
    // The center pixel should read the raw HDR emission (a bright pixel),
    // while the corners see only dark sky.
    // ---------------------------------------------------------------------
    CppTracer *t2 = cpp_tracer_create(W, H);
    cpp_tracer_clear_scene(t2);
    cpp_tracer_add_sphere(t2, v3(0.0f, 0.0f, 0.0f), 2.0f,
                          CPPMAT_EMISSIVE, col3(1.0f, 1.0f, 1.0f), 5.0f);
    cpp_tracer_set_sky(t2, col3(0.0f, 0.0f, 0.0f), col3(0.0f, 0.0f, 0.0f));
    cpp_tracer_set_camera(t2, v3(0.0f, 0.0f, 6.0f), v3(0.0f, 0.0f, 0.0f),
                          60.0f * CT_DEG2RAD, 0.0f);
    cpp_tracer_build(t2);
    cpp_tracer_reset_accum(t2);
    cpp_tracer_render(t2, SPP, BOUNCES);

    Framebuffer *fb2 = fb_create(W, H);
    cpp_tracer_resolve(t2, fb2);

    float center = lum(fb2, W / 2, H / 2);
    float corner = lum(fb2, 1, 1);
    std::printf("  info emissive center=%.4f corner=%.4f\n", center, corner);
    ok("emissive sphere yields bright pixel (center>1)", center > 1.0f);
    ok("emissive center brighter than dark corner", center > corner + 1.0f);

    // Resolve on a fresh (reset) accumulator with a second render doubles the
    // sample count but must keep the *average* stable (progressive refinement).
    cpp_tracer_render(t2, SPP, BOUNCES);   // now 16 spp total
    cpp_tracer_resolve(t2, fb2);
    float center2 = lum(fb2, W / 2, H / 2);
    std::printf("  info progressive center(16spp)=%.4f\n", center2);
    ok("progressive average stable", std::fabs(center2 - center) < 0.5f);

    fb_destroy(fb2);
    cpp_tracer_destroy(t2);

    std::printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) std::printf("ALL PASS\n");
    else           std::printf("SOME FAILED\n");
    return failures ? 1 : 0;
}
