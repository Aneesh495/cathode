// ==========================================================================
// simd_neon.s — hand-written AArch64 NEON, Mach-O / Apple ABI (arm64-darwin).
//
// Apple ABI notes baked into this file:
//   * Exported C symbols get a leading underscore (_mat4_mul_neon).
//   * Args: x0..x7 integer/pointer, s0..s7 / v0..v7 float/SIMD.
//   * v8..v15 are callee-saved (low 64 bits); we avoid them or save them.
//   * Return float in s0.
//   * .p2align 2 => 4-byte instruction alignment.
//
// Matrices are column-major float[16]: element (row,col) at [col*4 + row].
// A column is 4 contiguous floats => one q register via LD1 {v.4s}.
// ==========================================================================

.text

// --------------------------------------------------------------------------
// void mat4_mul_neon(float *out /*x0*/, const float *a /*x1*/, const float *b /*x2*/)
//
// out_col_j = A * b_col_j = sum_k A_col_k * b[k,j]
// Load A's four columns into v0..v3 (constant across the whole product).
// For each output column j, broadcast the 4 scalars of b's column j and FMA.
// --------------------------------------------------------------------------
.globl _mat4_mul_neon
.p2align 2
_mat4_mul_neon:
    ld1     {v0.4s, v1.4s, v2.4s, v3.4s}, [x1]   // v0=A col0 .. v3=A col3

    // ---- output column 0 ----
    ld1     {v4.4s}, [x2]                         // b col0 = [b0,b1,b2,b3]
    fmul    v16.4s, v0.4s, v4.s[0]                // A0 * b0
    fmla    v16.4s, v1.4s, v4.s[1]                // + A1 * b1
    fmla    v16.4s, v2.4s, v4.s[2]                // + A2 * b2
    fmla    v16.4s, v3.4s, v4.s[3]                // + A3 * b3
    str     q16, [x0]

    // ---- output column 1 ----
    add     x3, x2, #16
    ld1     {v5.4s}, [x3]
    fmul    v17.4s, v0.4s, v5.s[0]
    fmla    v17.4s, v1.4s, v5.s[1]
    fmla    v17.4s, v2.4s, v5.s[2]
    fmla    v17.4s, v3.4s, v5.s[3]
    str     q17, [x0, #16]

    // ---- output column 2 ----
    add     x3, x2, #32
    ld1     {v6.4s}, [x3]
    fmul    v18.4s, v0.4s, v6.s[0]
    fmla    v18.4s, v1.4s, v6.s[1]
    fmla    v18.4s, v2.4s, v6.s[2]
    fmla    v18.4s, v3.4s, v6.s[3]
    str     q18, [x0, #32]

    // ---- output column 3 ----
    add     x3, x2, #48
    ld1     {v7.4s}, [x3]
    fmul    v19.4s, v0.4s, v7.s[0]
    fmla    v19.4s, v1.4s, v7.s[1]
    fmla    v19.4s, v2.4s, v7.s[2]
    fmla    v19.4s, v3.4s, v7.s[3]
    str     q19, [x0, #48]

    ret

// --------------------------------------------------------------------------
// void mat4_transform_neon(float *out /*x0*/, const float *m /*x1*/, const float *v /*x2*/)
// out = m * v  (m column-major). Same idea, single output column.
// --------------------------------------------------------------------------
.globl _mat4_transform_neon
.p2align 2
_mat4_transform_neon:
    ld1     {v0.4s, v1.4s, v2.4s, v3.4s}, [x1]
    ld1     {v4.4s}, [x2]
    fmul    v16.4s, v0.4s, v4.s[0]
    fmla    v16.4s, v1.4s, v4.s[1]
    fmla    v16.4s, v2.4s, v4.s[2]
    fmla    v16.4s, v3.4s, v4.s[3]
    str     q16, [x0]
    ret

// --------------------------------------------------------------------------
// float vec4_dot_neon(const float *a /*x0*/, const float *b /*x1*/)
// --------------------------------------------------------------------------
.globl _vec4_dot_neon
.p2align 2
_vec4_dot_neon:
    ld1     {v0.4s}, [x0]
    ld1     {v1.4s}, [x1]
    fmul    v0.4s, v0.4s, v1.4s
    faddp   v0.4s, v0.4s, v0.4s      // pairwise add: [a+b, c+d, a+b, c+d]
    faddp   v0.2s, v0.2s, v0.2s      // [ (a+b)+(c+d), .. ]
    ret                              // result already in s0

// --------------------------------------------------------------------------
// float fast_rsqrt_neon(float x /*s0*/)
// frsqrte seed + 2 Newton-Raphson iterations via frsqrts.
//   y1 = y0 * frsqrts(x*y0, y0)  repeated.
// --------------------------------------------------------------------------
.globl _fast_rsqrt_neon
.p2align 2
_fast_rsqrt_neon:
    frsqrte s1, s0            // s1 = approx 1/sqrt(x)
    fmul    s2, s1, s1        // s2 = y0^2
    frsqrts s2, s0, s2        // s2 = (3 - x*y0^2)/2
    fmul    s1, s1, s2        // refine
    fmul    s2, s1, s1
    frsqrts s2, s0, s2
    fmul    s0, s1, s2        // result in s0
    ret

// --------------------------------------------------------------------------
// void vec3_normalize_neon(float *out /*x0*/, const float *v /*x1*/)
// --------------------------------------------------------------------------
.globl _vec3_normalize_neon
.p2align 2
_vec3_normalize_neon:
    ld1     {v0.4s}, [x1]            // [x,y,z,w]
    mov     v1.16b, v0.16b
    mov     v1.s[3], wzr            // zero w so it doesn't enter length
    fmul    v2.4s, v1.4s, v1.4s     // [x2,y2,z2,0]
    faddp   v2.4s, v2.4s, v2.4s
    faddp   v2.2s, v2.2s, v2.2s     // s2 = x2+y2+z2
    // rsqrt(len2) via frsqrte + 2 NR
    frsqrte s3, s2
    fmul    s4, s3, s3
    frsqrts s4, s2, s4
    fmul    s3, s3, s4
    fmul    s4, s3, s3
    frsqrts s4, s2, s4
    fmul    s3, s3, s4              // s3 = 1/sqrt(len2)
    dup     v3.4s, v3.s[0]
    fmul    v4.4s, v0.4s, v3.4s     // scale x,y,z (and w, we'll fix)
    mov     v4.s[3], v0.s[3]        // preserve original w
    str     q4, [x0]
    ret

// --------------------------------------------------------------------------
// void saxpy_neon(float *y /*x0*/, const float *x /*x1*/, float a /*s0*/, unsigned long n /*x2*/)
// Vectorized 4-at-a-time with scalar tail. Broadcasts a into v1.4s.
// --------------------------------------------------------------------------
.globl _saxpy_neon
.p2align 2
_saxpy_neon:
    dup     v1.4s, v0.s[0]          // v1 = [a,a,a,a]
    lsr     x3, x2, #2              // x3 = n / 4  (vector iterations)
    cbz     x3, .Lsaxpy_tail
.Lsaxpy_loop:
    ld1     {v2.4s}, [x0]           // y chunk
    ld1     {v3.4s}, [x1], #16      // x chunk, post-increment
    fmla    v2.4s, v3.4s, v1.4s     // y += a*x
    st1     {v2.4s}, [x0], #16      // store + post-increment y
    subs    x3, x3, #1
    b.ne    .Lsaxpy_loop
.Lsaxpy_tail:
    and     x4, x2, #3              // remainder = n & 3
    cbz     x4, .Lsaxpy_done
.Lsaxpy_tail_loop:
    ldr     s2, [x0]
    ldr     s3, [x1], #4
    fmadd   s2, s3, s0, s2         // s2 = x*a + y
    str     s2, [x0], #4
    subs    x4, x4, #1
    b.ne    .Lsaxpy_tail_loop
.Lsaxpy_done:
    ret
