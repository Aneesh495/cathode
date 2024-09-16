/* test_dsp.c  -  proves the NEON DSP kernels equal the C reference. */
#include "cathode/dsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0, checks = 0;
static int close(float a, float b, float eps){ float d=fabsf(a-b),m=fmaxf(fabsf(a),fabsf(b)); return d<=eps*(1.0f+m); }
static void chk(const char*name, unsigned long n, const float*g, const float*e, float eps){
    checks++;
    for(unsigned long i=0;i<n;++i) if(!close(g[i],e[i],eps)){
        float d=fabsf(g[i]-e[i]); float mm=fmaxf(fabsf(g[i]),fabsf(e[i])); float thr=eps*(1.0f+mm);
        printf("  FAIL %-24s[%lu] got %.9g exp %.9g |d|=%.4g m=%.4g eps=%.4g thr=%.4g\n",name,i,g[i],e[i],d,mm,eps,thr); failures++; return; }
    printf("  ok   %-24s(n=%lu)\n",name,n);
}
static float frand(void){ return (float)rand()/(float)RAND_MAX; }

int main(void){
    srand(99);
    printf("== CATHODE DSP NEON vs C reference ==\n");
    unsigned long sizes[] = {1,2,3,4,5,7,8,15,16,17,64,127,256,721};
    for(unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n = sizes[si];
        float *rgb=malloc(n*3*4),*y1=malloc(n*3*4),*y2=malloc(n*3*4),*r1=malloc(n*3*4),*r2=malloc(n*3*4);
        for(unsigned long i=0;i<n*3;++i) rgb[i]=frand();
        dsp_rgb2yiq_neon(y1,rgb,n); dsp_rgb2yiq_ref(y2,rgb,n); chk("rgb2yiq",n*3,y1,y2,1e-5f);
        dsp_yiq2rgb_neon(r1,y2,n);  dsp_yiq2rgb_ref(r2,y2,n);  chk("yiq2rgb",n*3,r1,r2,1e-5f);
        /* round-trip sanity: yiq2rgb(rgb2yiq(x)) ~= x */
        dsp_yiq2rgb_ref(r2,y2,n);
        chk("yiq roundtrip",n*3,r2,rgb,2e-3f);
        free(rgb);free(y1);free(y2);free(r1);free(r2);
    }
    /* iir blend */
    for(unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n=sizes[si];
        float *d1=malloc(n*4),*d2=malloc(n*4),*s=malloc(n*4);
        for(unsigned long i=0;i<n;++i){ float v=frand(); d1[i]=d2[i]=v; s[i]=frand(); }
        dsp_iir_blend_neon(d1,s,0.85f,n); dsp_iir_blend_ref(d2,s,0.85f,n);
        chk("iir_blend",n,d1,d2,1e-5f);
        free(d1);free(d2);free(s);
    }
    /* scale_bias_clamp */
    for(unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n=sizes[si];
        float *in=malloc(n*4),*o1=malloc(n*4),*o2=malloc(n*4);
        for(unsigned long i=0;i<n;++i) in[i]=frand()*2.0f-0.5f;
        dsp_scale_bias_clamp_neon(o1,in,1.3f,0.1f,1.0f,n);
        dsp_scale_bias_clamp_ref (o2,in,1.3f,0.1f,1.0f,n);
        chk("scale_bias_clamp",n,o1,o2,1e-6f);
        free(in);free(o1);free(o2);
    }
    /* fir_sym with several radii */
    int radii[]={1,2,3,5};
    for(unsigned ri=0; ri<sizeof(radii)/sizeof(radii[0]); ++ri){
        int r=radii[ri];
        float ker[8]; for(int k=0;k<=r;++k) ker[k]=1.0f/(1.0f+k); /* arbitrary taps */
        for(unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
            unsigned long n=sizes[si];
            float *in=malloc(n*4),*o1=malloc(n*4),*o2=malloc(n*4);
            for(unsigned long i=0;i<n;++i) in[i]=frand()*4.0f-2.0f;
            dsp_fir_sym_neon(o1,in,n,ker,r);
            dsp_fir_sym_ref (o2,in,n,ker,r);
            char nm[32]; snprintf(nm,sizeof(nm),"fir_sym r=%d",r);
            chk(nm,n,o1,o2,1e-4f);
            free(in);free(o1);free(o2);
        }
    }
    printf("\n%d checks, %d failures\n", checks, failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
