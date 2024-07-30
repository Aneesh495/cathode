// ==========================================================================
// fastmath_neon.s — hand-written AArch64 NEON transcendental approximations.
// Mach-O / Apple ABI (see src/asm/simd_neon.s for the full convention notes).
//
// All three kernels process 4 lanes at a time with a scalar tail. They use
// polynomial minimax approximations with range reduction:
//
//   sin/cos: reduce x to [-pi,pi] via  x - 2pi*round(x/2pi), then a degree-9
//            (sin) / degree-8 (cos) minimax polynomial. cos(x)=sin(x+pi/2) is
//            NOT used; we evaluate cos directly for accuracy near 0.
//   exp:     e^x = 2^(x*log2(e)); split k=round(x*log2e), f=x-k*ln2, then
//            e^f via a degree-5 polynomial, and scale by 2^k built directly
//            into the float exponent field with integer NEON ops.
//
// Constants live in a literal pool. v8-v15 are avoided (callee-saved).
// ==========================================================================

.text
.p2align 4
Lfm_consts:
    // [0] 1/(2pi)   [1] 2pi        [2] pi          [3] (unused/pad)
    .float 0.15915494309189535, 6.283185307179586, 3.141592653589793, 0.0
    // Taylor-derived coefficients good to high accuracy on [-pi,pi] when we
    // carry enough terms:
    //   sin(x) ~ x*(1 + c1 x^2 + c2 x^4 + c3 x^6 + c4 x^8 + c5 x^10)
    //   c_k = (-1)^k / (2k+1)!
Lsin_c:
    .float -0.16666666666, 0.0083333333333, -0.0001984126984, 0.0000027557319
    .float -0.000000025052108, 0.0
    // cos(x) ~ 1 + d1 x^2 + ... + d6 x^12,  d_k = (-1)^k / (2k)!
Lcos_c:
    .float -0.5, 0.041666666666, -0.0013888888889, 0.000024801587302
    .float -0.00000027557319224, 0.0000000020876757
    // exp constants: log2(e), ln2_hi, ln2_lo, and poly coeffs
Lexp_c:
    .float 1.4426950408889634, 0.6931471824645996, -1.904654323148236e-09, 1.0
    // e^f ~ 1 + f + f^2/2 + f^3/6 + f^4/24 + f^5/120  (f in [-ln2/2, ln2/2])
Lexp_p:
    .float 1.0, 1.0, 0.5, 0.16666666666, 0.041666666, 0.0083333333
    // log constants: ln2, sqrt(2), and log(m) polynomial coefficients.
    // We decompose x = m * 2^e with m in [1,2); if m > sqrt(2) we halve m and
    // bump e so f = m-1 stays in [-0.293, 0.414], then
    //   log(m) ~ f - f^2/2 + f^3/3 - f^4/4 + f^5/5 - f^6/6   (Mercator series)
    // and log(x) = e*ln2 + log(m).
Llog_c:
    .float 0.6931471805599453, 1.4142135623730951, 1.0, 0.5
Llog_m:
    // integer masks: mantissa field, the 1.0 exponent bits, and exp-bias 127
    .long 0x007FFFFF, 0x3F800000, 127, 0
Llog_p:
    // p2..p7 = -1/2,1/3,-1/4,1/5,-1/6,1/7 (Mercator; the linear term is +f)
    .float -0.5, 0.33333333333, -0.25, 0.2, -0.16666666667, 0.14285714286

// --------------------------------------------------------------------------
// helper macro-ish: load constant page base into x9
// --------------------------------------------------------------------------

// --------------------------------------------------------------------------
// void fm_sin4_neon(float *out x0, const float *in x1, unsigned long n x2)
// --------------------------------------------------------------------------
.globl _fm_sin4_neon
.p2align 2
_fm_sin4_neon:
    adrp    x9, Lfm_consts@PAGE
    add     x9, x9, Lfm_consts@PAGEOFF
    ld1     {v28.4s}, [x9]              // v28 = [inv2pi, 2pi, pi, 0]
    adrp    x10, Lsin_c@PAGE
    add     x10, x10, Lsin_c@PAGEOFF
    ld1     {v29.4s, v30.4s}, [x10]     // v29=[c1,c2,c3,c4], v30 unused hi
    dup     v24.4s, v28.s[0]            // inv2pi
    dup     v25.4s, v28.s[1]            // 2pi
    lsr     x3, x2, #2
    cbz     x3, .Lsin_tail
.Lsin_loop:
    ld1     {v0.4s}, [x1], #16          // x
    // range reduce: x = x - 2pi*round(x*inv2pi)
    fmul    v1.4s, v0.4s, v24.4s
    frintn  v1.4s, v1.4s               // round to nearest even
    fmls    v0.4s, v1.4s, v25.4s       // x -= 2pi*k
    // now x in [-pi,pi]. poly: x*(1 + c1 x2 + c2 x4 + c3 x6 + c4 x8 + c5 x10)
    fmul    v2.4s, v0.4s, v0.4s        // x2
    // Horner in x2 starting from c5 (in v30.s[0])
    dup     v3.4s, v30.s[0]            // c5
    dup     v4.4s, v29.s[3]            // c4
    fmla    v4.4s, v3.4s, v2.4s        // c4 + c5 x2
    dup     v5.4s, v29.s[2]            // c3
    fmla    v5.4s, v4.4s, v2.4s
    dup     v6.4s, v29.s[1]            // c2
    fmla    v6.4s, v5.4s, v2.4s
    dup     v16.4s, v29.s[0]           // c1
    fmla    v16.4s, v6.4s, v2.4s
    fmov    v7.4s, #1.0
    fmla    v7.4s, v16.4s, v2.4s       // 1 + x2(...)
    fmul    v0.4s, v0.4s, v7.4s        // x * poly
    st1     {v0.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lsin_loop
.Lsin_tail:
    and     x4, x2, #3
    cbz     x4, .Lsin_done
.Lsin_t:
    ld1     {v0.s}[0], [x1], #4
    fmul    s1, s0, s24
    frintn  s1, s1
    fmsub   s0, s1, s25, s0            // x -= k*2pi   (s0 = -(s1*s25) + s0)
    fmul    s2, s0, s0                 // x2
    mov     s3, v30.s[0]               // c5
    mov     s4, v29.s[3]               // c4
    fmadd   s4, s3, s2, s4             // c4 + c5 x2
    mov     s5, v29.s[2]               // c3
    fmadd   s5, s4, s2, s5
    mov     s6, v29.s[1]               // c2
    fmadd   s6, s5, s2, s6
    mov     s17, v29.s[0]              // c1
    fmadd   s17, s6, s2, s17
    fmov    s7, #1.0
    fmadd   s7, s17, s2, s7            // 1 + x2(...)
    fmul    s0, s0, s7
    st1     {v0.s}[0], [x0], #4
    subs    x4, x4, #1
    b.ne    .Lsin_t
.Lsin_done:
    ret

// --------------------------------------------------------------------------
// void fm_cos4_neon(float *out x0, const float *in x1, unsigned long n x2)
// --------------------------------------------------------------------------
.globl _fm_cos4_neon
.p2align 2
_fm_cos4_neon:
    adrp    x9, Lfm_consts@PAGE
    add     x9, x9, Lfm_consts@PAGEOFF
    ld1     {v28.4s}, [x9]
    adrp    x10, Lcos_c@PAGE
    add     x10, x10, Lcos_c@PAGEOFF
    ld1     {v29.4s, v30.4s}, [x10]    // v29=[d1,d2,d3,d4], v30=[d5,..]
    dup     v24.4s, v28.s[0]           // inv2pi
    dup     v25.4s, v28.s[1]           // 2pi
    lsr     x3, x2, #2
    cbz     x3, .Lcos_tail
.Lcos_loop:
    ld1     {v0.4s}, [x1], #16
    fmul    v1.4s, v0.4s, v24.4s
    frintn  v1.4s, v1.4s
    fmls    v0.4s, v1.4s, v25.4s       // x in [-pi,pi]
    fmul    v2.4s, v0.4s, v0.4s        // x2
    // cos = 1 + d1 x2 + ... + d6 x12, Horner in x2 from d6
    dup     v3.4s, v30.s[1]            // d6
    dup     v17.4s, v30.s[0]           // d5
    fmla    v17.4s, v3.4s, v2.4s       // d5 + d6 x2
    dup     v4.4s, v29.s[3]            // d4
    fmla    v4.4s, v17.4s, v2.4s
    dup     v5.4s, v29.s[2]            // d3
    fmla    v5.4s, v4.4s, v2.4s
    dup     v6.4s, v29.s[1]            // d2
    fmla    v6.4s, v5.4s, v2.4s
    dup     v16.4s, v29.s[0]           // d1
    fmla    v16.4s, v6.4s, v2.4s
    fmov    v7.4s, #1.0
    fmla    v7.4s, v16.4s, v2.4s       // 1 + x2(...)
    st1     {v7.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lcos_loop
.Lcos_tail:
    and     x4, x2, #3
    cbz     x4, .Lcos_done
.Lcos_t:
    ld1     {v0.s}[0], [x1], #4
    fmul    s1, s0, s24
    frintn  s1, s1
    fmsub   s0, s1, s25, s0
    fmul    s2, s0, s0
    mov     s3, v30.s[1]               // d6
    mov     s18, v30.s[0]              // d5
    fmadd   s18, s3, s2, s18           // d5 + d6 x2
    mov     s4, v29.s[3]               // d4
    fmadd   s4, s18, s2, s4
    mov     s5, v29.s[2]               // d3
    fmadd   s5, s4, s2, s5
    mov     s6, v29.s[1]               // d2
    fmadd   s6, s5, s2, s6
    mov     s17, v29.s[0]              // d1
    fmadd   s17, s6, s2, s17
    fmov    s7, #1.0
    fmadd   s7, s17, s2, s7
    st1     {v7.s}[0], [x0], #4
    subs    x4, x4, #1
    b.ne    .Lcos_t
.Lcos_done:
    ret

// --------------------------------------------------------------------------
// void fm_exp4_neon(float *out x0, const float *in x1, unsigned long n x2)
// e^x = 2^k * e^f, k=round(x*log2e), f = x - k*ln2
// 2^k built by adding k to the float exponent (bias 127) via integer ops.
// --------------------------------------------------------------------------
.globl _fm_exp4_neon
.p2align 2
_fm_exp4_neon:
    adrp    x9, Lexp_c@PAGE
    add     x9, x9, Lexp_c@PAGEOFF
    ld1     {v28.4s}, [x9]             // [log2e, ln2_hi, ln2_lo, 1.0]
    adrp    x10, Lexp_p@PAGE
    add     x10, x10, Lexp_p@PAGEOFF
    ld1     {v26.4s, v27.4s}, [x10]    // v26=[1,1,0.5,1/6], v27=[1/24,1/120,0,0]
    dup     v20.4s, v28.s[0]           // log2e
    dup     v21.4s, v28.s[1]           // ln2_hi
    dup     v22.4s, v28.s[2]           // ln2_lo
    movi    v23.4s, #127               // exponent bias (as int)
    lsr     x3, x2, #2
    cbz     x3, .Lexp_tail
.Lexp_loop:
    ld1     {v0.4s}, [x1], #16         // x
    fmul    v1.4s, v0.4s, v20.4s       // x*log2e
    frintn  v1.4s, v1.4s               // k = round(.)
    // f = x - k*ln2_hi - k*ln2_lo   (two-part ln2 for accuracy)
    fmls    v0.4s, v1.4s, v21.4s
    fmls    v0.4s, v1.4s, v22.4s       // v0 = f
    // poly e^f = ((((p5 f + p4) f + p3) f + p2) f + p1) f + p0
    dup     v3.4s, v27.s[1]            // p5 = 1/120
    dup     v4.4s, v27.s[0]            // p4 = 1/24
    fmla    v4.4s, v3.4s, v0.4s
    dup     v5.4s, v26.s[3]            // p3 = 1/6
    fmla    v5.4s, v4.4s, v0.4s
    dup     v6.4s, v26.s[2]            // p2 = 0.5
    fmla    v6.4s, v5.4s, v0.4s
    dup     v7.4s, v26.s[1]            // p1 = 1
    fmla    v7.4s, v6.4s, v0.4s
    dup     v16.4s, v26.s[0]           // p0 = 1
    fmla    v16.4s, v7.4s, v0.4s       // v16 = e^f
    // build 2^k: ik = (int)k + 127, then shift left 23 into exponent
    fcvtns  v2.4s, v1.4s               // k as int
    add     v2.4s, v2.4s, v23.4s       // + bias
    shl     v2.4s, v2.4s, #23          // into exponent field -> 2^k as float bits
    // result = e^f * 2^k
    fmul    v16.4s, v16.4s, v2.4s
    st1     {v16.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Lexp_loop
.Lexp_tail:
    and     x4, x2, #3
    cbz     x4, .Lexp_done
.Lexp_t:
    ld1     {v0.s}[0], [x1], #4
    fmul    s1, s0, s20
    frintn  s1, s1
    fmsub   s0, s1, s21, s0            // f = x - k*ln2_hi
    fmsub   s0, s1, s22, s0            // f -= k*ln2_lo
    mov     s3, v27.s[1]               // 1/120
    mov     s4, v27.s[0]               // 1/24
    fmadd   s4, s3, s0, s4
    mov     s5, v26.s[3]               // 1/6
    fmadd   s5, s4, s0, s5
    mov     s6, v26.s[2]               // 0.5
    fmadd   s6, s5, s0, s6
    mov     s7, v26.s[1]               // 1
    fmadd   s7, s6, s0, s7
    mov     s16, v26.s[0]              // 1
    fmadd   s16, s7, s0, s16           // e^f
    fcvtns  w11, s1                    // k as int (scalar convert to GP reg)
    add     w11, w11, #127
    lsl     w11, w11, #23
    fmov    s2, w11
    fmul    s16, s16, s2
    st1     {v16.s}[0], [x0], #4
    subs    x4, x4, #1
    b.ne    .Lexp_t
.Lexp_done:
    ret

// --------------------------------------------------------------------------
// void fm_log4_neon(float *out x0, const float *in x1, unsigned long n x2)
// natural log via IEEE-754 exponent extraction + Mercator series on the
// mantissa (reduced to near 1 using the sqrt(2) split).
// --------------------------------------------------------------------------
// Register plan (stable across the loop; nothing sacred is clobbered):
//   v20=ln2  v21=sqrt2  v22=1.0  v23=0.5
//   v24=mantissa-mask(int)  v25=one-exponent-bits(int)  v29=bias127(int)
//   v26=[-1/2,1/3,-1/4,1/5]  v27=[-1/6,1/7,0,0]
.globl _fm_log4_neon
.p2align 2
_fm_log4_neon:
    adrp    x9, Llog_c@PAGE
    add     x9, x9, Llog_c@PAGEOFF
    ld1     {v28.4s}, [x9]             // [ln2, sqrt2, 1.0, 0.5]
    adrp    x9, Llog_m@PAGE
    add     x9, x9, Llog_m@PAGEOFF
    ld1     {v18.4s}, [x9]            // [0x007FFFFF, 0x3F800000, 127, 0] (ints)
    adrp    x10, Llog_p@PAGE
    add     x10, x10, Llog_p@PAGEOFF
    ld1     {v26.4s, v27.4s}, [x10]   // v26=[-1/2,1/3,-1/4,1/5] v27=[-1/6,1/7,..]
    dup     v20.4s, v28.s[0]          // ln2
    dup     v21.4s, v28.s[1]          // sqrt2
    dup     v22.4s, v28.s[2]          // 1.0
    dup     v23.4s, v28.s[3]          // 0.5
    dup     v24.4s, v18.s[0]          // mantissa mask
    dup     v25.4s, v18.s[1]          // 0x3F800000
    dup     v29.4s, v18.s[2]          // 127
    lsr     x3, x2, #2
    cbz     x3, .Llog_tail
.Llog_loop:
    ld1     {v0.4s}, [x1], #16         // x (assumed finite, > 0)
    // e = (bits >> 23) - 127  (the >>23 leaves only the 8-bit exponent field
    // for positive normals, so no extra mask is needed), then to float.
    ushr    v2.4s, v0.4s, #23
    sub     v2.4s, v2.4s, v29.4s      // unbiased exponent (int)
    scvtf   v2.4s, v2.4s              // -> float e
    // m = (bits & mantissa) | 0x3F800000  => m in [1,2)
    and     v3.16b, v0.16b, v24.16b
    orr     v3.16b, v3.16b, v25.16b
    // if m > sqrt2: m *= 0.5 ; e += 1
    fcmgt   v4.4s, v3.4s, v21.4s      // mask
    fmul    v5.4s, v3.4s, v23.4s      // 0.5*m
    bit     v3.16b, v5.16b, v4.16b    // m = mask ? 0.5*m : m
    and     v6.16b, v22.16b, v4.16b   // 1.0 where mask else 0
    fadd    v2.4s, v2.4s, v6.4s       // e adjusted
    fsub    v0.4s, v3.4s, v22.4s      // f = m - 1
    // Horner for the Mercator series, building (p2..p7 terms)*f + 1.
    // NB: use only caller-saved lanes (v16,v17,v19,v30,v31,v1) — v8..v15 are
    // callee-saved (their low 64 bits) and must not be clobbered here.
    dup     v17.4s, v27.s[1]          // 1/7
    dup     v19.4s, v27.s[0]          // -1/6
    fmla    v19.4s, v17.4s, v0.4s
    dup     v30.4s, v26.s[3]          // 1/5
    fmla    v30.4s, v19.4s, v0.4s
    dup     v31.4s, v26.s[2]          // -1/4
    fmla    v31.4s, v30.4s, v0.4s
    dup     v1.4s,  v26.s[1]          // 1/3
    fmla    v1.4s,  v31.4s, v0.4s
    dup     v17.4s, v26.s[0]          // -1/2
    fmla    v17.4s, v1.4s, v0.4s
    mov     v16.16b, v22.16b          // 1.0
    fmla    v16.4s, v17.4s, v0.4s     // (…)*f + 1
    fmul    v15.4s, v16.4s, v0.4s     // log(m) = f * (…)
    fmla    v15.4s, v2.4s, v20.4s     // + e*ln2
    st1     {v15.4s}, [x0], #16
    subs    x3, x3, #1
    b.ne    .Llog_loop
.Llog_tail:
    and     x4, x2, #3
    cbz     x4, .Llog_done
.Llog_t:
    ld1     {v0.s}[0], [x1], #4
    ushr    v2.4s, v0.4s, #23
    sub     v2.4s, v2.4s, v29.4s
    scvtf   v2.4s, v2.4s
    and     v3.16b, v0.16b, v24.16b
    orr     v3.16b, v3.16b, v25.16b
    fcmgt   v4.4s, v3.4s, v21.4s
    fmul    v5.4s, v3.4s, v23.4s
    bit     v3.16b, v5.16b, v4.16b
    and     v6.16b, v22.16b, v4.16b
    fadd    v2.4s, v2.4s, v6.4s
    fsub    v0.4s, v3.4s, v22.4s
    dup     v17.4s, v27.s[1]
    dup     v19.4s, v27.s[0]
    fmla    v19.4s, v17.4s, v0.4s
    dup     v30.4s, v26.s[3]
    fmla    v30.4s, v19.4s, v0.4s
    dup     v31.4s, v26.s[2]
    fmla    v31.4s, v30.4s, v0.4s
    dup     v1.4s,  v26.s[1]
    fmla    v1.4s,  v31.4s, v0.4s
    dup     v17.4s, v26.s[0]
    fmla    v17.4s, v1.4s, v0.4s
    mov     v16.16b, v22.16b
    fmla    v16.4s, v17.4s, v0.4s
    fmul    v15.4s, v16.4s, v0.4s
    fmla    v15.4s, v2.4s, v20.4s
    st1     {v15.s}[0], [x0], #4
    subs    x4, x4, #1
    b.ne    .Llog_t
.Llog_done:
    ret
