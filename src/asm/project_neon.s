// ==========================================================================
// project_neon.s — batched 3D point projection (Mach-O / Apple AArch64 ABI).
//
// void project_points_neon(float *out_xyz x0, unsigned char *vis x1,
//                          const float *pts x2, unsigned long n x3,
//                          const float *m16 x4, float vw s0, float vh s1,
//                          float wclip s2)
//
// For each point: clip = M * (x,y,z,1), then if clip.w > wclip do the
// perspective divide and viewport map, else mark invisible. The 4x4 matrix
// columns are loaded once into v0..v3 (constant across all points); each
// point's clip vector is built with FMLA-by-lane (the same trick as
// mat4_transform in simd_neon.s), so the per-point transform is a few SIMD
// ops. The reciprocal/divide and conditional store are scalar per point.
//
// Note: s0/s1/s2 hold vw/vh/wclip on entry (FP arg bank). We move them to
// callee-untouched lanes before loading the matrix columns into v0..v3.
// ==========================================================================

.text
.globl _project_points_neon
.p2align 2
_project_points_neon:
    // stash the scalar params (they live in s0..s2, which we'd clobber loading
    // the matrix). Keep vw in s16, vh in s17, wclip in s18 (caller-saved).
    fmov    s16, s0                 // vw
    fmov    s17, s1                 // vh
    fmov    s18, s2                 // wclip
    fmov    s19, #0.5               // 0.5 constant
    fmov    s20, #1.0               // 1.0 constant

    // load the 4 matrix columns (column-major, 4 floats each)
    ld1     {v0.4s, v1.4s, v2.4s, v3.4s}, [x4]   // v0=col0 .. v3=col3 (translation)

    mov     x5, #0                  // i = 0
.Lpp_loop:
    cmp     x5, x3
    b.ge    .Lpp_done

    // byte offset x8 = i*12  (3 floats per point)
    lsl     x8, x5, #1            // 2i
    add     x8, x8, x5           // 3i
    lsl     x8, x8, #2           // 12i bytes
    add     x6, x2, x8           // &pts[3i]
    ldr     s21, [x6]             // x
    ldr     s22, [x6, #4]         // y
    ldr     s23, [x6, #8]         // z

    // clip = col0*x + col1*y + col2*z + col3
    fmul    v24.4s, v0.4s, v21.s[0]
    fmla    v24.4s, v1.4s, v22.s[0]
    fmla    v24.4s, v2.4s, v23.s[0]
    fadd    v24.4s, v24.4s, v3.4s        // v24 = (cx,cy,cz,cw)

    // cw = v24.s[3]; compare to wclip
    mov     s25, v24.s[3]
    fcmp    s25, s18
    b.gt    .Lpp_visible

    // invisible: out = 0,0,0 ; vis=0
    add     x9, x0, x8            // &out[3i]
    str     wzr, [x9]
    str     wzr, [x9, #4]
    str     wzr, [x9, #8]
    strb    wzr, [x1, x5]
    add     x5, x5, #1
    b       .Lpp_loop

.Lpp_visible:
    // iw = 1/cw
    fdiv    s26, s20, s25          // 1.0 / cw
    mov     s27, v24.s[0]          // cx
    mov     s28, v24.s[1]          // cy
    mov     s29, v24.s[2]          // cz
    // sx = (cx*iw*0.5 + 0.5) * vw
    fmul    s27, s27, s26          // cx*iw
    fmul    s27, s27, s19          // *0.5
    fadd    s27, s27, s19          // +0.5
    fmul    s27, s27, s16          // *vw
    // sy = (1 - (cy*iw*0.5 + 0.5)) * vh
    fmul    s28, s28, s26
    fmul    s28, s28, s19
    fadd    s28, s28, s19
    fsub    s28, s20, s28          // 1 - .
    fmul    s28, s28, s17          // *vh
    // depth = cz*iw
    fmul    s29, s29, s26
    // store
    add     x9, x0, x8            // &out[3i]
    str     s27, [x9]
    str     s28, [x9, #4]
    str     s29, [x9, #8]
    mov     w10, #1
    strb    w10, [x1, x5]
    add     x5, x5, #1
    b       .Lpp_loop
.Lpp_done:
    ret
