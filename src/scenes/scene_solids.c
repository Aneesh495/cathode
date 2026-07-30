/* ==========================================================================
 * scene_solids.c — rotating Phong-shaded procedural solids via the rasterizer.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/raster.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    Mesh *cube,*ico,*torus,*sphere;
    i32 w,h; f32 t; int wireframe; int show; /* which subset */
} SolidState;

static void so_init(Scene*sc,i32 w,i32 h){
    SolidState*s=sc->state; s->w=w;s->h=h;s->t=0;s->wireframe=0;s->show=0;
    s->cube=mesh_cube(0.9f);
    s->ico=mesh_icosphere(1.1f,3);
    s->torus=mesh_torus(0.9f,0.34f,36,24);
    s->sphere=mesh_sphere(1.0f,24,32);
}
static void so_update(Scene*sc,f32 dt,f32 t){ (void)dt; ((SolidState*)sc->state)->t=t; }

static void draw(Framebuffer*fb, RasterCtx*ctx, Mesh*m, Vec3 pos, f32 t, f32 spin){
    ctx->model = mat4_mul(mat4_translate(pos),
                          mat4_mul(mat4_rotate_y(t*spin), mat4_rotate_x(t*spin*0.6f)));
    raster_mesh(fb,m,ctx);
}

static void so_render(Scene*sc, Framebuffer*fb){
    SolidState*s=sc->state;
    fb_clear(fb, col3(0.02f,0.02f,0.05f));
    fb_clear_depth(fb, 1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    ctx.view=mat4_look_at(v3(0,1.2f,6.5f), v3(0,0,0), v3(0,1,0));
    ctx.proj=mat4_perspective(52.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    /* moving light */
    ctx.light_dir=v3_norm(v3(cosf(s->t*0.8f), 0.7f, sinf(s->t*0.8f)));
    ctx.light_color=col3(1.0f,0.95f,0.85f);
    ctx.ambient=col3(0.12f,0.12f,0.18f);
    ctx.cam_pos=v3(0,1.2f,6.5f);
    ctx.specular=0.8f; ctx.shininess=48.0f; ctx.wireframe=s->wireframe;

    if (s->show==0){
        draw(fb,&ctx,s->ico,  v3(-2.4f,0,0), s->t, 1.0f);
        draw(fb,&ctx,s->torus,v3(0,0,0),     s->t, 0.8f);
        draw(fb,&ctx,s->cube, v3(2.4f,0,0),  s->t, 1.2f);
    } else {
        Mesh*picks[4]={s->cube,s->ico,s->torus,s->sphere};
        draw(fb,&ctx,picks[s->show%4], v3(0,0,0), s->t, 1.0f);
    }
}
static void so_key(Scene*sc,int key){
    SolidState*s=sc->state;
    if(key==KEY_ENTER) s->wireframe=!s->wireframe;
    else if(key==KEY_TAB) s->show=(s->show+1)%5;
}
static CrtConfig so_crt(Scene*sc){(void)sc;return crt_config_preset("arcade");}
static void so_destroy(Scene*sc){
    if(sc){ SolidState*s=sc->state;
        mesh_free(s->cube);mesh_free(s->ico);mesh_free(s->torus);mesh_free(s->sphere);
        free(s); free(sc);
    }
}

Scene *scene_solids_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="solids"; sc->description="Phong-shaded procedural solids via the CPU rasterizer";
    sc->state=calloc(1,sizeof(SolidState));
    sc->init=so_init;sc->update=so_update;sc->render=so_render;sc->on_key=so_key;
    sc->destroy=so_destroy;sc->preferred_crt=so_crt;
    return sc;
}
