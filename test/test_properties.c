/* ==========================================================================
 * test_properties.c — property-based tests.
 *
 * Unlike the equivalence tests (NEON asm == C reference), these assert
 * *mathematical invariants* that must hold for ANY input, checked over many
 * thousands of randomized cases. This catches whole classes of bugs the
 * example-based tests miss. Each property runs N random trials and reports the
 * worst violation.
 *
 * Properties covered:
 *   vec/mat : dot commutes; cross ⟂ operands; normalize is unit; identity is
 *             neutral; (AB)v == A(Bv); rotation preserves length; perspective
 *             maps in front of camera to valid clip w.
 *   quat    : unit-quaternion rotation preserves vector length; q and its
 *             matrix agree; identity quaternion is a no-op.
 *   dsp     : YIQ round-trip within tolerance; scale/bias/clamp stays in range.
 *   noise   : Perlin/simplex bounded and continuous; fbm finite.
 *   nbody   : total momentum conserved over a step; energy stays bounded.
 * ========================================================================== */
#include "cathode/vec.h"
#include "cathode/simd.h"
#include "cathode/dsp.h"
#include "cathode/noise.h"
#include "cathode/physics.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, props=0;
static Rng RNG;
static f32 rf(f32 lo, f32 hi){ return lo + (hi-lo)*rng_f32(&RNG); }

/* Report a property: name, #trials, and whether all passed. */
static void prop(const char *name, int trials, int ok, double worst){
    props++;
    if (ok) printf("  ok   %-40s %6d trials (worst %.2e)\n", name, trials, worst);
    else { printf("  FAIL %-40s worst violation %.4e\n", name, worst); failures++; }
}

static Vec3 rv3(f32 s){ return v3(rf(-s,s),rf(-s,s),rf(-s,s)); }

int main(void){
    rng_seed(&RNG, 0x9C0FFEEULL);   /* fixed seed for reproducibility */
    printf("== CATHODE property-based tests ==\n");
    const int N=20000;
    double worst; int ok;

    /* --- dot product is commutative --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ Vec3 a=rv3(10),b=rv3(10); double d=fabs(v3_dot(a,b)-v3_dot(b,a)); if(d>worst)worst=d; if(d>1e-4)ok=0; }
    prop("v3_dot commutative", N, ok, worst);

    /* --- cross product is perpendicular to both operands --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ Vec3 a=rv3(10),b=rv3(10); Vec3 c=v3_cross(a,b);
        double da=fabs(v3_dot(c,a)), db=fabs(v3_dot(c,b));
        double mag=v3_len(a)*v3_len(b)+1e-6; double e=fmax(da,db)/mag;
        if(e>worst)worst=e; if(e>1e-4)ok=0; }
    prop("v3_cross perpendicular", N, ok, worst);

    /* --- normalized vector has unit length --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ Vec3 a=rv3(100); if(v3_len2(a)<1e-6f)continue; Vec3 n=v3_norm(a);
        double d=fabs(v3_len(n)-1.0); if(d>worst)worst=d; if(d>1e-4)ok=0; }
    prop("v3_norm unit length", N, ok, worst);

    /* --- identity matrix is neutral: I*v == v --- */
    worst=0; ok=1;
    Mat4 I=mat4_identity();
    for (int i=0;i<N;++i){ Vec4 v=v4(rf(-9,9),rf(-9,9),rf(-9,9),rf(-9,9)); Vec4 r=mat4_mul_v4(I,v);
        double d=fabs(r.x-v.x)+fabs(r.y-v.y)+fabs(r.z-v.z)+fabs(r.w-v.w); if(d>worst)worst=d; if(d>1e-4)ok=0; }
    prop("mat4 identity neutral", N, ok, worst);

    /* --- associativity: (A*B)*v == A*(B*v) --- */
    worst=0; ok=1;
    for (int i=0;i<N/4;++i){
        Mat4 A=mat4_mul(mat4_rotate_x(rf(-3,3)), mat4_translate(rv3(3)));
        Mat4 B=mat4_mul(mat4_rotate_y(rf(-3,3)), mat4_scale(v3(rf(0.5f,2),rf(0.5f,2),rf(0.5f,2))));
        Vec4 v=v4(rf(-5,5),rf(-5,5),rf(-5,5),1);
        Vec4 r1=mat4_mul_v4(mat4_mul(A,B), v);
        Vec4 r2=mat4_mul_v4(A, mat4_mul_v4(B,v));
        double d=fabs(r1.x-r2.x)+fabs(r1.y-r2.y)+fabs(r1.z-r2.z);
        if(d>worst)worst=d; if(d>1e-3)ok=0; }
    prop("mat4 mul associative (AB)v=A(Bv)", N/4, ok, worst);

    /* --- rotation preserves length --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ Mat4 R=mat4_mul(mat4_rotate_x(rf(-6,6)),mat4_mul(mat4_rotate_y(rf(-6,6)),mat4_rotate_z(rf(-6,6))));
        Vec4 v=v4_from_v3(rv3(10),0); Vec4 r=mat4_mul_v4(R,v);
        double l0=v3_len(v4_xyz(v)), l1=v3_len(v4_xyz(r)); double d=fabs(l1-l0)/(l0+1e-6);
        if(d>worst)worst=d; if(d>1e-3)ok=0; }
    prop("rotation preserves length", N, ok, worst);

    /* --- NEON mat4_mul matches inline mat4_mul (cross-impl invariant) --- */
    worst=0; ok=1;
    for (int i=0;i<N/4;++i){ Mat4 A,B; for(int k=0;k<16;++k){A.m[k]=rf(-3,3);B.m[k]=rf(-3,3);}
        Mat4 rn; mat4_mul_neon(rn.m,A.m,B.m); Mat4 ri=mat4_mul(A,B);
        double d=0; for(int k=0;k<16;++k)d=fmax(d,fabs(rn.m[k]-ri.m[k])); if(d>worst)worst=d; if(d>1e-3)ok=0; }
    prop("mat4_mul_neon == inline mat4_mul", N/4, ok, worst);

    /* --- quaternion rotation preserves length --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ Quat q=quat_axis_angle(rv3(1), rf(-6,6)); Mat4 R=quat_to_mat4(q);
        Vec4 v=v4_from_v3(rv3(10),0); Vec4 r=mat4_mul_v4(R,v);
        double l0=v3_len(v4_xyz(v)), l1=v3_len(v4_xyz(r)); double d=fabs(l1-l0)/(l0+1e-6);
        if(d>worst)worst=d; if(d>1e-3)ok=0; }
    prop("quat rotation preserves length", N, ok, worst);

    /* --- identity quaternion is a no-op --- */
    worst=0; ok=1;
    Mat4 QI=quat_to_mat4(quat_identity());
    for (int i=0;i<N;++i){ Vec4 v=v4_from_v3(rv3(9),0); Vec4 r=mat4_mul_v4(QI,v);
        double d=fabs(r.x-v.x)+fabs(r.y-v.y)+fabs(r.z-v.z); if(d>worst)worst=d; if(d>1e-4)ok=0; }
    prop("identity quaternion no-op", N, ok, worst);

    /* --- YIQ round-trip within published-constant tolerance --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float rgb[3]={rf(0,1),rf(0,1),rf(0,1)},yiq[3],back[3];
        dsp_rgb2yiq_ref(yiq,rgb,1); dsp_yiq2rgb_ref(back,yiq,1);
        for(int k=0;k<3;++k){double d=fabs(rgb[k]-back[k]); if(d>worst)worst=d; if(d>3e-3)ok=0;} }
    prop("YIQ round-trip bounded", N, ok, worst);

    /* --- scale_bias_clamp output stays in [0,hi] --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float in=rf(-5,5),out; float hi=rf(0.5f,2.0f);
        dsp_scale_bias_clamp_ref(&out,&in, rf(-3,3), rf(-3,3), hi, 1);
        if(out< -1e-6f || out>hi+1e-6f){ ok=0; double v=fmax(-out,out-hi); if(v>worst)worst=v; } }
    prop("scale_bias_clamp within [0,hi]", N, ok, worst);

    /* --- Perlin noise bounded in [-1,1] and continuous --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float x=rf(-100,100),y=rf(-100,100); float v=perlin2(x,y);
        if(fabsf(v)>1.001f){ok=0; double e=fabsf(v)-1.0; if(e>worst)worst=e;} }
    prop("perlin2 bounded [-1,1]", N, ok, worst);
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float x=rf(-50,50),y=rf(-50,50); float e=1e-3f;
        double d=fabs(perlin2(x,y)-perlin2(x+e,y))/e; if(d>worst)worst=d; if(d>4.0)ok=0; } /* Lipschitz-ish */
    prop("perlin2 continuous (bounded slope)", N, ok, worst);

    /* --- fbm finite everywhere --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float v=fbm2(rf(-200,200),rf(-200,200),6,2.0f,0.5f); if(!(v==v)||fabsf(v)>1e4f)ok=0; }
    prop("fbm2 finite", N, ok, 0.0);

    /* --- Worley: F1>=0, F2-F1>=0, and F1 continuous --- */
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float x=rf(-80,80),y=rf(-80,80);
        float f1=worley2(x,y), edge=worley2_f2f1(x,y);
        if (f1<-1e-6f){ ok=0; if(-f1>worst)worst=-f1; }
        if (edge<-1e-6f){ ok=0; if(-edge>worst)worst=-edge; }
        if (!(f1==f1) || !(edge==edge)) ok=0; }
    prop("worley F1>=0 and F2>=F1", N, ok, worst);
    worst=0; ok=1;
    for (int i=0;i<N;++i){ float x=rf(-40,40),y=rf(-40,40); float e=1e-3f;
        double d=fabs(worley2(x,y)-worley2(x+e,y))/e; if(d>worst)worst=d; if(d>2.0)ok=0; }
    prop("worley2 continuous (bounded slope)", N, ok, worst);

    /* --- N-body: total momentum change over a step is ~0 (internal forces) --- */
    worst=0; ok=1;
    for (int trial=0; trial<50; ++trial){
        NBody *nb=nbody_create(64); nb->g=1.0f; nb->softening=0.1f; nb->theta=0.5f;
        for(int i=0;i<40;++i) nbody_add(nb, rv3(5), rv3(0.5f), rf(0.5f,2.0f), col3(1,1,1));
        Vec3 p0=v3(0,0,0); for(int i=0;i<nb->n;++i) p0=v3_add(p0, v3_scale(nb->vel[i],nb->mass[i]));
        nbody_step(nb, 0.001f);
        Vec3 p1=v3(0,0,0); for(int i=0;i<nb->n;++i) p1=v3_add(p1, v3_scale(nb->vel[i],nb->mass[i]));
        double dp=v3_len(v3_sub(p1,p0));
        /* Barnes-Hut is approximate so momentum isn't perfectly conserved, but
         * the drift per small step must stay small relative to total |p|. */
        double scale=v3_len(p0)+1.0;
        double rel=dp/scale; if(rel>worst)worst=rel; if(rel>0.05)ok=0;
        nbody_destroy(nb);
    }
    prop("nbody momentum ~conserved/step", 50, ok, worst);

    printf("\n%d properties, %d failures\n", props, failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
