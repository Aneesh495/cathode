/* test_sdf.c  -  SDF ray-marcher, headless. */
#include "cathode/sdf.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures=0, checks=0;
static void ok(const char*n,int c){ checks++; if(c) printf("  ok   %s\n",n); else {printf("  FAIL %s\n",n);failures++;} }

static SdfScene base_scene(SdfFn f){
    SdfScene sc; memset(&sc,0,sizeof(sc));
    sc.field=f; sc.user=NULL;
    sc.cam_pos=v3(0,1.5f,5); sc.cam_target=v3(0,0,0); sc.fov=1.0f;
    sc.light_dir=v3_norm(v3(0.5f,0.8f,0.4f));
    sc.sky_top=col3(0.3f,0.5f,0.9f); sc.sky_bottom=col3(0.7f,0.8f,0.95f);
    for(int i=0;i<8;++i) sc.mat_albedo[i]=col3(0.8f,0.6f,0.4f);
    sc.mat_albedo[0]=col3(0.4f,0.7f,0.4f);
    sc.time=0.5f; sc.max_steps=96; sc.max_dist=40.0f; sc.epsilon=0.001f; sc.aa=1;
    return sc;
}
static int nonuniform(Framebuffer*fb){
    Color3 c0=fb_get(fb,0,0); int diff=0;
    for(int i=1;i<fb->w*fb->h;++i){ const f32*p=&fb->px[i*3]; if(fabsf(p[0]-c0.r)+fabsf(p[1]-c0.g)+fabsf(p[2]-c0.b)>0.01f){diff=1;break;} }
    return diff;
}
static int finite_nonneg(Framebuffer*fb){
    for(int i=0;i<fb->w*fb->h*3;++i){ float v=fb->px[i]; if(!(v==v)||v<0||v>1e10f) return 0; } return 1;
}

int main(void){
    printf("== CATHODE SDF ray-marcher ==\n");
    int W=64,H=48;

    /* primitives scene */
    Framebuffer*fb=fb_create(W,H);
    SdfScene sc=base_scene(sdf_scene_primitives);
    sdf_render(fb,&sc);
    ok("primitives finite/nonneg", finite_nonneg(fb));
    ok("primitives non-uniform (surface vs sky)", nonuniform(fb));
    /* center pixel should hit something and differ from a top-corner sky pixel */
    Color3 ctr=fb_get(fb,W/2,H/2+8), corner=fb_get(fb,2,2);
    ok("center differs from sky corner",
       fabsf(ctr.r-corner.r)+fabsf(ctr.g-corner.g)+fabsf(ctr.b-corner.b) > 0.02f);

    /* mandelbulb scene produces some non-sky pixels */
    Framebuffer*fb2=fb_create(W,H);
    SdfScene sc2=base_scene(sdf_scene_mandelbulb);
    sc2.cam_pos=v3(0,0,2.6f); sc2.cam_target=v3(0,0,0);
    sdf_render(fb2,&sc2);
    ok("mandelbulb finite", finite_nonneg(fb2));
    ok("mandelbulb non-uniform", nonuniform(fb2));

    /* infinite scene */
    Framebuffer*fb3=fb_create(W,H);
    SdfScene sc3=base_scene(sdf_scene_infinite);
    sdf_render(fb3,&sc3);
    ok("infinite finite", finite_nonneg(fb3));
    ok("infinite non-uniform", nonuniform(fb3));

    /* direct field test: ray straight down should report the ground plane.
       primitives scene has plane at y=-1.2; from (0,3,0) looking down, first
       hit distance ~ 4.2 */
    SdfScene s=base_scene(sdf_scene_primitives);
    /* march manually toward -y */
    Vec3 ro=v3(3.5f,3.0f,3.5f); /* off to the side to avoid the blob */
    Vec3 rd=v3(0,-1,0);
    float t=0; int mat; int hit=0;
    for(int i=0;i<200;++i){ Vec3 p=v3_add(ro,v3_scale(rd,t)); float d=sdf_scene_primitives(p,s.time,&mat,NULL); if(d<0.001f){hit=1;break;} t+=d; if(t>40)break; }
    ok("ground plane hit near y=-1.2", hit && fabsf((3.0f - t) - (-1.2f)) < 0.05f);

    fb_destroy(fb);fb_destroy(fb2);fb_destroy(fb3);
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
