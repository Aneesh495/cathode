// ==========================================================================
// gravkernel_neon.s — NEON gravitational force accumulation (Mach-O/Apple ABI).
//
// void grav_accum_neon(float *out3 x0, float px s0, float py s1, float pz s2,
//                      const float *sx x1, const float *sy x2,
//                      const float *sz x3, const float *sm x4,
//                      unsigned long n x5, float g s3, float eps2 s4)
//
// Processes 4 source bodies per iteration:
//   dx = sx-px ; dy = sy-py ; dz = sz-pz
//   r2 = dx^2+dy^2+dz^2 + eps2
//   invr  = 1/sqrt(r2)              (frsqrte + 2 Newton-Raphson)
//   f = g * sm * invr^3
//   ax += dx*f ; ay += dy*f ; az += dz*f    (4 lanes each)
// Then horizontally sum the 4 lanes of ax/ay/az and add the scalar tail.
//
// Uses only caller-saved SIMD regs (v0..v7, v16..v31). rsqrt matches the exact
// frsqrte/frsqrts refinement used in simd_neon.s so results agree with the C
// reference (which uses 1/sqrtf) to within float tolerance.
// ==========================================================================

.text
.globl _grav_accum_neon
.p2align 2
_grav_accum_neon:
    // broadcast target position + constants to all lanes
    dup     v20.4s, v0.s[0]          // px
    dup     v21.4s, v1.s[0]          // py
    dup     v22.4s, v2.s[0]          // pz
    dup     v23.4s, v3.s[0]          // g
    dup     v24.4s, v4.s[0]          // eps2

    movi    v16.4s, #0               // ax accumulator
    movi    v17.4s, #0               // ay
    movi    v18.4s, #0               // az

    lsr     x6, x5, #2               // vector iterations = n/4
    cbz     x6, .Lg_tail
.Lg_loop:
    ld1     {v0.4s}, [x1], #16       // sx (4)
    ld1     {v1.4s}, [x2], #16       // sy
    ld1     {v2.4s}, [x3], #16       // sz
    ld1     {v3.4s}, [x4], #16       // sm
    fsub    v0.4s, v0.4s, v20.4s     // dx
    fsub    v1.4s, v1.4s, v21.4s     // dy
    fsub    v2.4s, v2.4s, v22.4s     // dz
    // r2 = dx^2+dy^2+dz^2+eps2
    fmul    v4.4s, v0.4s, v0.4s
    fmla    v4.4s, v1.4s, v1.4s
    fmla    v4.4s, v2.4s, v2.4s
    fadd    v4.4s, v4.4s, v24.4s     // v4 = r2
    // invr = 1/sqrt(r2) via frsqrte + 2 NR steps
    frsqrte v5.4s, v4.4s
    fmul    v6.4s, v5.4s, v5.4s
    frsqrts v6.4s, v4.4s, v6.4s
    fmul    v5.4s, v5.4s, v6.4s
    fmul    v6.4s, v5.4s, v5.4s
    frsqrts v6.4s, v4.4s, v6.4s
    fmul    v5.4s, v5.4s, v6.4s      // v5 = invr
    // f = g * sm * invr^3
    fmul    v7.4s, v5.4s, v5.4s
    fmul    v7.4s, v7.4s, v5.4s      // invr^3
    fmul    v7.4s, v7.4s, v3.4s      // * sm
    fmul    v7.4s, v7.4s, v23.4s     // * g   -> v7 = f
    // accumulate
    fmla    v16.4s, v0.4s, v7.4s     // ax += dx*f
    fmla    v17.4s, v1.4s, v7.4s     // ay += dy*f
    fmla    v18.4s, v2.4s, v7.4s     // az += dz*f
    subs    x6, x6, #1
    b.ne    .Lg_loop
.Lg_tail:
    // horizontal-sum the 4 lanes of each accumulator into s16/s17/s18
    faddp   v16.4s, v16.4s, v16.4s
    faddp   v16.2s, v16.2s, v16.2s
    faddp   v17.4s, v17.4s, v17.4s
    faddp   v17.2s, v17.2s, v17.2s
    faddp   v18.4s, v18.4s, v18.4s
    faddp   v18.2s, v18.2s, v18.2s
    // scalar tail for remaining (n & 3) sources
    and     x7, x5, #3
    cbz     x7, .Lg_store
.Lg_stail:
    ldr     s0, [x1], #4             // sx
    ldr     s1, [x2], #4             // sy
    ldr     s2, [x3], #4             // sz
    ldr     s3, [x4], #4             // sm
    fsub    s0, s0, s20              // dx  (s20 = px lane0)
    fsub    s1, s1, s21              // dy
    fsub    s2, s2, s22              // dz
    fmul    s4, s0, s0
    fmadd   s4, s1, s1, s4
    fmadd   s4, s2, s2, s4
    fadd    s4, s4, s24              // r2
    frsqrte s5, s4
    fmul    s6, s5, s5
    frsqrts s6, s4, s6
    fmul    s5, s5, s6
    fmul    s6, s5, s5
    frsqrts s6, s4, s6
    fmul    s5, s5, s6              // invr
    fmul    s7, s5, s5
    fmul    s7, s7, s5
    fmul    s7, s7, s3
    fmul    s7, s7, s23            // f
    fmadd   s16, s0, s7, s16       // ax += dx*f
    fmadd   s17, s1, s7, s17
    fmadd   s18, s2, s7, s18
    subs    x7, x7, #1
    b.ne    .Lg_stail
.Lg_store:
    str     s16, [x0]
    str     s17, [x0, #4]
    str     s18, [x0, #8]
    ret
