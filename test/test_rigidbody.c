/* ==========================================================================
 * test_rigidbody.c  -  invariants for the 2D impulse rigid-body solver.
 *
 * Physical properties that must hold regardless of the exact numbers:
 *   - a box dropped onto the floor comes to REST above y=0 (doesn't sink or
 *     tunnel through), and its kinetic energy decays toward ~0;
 *   - a static body never moves;
 *   - all bodies stay within the domain [0,W]x[0,H≈] (no escaping walls);
 *   - a settled stack has bounded (non-exploding) energy  -  the solver is stable;
 *   - free-fall for one step matches v = g*dt (integration sanity).
 * Deterministic (no RNG in the asserts that matter).
 * ========================================================================== */
#include "cathode/physics.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static void check(const char*n,int ok){ checks++; if(ok)printf("  ok   %s\n",n); else{printf("  FAIL %s\n",n);failures++;} }

int main(void){
    printf("== CATHODE 2D rigid-body tests ==\n");
    const f32 W=20, H=20;

    /* free-fall integration: one step, v should be g*dt */
    {
        RigidWorld *rw=rb_create(W,H,4);
        rb_set_gravity(rw,0,-10.0f);
        rb_add_box(rw, 10, 15, 0.5f, 0.5f, 0, 1.0f);
        rb_step(rw, 0.01f, 4);
        /* read back via poly center before/after isn't exposed; use energy:
         * after 1 step v=|g|*dt=0.1, KE=0.5*m*v^2. m = density*area = 1*(1*1)=1. */
        f32 e = rb_total_energy(rw);
        f32 expect = 0.5f*1.0f*0.1f*0.1f;
        check("free-fall energy after 1 step ~ 0.5 m (g dt)^2",
              fabsf(e-expect) < 0.2f*expect + 1e-4f);
        rb_destroy(rw);
    }

    /* a box dropped onto the floor settles above y=0 and loses its energy */
    {
        RigidWorld *rw=rb_create(W,H,4);
        rb_set_gravity(rw,0,-9.8f);
        int id=rb_add_box(rw, 10, 8, 1.0f, 1.0f, 0, 1.0f);
        (void)id;
        for (int i=0;i<1200;++i) rb_step(rw, 1.0f/120.0f, 12);
        f32 xy[16]; i32 nn; f32 cx,cy,ang;
        rb_body_poly(rw, 0, xy, 8, &nn, &cx, &cy, &ang);
        /* lowest vertex must be >= -small (resting on floor, not through it) */
        f32 lowest=1e30f; for(int k=0;k<nn;++k) if(xy[2*k+1]<lowest) lowest=xy[2*k+1];
        check("dropped box rests on/above the floor", lowest > -0.15f);
        check("box center settled near its half-height", cy > 0.5f && cy < 2.0f);
        check("box came to rest (low KE)", rb_total_energy(rw) < 0.5f);
        rb_destroy(rw);
    }

    /* static body never moves even when hit */
    {
        RigidWorld *rw=rb_create(W,H,4);
        rb_set_gravity(rw,0,-9.8f);
        int sid=rb_add_box(rw, 10, 3, 3.0f, 0.5f, 0, 0.0f);   /* static platform */
        f32 sx0,sy0,sa0; f32 xy[16]; i32 nn;
        rb_body_poly(rw, sid, xy, 8, &nn, &sx0, &sy0, &sa0);
        rb_add_box(rw, 10, 10, 0.8f, 0.8f, 0.2f, 1.0f);        /* dropped on it */
        for (int i=0;i<600;++i) rb_step(rw, 1.0f/120.0f, 10);
        f32 sx1,sy1,sa1;
        rb_body_poly(rw, sid, xy, 8, &nn, &sx1, &sy1, &sa1);
        check("static body did not translate",
              fabsf(sx1-sx0)<1e-4f && fabsf(sy1-sy0)<1e-4f);
        check("static body did not rotate", fabsf(sa1-sa0)<1e-4f);
        rb_destroy(rw);
    }

    /* a small stack stays inside the domain and doesn't explode */
    {
        RigidWorld *rw=rb_create(W,H,16);
        rb_set_gravity(rw,0,-9.8f);
        for (int k=0;k<5;++k) rb_add_box(rw, 10, 2.0f+k*1.4f, 0.6f, 0.6f, 0, 1.0f);
        rb_add_ngon(rw, 11, 12, 5, 0.7f, 0.3f, 1.0f);   /* a pentagon on top */
        for (int i=0;i<1500;++i) rb_step(rw, 1.0f/120.0f, 12);
        int inside=1; f32 maxe=0;
        for (int b=0;b<rb_count(rw);++b){
            f32 xy[16]; i32 nn; f32 cx,cy,ang;
            rb_body_poly(rw, b, xy, 8, &nn, &cx,&cy,&ang);
            for (int k=0;k<nn;++k){
                if (xy[2*k]< -0.5f || xy[2*k]>W+0.5f || xy[2*k+1]< -0.5f) inside=0;
            }
        }
        maxe=rb_total_energy(rw);
        check("stack stays within domain walls/floor", inside);
        check("stack energy bounded (solver stable)", maxe < 5.0f);
        rb_destroy(rw);
    }

    /* bad args */
    check("rb_create bad dims -> NULL", rb_create(-1,10,4)==NULL);
    check("rb_add_box on NULL world -> -1", rb_add_box(NULL,0,0,1,1,0,1)==-1);

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
