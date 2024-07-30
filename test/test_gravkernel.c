/* test_gravkernel.c — NEON gravity force kernel vs C reference. */
#include "cathode/gravkernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static float frand(void){ return (float)rand()/(float)RAND_MAX*2.0f-1.0f; }

static void chk(const char*name, const float*g, const float*e){
    checks++;
    float worst=0;
    for (int i=0;i<3;++i){ float d=fabsf(g[i]-e[i]); float rel=d/(fabsf(e[i])+1e-4f); if(rel>worst)worst=rel; }
    if (worst>2e-3f){ printf("  FAIL %-16s rel err %.4g (got %.5f,%.5f,%.5f  exp %.5f,%.5f,%.5f)\n",
        name,worst,g[0],g[1],g[2],e[0],e[1],e[2]); failures++; }
    else printf("  ok   %-16s (rel %.2e)\n",name,worst);
}

int main(void){
    srand(77);
    printf("== CATHODE gravkernel NEON vs C reference ==\n");
    unsigned long sizes[]={1,2,3,4,7,8,16,17,63,200,999};
    for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n=sizes[si];
        float *sx=malloc(n*4),*sy=malloc(n*4),*sz=malloc(n*4),*sm=malloc(n*4);
        for (unsigned long i=0;i<n;++i){ sx[i]=frand()*10; sy[i]=frand()*10; sz[i]=frand()*10; sm[i]=0.5f+fabsf(frand())*3; }
        float px=frand()*10, py=frand()*10, pz=frand()*10;
        float g=1.0f, eps2=0.05f;
        float o1[3], o2[3];
        grav_accum_neon(o1, px,py,pz, sx,sy,sz,sm, n, g, eps2);
        grav_accum_ref (o2, px,py,pz, sx,sy,sz,sm, n, g, eps2);
        chk("grav_accum", o1, o2);
        free(sx);free(sy);free(sz);free(sm);
    }
    /* known: a single unit mass at +x distance 2 pulls a body at origin toward +x */
    {
        float sx[1]={2.0f}, sy[1]={0}, sz[1]={0}, sm[1]={1.0f};
        float o1[3],o2[3];
        grav_accum_neon(o1,0,0,0,sx,sy,sz,sm,1,1.0f,0.0f);
        grav_accum_ref (o2,0,0,0,sx,sy,sz,sm,1,1.0f,0.0f);
        chk("single-body known", o1, o2);
        checks++;
        if (o1[0]>0 && fabsf(o1[1])<1e-5f && fabsf(o1[2])<1e-5f) printf("  ok   pulls toward +x (ax=%.4f)\n",o1[0]);
        else { printf("  FAIL direction ax=%.4f ay=%.4f az=%.4f\n",o1[0],o1[1],o1[2]); failures++; }
    }
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
