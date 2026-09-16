/* ==========================================================================
 * scene_terrain.c  -  flight over an infinite scrolling fractal landscape.
 * A heightfield generated with fbm/ridged noise scrolls under the camera;
 * rasterized with altitude/slope coloring and distance fog.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/raster.h"
#include "cathode/noise.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GRID 96

typedef struct {
    Mesh *mesh;
    f32 *h;             /* GRID*GRID heights */
    i32 w,ht; f32 t;
    f32 speed, rough, alt;
} TerrainState;

static f32 sample_height(f32 x, f32 z, f32 rough){
    /* ridged mountains + rolling fbm hills */
    f32 r = ridged2(x*0.15f, z*0.15f, 6, 2.0f, 0.5f);
    f32 hills = fbm2(x*0.06f, z*0.06f, 5, 2.0f, 0.5f);
    return (r*1.6f + hills*0.6f) * rough;
}

static void rebuild(TerrainState*s){
    /* domain offset scrolls forward with time to fake infinite flight */
    f32 off = s->t * s->speed;
    for (int j=0;j<GRID;++j)
        for (int i=0;i<GRID;++i){
            f32 wx=(f32)i*0.18f;
            f32 wz=(f32)j*0.18f + off;
            s->h[j*GRID+i]=sample_height(wx,wz,s->rough);
        }
    if (s->mesh) mesh_free(s->mesh);
    s->mesh = mesh_from_heightfield(s->h, GRID, GRID, 18.0f, 2.2f);
}

static void te_init(Scene*sc,i32 w,i32 h){
    TerrainState*s=sc->state; s->w=w;s->ht=h;s->t=0;
    s->speed=3.0f; s->rough=1.0f; s->alt=3.5f;
    s->h=malloc(sizeof(f32)*GRID*GRID);
    s->mesh=NULL;
    rebuild(s);
}
static void te_update(Scene*sc,f32 dt,f32 t){ (void)dt; TerrainState*s=sc->state; s->t=t; rebuild(s); }

static void te_render(Scene*sc, Framebuffer*fb){
    TerrainState*s=sc->state;
    Color3 sky=col3(0.5f,0.65f,0.9f);
    /* sky gradient */
    for (i32 y=0;y<fb->h;++y){
        f32 tt=(f32)y/fb->h;
        Color3 c=col_lerp(col3(0.35f,0.55f,0.95f), col3(0.8f,0.85f,0.95f), tt*0.7f);
        for (i32 x=0;x<fb->w;++x) fb_set(fb,x,y,c);
    }
    fb_clear_depth(fb,1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    /* camera flies above the terrain looking forward+down */
    Vec3 eye=v3(0, s->alt, -6.0f);
    Vec3 target=v3(0, s->alt-2.0f, 2.0f);
    ctx.view=mat4_look_at(eye,target,v3(0,1,0));
    ctx.proj=mat4_perspective(60.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,80.0f);
    ctx.light_dir=v3_norm(v3(0.4f,0.55f,0.3f));   /* low sun */
    ctx.light_color=col3(1.0f,0.92f,0.78f);
    ctx.ambient=col3(0.25f,0.28f,0.35f);
    ctx.cam_pos=eye; ctx.specular=0.15f; ctx.shininess=8.0f; ctx.wireframe=0;
    ctx.model=mat4_translate(v3(0,-1.0f,0));
    raster_mesh(fb, s->mesh, &ctx);
    (void)sky;
}
static void te_key(Scene*sc,int key){
    TerrainState*s=sc->state;
    if(key==KEY_PLUS)s->speed*=1.3f; else if(key==KEY_MINUS)s->speed/=1.3f;
    else if(key==KEY_UP)s->alt+=0.5f; else if(key==KEY_DOWN)s->alt-=0.5f;
    else if(key==KEY_TAB){ s->rough = s->rough>1.5f?1.0f:2.0f; }
    if(s->speed<0.3f)s->speed=0.3f; if(s->speed>15)s->speed=15;
    if(s->alt<1.5f)s->alt=1.5f; if(s->alt>10)s->alt=10;
}
static CrtConfig te_crt(Scene*sc){(void)sc;return crt_config_preset("broadcast");}
static void te_destroy(Scene*sc){ if(sc){ TerrainState*s=sc->state; if(s->mesh)mesh_free(s->mesh); free(s->h); free(s); free(sc);} }

Scene *scene_terrain_create(void){
    Scene*sc=calloc(1,sizeof(Scene));
    sc->name="terrain"; sc->description="Flight over an infinite fractal landscape";
    sc->state=calloc(1,sizeof(TerrainState));
    sc->init=te_init;sc->update=te_update;sc->render=te_render;sc->on_key=te_key;
    sc->destroy=te_destroy;sc->preferred_crt=te_crt;
    return sc;
}
