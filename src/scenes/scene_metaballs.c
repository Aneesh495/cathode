/* ==========================================================================
 * scene_metaballs.c  -  animated 3D metaballs: C++ marching cubes + C rasterizer.
 *
 * A handful of moving spheres define a scalar field (sum of 1/r^2 potentials,
 * cpp_metaball_field). Each frame we extract the iso-surface as a triangle mesh
 * via marching cubes (cpp_marching_cubes) and render it with the Phong
 * rasterizer. The blobs merge and separate organically  -  the quintessential
 * "liquid mercury" demoscene look, but real 3D geometry, not a 2D fake.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/raster.h"
#include "cathode/cppcore.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define GRID 40
#define NBALLS 5
#define MAX_TRIS 80000

typedef struct {
    f32 *field;              /* GRID^3 scalar field */
    CppMcVertex *mcverts;    /* marching-cubes output (MAX_TRIS*3) */
    Mesh *mesh;              /* rebuilt each frame from mcverts */
    Vec3 centers[NBALLS];
    f32  radii[NBALLS];
    i32 w, h; f32 t;
    int palette;
} MetaState;

static void mb_init(Scene *sc, i32 w, i32 h){
    MetaState *s=sc->state; s->w=w; s->h=h; s->t=0; s->palette=0;
    s->field=malloc((size_t)GRID*GRID*GRID*sizeof(f32));
    s->mcverts=malloc((size_t)MAX_TRIS*3*sizeof(CppMcVertex));
    /* mesh with a fixed max capacity; we set ntris each frame */
    s->mesh=calloc(1,sizeof(Mesh));
    s->mesh->verts=calloc((size_t)MAX_TRIS*3, sizeof(Vertex));
    s->mesh->tris =calloc((size_t)MAX_TRIS,   sizeof(Tri));
    for (int i=0;i<NBALLS;++i) s->radii[i]=GRID*0.16f;
}

static void mb_update(Scene *sc, f32 dt, f32 t){
    MetaState *s=sc->state; (void)dt; s->t=t;
    /* animate ball centers on Lissajous paths inside the grid */
    f32 c=GRID*0.5f, r=GRID*0.28f;
    for (int i=0;i<NBALLS;++i){
        f32 pi=(f32)i;
        s->centers[i]=v3(
            c + r*sinf(t*(0.7f+0.1f*pi) + pi*1.3f),
            c + r*sinf(t*(0.5f+0.13f*pi) + pi*2.1f),
            c + r*cosf(t*(0.6f+0.09f*pi) + pi*0.7f));
        s->radii[i]=GRID*(0.14f+0.03f*sinf(t*0.8f+pi));
    }
    /* build field + extract surface (C++), then pack into the C mesh */
    cpp_metaball_field(s->field, GRID,GRID,GRID, s->centers, s->radii, NBALLS);
    i32 tris=cpp_marching_cubes(s->field, GRID,GRID,GRID, 1.2f, s->mcverts, MAX_TRIS);
    s->mesh->nverts=(u32)(tris*3);
    s->mesh->ntris=(u32)tris;
    Color3 base = col3(0.6f,0.75f,0.95f);
    for (i32 i=0;i<tris*3;++i){
        s->mesh->verts[i].pos    = s->mcverts[i].pos;
        s->mesh->verts[i].normal = s->mcverts[i].normal;
        s->mesh->verts[i].color  = base;
    }
    for (i32 t2=0;t2<tris;++t2){ s->mesh->tris[t2].a=t2*3+0; s->mesh->tris[t2].b=t2*3+1; s->mesh->tris[t2].c=t2*3+2; }
}

static void mb_render(Scene *sc, Framebuffer *fb){
    MetaState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.02f,0.05f));
    fb_clear_depth(fb, 1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    /* center the grid at the origin; orbit the camera around it */
    f32 half=GRID*0.5f;
    f32 a=s->t*0.3f;
    Vec3 eye=v3(cosf(a)*GRID*1.5f, GRID*0.4f, sinf(a)*GRID*1.5f);
    ctx.view=mat4_look_at(eye, v3(0,0,0), v3(0,1,0));
    ctx.proj=mat4_perspective(50.0f*CT_DEG2RAD,(f32)fb->w/fb->h,1.0f, GRID*6.0f);
    ctx.light_dir=v3_norm(v3(0.5f,0.8f,0.6f));
    ctx.light_color=col3(1.0f,0.95f,0.9f);
    ctx.ambient=col3(0.12f,0.14f,0.2f);
    ctx.cam_pos=eye; ctx.specular=0.9f; ctx.shininess=64.0f; ctx.wireframe=0;
    /* translate grid coords ([0,GRID]) to centered world space */
    ctx.model=mat4_translate(v3(-half,-half,-half));
    if (s->mesh->ntris>0) raster_mesh(fb, s->mesh, &ctx);
}

static void mb_key(Scene *sc, int key){
    MetaState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
}

static CrtConfig mb_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("arcade"); c.bloom=0.55f; return c; }
static void mb_destroy(Scene *sc){
    if(sc){ MetaState*s=sc->state;
        if(s->mesh){ free(s->mesh->verts); free(s->mesh->tris); free(s->mesh); }
        free(s->field); free(s->mcverts); free(s); free(sc);
    }
}

Scene *scene_metaballs_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="metaballs";
    sc->description="Animated 3D metaballs: C++ marching cubes + C Phong rasterizer";
    sc->state=calloc(1,sizeof(MetaState));
    sc->init=mb_init; sc->update=mb_update; sc->render=mb_render;
    sc->on_key=mb_key; sc->destroy=mb_destroy; sc->preferred_crt=mb_crt;
    return sc;
}
