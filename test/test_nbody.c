/* test_nbody.c — Barnes-Hut correctness + orbit stability. */
#include "cathode/physics.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>

static int failures=0, checks=0;
static void ok(const char*n,int c){ checks++; if(c) printf("  ok   %s\n",n); else {printf("  FAIL %s\n",n);failures++;} }

/* direct O(N^2) acceleration for reference */
static Vec3 direct_accel(const NBody*nb, int i){
    Vec3 a=v3(0,0,0); f32 eps2=nb->softening*nb->softening;
    for(int j=0;j<nb->n;++j){ if(j==i)continue;
        Vec3 d=v3_sub(nb->pos[j],nb->pos[i]);
        f32 r2=d.x*d.x+d.y*d.y+d.z*d.z+eps2;
        f32 inv=1.0f/sqrtf(r2);
        f32 f=nb->g*nb->mass[j]*inv*inv*inv;
        a=v3_add(a,v3_scale(d,f));
    }
    return a;
}

int main(void){
    printf("== CATHODE N-body (Barnes-Hut) ==\n");

    /* (a) two-body circular orbit stays bounded */
    NBody*nb=nbody_create(4);
    nb->g=1.0f; nb->softening=0.001f; nb->theta=0.4f;
    f32 M=100.0f, r0=2.0f;
    nbody_add(nb, v3(0,0,0), v3(0,0,0), M, col3(1,1,1));       /* central */
    f32 vc=sqrtf(nb->g*M/r0);
    nbody_add(nb, v3(r0,0,0), v3(0,0,vc), 1.0f, col3(1,0,0));  /* orbiter */
    f32 rmin=1e9f, rmax=0;
    for(int s=0;s<4000;++s){
        nbody_step(nb, 0.002f);
        Vec3 d=v3_sub(nb->pos[1],nb->pos[0]); f32 r=v3_len(d);
        if(r<rmin)rmin=r; if(r>rmax)rmax=r;
    }
    printf("  orbit r in [%.3f, %.3f] (r0=%.2f)\n", rmin, rmax, r0);
    ok("two-body orbit bounded", rmin>r0*0.75f && rmax<r0*1.35f);

    /* (b) Barnes-Hut accel matches direct within a few percent on a cloud */
    NBody*c=nbody_create(300);
    c->g=1.0f; c->softening=0.05f; c->theta=0.5f;
    Rng rng; rng_seed(&rng, 42);
    for(int i=0;i<200;++i)
        nbody_add(c, v3(rng_normal(&rng),rng_normal(&rng),rng_normal(&rng)), v3(0,0,0),
                  0.5f+rng_f32(&rng), col3(1,1,1));
    /* trigger a tree build+accel by stepping with dt≈0 won't expose accel;
       instead compare via a public step is indirect. Recompute here: */
    /* We can't call the internal compute_accel; approximate by taking one tiny
       leapfrog step and checking velocity delta ~ accel*dt against direct. */
    Vec3 v_before[200]; for(int i=0;i<200;++i) v_before[i]=c->vel[i];
    f32 dt=1e-4f;
    nbody_step(c, dt);
    int sample[5]={0,37,88,150,199};
    f32 worst=0;
    for(int k=0;k<5;++k){ int i=sample[k];
        Vec3 dv=v3_sub(c->vel[i], v_before[i]);
        Vec3 a_bh=v3_scale(dv, 1.0f/dt);      /* ~ average accel over step */
        Vec3 a_dir=direct_accel(c, i);        /* accel at start (approx) */
        Vec3 diff=v3_sub(a_bh,a_dir);
        f32 rel=v3_len(diff)/(v3_len(a_dir)+1e-6f);
        if(rel>worst)worst=rel;
    }
    printf("  worst BH-vs-direct rel err = %.4f\n", worst);
    ok("Barnes-Hut accel ~ direct (<8%)", worst < 0.08f);

    /* (c) galaxy seeding runs and stays finite for many steps */
    NBody*g=nbody_create(600);
    nbody_seed_galaxy(g, 500, v3(0,0,0), 3.0f, 2000.0f);
    ok("galaxy seeded", g->n==501);
    int finite=1;
    for(int s=0;s<200;++s){
        nbody_step(g, 0.001f);
    }
    for(int i=0;i<g->n;++i){ Vec3 p=g->pos[i]; if(!(p.x==p.x)||fabsf(p.x)>1e6f){finite=0;break;} }
    ok("galaxy stays finite over 200 steps", finite);

    /* (d) collision seeding */
    NBody*col=nbody_create(400);
    nbody_seed_collision(col, 150);
    ok("collision seeded two galaxies", col->n == 2*151);

    nbody_destroy(nb);nbody_destroy(c);nbody_destroy(g);nbody_destroy(col);
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
