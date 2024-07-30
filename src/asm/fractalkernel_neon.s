// ==========================================================================
// fractalkernel_neon.s — NEON escape-time iteration inner loop (4 lanes).
// Mach-O / Apple ABI. This is the expensive part of a Mandelbrot/Julia render;
// the O(1) smooth-log tail is applied by the C wrapper in fractalkernel.c.
//
// void fk_iter4_neon(float *niter /*x0*/, float *mag2 /*x1*/,
//                    const float *zr0 /*x2*/, const float *zi0 /*x3*/,
//                    const float *cr /*x4*/, const float *ci /*x5*/,
//                    int max_iter /*w6*/, float bail2 /*s0*/)
//
// Iterates z = z^2 + c for all 4 lanes in lockstep, freezing each lane's z and
// iteration count the moment it escapes (|z|^2 > bail2), via per-lane masks.
// Matches the C reference: a lane's count is the number of z-updates performed,
// and its stored mag2 is |z|^2 at escape (or the final value if never escaped).
// Only caller-saved SIMD regs are used (v0..v7, v16..v31); no LR save needed
// (leaf routine, no calls).
// ==========================================================================

.text
.globl _fk_iter4_neon
.p2align 2
_fk_iter4_neon:
    ld1     {v1.4s}, [x2]                 // zr
    ld1     {v2.4s}, [x3]                 // zi
    ld1     {v3.4s}, [x4]                 // cr
    ld1     {v4.4s}, [x5]                 // ci
    dup     v0.4s, v0.s[0]                // bail2 broadcast

    movi    v5.4s, #0                     // iter accumulator (float 0.0)
    fmov    v6.4s, #1.0                   // constant 1.0 for counting

    // initial mag2 = zr^2 + zi^2. Use separate fmul+fadd (NOT fmla) so the
    // rounding matches the non-fused C reference exactly — fractal escape is
    // chaotic, and a single fused rounding here would flip boundary points'
    // iteration counts.
    fmul    v16.4s, v1.4s, v1.4s
    fmul    v25.4s, v2.4s, v2.4s
    fadd    v16.4s, v16.4s, v25.4s        // v16 = mag2
    // active = mag2 <= bail2   (0xFFFFFFFF where still iterating)
    fcmge   v7.4s, v0.4s, v16.4s          // bail2 >= mag2 ?

    // loop counter in w6 (max_iter). If <=0, skip.
    cmp     w6, #0
    b.le    .Lfk_done
.Lfk_loop:
    // none active? (horizontal max of the active mask == 0 -> break)
    umaxv   s17, v7.4s
    fmov    w7, s17
    cbz     w7, .Lfk_done

    // z^2 + c. Mirror the reference exactly: compute zr2,zi2 with plain fmul,
    // derive new_zr/new_zi, then mag2_new = new_zr^2 + new_zi^2 with fmul+fadd
    // (no fused multiply-add anywhere on the escape-test path).
    fmul    v18.4s, v1.4s, v1.4s          // zr^2
    fmul    v19.4s, v2.4s, v2.4s          // zi^2
    // new_zi = (2*zr)*zi + ci  — match the reference's factor order exactly:
    // it computes 2.0f*zr first, then *zi. ((zr*zi)*2) rounds differently.
    fadd    v20.4s, v1.4s, v1.4s          // 2*zr  (== 2.0*zr, exact)
    fmul    v20.4s, v20.4s, v2.4s         // (2*zr)*zi
    fadd    v20.4s, v20.4s, v4.4s         // + ci   -> new_zi
    // new_zr = zr^2 - zi^2 + cr
    fsub    v21.4s, v18.4s, v19.4s        // zr^2 - zi^2
    fadd    v21.4s, v21.4s, v3.4s         // + cr   -> new_zr
    // commit where active: z = active ? new : z   (BIT inserts new into z)
    bit     v1.16b, v21.16b, v7.16b       // zr
    bit     v2.16b, v20.16b, v7.16b       // zi
    // iter += active ? 1 : 0   (mask 1.0 by active, then add)
    and     v22.16b, v6.16b, v7.16b       // 1.0 where active else 0
    fadd    v5.4s, v5.4s, v22.4s
    // mag2_new = zr^2 + zi^2 from committed z (fmul+fadd, not fmla)
    fmul    v23.4s, v1.4s, v1.4s
    fmul    v25.4s, v2.4s, v2.4s
    fadd    v23.4s, v23.4s, v25.4s        // mag2_new
    bit     v16.16b, v23.16b, v7.16b      // mag2 = active ? mag2_new : mag2
    // active &= (mag2_new <= bail2)
    fcmge   v24.4s, v0.4s, v23.4s         // bail2 >= mag2_new ?
    and     v7.16b, v7.16b, v24.16b

    subs    w6, w6, #1
    b.ne    .Lfk_loop
.Lfk_done:
    st1     {v5.4s},  [x0]                // niter (float per lane)
    st1     {v16.4s}, [x1]                // mag2  (final per lane)
    ret
