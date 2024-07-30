// ==========================================================================
// blur_neon.s — NEON separable 1D Gaussian blur (Mach-O / Apple ABI).
//
// blur_v is the clean SIMD case: for a fixed output row y, the vertical kernel
// reads rows (y-k) and (y+k) which are whole contiguous scanlines, so 4
// adjacent x columns are processed at once with identical (clamped) y indices —
// no per-lane divergence. We vectorize the full width in groups of 4 + scalar
// tail.
//
// blur_h has per-pixel-varying x clamps at the row edges, so we vectorize only
// the safe interior [r, w-1-r] (where no clamping occurs and a shifted load is
// valid) and hand the edges to the C reference via a fallback. To keep this
// file self-contained the asm does the interior; the C wrapper (blur.c is not
// used — instead blur_h_neon here also handles edges scalar).
// ==========================================================================

.text

// --------------------------------------------------------------------------
// void blur_v_neon(float*dst x0, const float*src x1, int w w2, int h w3,
//                  const float*ker x4, int r w5)
// --------------------------------------------------------------------------
.globl _blur_v_neon
.p2align 2
_blur_v_neon:
    // save callee-saved GP regs we use
    stp     x19, x20, [sp, #-64]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    mov     w19, w2                 // w
    mov     w20, w3                 // h
    mov     x21, x4                 // ker
    mov     w22, w5                 // r
    sxtw    x19, w19
    sxtw    x20, w20

    mov     x23, #0                 // y = 0
.Lbv_y:
    cmp     x23, x20
    b.ge    .Lbv_done
    // dst row base = dst + y*w ; src center row base = src + y*w
    mul     x24, x23, x19
    add     x25, x0, x24, lsl #2    // &dst[y*w]
    add     x26, x1, x24, lsl #2    // &src[y*w]  (center row)

    mov     x9, #0                  // x = 0
    // vector loop: 4 columns at a time while x+4 <= w
.Lbv_vx:
    add     x10, x9, #4
    cmp     x10, x19
    b.gt    .Lbv_sx                 // remaining < 4 -> scalar tail
    // acc = ker[0] * center[x..x+3]
    ldr     s0, [x21]               // ker[0]
    dup     v0.4s, v0.s[0]
    add     x11, x26, x9, lsl #2
    ld1     {v1.4s}, [x11]          // center 4 cols
    fmul    v16.4s, v1.4s, v0.4s    // acc
    mov     w12, #1                 // k=1
.Lbv_vk:
    cmp     w12, w22
    b.gt    .Lbv_vstore
    // lo = clamp(y-k,0), hi = clamp(y+k,h-1)
    sxtw    x14, w12                // k (64-bit)
    sub     x13, x23, x14           // y-k
    cmp     x13, #0
    csel    x13, xzr, x13, lt       // clamp lo>=0
    add     x15, x23, x14           // y+k
    sub     x16, x20, #1            // h-1
    cmp     x15, x16
    csel    x15, x16, x15, gt       // clamp hi<=h-1
    // load src[lo*w + x..], src[hi*w + x..]
    mul     x17, x13, x19
    add     x17, x1, x17, lsl #2
    add     x17, x17, x9, lsl #2
    ld1     {v2.4s}, [x17]
    mul     x7, x15, x19
    add     x7, x1, x7, lsl #2
    add     x7, x7, x9, lsl #2
    ld1     {v3.4s}, [x7]
    fadd    v2.4s, v2.4s, v3.4s     // src[lo]+src[hi]
    ldr     s4, [x21, x14, lsl #2]  // ker[k]
    dup     v4.4s, v4.s[0]
    fmla    v16.4s, v2.4s, v4.4s
    add     w12, w12, #1
    b       .Lbv_vk
.Lbv_vstore:
    add     x11, x25, x9, lsl #2
    st1     {v16.4s}, [x11]
    mov     x9, x10                 // x += 4
    b       .Lbv_vx
.Lbv_sx:
    // scalar tail for the last <4 columns
    cmp     x9, x19
    b.ge    .Lbv_ynext
    ldr     s0, [x21]               // ker[0]
    add     x11, x26, x9, lsl #2
    ldr     s16, [x11]
    fmul    s16, s16, s0
    mov     w12, #1
.Lbv_sk:
    cmp     w12, w22
    b.gt    .Lbv_sstore
    sxtw    x14, w12
    sub     x13, x23, x14
    cmp     x13, #0
    csel    x13, xzr, x13, lt
    add     x15, x23, x14
    sub     x16, x20, #1
    cmp     x15, x16
    csel    x15, x16, x15, gt
    mul     x17, x13, x19
    add     x17, x1, x17, lsl #2
    add     x17, x17, x9, lsl #2
    ldr     s2, [x17]
    mul     x7, x15, x19
    add     x7, x1, x7, lsl #2
    add     x7, x7, x9, lsl #2
    ldr     s3, [x7]
    fadd    s2, s2, s3
    ldr     s4, [x21, x14, lsl #2]
    fmadd   s16, s2, s4, s16
    add     w12, w12, #1
    b       .Lbv_sk
.Lbv_sstore:
    add     x11, x25, x9, lsl #2
    str     s16, [x11]
    add     x9, x9, #1
    b       .Lbv_sx
.Lbv_ynext:
    add     x23, x23, #1
    b       .Lbv_y
.Lbv_done:
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #64
    ret

// --------------------------------------------------------------------------
// void blur_h_neon(float*dst x0, const float*src x1, int w w2, int h w3,
//                  const float*ker x4, int r w5)
// Per-row horizontal blur. Interior pixels [r, w-1-r] use shifted loads with no
// clamping (vectorizable); the r pixels at each edge are done scalar with
// clamping. For simplicity and robustness we do every pixel scalar-with-clamp
// but 4-wide where the whole 4-window is interior.
// --------------------------------------------------------------------------
.globl _blur_h_neon
.p2align 2
_blur_h_neon:
    stp     x19, x20, [sp, #-48]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    sxtw    x19, w2                 // w
    sxtw    x20, w3                 // h
    mov     x21, x4                 // ker
    mov     w22, w5                 // r
    sxtw    x22, w22

    mov     x23, #0                 // y
.Lbh_y:
    cmp     x23, x20
    b.ge    .Lbh_done
    mul     x24, x23, x19
    add     x11, x1, x24, lsl #2    // src row base
    add     x12, x0, x24, lsl #2    // dst row base
    mov     x9, #0                  // x
.Lbh_x:
    cmp     x9, x19
    b.ge    .Lbh_ynext
    // decide: can we do a 4-wide interior block? need x>=r and x+3 <= w-1-r
    add     x13, x9, #3
    sub     x14, x19, #1
    sub     x14, x14, x22           // w-1-r
    cmp     x9, x22
    b.lt    .Lbh_scalar
    cmp     x13, x14
    b.gt    .Lbh_scalar
    // 4-wide interior: no clamping needed
    ldr     s0, [x21]
    dup     v0.4s, v0.s[0]
    add     x15, x11, x9, lsl #2
    ld1     {v1.4s}, [x15]
    fmul    v16.4s, v1.4s, v0.4s
    mov     w16, #1
.Lbh_vk:
    cmp     w16, w22
    b.gt    .Lbh_vstore
    sxtw    x17, w16
    sub     x7, x15, x17, lsl #2    // &src[x-k]
    ldr     q2, [x7]
    add     x7, x15, x17, lsl #2    // &src[x+k]
    ldr     q3, [x7]
    fadd    v2.4s, v2.4s, v3.4s
    ldr     s4, [x21, x17, lsl #2]
    dup     v4.4s, v4.s[0]
    fmla    v16.4s, v2.4s, v4.4s
    add     w16, w16, #1
    b       .Lbh_vk
.Lbh_vstore:
    add     x15, x12, x9, lsl #2
    st1     {v16.4s}, [x15]
    add     x9, x9, #4
    b       .Lbh_x
.Lbh_scalar:
    // scalar with clamping for one pixel
    ldr     s0, [x21]
    add     x15, x11, x9, lsl #2
    ldr     s16, [x15]
    fmul    s16, s16, s0
    mov     w16, #1
.Lbh_sk:
    cmp     w16, w22
    b.gt    .Lbh_sstore
    sxtw    x17, w16
    sub     x7, x9, x17             // x-k
    cmp     x7, #0
    csel    x7, xzr, x7, lt
    add     x8, x9, x17             // x+k
    sub     x5, x19, #1
    cmp     x8, x5
    csel    x8, x5, x8, gt
    ldr     s2, [x11, x7, lsl #2]
    ldr     s3, [x11, x8, lsl #2]
    fadd    s2, s2, s3
    ldr     s4, [x21, x17, lsl #2]
    fmadd   s16, s2, s4, s16
    add     w16, w16, #1
    b       .Lbh_sk
.Lbh_sstore:
    add     x15, x12, x9, lsl #2
    str     s16, [x15]
    add     x9, x9, #1
    b       .Lbh_x
.Lbh_ynext:
    add     x23, x23, #1
    b       .Lbh_y
.Lbh_done:
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #48
    ret
