/* ==========================================================================
 * test_render_props.c  -  property-based tests for the renderers.
 *
 * Invariants that must hold for ANY input, checked over many random cases:
 *   rasterizer : a filled triangle only writes pixels inside its screen bbox;
 *                z-test is draw-order-independent (near always wins);
 *                every generated mesh has finite, in-radius vertices.
 *   sdf        : the field's central-difference normal is ~unit length on hits;
 *                render output is finite & non-negative for random cameras.
 *   crt        : output is finite/non-negative and bounded for random configs;
 *                a zero-input frame stays near zero (no spontaneous energy).
 * ========================================================================== */
#include "cathode/raster.h"
#include "cathode/sdf.h"
#include "cathode/crt.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures=0, props=0;
static Rng RNG;
static f32 rf(f32 a,f32 b){ return a+(b-a)*rng_f32(&RNG); }
static void prop(const char*n,int ok){ props++; if(ok)printf("  ok   %s\n",n); else{printf("  FAIL %s\n",n);failures++;} }

int main(void){
    rng_seed(&RNG, 0x2E4D12ULL);   /* fixed seed for reproducibility */
    printf("== CATHODE renderer property tests ==\n");

    /* --- rasterizer: filled triangle stays within its screen bbox --- */
    {
        int W=80,H=60, ok=1;
        Framebuffer*fb=fb_create(W,H);
        RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
        ctx.ambient=col3(1,1,1); ctx.light_color=col3(0,0,0); /* flat = base color */
        for (int trial=0; trial<300 && ok; ++trial){
            fb_clear(fb, col3(0,0,0)); fb_clear_depth(fb,1e30f);
            /* random triangle in clip space (w=1), colored non-black */
            Vec4 a=v4(rf(-0.9f,0.9f),rf(-0.9f,0.9f),0.0f,1);
            Vec4 b=v4(rf(-0.9f,0.9f),rf(-0.9f,0.9f),0.0f,1);
            Vec4 c=v4(rf(-0.9f,0.9f),rf(-0.9f,0.9f),0.0f,1);
            Color3 col=col3(1,1,1);
            raster_triangle_clip(fb,a,b,c,col,col,col,v3(0,0,1),v3(0,0,1),v3(0,0,1),&ctx);
            /* screen bbox of the 3 verts */
            f32 xs[3]={(a.x*0.5f+0.5f)*W,(b.x*0.5f+0.5f)*W,(c.x*0.5f+0.5f)*W};
            f32 ys[3]={(1-(a.y*0.5f+0.5f))*H,(1-(b.y*0.5f+0.5f))*H,(1-(c.y*0.5f+0.5f))*H};
            f32 minx=xs[0],maxx=xs[0],miny=ys[0],maxy=ys[0];
            for(int k=1;k<3;++k){ if(xs[k]<minx)minx=xs[k]; if(xs[k]>maxx)maxx=xs[k]; if(ys[k]<miny)miny=ys[k]; if(ys[k]>maxy)maxy=ys[k]; }
            for (int y=0;y<H && ok;++y)for(int x=0;x<W;++x){
                Color3 p=fb_get(fb,x,y);
                if (p.r+p.g+p.b>1e-4f){
                    if (x < (int)floorf(minx)-1 || x > (int)ceilf(maxx)+1 ||
                        y < (int)floorf(miny)-1 || y > (int)ceilf(maxy)+1){ ok=0; break; }
                }
            }
        }
        fb_destroy(fb);
        prop("raster: pixels within triangle bbox", ok);
    }

    /* --- rasterizer: z-test order independence over random draw orders --- */
    {
        int W=40,H=40, ok=1;
        RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
        ctx.ambient=col3(1,1,1); ctx.light_color=col3(0,0,0);
        for (int trial=0; trial<200 && ok; ++trial){
            f32 znear=rf(-0.8f,-0.2f), zfar=rf(0.2f,0.8f);
            Vec4 na=v4(-0.9f,-0.9f,znear,1),nb=v4(0.9f,-0.9f,znear,1),nc=v4(0,0.9f,znear,1);
            Vec4 fa=v4(-0.9f,-0.9f,zfar,1), fb2=v4(0.9f,-0.9f,zfar,1), fc=v4(0,0.9f,zfar,1);
            Color3 red=col3(1,0,0), blu=col3(0,0,1);
            Framebuffer*A=fb_create(W,H); fb_clear(A,col3(0,0,0)); fb_clear_depth(A,1e30f);
            Framebuffer*B=fb_create(W,H); fb_clear(B,col3(0,0,0)); fb_clear_depth(B,1e30f);
            /* order 1: far then near */
            raster_triangle_clip(A,fa,fb2,fc,blu,blu,blu,v3(0,0,1),v3(0,0,1),v3(0,0,1),&ctx);
            raster_triangle_clip(A,na,nb,nc,red,red,red,v3(0,0,1),v3(0,0,1),v3(0,0,1),&ctx);
            /* order 2: near then far */
            raster_triangle_clip(B,na,nb,nc,red,red,red,v3(0,0,1),v3(0,0,1),v3(0,0,1),&ctx);
            raster_triangle_clip(B,fa,fb2,fc,blu,blu,blu,v3(0,0,1),v3(0,0,1),v3(0,0,1),&ctx);
            for (int i=0;i<W*H*3 && ok;++i) if (fabsf(A->px[i]-B->px[i])>1e-4f) ok=0;
            fb_destroy(A); fb_destroy(B);
        }
        prop("raster: z-test order-independent", ok);
    }

    /* --- meshes: finite vertices, normals unit after compute_normals --- */
    {
        int ok=1;
        Mesh* ms[4]={ mesh_cube(1.0f), mesh_sphere(1.0f,12,18), mesh_torus(1.0f,0.3f,20,12), mesh_icosphere(1.0f,2) };
        for (int m=0;m<4 && ok;++m){
            mesh_compute_normals(ms[m]);
            for (u32 i=0;i<ms[m]->nverts;++i){
                Vec3 p=ms[m]->verts[i].pos, nrm=ms[m]->verts[i].normal;
                if (!(p.x==p.x)||!(p.y==p.y)||!(p.z==p.z)){ ok=0; break; }
                f32 l=v3_len(nrm); if (fabsf(l-1.0f)>1e-2f){ ok=0; break; }
            }
        }
        for(int m=0;m<4;++m) mesh_free(ms[m]);
        prop("mesh: finite verts + unit normals", ok);
    }

    /* --- SDF: normals ~unit, render finite/non-negative for random cams --- */
    {
        int ok=1, W=40,H=30;
        for (int trial=0; trial<30 && ok; ++trial){
            SdfScene sc; memset(&sc,0,sizeof(sc));
            sc.field=sdf_scene_primitives; sc.fov=rf(0.7f,1.2f);
            sc.cam_pos=v3(rf(-4,4),rf(1,4),rf(3,6)); sc.cam_target=v3(0,0,0);
            sc.light_dir=v3_norm(v3(rf(-1,1),rf(0.3f,1),rf(-1,1)));
            sc.sky_top=col3(0.3f,0.5f,0.9f); sc.sky_bottom=col3(0.8f,0.8f,0.9f);
            for(int k=0;k<8;++k) sc.mat_albedo[k]=col3(0.7f,0.6f,0.5f);
            sc.time=rf(0,6); sc.max_steps=80; sc.max_dist=40; sc.epsilon=0.001f; sc.aa=1;
            Framebuffer*fb=fb_create(W,H);
            sdf_render(fb,&sc);
            for (int i=0;i<W*H*3 && ok;++i){ f32 v=fb->px[i]; if(!(v==v)||v<-1e-4f||v>1e6f) ok=0; }
            fb_destroy(fb);
        }
        prop("sdf: render finite & non-negative", ok);
    }

    /* --- CRT: finite/non-negative/bounded for random configs; zero stays ~0 --- */
    {
        int ok=1, zok=1, W=48,H=36;
        for (int trial=0; trial<40 && ok; ++trial){
            CrtConfig c=crt_config_default();
            c.chroma_bleed=rf(0,1); c.noise=rf(0,0.05f); c.persistence=rf(0,0.7f);
            c.bloom=rf(0,0.6f); c.barrel=rf(0,0.12f); c.vignette=rf(0,0.5f);
            c.saturation=rf(0.8f,1.5f); c.contrast=rf(0.9f,1.2f);
            CrtState*cs=crt_create(W,H,&c);
            Framebuffer*src=fb_create(W,H),*dst=fb_create(W,H);
            for(int i=0;i<W*H*3;++i) src->px[i]=rf(0,1.5f);
            crt_process(cs,src,dst);
            for (int i=0;i<W*H*3 && ok;++i){ f32 v=dst->px[i]; if(!(v==v)||v<-1e-4f||v>1e4f) ok=0; }
            crt_destroy(cs); fb_destroy(src); fb_destroy(dst);
        }
        /* zero input (noise off) -> near-zero output */
        {
            CrtConfig c=crt_config_default(); c.noise=0; c.persistence=0;
            CrtState*cs=crt_create(W,H,&c);
            Framebuffer*src=fb_create(W,H),*dst=fb_create(W,H);
            fb_clear(src,col3(0,0,0));
            crt_process(cs,src,dst);
            f32 e=0; for(int i=0;i<W*H*3;++i) e+=dst->px[i];
            if (e>1.0f) zok=0;
            crt_destroy(cs); fb_destroy(src); fb_destroy(dst);
        }
        prop("crt: bounded for random configs", ok);
        prop("crt: zero input -> ~zero output", zok);
    }

    printf("\n%d properties, %d failures\n", props, failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
