// ==========================================================================
// dsp_neon.s — hand-written AArch64 NEON DSP kernels (Mach-O / Apple ABI).
// See src/asm/simd_neon.s for the full convention notes.
//
// The colour-space conversions use LD3/ST3, which de-interleave/re-interleave
// 3-channel data on load/store: LD3 {v0.4s,v1.4s,v2.4s},[x] loads 12 floats
// as R->v0, G->v1, B->v2 (4 pixels), letting us do the 3x3 matrix as pure
// SIMD broadcasts+FMLAs. ST3 writes them back interleaved.
// ==========================================================================

.text

// Literal pool of NTSC coefficients (must match dsp_ref.c exactly).
.p2align 4
_yiq_coef:                    // RGB->YIQ, row-major 3x3
    .float 0.299000, 0.587000, 0.114000     // Y = [0..2]
    .float 0.595716, -0.274453, -0.321263   // I = [3..5]
    .float 0.211456, -0.522591, 0.311135    // Q = [6..8]
_rgb_coef:                    // YIQ->RGB
    .float 1.0,  0.956000, 0.621000         // R = [0..2]
    .float 1.0, -0.272000, -0.647000        // G = [3..5]
    .float 1.0, -1.107000, 1.704600         // B = [6..8]

// --------------------------------------------------------------------------
// void dsp_rgb2yiq_neon(float *out x0, const float *rgb x1, unsigned long n x2)
// --------------------------------------------------------------------------
.globl _dsp_rgb2yiq_neon
.p2align 2
_dsp_rgb2yiq_neon:
    adrp    x3, _yiq_coef@PAGE
    add     x3, x3, _yiq_coef@PAGEOFF
    ld1     {v24.4s, v25.4s}, [x3]          // v24=[Yr,Yg,Yb,Ir], v25=[Ig,Ib,Qr,Qg]
    ldr     s26, [x3, #32]                  // Qb
    // Broadcast individual coefficients into scalars-in-vectors for FMLA-by-lane.
    // Y row
    dup     v0.4s, v24.s[0]  // Yr
    dup     v1.4s, v24.s[1]  // Yg
    dup     v2.4s, v24.s[2]  // Yb
    // I row
    dup     v3.4s, v24.s[3]  // Ir
    dup     v4.4s, v25.s[0]  // Ig
    dup     v5.4s, v25.s[1]  // Ib
    // Q row
    dup     v6.4s, v25.s[2]  // Qr
    dup     v7.4s, v25.s[3]  // Qg
    dup     v16.4s, v26.s[0] // Qb

    lsr     x4, x2, #2                       // vector pixels /4
    cbz     x4, .Lr2y_tail
.Lr2y_loop:
    ld3     {v17.4s, v18.4s, v19.4s}, [x1], #48   // R=v17 G=v18 B=v19 (4 px)
    // Y = Yr*R + Yg*G + Yb*B
    fmul    v20.4s, v17.4s, v0.4s
    fmla    v20.4s, v18.4s, v1.4s
    fmla    v20.4s, v19.4s, v2.4s
    // I
    fmul    v21.4s, v17.4s, v3.4s
    fmla    v21.4s, v18.4s, v4.4s
    fmla    v21.4s, v19.4s, v5.4s
    // Q
    fmul    v22.4s, v17.4s, v6.4s
    fmla    v22.4s, v18.4s, v7.4s
    fmla    v22.4s, v19.4s, v16.4s
    st3     {v20.4s, v21.4s, v22.4s}, [x0], #48
    subs    x4, x4, #1
    b.ne    .Lr2y_loop
.Lr2y_tail:
    and     x5, x2, #3
    cbz     x5, .Lr2y_done
.Lr2y_t:
    ldr     s17, [x1]
    ldr     s18, [x1, #4]
    ldr     s19, [x1, #8]
    add     x1, x1, #12
    fmul    s20, s17, s0
    fmadd   s20, s18, s1, s20
    fmadd   s20, s19, s2, s20
    fmul    s21, s17, s3
    fmadd   s21, s18, s4, s21
    fmadd   s21, s19, s5, s21
    fmul    s22, s17, s6
    fmadd   s22, s18, s7, s22
    fmadd   s22, s19, s16, s22
    str     s20, [x0]
    str     s21, [x0, #4]
    str     s22, [x0, #8]
    add     x0, x0, #12
    subs    x5, x5, #1
    b.ne    .Lr2y_t
.Lr2y_done:
    ret

// --------------------------------------------------------------------------
// void dsp_yiq2rgb_neon(float *out x0, const float *yiq x1, unsigned long n x2)
// --------------------------------------------------------------------------
.globl _dsp_yiq2rgb_neon
.p2align 2
_dsp_yiq2rgb_neon:
    adrp    x3, _rgb_coef@PAGE
    add     x3, x3, _rgb_coef@PAGEOFF
    ld1     {v24.4s, v25.4s}, [x3]
    ldr     s26, [x3, #32]
    dup     v0.4s, v24.s[0]  // Ry (1)
    dup     v1.4s, v24.s[1]  // Ri
    dup     v2.4s, v24.s[2]  // Rq
    dup     v3.4s, v24.s[3]  // Gy (1)
    dup     v4.4s, v25.s[0]  // Gi
    dup     v5.4s, v25.s[1]  // Gq
    dup     v6.4s, v25.s[2]  // By (1)
    dup     v7.4s, v25.s[3]  // Bi
    dup     v16.4s, v26.s[0] // Bq

    lsr     x4, x2, #2
    cbz     x4, .Ly2r_tail
.Ly2r_loop:
    ld3     {v17.4s, v18.4s, v19.4s}, [x1], #48   // Y=v17 I=v18 Q=v19
    fmul    v20.4s, v17.4s, v0.4s
    fmla    v20.4s, v18.4s, v1.4s
    fmla    v20.4s, v19.4s, v2.4s
    fmul    v21.4s, v17.4s, v3.4s
    fmla    v21.4s, v18.4s, v4.4s
    fmla    v21.4s, v19.4s, v5.4s
    fmul    v22.4s, v17.4s, v6.4s
    fmla    v22.4s, v18.4s, v7.4s
    fmla    v22.4s, v19.4s, v16.4s
    st3     {v20.4s, v21.4s, v22.4s}, [x0], #48
    subs    x4, x4, #1
    b.ne    .Ly2r_loop
.Ly2r_tail:
    and     x5, x2, #3
    cbz     x5, .Ly2r_done
.Ly2r_t:
    ldr     s17, [x1]
    ldr     s18, [x1, #4]
    ldr     s19, [x1, #8]
    add     x1, x1, #12
    fmul    s20, s17, s0
    fmadd   s20, s18, s1, s20
    fmadd   s20, s19, s2, s20
    fmul    s21, s17, s3
    fmadd   s21, s18, s4, s21
    fmadd   s21, s19, s5, s21
    fmul    s22, s17, s6
    fmadd   s22, s18, s7, s22
    fmadd   s22, s19, s16, s22
    str     s20, [x0]
    str     s21, [x0, #4]
    str     s22, [x0, #8]
    add     x0, x0, #12
    subs    x5, x5, #1
    b.ne    .Ly2r_t
.Ly2r_done:
    ret

// --------------------------------------------------------------------------
// void dsp_iir_blend_neon(float *dst x0, const float *src x1, float a s0, unsigned long n x2)
// dst = dst*a + src*(1-a)
// --------------------------------------------------------------------------
.globl _dsp_iir_blend_neon
.p2align 2
_dsp_iir_blend_neon:
    fmov    s1, #1.0
    fsub    s1, s1, s0                       // b = 1-a
    dup     v2.4s, v0.s[0]                   // [a x4]
    dup     v3.4s, v1.s[0]                   // [b x4]
    lsr     x3, x2, #2
    cbz     x3, .Liir_tail
.Liir_loop:
    ld1     {v4.4s}, [x0]                    // dst
    ld1     {v5.4s}, [x1], #16               // src
    fmul    v4.4s, v4.4s, v2.4s              // dst*a
    fmla    v4.4s, v5.4s, v3.4s              // + src*b
    st1     {v4.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Liir_loop
.Liir_tail:
    and     x4, x2, #3
    cbz     x4, .Liir_done
.Liir_t:
    ldr     s4, [x0]
    ldr     s5, [x1], #4
    fmul    s4, s4, s0
    fmadd   s4, s5, s1, s4
    str     s4, [x0], #4
    subs    x4, x4, #1
    b.ne    .Liir_t
.Liir_done:
    ret

// --------------------------------------------------------------------------
// void dsp_scale_bias_clamp_neon(float *out x0, const float *in x1,
//                                float g s0, float b s1, float hi s2, unsigned long n x2)
// n is x2 (not x3): GP and FP args use separate banks, so the floats g/b/hi
// occupy s0/s1/s2 while the only prior integer args (out,in) took x0/x1.
// out = clamp(in*g + b, 0, hi)
// --------------------------------------------------------------------------
.globl _dsp_scale_bias_clamp_neon
.p2align 2
_dsp_scale_bias_clamp_neon:
    dup     v0.4s, v0.s[0]                   // g
    dup     v1.4s, v1.s[0]                   // b
    dup     v2.4s, v2.s[0]                   // hi
    movi    v6.4s, #0
    lsr     x4, x2, #2
    cbz     x4, .Lsbc_tail
.Lsbc_loop:
    ld1     {v4.4s}, [x1], #16
    fmul    v4.4s, v4.4s, v0.4s
    fadd    v4.4s, v4.4s, v1.4s
    fmax    v4.4s, v4.4s, v6.4s
    fmin    v4.4s, v4.4s, v2.4s
    st1     {v4.4s}, [x0], #16
    subs    x4, x4, #1
    b.ne    .Lsbc_loop
.Lsbc_tail:
    and     x5, x2, #3
    cbz     x5, .Lsbc_done
    // scalar copies of g,b,hi are still in s0/s1/s2 lane 0
    movi    v6.2s, #0
.Lsbc_t:
    ldr     s4, [x1], #4
    fmul    s4, s4, s0
    fadd    s4, s4, s1
    fmax    s4, s4, s6
    fmin    s4, s4, s2
    str     s4, [x0], #4
    subs    x5, x5, #1
    b.ne    .Lsbc_t
.Lsbc_done:
    ret

// --------------------------------------------------------------------------
// void dsp_fir_sym_neon(float *out x0, const float *in x1, unsigned long n x2,
//                       const float *ker x3, int r w4)
// out[i] = ker[0]*in[i] + sum_{k=1..r} ker[k]*(in[clamp(i-k)]+in[clamp(i+k)])
// Interior (i>=r && i<n-r) is done branch-free & vectorized; edges scalar.
// --------------------------------------------------------------------------
.globl _dsp_fir_sym_neon
.p2align 2
_dsp_fir_sym_neon:
    str     x30, [sp, #-16]!                  // save LR: we use bl to a helper
    cbz     x2, .Lfir_ret
    // If n <= 2r, every element touches an edge -> all scalar.
    sxtw    x4, w4                            // r as 64-bit
    lsl     x5, x4, #1                        // 2r
    cmp     x2, x5
    b.ls    .Lfir_all_scalar

    // ---- left edge: i = 0 .. r-1 (scalar, clamped) ----
    mov     x6, #0                            // i
.Lfir_left:
    cmp     x6, x4
    b.hs    .Lfir_interior_setup
    bl      .Lfir_scalar_one                  // computes out[i] using clamps
    add     x6, x6, #1
    b       .Lfir_left

.Lfir_interior_setup:
    // interior i = r .. n-r-1  (count = n - 2r)
    sub     x7, x2, x5                         // count = n - 2r
    // out ptr -> &out[r]; center in ptr -> &in[r]
    add     x8, x0, x4, lsl #2                 // &out[r]
    add     x9, x1, x4, lsl #2                 // &in[r]
    lsr     x10, x7, #2                        // vector iters
    cbz     x10, .Lfir_interior_scalar_pre
.Lfir_vloop:
    // acc = ker[0]*center
    ld1     {v20.4s}, [x9]                     // in[i..i+3]
    ldr     s0, [x3]                           // ker[0]
    dup     v0.4s, v0.s[0]
    fmul    v20.4s, v20.4s, v0.4s
    mov     x11, #1                            // k
.Lfir_vk:
    cmp     x11, x4
    b.hi    .Lfir_vstore
    // in[i-k .. i-k+3] and in[i+k .. i+k+3]
    sub     x12, x9, x11, lsl #2               // &in[i-k]
    add     x13, x9, x11, lsl #2               // &in[i+k]
    ldr     q1, [x12]
    ldr     q2, [x13]
    fadd    v1.4s, v1.4s, v2.4s                // in[i-k]+in[i+k]
    ldr     s3, [x3, x11, lsl #2]              // ker[k]
    dup     v3.4s, v3.s[0]
    fmla    v20.4s, v1.4s, v3.4s
    add     x11, x11, #1
    b       .Lfir_vk
.Lfir_vstore:
    st1     {v20.4s}, [x8], #16
    add     x9, x9, #16
    subs    x10, x10, #1
    b.ne    .Lfir_vloop
.Lfir_interior_scalar_pre:
    // remaining interior elements (count & 3): current index = r + (count - rem)
    and     x14, x7, #3
    cbz     x14, .Lfir_right_setup
    sub     x6, x2, x4                          // n - r  (right edge start)
    sub     x6, x6, x14                         // first remaining interior index
.Lfir_interior_scalar:
    bl      .Lfir_scalar_one
    add     x6, x6, #1
    sub     x14, x14, #1
    cbnz    x14, .Lfir_interior_scalar

.Lfir_right_setup:
    // ---- right edge: i = n-r .. n-1 (scalar, clamped) ----
    sub     x6, x2, x4                          // n - r
.Lfir_right:
    cmp     x6, x2
    b.hs    .Lfir_ret
    bl      .Lfir_scalar_one
    add     x6, x6, #1
    b       .Lfir_right
.Lfir_ret:
    ldr     x30, [sp], #16                    // restore LR
    ret

.Lfir_all_scalar:
    mov     x6, #0
.Lfir_as:
    cmp     x6, x2
    b.hs    .Lfir_ret
    bl      .Lfir_scalar_one
    add     x6, x6, #1
    b       .Lfir_as

// Helper: compute out[x6] with edge clamping. Uses x0,x1,x2,x3,x4 (out,in,n,ker,r)
// and x6 (i). Uses ONLY caller-saved SIMD regs (s16-s19): v8-v15 are
// callee-saved and the C caller parks live values there across our call.
// Clobbers x15,x16,x17,x11, s16-s19. Preserves x6.
.Lfir_scalar_one:
    ldr     s16, [x3]                           // ker[0]
    ldr     s17, [x1, x6, lsl #2]               // in[i]
    fmul    s18, s16, s17                       // acc
    mov     x15, #1                             // k
.Lfso_k:
    cmp     x15, x4
    b.hi    .Lfso_done
    // lo = clamp(i-k, 0)
    subs    x16, x6, x15
    csel    x16, xzr, x16, lt                   // if i-k<0 -> 0
    // hi = clamp(i+k, n-1)
    add     x17, x6, x15
    sub     x11, x2, #1
    cmp     x17, x11
    csel    x17, x11, x17, gt
    ldr     s17, [x1, x16, lsl #2]              // in[lo]
    ldr     s19, [x1, x17, lsl #2]             // in[hi]
    fadd    s17, s17, s19
    ldr     s19, [x3, x15, lsl #2]             // ker[k]
    fmadd   s18, s17, s19, s18
    add     x15, x15, #1
    b       .Lfso_k
.Lfso_done:
    str     s18, [x0, x6, lsl #2]
    ret
