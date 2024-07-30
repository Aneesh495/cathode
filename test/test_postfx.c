/* test_postfx.c — proves the NEON post-FX kernels equal the C reference. */
#include "app/postfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0, checks = 0;

static int close(float a, float b, float eps) {
    float d = fabsf(a - b), m = fmaxf(fabsf(a), fabsf(b));
    return d <= eps * (1.0f + m);
}
static void chk_arr(const char *name, unsigned long n, const float *g, const float *e, float eps) {
    checks++;
    for (unsigned long i = 0; i < n; ++i)
        if (!close(g[i], e[i], eps)) {
            printf("  FAIL %-22s [%lu]: got %.7g exp %.7g\n", name, i, g[i], e[i]);
            failures++; return;
        }
    printf("  ok   %-22s (n=%lu)\n", name, n);
}
static void chk_scalar(const char *name, float g, float e, float eps) {
    checks++;
    if (!close(g, e, eps)) { printf("  FAIL %-22s: got %.7g exp %.7g\n", name, g, e); failures++; }
    else printf("  ok   %-22s (%.7g)\n", name, g);
}
static float frand(void){ return (float)rand()/(float)RAND_MAX*2.0f-1.0f; }

int main(void) {
    srand(1234);
    printf("== CATHODE post-FX NEON vs C reference ==\n");

    /* test several n including non-multiples of 4 and tiny sizes */
    unsigned long sizes[] = {1,2,3,4,5,7,8,15,16,17,63,64,127,257,1000};
    for (unsigned si = 0; si < sizeof(sizes)/sizeof(sizes[0]); ++si) {
        unsigned long n = sizes[si];
        float *a1=malloc(n*4), *a2=malloc(n*4), *b=malloc(n*4), *o1=malloc(n*4), *o2=malloc(n*4);
        for (unsigned long i=0;i<n;++i){ float s=frand(); a1[i]=a2[i]=s; b[i]=frand(); }

        /* accumulate */
        float scale=0.37f;
        postfx_accumulate_neon(a1,b,scale,n);
        postfx_accumulate_ref (a2,b,scale,n);
        chk_arr("accumulate",n,a1,a2,1e-5f);

        /* screen (needs [0,1]-ish inputs) */
        for(unsigned long i=0;i<n;++i){ a1[i]=a2[i]=(frand()*0.5f+0.5f); b[i]=frand()*0.5f+0.5f; }
        postfx_screen_neon(a1,b,n);
        postfx_screen_ref (a2,b,n);
        chk_arr("screen",n,a1,a2,1e-5f);

        /* brightpass */
        for(unsigned long i=0;i<n;++i) b[i]=frand()*3.0f;
        postfx_brightpass_neon(o1,b,0.8f,1.5f,n);
        postfx_brightpass_ref (o2,b,0.8f,1.5f,n);
        chk_arr("brightpass",n,o1,o2,1e-5f);

        /* blur5 */
        for(unsigned long i=0;i<n;++i) b[i]=frand()*10.0f;
        postfx_blur5_neon(o1,b,n);
        postfx_blur5_ref (o2,b,n);
        chk_arr("blur5",n,o1,o2,1e-4f);

        /* sum + max */
        for(unsigned long i=0;i<n;++i) b[i]=frand()*5.0f;
        chk_scalar("sum",postfx_sum_neon(b,n),postfx_sum_ref(b,n), 1e-4f);
        chk_scalar("max",postfx_max_neon(b,n),postfx_max_ref(b,n), 1e-6f);

        free(a1);free(a2);free(b);free(o1);free(o2);
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    if (!failures) printf("ALL PASS\n");
    return failures?1:0;
}
