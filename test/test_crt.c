/* test_crt.c  -  functional tests for the NTSC/CRT signal chain. */
#include "cathode/crt.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <math.h>

static int failures=0, checks=0;
static void ok(const char*name, int cond){ checks++; if(cond) printf("  ok   %s\n",name); else {printf("  FAIL %s\n",name); failures++;} }

static int all_finite(const Framebuffer*fb){
    for(int i=0;i<fb->w*fb->h*3;++i){ float v=fb->px[i]; if(!(v==v) || v>1e30f || v<-1e30f) return 0; }
    return 1;
}
static int all_nonneg(const Framebuffer*fb){
    for(int i=0;i<fb->w*fb->h*3;++i) if(fb->px[i]<0.0f) return 0;
    return 1;
}
static float frame_energy(const Framebuffer*fb){
    float s=0; for(int i=0;i<fb->w*fb->h*3;++i) s+=fb->px[i]; return s;
}

int main(void){
    printf("== CATHODE CRT signal chain ==\n");
    int W=64,H=48;
    CrtConfig cfg = crt_config_default();
    CrtState *crt = crt_create(W,H,&cfg);
    Framebuffer *src=fb_create(W,H), *dst=fb_create(W,H);

    /* (a) mid-gray + bright block -> finite, nonneg, differs from src */
    fb_clear(src, col3(0.5f,0.5f,0.5f));
    for(int y=10;y<20;++y)for(int x=10;x<25;++x) fb_set(src,x,y,col3(1.5f,1.4f,1.3f));
    crt_process(crt, src, dst);
    ok("output finite", all_finite(dst));
    ok("output non-negative", all_nonneg(dst));
    float diff=0; for(int i=0;i<W*H*3;++i) diff+=fabsf(dst->px[i]-src->px[i]);
    ok("CRT modifies the image", diff > 1.0f);

    /* (b) persistence: bright frame then black -> residual trail energy>0 */
    CrtConfig pcfg = crt_config_default(); pcfg.persistence=0.7f; pcfg.bloom=0.0f; pcfg.noise=0.0f;
    CrtState *crt2 = crt_create(W,H,&pcfg);
    Framebuffer *b1=fb_create(W,H), *o1=fb_create(W,H);
    fb_clear(b1, col3(1.0f,1.0f,1.0f));
    crt_process(crt2, b1, o1);         /* bright */
    fb_clear(b1, col3(0.0f,0.0f,0.0f));
    crt_process(crt2, b1, o1);         /* now black input */
    ok("phosphor leaves a trail", frame_energy(o1) > 1.0f);

    /* (c) vignette+barrel: uniform white -> corner darker than center */
    CrtConfig vcfg = crt_config_default(); vcfg.vignette=0.5f; vcfg.noise=0.0f; vcfg.bloom=0.0f;
    CrtState *crt3 = crt_create(W,H,&vcfg);
    Framebuffer *w1=fb_create(W,H), *ow=fb_create(W,H);
    fb_clear(w1, col3(0.8f,0.8f,0.8f));
    crt_process(crt3, w1, ow);
    float center = ow->px[((H/2)*W + W/2)*3];
    float corner = ow->px[(2*W + 2)*3];   /* near top-left */
    ok("vignette darkens corners", corner < center);

    /* (d) presets differ */
    CrtConfig a=crt_config_preset("trinitron"), b=crt_config_preset("vhs");
    ok("presets differ", a.chroma_bleed != b.chroma_bleed || a.persistence != b.persistence);

    /* (e) chroma bleed: a vertical color edge should smear horizontally.
       Put a red left half / black right half; after CRT, some red should
       bleed past the boundary column. */
    CrtConfig ccfg = crt_config_default(); ccfg.chroma_bleed=0.9f; ccfg.noise=0.0f; ccfg.bloom=0.0f; ccfg.barrel=0.0f;
    CrtState *crt4=crt_create(W,H,&ccfg);
    Framebuffer *ce=fb_create(W,H), *co=fb_create(W,H);
    fb_clear(ce, col3(0,0,0));
    for(int y=0;y<H;++y)for(int x=0;x<W/2;++x) fb_set(ce,x,y,col3(0.9f,0.0f,0.0f));
    crt_process(crt4, ce, co);
    /* Red decays over ~2-3 px past the boundary; sample the immediate bleed
     * zone (x = W/2 .. W/2+3) where the smear is measurable. */
    float past=0; for(int y=0;y<H;++y) for(int x=W/2;x<W/2+4;++x) past+=co->px[(y*W+x)*3+0];
    ok("chroma bleeds across edge", past > 0.5f);

    fb_destroy(src);fb_destroy(dst);fb_destroy(b1);fb_destroy(o1);
    fb_destroy(w1);fb_destroy(ow);fb_destroy(ce);fb_destroy(co);
    crt_destroy(crt);crt_destroy(crt2);crt_destroy(crt3);crt_destroy(crt4);

    printf("\n%d checks, %d failures\n", checks, failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
