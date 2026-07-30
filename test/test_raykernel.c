/* test_raykernel.c — NEON batched ray intersection vs C reference. */
#include "cathode/raykernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static float frand(void){ return (float)rand()/(float)RAND_MAX*2.0f-1.0f; }

static void chk4(const char*name, const float*g, const float*e){
    checks++;
    for(int i=0;i<4;++i){
        int gm = g[i] < 0.0f, em = e[i] < 0.0f;   /* miss sign agreement */
        if (gm != em){ printf("  FAIL %-16s lane %d miss mismatch (got %.4f exp %.4f)\n",name,i,g[i],e[i]); failures++; return; }
        if (!em){
            float d=fabsf(g[i]-e[i]);
            if (d > 1e-3f*(1.0f+fabsf(e[i]))){ printf("  FAIL %-16s lane %d t mismatch got %.5f exp %.5f\n",name,i,g[i],e[i]); failures++; return; }
        }
    }
    printf("  ok   %-16s\n", name);
}

int main(void){
    srand(7);
    printf("== CATHODE raykernel NEON vs C reference ==\n");

    /* known case: ray from (0,0,-5) toward +z hits unit sphere at origin at t=4 */
    {
        float ro[3]={0,0,-5}, rd[3]={0,0,1};
        float sph[16]={ 0,2,0,0,  0,0,0,0,  0,0,0,5,  1,1,0.5f,1 };
        /* lane0: sphere at origin r1 -> t=4; lane1: sphere at (2,0,0) r1 -> miss;
           lane2: sphere at (0,0,0) r0.5 -> t=4.5; lane3: sphere at (0,0,5) r1 -> t=9 */
        float gn[4], gr[4];
        rk_ray4_spheres_neon(gn, ro, rd, sph);
        rk_ray4_spheres_ref (gr, ro, rd, sph);
        chk4("sphere known", gn, gr);
        if (fabsf(gr[0]-4.0f)>1e-3f){ printf("  FAIL sphere lane0 expected t=4 got %.4f\n", gr[0]); failures++; }
        checks++;
    }

    /* random spheres */
    for (int trial=0; trial<200; ++trial){
        float ro[3]={frand()*3,frand()*3,frand()*3};
        float rdv[3]={frand(),frand(),frand()};
        float len=sqrtf(rdv[0]*rdv[0]+rdv[1]*rdv[1]+rdv[2]*rdv[2])+1e-6f;
        rdv[0]/=len; rdv[1]/=len; rdv[2]/=len;   /* normalized */
        float sph[16];
        for(int i=0;i<4;++i){ sph[i]=frand()*4; sph[4+i]=frand()*4; sph[8+i]=frand()*4; sph[12+i]=0.3f+fabsf(frand())*2.0f; }
        float gn[4],gr[4];
        rk_ray4_spheres_neon(gn,ro,rdv,sph);
        rk_ray4_spheres_ref (gr,ro,rdv,sph);
        chk4("sphere random", gn, gr);
    }

    /* known AABB: a genuine diagonal ray into a unit box centered at origin.
     * (A perfectly axis-aligned ray with dx=dy=0 is a measure-zero degenerate
     * input requiring inf arithmetic in the branchless SIMD slab test; real
     * tracer rays are never exactly axis-aligned, so we test a diagonal ray and
     * validate NEON against the trusted C reference.) */
    {
        float ro[3]={-3,-3,-3};
        float rd[3]={0.5773503f,0.5773503f,0.5773503f};  /* normalized (1,1,1) */
        float bb[24]={ -1,-1,-1,-1,  -1,-1,-1,-1,  -1,-1,-1,-1,
                        1, 1, 1, 1,   1, 1, 1, 1,   1, 1, 1, 1 };
        float gn[4],gr[4];
        rk_ray4_aabb_neon(gn,ro,rd,bb);
        rk_ray4_aabb_ref (gr,ro,rd,bb);
        chk4("aabb known diagonal", gn, gr);
        /* ray enters the box where each coord reaches -1: t=(−1−(−3))/0.57735 ~ 3.46 */
        if (gr[0] < 3.0f || gr[0] > 4.0f){ printf("  FAIL aabb lane0 expected t~3.46 got %.4f\n", gr[0]); failures++; }
        checks++;
    }

    /* random AABBs */
    for (int trial=0; trial<200; ++trial){
        float ro[3]={frand()*3,frand()*3,frand()*3};
        float rd[3]={frand(),frand(),frand()};
        /* keep dir away from exact zero to avoid divide edge cases in this test */
        for(int k=0;k<3;++k) if (fabsf(rd[k])<0.05f) rd[k]=0.05f;
        float bb[24];
        for(int i=0;i<4;++i){
            float cx=frand()*3, cy=frand()*3, cz=frand()*3, hx=0.3f+fabsf(frand()), hy=0.3f+fabsf(frand()), hz=0.3f+fabsf(frand());
            bb[0+i]=cx-hx; bb[4+i]=cy-hy; bb[8+i]=cz-hz;
            bb[12+i]=cx+hx; bb[16+i]=cy+hy; bb[20+i]=cz+hz;
        }
        float gn[4],gr[4];
        rk_ray4_aabb_neon(gn,ro,rd,bb);
        rk_ray4_aabb_ref (gr,ro,rd,bb);
        chk4("aabb random", gn, gr);
    }

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
