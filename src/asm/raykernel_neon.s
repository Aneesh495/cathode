// ==========================================================================
// raykernel_neon.s — hand-written AArch64 NEON batched ray intersection.
// Mach-O / Apple ABI (arm64-darwin). 4 primitives per call, all lanes in
// parallel. See src/asm/simd_neon.s for the full ABI convention notes.
//
// ABI recap that matters here:
//   * Exported C symbols get a leading underscore (_rk_ray4_spheres_neon).
//   * Pointer args arrive in x0..x3; there are no float args.
//   * v8..v15 are callee-saved (low 64 bits). Both routines below are LEAF
//     functions that touch NO callee-saved register — every vector temporary
//     lives in v0..v7 or v16..v31, so there is nothing to save/restore.
//   * SoA layout: each of the 4 lanes is one element of a .4s q-register, so a
//     single LD1 pulls "the x of all four primitives", etc. The single ray is
//     broadcast to all lanes with LD1R.
// ==========================================================================

.text

// --------------------------------------------------------------------------
// void rk_ray4_spheres_neon(float *out4  /*x0*/, const float *ro  /*x1*/,
//                           const float *rd /*x2*/, const float *sph /*x3*/)
//
// sph SoA = [cx0..3, cy0..3, cz0..3, r0..3] (16 floats). rd is assumed
// normalized so the quadratic leading coefficient a = dot(d,d) = 1.
//
//   oc   = ro - c
//   b    = dot(oc, rd)
//   c2   = dot(oc, oc) - r^2
//   disc = b*b - c2
//   sq   = sqrt(max(disc, 0))          // clamp so a missing lane can't NaN
//   t0   = -b - sq   (near root)
//   t1   = -b + sq   (far  root)
//   t    = t0>eps ? t0 : t1            // prefer the near root when in front
//   hit  = disc>=0 AND (t0>eps OR t1>eps)
//   out  = hit ? t : -1                // -1 sentinel = miss
// --------------------------------------------------------------------------
.globl _rk_ray4_spheres_neon
.p2align 2
_rk_ray4_spheres_neon:
    // ---- load the 4 spheres (SoA) ----
    ld1     {v0.4s, v1.4s, v2.4s, v3.4s}, [x3]   // v0=cx v1=cy v2=cz v3=r

    // ---- broadcast the single ray origin across all 4 lanes ----
    ld1r    {v16.4s}, [x1]                        // ro.x in all lanes
    add     x4, x1, #4
    ld1r    {v17.4s}, [x4]                        // ro.y
    add     x4, x1, #8
    ld1r    {v18.4s}, [x4]                        // ro.z
    // ---- broadcast the ray direction ----
    ld1r    {v19.4s}, [x2]                        // rd.x
    add     x4, x2, #4
    ld1r    {v20.4s}, [x4]                        // rd.y
    add     x4, x2, #8
    ld1r    {v21.4s}, [x4]                        // rd.z

    // ---- oc = ro - c ----
    fsub    v4.4s, v16.4s, v0.4s                 // ocx
    fsub    v5.4s, v17.4s, v1.4s                 // ocy
    fsub    v6.4s, v18.4s, v2.4s                 // ocz

    // ---- b = dot(oc, rd) ----
    fmul    v7.4s, v4.4s, v19.4s
    fmla    v7.4s, v5.4s, v20.4s
    fmla    v7.4s, v6.4s, v21.4s                 // v7 = b   (v16..v21 now free)

    // ---- c2 = dot(oc,oc) - r^2 ----
    fmul    v22.4s, v4.4s, v4.4s
    fmla    v22.4s, v5.4s, v5.4s
    fmla    v22.4s, v6.4s, v6.4s                 // oc.oc
    fmul    v23.4s, v3.4s, v3.4s                 // r^2
    fsub    v22.4s, v22.4s, v23.4s               // v22 = c2

    // ---- disc = b*b - c2 ----
    fmul    v24.4s, v7.4s, v7.4s
    fsub    v24.4s, v24.4s, v22.4s               // v24 = disc

    // ---- sq = sqrt(max(disc,0)) : clamp keeps missing lanes finite (no NaN) --
    movi    v25.4s, #0
    fmax    v26.4s, v24.4s, v25.4s
    fsqrt   v26.4s, v26.4s                       // v26 = sq

    // ---- the two roots ----
    fneg    v27.4s, v7.4s                        // -b
    fsub    v28.4s, v27.4s, v26.4s               // t0 = -b - sq  (near)
    fadd    v29.4s, v27.4s, v26.4s               // t1 = -b + sq  (far)

    // ---- eps = 1e-4 in all lanes ----
    adrp    x5, Lrk_eps@PAGE
    add     x5, x5, Lrk_eps@PAGEOFF
    ld1r    {v31.4s}, [x5]

    // ---- masks: t0>eps, t1>eps, disc>=0 ----
    fcmgt   v16.4s, v28.4s, v31.4s               // mask_t0
    fcmgt   v17.4s, v29.4s, v31.4s               // mask_t1
    fcmge   v18.4s, v24.4s, v25.4s               // mask_disc (disc>=0)

    // ---- pick near root when it is in front, else the far root ----
    // BIT vd,vn,vm : insert bits of vn into vd where vm is set.
    bit     v29.16b, v28.16b, v16.16b            // v29 = mask_t0 ? t0 : t1

    // ---- hit = (mask_t0 | mask_t1) & mask_disc ----
    orr     v16.16b, v16.16b, v17.16b
    and     v16.16b, v16.16b, v18.16b            // v16 = hit mask

    // ---- out = hit ? t : -1 ----
    fmov    v19.4s, #-1.0
    bit     v19.16b, v29.16b, v16.16b
    st1     {v19.4s}, [x0]
    ret

// --------------------------------------------------------------------------
// void rk_ray4_aabb_neon(float *out4  /*x0*/, const float *ro  /*x1*/,
//                        const float *rd /*x2*/, const float *bb /*x3*/)
//
// bb SoA = [minx0..3, miny0..3, minz0..3, maxx0..3, maxy0..3, maxz0..3] (24 f).
// Branchless slab method. rd need not be normalized.
//
// Axis-parallel rays (a direction component == 0) would make 1/d = +-inf and
// then inf*0 = NaN. To stay branchless AND avoid inf/NaN entirely, nudge any
// |d| < tiny to copysign(tiny, d): 1/d becomes a huge-but-finite value of the
// correct sign, so (min-o)*inv and (max-o)*inv stay finite. That axis then
// contributes an effectively unbounded slab [-huge,+huge] when the origin is
// inside it, and forces a miss when the origin is outside it — the same
// outcome as the reference's explicit "parallel: skip or miss" branch.
//
//   inv   = 1 / d
//   tlo   = (min - o)*inv ; thi = (max - o)*inv
//   near  = min(tlo,thi)  ; far  = max(tlo,thi)     (per axis)
//   tmin  = max over axes of near ; tmax = min over axes of far
//   hit   = tmax>=tmin AND tmax>=0
//   t     = tmin>0 ? tmin : tmax                    (origin-inside -> tmax)
//   hit  &= |t| < 1e29                              (reject unbounded interval)
//   out   = hit ? t : -1
// --------------------------------------------------------------------------
.globl _rk_ray4_aabb_neon
.p2align 2
_rk_ray4_aabb_neon:
    // ---- load the 6 slab planes (SoA) ----
    ld1     {v0.4s, v1.4s, v2.4s}, [x3]          // v0=minx v1=miny v2=minz
    add     x4, x3, #48
    ld1     {v3.4s, v4.4s, v5.4s}, [x4]          // v3=maxx v4=maxy v5=maxz

    // ---- broadcast ray origin / direction ----
    ld1r    {v16.4s}, [x1]                       // ro.x
    add     x5, x1, #4
    ld1r    {v17.4s}, [x5]                       // ro.y
    add     x5, x1, #8
    ld1r    {v18.4s}, [x5]                       // ro.z
    ld1r    {v19.4s}, [x2]                       // rd.x
    add     x5, x2, #4
    ld1r    {v20.4s}, [x5]                       // rd.y
    add     x5, x2, #8
    ld1r    {v21.4s}, [x5]                       // rd.z

    // ---- constants: tiny (1e-20) and sign mask (0x80000000) ----
    adrp    x6, Lrk_tiny@PAGE
    add     x6, x6, Lrk_tiny@PAGEOFF
    ld1r    {v22.4s}, [x6]                       // v22 = tiny (all lanes)
    movi    v23.4s, #0x80, lsl #24               // v23 = 0x80000000 (sign bit)

    // ---- nudge near-zero dir: d = |d|<tiny ? copysign(tiny,d) : d ----
    // copysign(tiny,d) = (d & signbit) | tiny   (tiny > 0, so its sign bit is 0)
    // X
    and     v24.16b, v19.16b, v23.16b            // sign(dx)
    orr     v24.16b, v24.16b, v22.16b            // copysign(tiny, dx)
    fabs    v25.4s, v19.4s
    fcmgt   v26.4s, v22.4s, v25.4s               // tiny > |dx| ?
    bit     v19.16b, v24.16b, v26.16b
    // Y
    and     v24.16b, v20.16b, v23.16b
    orr     v24.16b, v24.16b, v22.16b
    fabs    v25.4s, v20.4s
    fcmgt   v26.4s, v22.4s, v25.4s
    bit     v20.16b, v24.16b, v26.16b
    // Z
    and     v24.16b, v21.16b, v23.16b
    orr     v24.16b, v24.16b, v22.16b
    fabs    v25.4s, v21.4s
    fcmgt   v26.4s, v22.4s, v25.4s
    bit     v21.16b, v24.16b, v26.16b

    // ---- inverse direction (v22 tiny / v23 signmask now free) ----
    fmov    v22.4s, #1.0
    fdiv    v23.4s, v22.4s, v19.4s               // invx
    fdiv    v24.4s, v22.4s, v20.4s               // invy
    fdiv    v25.4s, v22.4s, v21.4s               // invz

    // ---- X slab ----
    fsub    v6.4s, v0.4s, v16.4s
    fmul    v6.4s, v6.4s, v23.4s                 // tlo_x
    fsub    v7.4s, v3.4s, v16.4s
    fmul    v7.4s, v7.4s, v23.4s                 // thi_x
    fmin    v26.4s, v6.4s, v7.4s                 // near_x
    fmax    v27.4s, v6.4s, v7.4s                 // far_x
    // ---- Y slab ----
    fsub    v6.4s, v1.4s, v17.4s
    fmul    v6.4s, v6.4s, v24.4s
    fsub    v7.4s, v4.4s, v17.4s
    fmul    v7.4s, v7.4s, v24.4s
    fmin    v28.4s, v6.4s, v7.4s                 // near_y
    fmax    v29.4s, v6.4s, v7.4s                 // far_y
    // ---- Z slab ----
    fsub    v6.4s, v2.4s, v18.4s
    fmul    v6.4s, v6.4s, v25.4s
    fsub    v7.4s, v5.4s, v18.4s
    fmul    v7.4s, v7.4s, v25.4s
    fmin    v30.4s, v6.4s, v7.4s                 // near_z
    fmax    v31.4s, v6.4s, v7.4s                 // far_z

    // ---- tmin = max(near_*) ; tmax = min(far_*) ----
    fmax    v26.4s, v26.4s, v28.4s
    fmax    v26.4s, v26.4s, v30.4s               // v26 = tmin
    fmin    v27.4s, v27.4s, v29.4s
    fmin    v27.4s, v27.4s, v31.4s               // v27 = tmax

    // ---- hit = (tmax>=tmin) AND (tmax>=0) ----
    movi    v16.4s, #0
    fcmge   v17.4s, v27.4s, v26.4s               // tmax>=tmin
    fcmge   v18.4s, v27.4s, v16.4s               // tmax>=0
    and     v17.16b, v17.16b, v18.16b            // hit mask

    // ---- t = tmin>0 ? tmin : tmax ----
    fcmgt   v19.4s, v26.4s, v16.4s               // tmin>0
    mov     v20.16b, v27.16b                     // default = tmax
    bit     v20.16b, v26.16b, v19.16b            // v20 = tmin>0 ? tmin : tmax

    // ---- reject unbounded interval (parallel-inside ray): hit &= |t| < 1e29 --
    adrp    x6, Lrk_big@PAGE
    add     x6, x6, Lrk_big@PAGEOFF
    ld1r    {v21.4s}, [x6]                       // 1e29
    fabs    v22.4s, v20.4s
    fcmgt   v23.4s, v21.4s, v22.4s               // 1e29 > |t| ?
    and     v17.16b, v17.16b, v23.16b

    // ---- out = hit ? t : -1 ----
    fmov    v24.4s, #-1.0
    bit     v24.16b, v20.16b, v17.16b
    st1     {v24.4s}, [x0]
    ret

// --------------------------------------------------------------------------
// Read-only float constants (broadcast to all 4 lanes with LD1R).
// --------------------------------------------------------------------------
.p2align 2
Lrk_eps:
    .float 0.0001
Lrk_tiny:
    .float 1.0e-20
Lrk_big:
    .float 1.0e29
