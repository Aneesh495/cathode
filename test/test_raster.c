/* test_raster.c  -  rasterizer + mesh generators, headless. */
#include "cathode/raster.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int failures=0, checks=0;
static void ok(const char*n,int c){ checks++; if(c) printf("  ok   %s\n",n); else {printf("  FAIL %s\n",n);failures++;} }

static int count_nonblack(Framebuffer*fb){ int n=0; for(int i=0;i<fb->w*fb->h;++i){ if(fb->px[i*3]+fb->px[i*3+1]+fb->px[i*3+2]>1e-4f) n++; } return n; }
static int depth_written(Framebuffer*fb){ int n=0; for(int i=0;i<fb->w*fb->h;++i) if(fb->depth[i]<1e29f) n++; return n; }

int main(void){
    printf("== CATHODE rasterizer ==\n");

    /* generator sanity */
    Mesh *cube=mesh_cube(1.0f);
    Mesh *sph=mesh_sphere(1.0f,16,24);
    Mesh *tor=mesh_torus(1.0f,0.4f,24,16);
    Mesh *ico=mesh_icosphere(1.0f,2);
    ok("cube verts/tris", cube->nverts==24 && cube->ntris==12);
    ok("sphere counts>0", sph->nverts>0 && sph->ntris>0);
    ok("torus counts>0", tor->nverts>0 && tor->ntris>0);
    ok("icosphere subdiv grows", ico->ntris==20*16); /* 4^2 * 20 */

    /* normals unit length */
    mesh_compute_normals(sph);
    int bad=0; for(u32 i=0;i<sph->nverts;++i){ float l=v3_len(sph->verts[i].normal); if(fabsf(l-1.0f)>1e-3f) bad++; }
    ok("sphere normals unit", bad==0);

    /* render a cube facing camera */
    int W=80,H=80;
    Framebuffer *fb=fb_create(W,H);
    fb_clear(fb, col3(0,0,0)); fb_clear_depth(fb,1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    ctx.model=mat4_mul(mat4_rotate_y(0.7f),mat4_rotate_x(0.5f));
    ctx.view=mat4_look_at(v3(0,0,4),v3(0,0,0),v3(0,1,0));
    ctx.proj=mat4_perspective(60.0f*CT_DEG2RAD,(f32)W/H,0.1f,100.0f);
    ctx.light_dir=v3_norm(v3(0.5f,0.7f,1.0f));
    ctx.light_color=col3(1,1,1); ctx.ambient=col3(0.15f,0.15f,0.2f);
    ctx.cam_pos=v3(0,0,4); ctx.specular=0.6f; ctx.shininess=32.0f; ctx.wireframe=0;
    raster_mesh(fb, cube, &ctx);
    int nb=count_nonblack(fb);
    ok("cube renders pixels", nb>50);
    ok("depth buffer written", depth_written(fb)>50);

    /* finite check */
    int fin=1; for(int i=0;i<W*H*3;++i){ float v=fb->px[i]; if(!(v==v)||v>1e30f){fin=0;break;} }
    ok("output finite", fin);

    /* occlusion: near triangle should win over far one regardless of order.
       Draw FAR blue first, then NEAR red, both covering screen center. */
    Framebuffer *f2=fb_create(W,H); fb_clear(f2,col3(0,0,0)); fb_clear_depth(f2,1e30f);
    RasterCtx c2=ctx; c2.ambient=col3(1,1,1); c2.light_color=col3(0,0,0); c2.specular=0; /* flat = base color */
    /* far blue quad at z ndc ~ +0.5 */
    Vec4 fa=v4(-0.8f,-0.8f,0.5f,1), fb2=v4(0.8f,-0.8f,0.5f,1), fc=v4(0.0f,0.8f,0.5f,1);
    raster_triangle_clip(f2, fa,fb2,fc, col3(0,0,1),col3(0,0,1),col3(0,0,1), v3(0,0,1),v3(0,0,1),v3(0,0,1),&c2);
    /* near red quad at z ndc ~ -0.5 (nearer) */
    Vec4 na=v4(-0.8f,-0.8f,-0.5f,1), nb2=v4(0.8f,-0.8f,-0.5f,1), nc=v4(0.0f,0.8f,-0.5f,1);
    raster_triangle_clip(f2, na,nb2,nc, col3(1,0,0),col3(1,0,0),col3(1,0,0), v3(0,0,1),v3(0,0,1),v3(0,0,1),&c2);
    Color3 ctr = fb_get(f2, W/2, H/2+8);
    ok("near occludes far (red wins)", ctr.r>0.5f && ctr.b<0.5f);

    /* draw order independence: reverse */
    Framebuffer *f3=fb_create(W,H); fb_clear(f3,col3(0,0,0)); fb_clear_depth(f3,1e30f);
    raster_triangle_clip(f3, na,nb2,nc, col3(1,0,0),col3(1,0,0),col3(1,0,0), v3(0,0,1),v3(0,0,1),v3(0,0,1),&c2);
    raster_triangle_clip(f3, fa,fb2,fc, col3(0,0,1),col3(0,0,1),col3(0,0,1), v3(0,0,1),v3(0,0,1),v3(0,0,1),&c2);
    Color3 ctr2 = fb_get(f3, W/2, H/2+8);
    ok("occlusion order-independent", ctr2.r>0.5f && ctr2.b<0.5f);

    mesh_free(cube);mesh_free(sph);mesh_free(tor);mesh_free(ico);
    fb_destroy(fb);fb_destroy(f2);fb_destroy(f3);

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
