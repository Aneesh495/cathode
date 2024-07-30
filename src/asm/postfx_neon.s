// ==========================================================================
// postfx_neon.s — hand-written AArch64 NEON post-processing kernels.
// Mach-O / Apple ABI (see src/asm/simd_neon.s for the full convention notes).
// All routines: 4-wide vector body + scalar tail for n not a multiple of 4.
// ==========================================================================

.text

// --------------------------------------------------------------------------
// void postfx_accumulate_neon(float *dst x0, const float *src x1, float scale s0, unsigned long n x2)
// dst[i] += src[i]*scale
// --------------------------------------------------------------------------
.globl _postfx_accumulate_neon
.p2align 2
_postfx_accumulate_neon:
    dup     v1.4s, v0.s[0]           // [scale x4]
    lsr     x3, x2, #2
    cbz     x3, .Lacc_tail
.Lacc_loop:
    ld1     {v2.4s}, [x0]            // dst
    ld1     {v3.4s}, [x1], #16       // src++
    fmla    v2.4s, v3.4s, v1.4s      // dst += src*scale
    st1     {v2.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lacc_loop
.Lacc_tail:
    and     x4, x2, #3
    cbz     x4, .Lacc_done
.Lacc_t:
    ldr     s2, [x0]
    ldr     s3, [x1], #4
    fmadd   s2, s3, s0, s2
    str     s2, [x0], #4
    subs    x4, x4, #1
    b.ne    .Lacc_t
.Lacc_done:
    ret

// --------------------------------------------------------------------------
// void postfx_screen_neon(float *dst x0, const float *src x1, unsigned long n x2)
// dst = clamp(1 - (1-dst)*(1-src), 0, 1)
// --------------------------------------------------------------------------
.globl _postfx_screen_neon
.p2align 2
_postfx_screen_neon:
    fmov    v5.4s, #1.0              // [1,1,1,1]
    movi    v6.4s, #0                // [0,0,0,0]
    lsr     x3, x2, #2
    cbz     x3, .Lscr_tail
.Lscr_loop:
    ld1     {v2.4s}, [x0]            // dst
    ld1     {v3.4s}, [x1], #16       // src
    fsub    v7.4s, v5.4s, v2.4s      // 1-dst
    fsub    v16.4s, v5.4s, v3.4s     // 1-src
    fmul    v7.4s, v7.4s, v16.4s     // (1-dst)(1-src)
    fsub    v7.4s, v5.4s, v7.4s      // 1 - that
    fmax    v7.4s, v7.4s, v6.4s      // clamp low
    fmin    v7.4s, v7.4s, v5.4s      // clamp high
    st1     {v7.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lscr_loop
.Lscr_tail:
    and     x4, x2, #3
    cbz     x4, .Lscr_done
    fmov    s5, #1.0
    movi    v6.2s, #0
.Lscr_t:
    ldr     s2, [x0]
    ldr     s3, [x1], #4
    fsub    s7, s5, s2
    fsub    s16, s5, s3
    fmul    s7, s7, s16
    fsub    s7, s5, s7
    fmax    s7, s7, s6
    fmin    s7, s7, s5
    str     s7, [x0], #4
    subs    x4, x4, #1
    b.ne    .Lscr_t
.Lscr_done:
    ret

// --------------------------------------------------------------------------
// void postfx_brightpass_neon(float *out x0, const float *in x1, float threshold s0, float knee s1, unsigned long n x2)
// out = max(0, in-threshold) * knee
// --------------------------------------------------------------------------
.globl _postfx_brightpass_neon
.p2align 2
_postfx_brightpass_neon:
    dup     v2.4s, v0.s[0]           // threshold x4
    dup     v3.4s, v1.s[0]           // knee x4
    movi    v6.4s, #0
    lsr     x3, x2, #2
    cbz     x3, .Lbp_tail
.Lbp_loop:
    ld1     {v4.4s}, [x1], #16
    fsub    v4.4s, v4.4s, v2.4s      // in - threshold
    fmax    v4.4s, v4.4s, v6.4s      // max(0, .)
    fmul    v4.4s, v4.4s, v3.4s      // * knee
    st1     {v4.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lbp_loop
.Lbp_tail:
    and     x4, x2, #3
    cbz     x4, .Lbp_done
    movi    v6.2s, #0
.Lbp_t:
    ldr     s4, [x1], #4
    fsub    s4, s4, s0
    fmax    s4, s4, s6
    fmul    s4, s4, s1
    str     s4, [x0], #4
    subs    x4, x4, #1
    b.ne    .Lbp_t
.Lbp_done:
    ret

// --------------------------------------------------------------------------
// void postfx_blur5_neon(float *out x0, const float *in x1, unsigned long n x2)
// out[i] = (in[i-2] + 4 in[i-1] + 6 in[i] + 4 in[i+1] + in[i+2]) / 16, clamped edges.
// Scalar per-element with clamped gather — correctness over throughput at the
// n<4 fast path; the bulk interior is done 4-wide with unaligned shifted loads.
// --------------------------------------------------------------------------
.globl _postfx_blur5_neon
.p2align 2
_postfx_blur5_neon:
    cbz     x2, .Lblur_ret           // n==0
    cmp     x2, #5
    b.lo    .Lblur_scalar_all        // tiny n: do it all scalar with clamps

    // --- element 0 and 1 (left edge, clamped) scalar ---
    // i=0: (in0 + 4 in0 + 6 in0 + 4 in1 + in2)/16 = (11 in0 + 4 in1 + in2)/16
    ldr     s0, [x1]                 // in0
    ldr     s1, [x1, #4]             // in1
    ldr     s2, [x1, #8]             // in2
    fmov    s10, #11.0
    fmul    s3, s0, s10
    fmov    s10, #4.0
    fmadd   s3, s1, s10, s3
    fadd    s3, s3, s2
    fmov    s10, #16.0
    fdiv    s3, s3, s10
    str     s3, [x0]
    // i=1: (in0 + 4 in0? ) -> im2 clamps to 0 => (in0 + 4 in0 + 6 in1 + 4 in2 + in3)/16
    //   = (5 in0 + 6 in1 + 4 in2 + in3)/16
    ldr     s3, [x1, #12]            // in3
    fmov    s10, #5.0
    fmul    s4, s0, s10
    fmov    s10, #6.0
    fmadd   s4, s1, s10, s4
    fmov    s10, #4.0
    fmadd   s4, s2, s10, s4
    fadd    s4, s4, s3
    fmov    s10, #16.0
    fdiv    s4, s4, s10
    str     s4, [x0, #4]

    // --- interior i = 2 .. n-3 : full 1-4-6-4-1 with no clamping ---
    // vector body processes 4 outputs per iteration.
    // out ptr starts at &out[2]; in window base at &in[0].
    mov     x5, #2                   // i = 2
    sub     x6, x2, #2               // limit = n-2 (exclusive) for interior end index
    // number of interior elements = (n-2) - 2 = n-4
    sub     x7, x2, #4               // count
    add     x8, x0, #8               // &out[2]
    mov     x9, x1                   // &in[0] (window left tap for i=2 is in[0])
    fmov    v20.4s, #6.0
    fmov    v21.4s, #4.0
    fmov    v22.4s, #16.0
    // 1/16 reciprocal for multiply
    fmov    v23.4s, #1.0
    fdiv    v23.4s, v23.4s, v22.4s   // 0.0625 x4
    lsr     x10, x7, #2              // vector iters
    cbz     x10, .Lblur_interior_scalar
.Lblur_vloop:
    // for output index i (i=2.. ), taps are in[i-2..i+2] = x9[0..4]
    ld1     {v0.4s}, [x9]            // in[i-2..i+1]   (4)
    ldur    q1, [x9, #4]             // in[i-1..i+2]
    ldur    q2, [x9, #8]            // in[i  ..i+3]
    ldur    q3, [x9, #12]           // in[i+1..i+4]
    ldur    q4, [x9, #16]           // in[i+2..i+5]
    fadd    v5.4s, v0.4s, v4.4s      // in[i-2]+in[i+2]
    fadd    v6.4s, v1.4s, v3.4s      // in[i-1]+in[i+1]
    fmla    v5.4s, v6.4s, v21.4s     // + 4*(...)
    fmla    v5.4s, v2.4s, v20.4s     // + 6*in[i]
    fmul    v5.4s, v5.4s, v23.4s     // /16
    st1     {v5.4s}, [x8], #16
    add     x9, x9, #16
    subs    x10, x10, #1
    b.ne    .Lblur_vloop
.Lblur_interior_scalar:
    and     x11, x7, #3              // remaining interior elems
    cbz     x11, .Lblur_right_edge
    // current output index = 2 + (x7 - x11); x8 already advanced, x9 advanced.
.Lblur_is_loop:
    ldr     s0, [x9]                 // in[i-2]
    ldr     s1, [x9, #4]
    ldr     s2, [x9, #8]
    ldr     s3, [x9, #12]
    ldr     s4, [x9, #16]
    fadd    s5, s0, s4
    fadd    s6, s1, s3
    fmov    s10, #4.0
    fmadd   s5, s6, s10, s5
    fmov    s10, #6.0
    fmadd   s5, s2, s10, s5
    fmov    s10, #16.0
    fdiv    s5, s5, s10
    str     s5, [x8], #4
    add     x9, x9, #4
    subs    x11, x11, #1
    b.ne    .Lblur_is_loop
.Lblur_right_edge:
    // --- last two elements i = n-2, n-1 (right edge clamp) scalar ---
    // i = n-2: im2..ip2 with ip2 clamped to n-1
    //   (in[n-4] + 4 in[n-3] + 6 in[n-2] + 4 in[n-1] + in[n-1])/16
    //   = (in[n-4] + 4 in[n-3] + 6 in[n-2] + 5 in[n-1])/16
    sub     x12, x2, #1              // n-1
    lsl     x13, x12, #2             // (n-1)*4 byte offset
    add     x14, x1, x13             // &in[n-1]
    ldr     s7, [x14]                // in[n-1]
    ldur    s6, [x14, #-4]           // in[n-2]
    ldur    s5, [x14, #-8]           // in[n-3]
    ldur    s4, [x14, #-12]          // in[n-4]
    // i=n-2:
    fmov    s10, #6.0
    fmul    s0, s6, s10              // 6 in[n-2]
    fmov    s10, #4.0
    fmadd   s0, s5, s10, s0          // + 4 in[n-3]
    fadd    s0, s0, s4               // + in[n-4]
    fmov    s10, #5.0
    fmadd   s0, s7, s10, s0          // + 5 in[n-1]
    fmov    s10, #16.0
    fdiv    s0, s0, s10
    add     x15, x0, x13
    stur    s0, [x15, #-4]           // out[n-2]
    // i=n-1: (in[n-3] + 4 in[n-2] + 11 in[n-1])/16
    fmov    s10, #11.0
    fmul    s0, s7, s10
    fmov    s10, #4.0
    fmadd   s0, s6, s10, s0
    fadd    s0, s0, s5
    fmov    s10, #16.0
    fdiv    s0, s0, s10
    str     s0, [x15]                // out[n-1]
.Lblur_ret:
    ret

// tiny n (<5): fully scalar with clamped indices
.Lblur_scalar_all:
    mov     x3, #0                   // i
.Lbsa_loop:
    // im2
    sub     x4, x3, #2
    cmp     x4, #0
    csel    x4, xzr, x4, lt
    // im1
    sub     x5, x3, #1
    cmp     x5, #0
    csel    x5, xzr, x5, lt
    // ip1
    add     x6, x3, #1
    sub     x11, x2, #1
    cmp     x6, x11
    csel    x6, x11, x6, gt
    // ip2
    add     x7, x3, #2
    cmp     x7, x11
    csel    x7, x11, x7, gt
    ldr     s0, [x1, x4, lsl #2]
    ldr     s1, [x1, x5, lsl #2]
    ldr     s2, [x1, x3, lsl #2]
    ldr     s3, [x1, x6, lsl #2]
    ldr     s4, [x1, x7, lsl #2]
    fadd    s5, s0, s4
    fadd    s6, s1, s3
    fmov    s10, #4.0
    fmadd   s5, s6, s10, s5
    fmov    s10, #6.0
    fmadd   s5, s2, s10, s5
    fmov    s10, #16.0
    fdiv    s5, s5, s10
    str     s5, [x0, x3, lsl #2]
    add     x3, x3, #1
    cmp     x3, x2
    b.lo    .Lbsa_loop
    ret

// --------------------------------------------------------------------------
// float postfx_sum_neon(const float *in x0, unsigned long n x1)
// --------------------------------------------------------------------------
.globl _postfx_sum_neon
.p2align 2
_postfx_sum_neon:
    movi    v0.4s, #0                // accumulator
    lsr     x2, x1, #2
    cbz     x2, .Lsum_tail
.Lsum_loop:
    ld1     {v1.4s}, [x0], #16
    fadd    v0.4s, v0.4s, v1.4s
    subs    x2, x2, #1
    b.ne    .Lsum_loop
.Lsum_tail:
    faddp   v0.4s, v0.4s, v0.4s
    faddp   v0.2s, v0.2s, v0.2s      // s0 = horizontal sum of vector part
    and     x3, x1, #3
    cbz     x3, .Lsum_done
.Lsum_t:
    ldr     s1, [x0], #4
    fadd    s0, s0, s1
    subs    x3, x3, #1
    b.ne    .Lsum_t
.Lsum_done:
    ret

// --------------------------------------------------------------------------
// float postfx_max_neon(const float *in x0, unsigned long n x1)
// returns 0 if n==0.
// --------------------------------------------------------------------------
.globl _postfx_max_neon
.p2align 2
_postfx_max_neon:
    cbz     x1, .Lmax_zero
    lsr     x2, x1, #2
    cbz     x2, .Lmax_scalar_init
    // init vector accumulator from first 4
    ld1     {v0.4s}, [x0], #16
    subs    x2, x2, #1
    cbz     x2, .Lmax_reduce
.Lmax_loop:
    ld1     {v1.4s}, [x0], #16
    fmax    v0.4s, v0.4s, v1.4s
    subs    x2, x2, #1
    b.ne    .Lmax_loop
.Lmax_reduce:
    fmaxp   v0.4s, v0.4s, v0.4s
    fmaxp   v0.2s, v0.2s, v0.2s      // s0 = max of vector part
    and     x3, x1, #3
    cbz     x3, .Lmax_done
    b       .Lmax_tail
.Lmax_scalar_init:
    // n<4: seed with first element
    ldr     s0, [x0], #4
    mov     x3, x1
    sub     x3, x3, #1
    cbz     x3, .Lmax_done
.Lmax_tail:
    ldr     s1, [x0], #4
    fmax    s0, s0, s1
    subs    x3, x3, #1
    b.ne    .Lmax_tail
.Lmax_done:
    ret
.Lmax_zero:
    movi    v0.2s, #0
    ret
