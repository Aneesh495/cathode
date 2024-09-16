/* test_fastmath.c  -  NEON transcendental approximations vs libm reference. */
#include "cathode/fastmath.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;

static void chk_abs(const char*name, const float*g, const float*e, unsigned long n, float tol){
    checks++;
    float worst=0; unsigned long wi=0;
    for(unsigned long i=0;i<n;++i){ float d=fabsf(g[i]-e[i]); if(d>worst){worst=d;wi=i;} }
    if(worst>tol){ printf("  FAIL %-16s worst abs err=%.3g at [%lu] (got %.5f exp %.5f)\n",name,worst,wi,g[wi],e[wi]); failures++; }
    else printf("  ok   %-16s worst abs err=%.3g\n",name,worst);
}
static void chk_rel(const char*name, const float*g, const float*e, unsigned long n, float tol){
    checks++;
    float worst=0; unsigned long wi=0;
    for(unsigned long i=0;i<n;++i){ float d=fabsf(g[i]-e[i])/(fabsf(e[i])+1e-6f); if(d>worst){worst=d;wi=i;} }
    if(worst>tol){ printf("  FAIL %-16s worst rel err=%.3g at [%lu] (got %.5f exp %.5f)\n",name,worst,wi,g[wi],e[wi]); failures++; }
    else printf("  ok   %-16s worst rel err=%.3g\n",name,worst);
}

int main(void){
    printf("== CATHODE fastmath NEON vs libm ==\n");
    unsigned long sizes[]={1,2,3,4,5,7,8,16,17,63,257,1000};
    for(unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n=sizes[si];
        float *in=malloc(n*4),*g=malloc(n*4),*e=malloc(n*4);
        /* sin/cos over [-4pi,4pi] */
        for(unsigned long i=0;i<n;++i) in[i]=(-4.0f*(float)M_PI) + (8.0f*(float)M_PI)*((float)i/(n>1?n-1:1));
        fm_sin4_neon(g,in,n); fm_sin4_ref(e,in,n); chk_abs("sin",g,e,n,1e-3f);
        fm_cos4_neon(g,in,n); fm_cos4_ref(e,in,n); chk_abs("cos",g,e,n,1e-3f);
        /* exp over [-10,10] */
        for(unsigned long i=0;i<n;++i) in[i]=-10.0f + 20.0f*((float)i/(n>1?n-1:1));
        fm_exp4_neon(g,in,n); fm_exp4_ref(e,in,n); chk_rel("exp",g,e,n,1e-3f);
        /* log over a wide positive range [1e-3, 1e6], sampled log-uniformly */
        for(unsigned long i=0;i<n;++i){
            float u=(float)i/(n>1?n-1:1);            /* [0,1] */
            in[i]=expf(-6.9077553f + u*(13.8155106f + 6.9077553f)); /* 1e-3 .. 1e6 */
        }
        fm_log4_neon(g,in,n); fm_log4_ref(e,in,n); chk_abs("log",g,e,n,1e-3f);
        free(in);free(g);free(e);
    }
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
