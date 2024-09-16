// ==========================================================================
// softbody.cpp  -  2D pressurized soft body (Verlet blob).
//
// A closed ring of point masses joined by springs, inflated by an ideal-gas
// pressure force (Matthias Müller's "pressurized soft body"). Each step:
//
//   1. Verlet-integrate points under gravity (position-based; velocity is
//      implicit in the previous position).
//   2. Relax the spring distance-constraints for `iterations` passes so the
//      outline keeps its rest shape without stiff explicit springs.
//   3. Compute the enclosed area (shoelace) and push each edge outward along
//      its outward normal with force ∝ (targetArea/area − 1)  -  the gas law that
//      makes the blob hold volume and bounce back when squashed.
//   4. Resolve collisions with the world box (floor + walls) by projecting
//      points back inside and damping the normal component.
//
// All state is POD behind an opaque handle; the boundary stays extern "C".
// ==========================================================================
#include "cathode/cppcore.h"
#include <vector>
#include <cmath>

namespace {

struct V2 { float x, y; };

struct Body {
    int n;
    std::vector<V2> pos, prev;   // current + previous positions (Verlet)
    float rest_len;              // spring rest length between adjacent points
    float target_area;          // area the gas pressure tries to hold
    float pressure;             // gas amount multiplier
    float gx, gy;               // gravity
    float w, h;                 // world box
};

static float ring_area(const Body& b) {
    // shoelace formula (signed); return absolute area
    double a = 0.0;
    for (int i = 0; i < b.n; ++i) {
        const V2& p = b.pos[i];
        const V2& q = b.pos[(i + 1) % b.n];
        a += (double)p.x * q.y - (double)q.x * p.y;
    }
    return (float)std::fabs(a * 0.5);
}

} // namespace

struct CppSoftBody { Body impl; };

extern "C" CppSoftBody* cpp_softbody_create(i32 npoints, f32 cx, f32 cy,
                                            f32 radius, f32 w, f32 h) {
    if (npoints < 3) npoints = 3;
    if (npoints > 512) npoints = 512;
    CppSoftBody* sb = new CppSoftBody();
    Body& b = sb->impl;
    b.n = npoints;
    b.pos.resize(npoints);
    b.prev.resize(npoints);
    for (int i = 0; i < npoints; ++i) {
        float a = 6.2831853f * (float)i / (float)npoints;
        b.pos[i] = { cx + radius * std::cos(a), cy + radius * std::sin(a) };
        b.prev[i] = b.pos[i];   // start at rest
    }
    // rest length between adjacent points on the circle
    float chord = 2.0f * radius * std::sin(3.1415926f / (float)npoints);
    b.rest_len = chord;
    b.target_area = 3.1415926f * radius * radius;
    b.pressure = 1.0f;
    b.gx = 0.0f; b.gy = -9.8f;
    b.w = w; b.h = h;
    return sb;
}

extern "C" void cpp_softbody_destroy(CppSoftBody* b) { delete b; }

extern "C" void cpp_softbody_set_gravity(CppSoftBody* b, f32 gx, f32 gy) {
    if (b) { b->impl.gx = gx; b->impl.gy = gy; }
}
extern "C" void cpp_softbody_set_pressure(CppSoftBody* b, f32 p) {
    if (b) b->impl.pressure = p;
}
extern "C" void cpp_softbody_kick(CppSoftBody* b, f32 vx, f32 vy) {
    if (!b) return;
    // shift prev backward => imparts velocity (Verlet: v = pos - prev)
    for (int i = 0; i < b->impl.n; ++i) {
        b->impl.prev[i].x -= vx;
        b->impl.prev[i].y -= vy;
    }
}

extern "C" void cpp_softbody_step(CppSoftBody* sb, f32 dt, i32 iterations) {
    if (!sb || dt <= 0.0f) return;
    Body& b = sb->impl;
    if (iterations < 1) iterations = 4;
    const float damp = 0.999f;

    // 1. Verlet integrate under gravity
    for (int i = 0; i < b.n; ++i) {
        V2 cur = b.pos[i];
        float vx = (cur.x - b.prev[i].x) * damp;
        float vy = (cur.y - b.prev[i].y) * damp;
        b.prev[i] = cur;
        b.pos[i].x = cur.x + vx + b.gx * dt * dt;
        b.pos[i].y = cur.y + vy + b.gy * dt * dt;
    }

    // 2. relax spring distance constraints (ring neighbours)
    for (int it = 0; it < iterations; ++it) {
        for (int i = 0; i < b.n; ++i) {
            int j = (i + 1) % b.n;
            float dx = b.pos[j].x - b.pos[i].x;
            float dy = b.pos[j].y - b.pos[i].y;
            float d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-6f) continue;
            float diff = (d - b.rest_len) / d * 0.5f;
            float ox = dx * diff, oy = dy * diff;
            b.pos[i].x += ox; b.pos[i].y += oy;
            b.pos[j].x -= ox; b.pos[j].y -= oy;
        }
    }

    // 3. pressure force: push each vertex out along the local outward normal,
    //    scaled by how much the blob is under-inflated vs its target area.
    float area = ring_area(b);
    if (area < 1e-4f) area = 1e-4f;
    float inflate = b.pressure * (b.target_area / area - 1.0f);
    // clamp so a fully-collapsed blob can't launch to infinity in one step
    if (inflate > 2.0f) inflate = 2.0f;
    if (inflate < -1.0f) inflate = -1.0f;
    for (int i = 0; i < b.n; ++i) {
        int prev = (i - 1 + b.n) % b.n;
        int next = (i + 1) % b.n;
        // outward normal ~ perpendicular to the edge from prev to next
        float ex = b.pos[next].x - b.pos[prev].x;
        float ey = b.pos[next].y - b.pos[prev].y;
        float nx = ey, ny = -ex;   // right-hand perpendicular (CCW ring => outward)
        float len = std::sqrt(nx * nx + ny * ny);
        if (len < 1e-6f) continue;
        nx /= len; ny /= len;
        float f = inflate * b.rest_len * 0.5f;
        b.pos[i].x += nx * f;
        b.pos[i].y += ny * f;
    }

    // 4. collide with the world box: clamp inside and kill the normal velocity
    const float e = 0.3f;   // restitution against walls
    for (int i = 0; i < b.n; ++i) {
        if (b.pos[i].y < 0.0f) {
            b.pos[i].y = 0.0f;
            float vy = b.pos[i].y - b.prev[i].y;
            b.prev[i].y = b.pos[i].y + vy * e;
        }
        if (b.pos[i].y > b.h) {
            b.pos[i].y = b.h;
            float vy = b.pos[i].y - b.prev[i].y;
            b.prev[i].y = b.pos[i].y + vy * e;
        }
        if (b.pos[i].x < 0.0f) {
            b.pos[i].x = 0.0f;
            float vx = b.pos[i].x - b.prev[i].x;
            b.prev[i].x = b.pos[i].x + vx * e;
        }
        if (b.pos[i].x > b.w) {
            b.pos[i].x = b.w;
            float vx = b.pos[i].x - b.prev[i].x;
            b.prev[i].x = b.pos[i].x + vx * e;
        }
    }
}

extern "C" i32 cpp_softbody_npoints(const CppSoftBody* b) {
    return b ? b->impl.n : 0;
}
extern "C" void cpp_softbody_points(const CppSoftBody* b, f32* out_xy) {
    if (!b || !out_xy) return;
    for (int i = 0; i < b->impl.n; ++i) {
        out_xy[2 * i] = b->impl.pos[i].x;
        out_xy[2 * i + 1] = b->impl.pos[i].y;
    }
}
extern "C" f32 cpp_softbody_area(const CppSoftBody* b) {
    return b ? ring_area(b->impl) : 0.0f;
}
