/* test_sph.c  -  SPH fluid: containment, settling, stability. */
#include "cathode/physics.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static void ok(const char*n,int c){ ++checks; if(c) printf("  ok   %s\n",n); else { printf("  FAIL %s\n",n); ++failures; } }

int main(void){
    printf("== CATHODE SPH fluid ==\n");
    f32 W=20.0f, H=20.0f;
    SphSim *s=sph_create(4000, W, H);
    sph_set_gravity(s, 0.0f, -9.8f);
    /* drop a block of fluid in the upper region */
    sph_add_block(s, 3.0f, 10.0f, 12.0f, 18.0f, 0.7f);
    int n=sph_count(s);
    printf("  seeded %d particles\n", n);
    ok("particles created", n>50);

    f32 *xy=malloc((size_t)n*2*4), *sp=malloc((size_t)n*4);

    /* simulate; track containment + finiteness every step */
    int contained=1, finite=1;
    float avg_y0=0; sph_positions(s,xy,sp); for(int i=0;i<n;++i) avg_y0+=xy[i*2+1]; avg_y0/=n;
    for (int step=0; step<600; ++step){
        sph_step(s, 0.008f);
        sph_positions(s, xy, sp);
        for (int i=0;i<n;++i){
            float x=xy[i*2], y=xy[i*2+1];
            if (!(x==x) || !(y==y) || fabsf(x)>1e4f) finite=0;
            if (x< -0.5f || x>W+0.5f || y< -0.5f || y>H+0.5f) contained=0;
        }
        if (!finite) break;
    }
    ok("stays finite over 600 steps", finite);
    ok("particles stay in domain", contained);

    /* fluid should have fallen under gravity: average y decreased */
    float avg_y1=0; sph_positions(s,xy,sp); for(int i=0;i<n;++i) avg_y1+=xy[i*2+1]; avg_y1/=n;
    printf("  avg y: %.2f -> %.2f\n", avg_y0, avg_y1);
    ok("fluid fell under gravity", avg_y1 < avg_y0);

    /* fluid should pool near the bottom (a good chunk below mid-height) */
    int low=0; for(int i=0;i<n;++i) if(xy[i*2+1] < H*0.5f) low++;
    printf("  %d/%d particles below mid-height\n", low, n);
    ok("fluid pooled at the bottom", low > n/2);

    /* after settling, kinetic energy should be modest (came ~to rest) */
    sph_positions(s,xy,sp);
    float maxspeed=0; for(int i=0;i<n;++i) if(sp[i]>maxspeed)maxspeed=sp[i];
    printf("  max speed after settle: %.3f\n", maxspeed);
    ok("settles (bounded max speed)", maxspeed < 30.0f);

    free(xy); free(sp); sph_destroy(s);
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
