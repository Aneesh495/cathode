/* test_fractalkernel.c  -  NEON escape-time fractal kernels vs C reference. */
#include "cathode/fractalkernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static float frand(void){ return (float)rand()/(float)RAND_MAX*2.0f-1.0f; }

static void chk4(const char*name, const float*g, const float*e, int max_iter){
    checks++;
    for (int i=0;i<4;++i){
        /* interior points (== max_iter) must match exactly; escaped points
         * match within a small tolerance (the smooth log). */
        int gi = g[i]>=max_iter-0.5f, ei = e[i]>=max_iter-0.5f;
        if (gi != ei){ printf("  FAIL %-14s lane %d interior mismatch (got %.3f exp %.3f)\n",name,i,g[i],e[i]); failures++; return; }
        if (!ei){
            float d=fabsf(g[i]-e[i]);
            if (d > 2e-3f){ printf("  FAIL %-14s lane %d got %.5f exp %.5f (d=%.4g)\n",name,i,g[i],e[i],d); failures++; return; }
        }
    }
    printf("  ok   %-14s\n", name);
}

int main(void){
    srand(123);
    printf("== CATHODE fractalkernel NEON vs C reference ==\n");
    int MI=200; float B=256.0f;

    /* known: c=0 stays bounded (interior); c=1 escapes fast */
    {
        float cre[4]={0.0f, 1.0f, -1.0f, -0.75f};
        float cim[4]={0.0f, 0.0f,  0.0f,  0.1f};
        float gn[4],gr[4];
        fk_mandel4_neon(gn,cre,cim,MI,B);
        fk_mandel4_ref (gr,cre,cim,MI,B);
        chk4("mandel known", gn, gr, MI);
        if (gr[0] < MI-0.5f){ printf("  FAIL c=0 should be interior, got %.3f\n", gr[0]); failures++; }
        checks++;
    }

    /* random points across the interesting region */
    for (int t=0;t<300;++t){
        float cre[4],cim[4],gn[4],gr[4];
        for(int i=0;i<4;++i){ cre[i]=frand()*2.0f-0.5f; cim[i]=frand()*1.5f; }
        fk_mandel4_neon(gn,cre,cim,MI,B);
        fk_mandel4_ref (gr,cre,cim,MI,B);
        chk4("mandel random", gn, gr, MI);
    }

    /* Julia sets with a few classic constants */
    float jc[][2]={{-0.4f,0.6f},{0.285f,0.01f},{-0.8f,0.156f},{-0.70176f,-0.3842f}};
    for (int k=0;k<4;++k){
        for (int t=0;t<80;++t){
            float zr[4],zi[4],gn[4],gr[4];
            for(int i=0;i<4;++i){ zr[i]=frand()*1.6f; zi[i]=frand()*1.6f; }
            fk_julia4_neon(gn,zr,zi,jc[k][0],jc[k][1],MI,B);
            fk_julia4_ref (gr,zr,zi,jc[k][0],jc[k][1],MI,B);
            chk4("julia random", gn, gr, MI);
        }
    }

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
