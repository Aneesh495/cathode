/* ==========================================================================
 * bench_all.c  -  throughput benchmarks for NEON kernels vs C references,
 * sustained composite DSP throughput, and CRT frame render timing across
 * resolutions.
 * ========================================================================== */
#include "cathode/simd.h"
#include "cathode/dsp.h"
#include "cathode/fastmath.h"
#include "cathode/raykernel.h"
#include "cathode/blur.h"
#include "cathode/fractalkernel.h"
#include "cathode/crt.h"
#include "cathode/framebuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

#define TIME(reps, stmt) ({ double _t0=now(); for(long _i=0;_i<(reps);++_i){ stmt; } now()-_t0; })

static void line(const char*name, double neon_s, double ref_s, double ops){
    printf("  %-24s NEON %8.1f Mops/s | C %8.1f Mops/s | %5.2fx\n",
        name, ops/neon_s/1e6, ops/ref_s/1e6, ref_s/neon_s);
}

int main(void){
    printf("== CATHODE NEON kernel microbenchmarks (Apple Silicon) ==\n");
    volatile float sink=0;

    /* mat4_mul */
    { float a[16],b[16],o[16]; for(int i=0;i<16;++i){a[i]=i*0.1f;b[i]=(16-i)*0.07f;}
      long R=20000000;
      double tn=TIME(R, mat4_mul_neon(o,a,b)); sink+=o[0];
      double tr=TIME(R, mat4_mul_ref(o,a,b));  sink+=o[0];
      line("mat4_mul", tn,tr,(double)R); }

    /* dsp rgb2yiq */
    { unsigned long P=4096; float*rgb=malloc(P*3*4),*out=malloc(P*3*4);
      for(unsigned long i=0;i<P*3;++i) rgb[i]=(i%255)/255.0f;
      long R=100000;
      double tn=TIME(R, dsp_rgb2yiq_neon(out,rgb,P)); sink+=out[0];
      double tr=TIME(R, dsp_rgb2yiq_ref(out,rgb,P));  sink+=out[0];
      line("dsp_rgb2yiq", tn,tr,(double)R*P); free(rgb);free(out); }

    /* fastmath sin / exp */
    { unsigned long P=1<<16; float*in=malloc(P*4),*out=malloc(P*4);
      for(unsigned long i=0;i<P;++i) in[i]=(i%6283)*0.001f-3.14f;
      long R=200;
      double tn=TIME(R, fm_sin4_neon(out,in,P)); sink+=out[0];
      double tr=TIME(R, fm_sin4_ref(out,in,P));  sink+=out[0];
      line("fm_sin4", tn,tr,(double)R*P);
      tn=TIME(R, fm_exp4_neon(out,in,P)); sink+=out[0];
      tr=TIME(R, fm_exp4_ref(out,in,P));  sink+=out[0];
      line("fm_exp4", tn,tr,(double)R*P);
      /* log needs positive inputs */
      for(unsigned long i=0;i<P;++i) in[i]=0.01f+(i%9973)*0.1f;
      tn=TIME(R, fm_log4_neon(out,in,P)); sink+=out[0];
      tr=TIME(R, fm_log4_ref(out,in,P));  sink+=out[0];
      line("fm_log4", tn,tr,(double)R*P); free(in);free(out); }

    /* raykernel: ray vs 4 spheres */
    { float ro[3]={0,0,-5},rd[3]={0,0,1}; float sph[16];
      for(int i=0;i<4;++i){sph[i]=i-1.5f;sph[4+i]=0;sph[8+i]=0;sph[12+i]=1;}
      float out[4]; long R=5000000;
      double tn=TIME(R, rk_ray4_spheres_neon(out,ro,rd,sph)); sink+=out[0];
      double tr=TIME(R, rk_ray4_spheres_ref(out,ro,rd,sph));  sink+=out[0];
      line("rk_ray4_spheres", tn,tr,(double)R*4); }

    /* blur: horizontal pass of a 256x256 image, r=4 */
    { int W=256,Hh=256; float*src=malloc((size_t)W*Hh*4),*dst=malloc((size_t)W*Hh*4);
      for(int i=0;i<W*Hh;++i) src[i]=(i%255)/255.0f;
      float ker[16]; int r=blur_gaussian_kernel(ker,4,0.0f);
      long R=2000;
      double tn=TIME(R, blur_v_neon(dst,src,W,Hh,ker,r)); sink+=dst[0];
      double tr=TIME(R, blur_v_ref(dst,src,W,Hh,ker,r));  sink+=dst[0];
      line("blur_v (256x256,r4)", tn,tr,(double)R*W*Hh); free(src);free(dst); }

    /* fractalkernel: Mandelbrot 4-lane, 256 iters */
    { float cre[4]={-0.5f,0.2f,-1.0f,0.3f},cim[4]={0.0f,0.3f,0.1f,0.5f},out[4];
      long R=500000;
      double tn=TIME(R, fk_mandel4_neon(out,cre,cim,256,256.0f)); sink+=out[0];
      double tr=TIME(R, fk_mandel4_ref(out,cre,cim,256,256.0f));  sink+=out[0];
      line("fk_mandel4 (256it)", tn,tr,(double)R*4); }

    /* Composite I/Q DSP path throughput (modulate + demodulate) */
    printf("\n== CATHODE NTSC composite DSP throughput ==\n");
    {
        unsigned long W = 1920;
        float *rgb = malloc(W*3*sizeof(float));
        float *yiq = malloc(W*3*sizeof(float));
        float *comp = malloc(W*sizeof(float));
        float *tmp = malloc(W*sizeof(float));
        float *cosph = malloc(W*sizeof(float));
        float *sinph = malloc(W*sizeof(float));
        float *dY = malloc(W*sizeof(float));
        float *dI = malloc(W*sizeof(float));
        float *dQ = malloc(W*sizeof(float));
        float *out_rgb = malloc(W*3*sizeof(float));
        float lk[3] = {0.5f, 0.22f, 0.03f};
        float ck[5] = {0.3f, 0.25f, 0.15f, 0.1f, 0.05f};
        for(unsigned long i=0; i<W*3; ++i) rgb[i] = (float)(i%255)/255.0f;
        for(unsigned long i=0; i<W; ++i) { cosph[i] = 0.707f; sinph[i] = 0.707f; }

        long reps = 20000;
        double t0 = now();
        for(long r=0; r<reps; ++r) {
            dsp_rgb2yiq_neon(yiq, rgb, W);
            for(unsigned long x=0; x<W; ++x) comp[x] = yiq[3*x+0] + yiq[3*x+1]*cosph[x] + yiq[3*x+2]*sinph[x];
            dsp_fir_sym_neon(dY, comp, W, lk, 2);
            for(unsigned long x=0; x<W; ++x) tmp[x] = 2.0f * comp[x] * cosph[x];
            dsp_fir_sym_neon(dI, tmp, W, ck, 4);
            for(unsigned long x=0; x<W; ++x) tmp[x] = 2.0f * comp[x] * sinph[x];
            dsp_fir_sym_neon(dQ, tmp, W, ck, 4);
            for(unsigned long x=0; x<W; ++x) { yiq[3*x+0] = dY[x]; yiq[3*x+1] = dI[x]; yiq[3*x+2] = dQ[x]; }
            dsp_yiq2rgb_neon(out_rgb, yiq, W);
        }
        double elapsed = now() - t0;
        double samples = (double)W * reps;
        double msps = (samples / elapsed) / 1e6;
        printf("  Composite I/Q encode+decode: %8.1f MS/s (gate: >=114 MS/s) -> %s\n",
               msps, msps >= 114.0 ? "PASS" : "FAIL");
        sink += out_rgb[0];
        free(rgb); free(yiq); free(comp); free(tmp); free(cosph); free(sinph);
        free(dY); free(dI); free(dQ); free(out_rgb);
    }

    /* CRT signal chain frame timing across resolutions */
    printf("\n== CATHODE CRT frame timing (single-threaded CPU) ==\n");
    {
        int res[][2] = { {96,72}, {320,240}, {640,480}, {1280,720}, {2560,1440} };
        const char *labels[] = { "96x72   (baseline)", "320x240 (retro capture)", "640x480 (VGA)", "1280x720 (720p)", "2560x1440 (1440p)" };
        for(int i=0; i<5; ++i) {
            int w = res[i][0], h = res[i][1];
            CrtConfig cfg = crt_config_default();
            CrtState *crt = crt_create(w, h, &cfg);
            Framebuffer *sfb = fb_create(w, h);
            Framebuffer *dfb = fb_create(w, h);
            for(int k=0; k<w*h*3; ++k) sfb->px[k] = 0.5f;
            crt_process(crt, sfb, dfb); // warmup
            int iters = (i >= 4) ? 2 : (i >= 3 ? 5 : 20);
            double t0 = now();
            for(int it=0; it<iters; ++it) crt_process(crt, sfb, dfb);
            double ms = (now() - t0) / iters * 1000.0;
            printf("  %-24s: %6.2f ms/frame (%7d px)\n", labels[i], ms, w*h);
            sink += dfb->px[0];
            crt_destroy(crt); fb_destroy(sfb); fb_destroy(dfb);
        }
    }

    printf("(sink=%g)\n",(double)sink);
    return 0;
}
