/* test_blur.c  -  NEON separable blur vs C reference + blur properties. */
#include "cathode/blur.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static float frand(void){ return (float)rand()/(float)RAND_MAX; }

static void chk(const char*name, const float*g, const float*e, int n, float tol){
    checks++;
    float worst=0; int wi=0;
    for (int i=0;i<n;++i){ float d=fabsf(g[i]-e[i]); if(d>worst){worst=d;wi=i;} }
    if (worst>tol){ printf("  FAIL %-22s worst %.4g at %d (got %.5f exp %.5f)\n",name,worst,wi,g[wi],e[wi]); failures++; }
    else printf("  ok   %-22s (worst %.2e)\n",name,worst);
}

int main(void){
    srand(55);
    printf("== CATHODE separable blur NEON vs C reference ==\n");
    int sizes[][2]={{8,6},{16,16},{17,13},{64,48},{100,1},{1,100},{129,97}};
    for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        int w=sizes[si][0], h=sizes[si][1];
        float *src=malloc((size_t)w*h*4),*gn=malloc((size_t)w*h*4),*gr=malloc((size_t)w*h*4);
        for (int i=0;i<w*h;++i) src[i]=frand()*4.0f-2.0f;
        for (int r=1;r<=6;++r){
            float ker[16]; blur_gaussian_kernel(ker,r,0.0f);
            blur_h_neon(gn,src,w,h,ker,r); blur_h_ref(gr,src,w,h,ker,r);
            chk("blur_h", gn,gr,w*h,1e-4f);
            blur_v_neon(gn,src,w,h,ker,r); blur_v_ref(gr,src,w,h,ker,r);
            chk("blur_v", gn,gr,w*h,1e-4f);
        }
        free(src);free(gn);free(gr);
    }

    /* property: blurring a constant field yields the same constant (kernel
     * normalized => DC gain 1). */
    {
        int w=40,h=30; float *src=malloc((size_t)w*h*4),*dst=malloc((size_t)w*h*4);
        for(int i=0;i<w*h;++i) src[i]=3.14159f;
        float ker[16]; int r=blur_gaussian_kernel(ker,5,0.0f);
        blur_h_neon(dst,src,w,h,ker,r);
        float worst=0; for(int i=0;i<w*h;++i){float d=fabsf(dst[i]-3.14159f); if(d>worst)worst=d;}
        checks++; if(worst>1e-4f){printf("  FAIL blur preserves DC (worst %.4g)\n",worst);failures++;} else printf("  ok   blur preserves DC       (worst %.2e)\n",worst);
        /* property: blur is non-negative-preserving and bounded by input range */
        for(int i=0;i<w*h;++i) src[i]=frand();
        blur_v_neon(dst,src,w,h,ker,r);
        int ok=1; for(int i=0;i<w*h;++i) if(dst[i]<-1e-5f||dst[i]>1.0001f) ok=0;
        checks++; if(!ok){printf("  FAIL blur stays in [0,1]\n");failures++;} else printf("  ok   blur stays in input range\n");
        free(src);free(dst);
    }

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
