/* ==========================================================================
 * scene_csg.c — constructive solid geometry: an animated boolean solid.
 *
 * Builds a CSG tree in the C++ core (cpp_csg_eval) — a rounded box with a
 * sphere bored out of it, unioned with a spinning torus, all with animated
 * smooth-blend radii — samples it into a scalar field, extracts the surface
 * with marching cubes (cpp_marching_cubes), and Phong-shades it with the C
 * rasterizer. The boolean structure morphs over time: the sphere pumps in and
 * out (carving a deeper/shallower cavity) and the blend radius breathes, so the
 * joints melt together and pull apart like machined-then-melted metal.
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

#define GRID 44
#define MAX_TRIS 90000

typedef struct {
    f32 *field;
    CppMcVertex *mcverts;
    Mesh *mesh;
    i32 w, h; f32 t;
    int preset;
} CsgState;

static void cg_init(Scene *sc, i32 w, i32 h){
    CsgState *s=sc->state; s->w=w; s->h=h; s->t=0; s->preset=0;
    s->field=malloc((size_t)GRID*GRID*GRID*sizeof(f32));
    s->mcverts=malloc((size_t)MAX_TRIS*3*sizeof(CppMcVertex));
    s->mesh=calloc(1,sizeof(Mesh));
    s->mesh->verts=calloc((size_t)MAX_TRIS*3, sizeof(Vertex));
    s->mesh->tris =calloc((size_t)MAX_TRIS,   sizeof(Tri));
}

static void cg_update(Scene *sc, f32 dt, f32 t){
    CsgState *s=sc->state; (void)dt; s->t=t;
    f32 c=GRID*0.5f;
    Vec3 mid=v3(c,c,c);
    f32 blend = 2.5f + 2.0f*sinf(t*0.5f);            /* breathing smooth-blend */
    f32 bore  = GRID*(0.16f + 0.06f*sinf(t*0.9f));   /* carving sphere radius */

    /* Tree layout (indices):
     *   0 box            1 bore-sphere      2 = 0 SUBTRACT 1
     *   3 torus          4 = 2 UNION 3  (root)         */
    CsgNode nodes[5];
    memset(nodes,0,sizeof(nodes));
    nodes[0].kind=CSG_BOX; nodes[0].a=-1; nodes[0].b=-1;
    nodes[0].center=mid; nodes[0].size=v3(GRID*0.26f, GRID*0.26f, GRID*0.26f); nodes[0].k=0;

    nodes[1].kind=CSG_SPHERE; nodes[1].a=-1; nodes[1].b=-1;
    /* bore sphere drifts along the box's main diagonal */
    nodes[1].center=v3(c + GRID*0.10f*sinf(t*0.7f),
                       c + GRID*0.10f*cosf(t*0.6f),
                       c + GRID*0.10f*sinf(t*0.9f));
    nodes[1].size=v3(bore,0,0); nodes[1].k=0;

    nodes[2].kind=CSG_SUBTRACT; nodes[2].a=0; nodes[2].b=1; nodes[2].k=blend*0.4f;

    nodes[3].kind=CSG_TORUS; nodes[3].a=-1; nodes[3].b=-1;
    nodes[3].center=mid; nodes[3].size=v3(GRID*0.30f, GRID*0.075f, 0); nodes[3].k=0;

    nodes[4].kind=CSG_UNION; nodes[4].a=2; nodes[4].b=3; nodes[4].k=blend;

    cpp_csg_eval(s->field, GRID,GRID,GRID, nodes, 5, 4);
    /* the field is +inside; extract just above 0 for a watertight surface */
    i32 tris=cpp_marching_cubes(s->field, GRID,GRID,GRID, 0.15f, s->mcverts, MAX_TRIS);
    s->mesh->nverts=(u32)(tris*3);
    s->mesh->ntris=(u32)tris;
    for (i32 i=0;i<tris*3;++i){
        s->mesh->verts[i].pos=s->mcverts[i].pos;
        s->mesh->verts[i].normal=s->mcverts[i].normal;
        /* copper/brass gradient by height for a machined look */
        f32 yv=s->mcverts[i].pos.y/GRID;
        s->mesh->verts[i].color=col3(0.85f, 0.5f+0.3f*yv, 0.2f+0.2f*yv);
    }
    for (i32 t2=0;t2<tris;++t2){ s->mesh->tris[t2].a=t2*3; s->mesh->tris[t2].b=t2*3+1; s->mesh->tris[t2].c=t2*3+2; }
}

static void cg_render(Scene *sc, Framebuffer *fb){
    CsgState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.02f,0.04f)); fb_clear_depth(fb,1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    f32 half=GRID*0.5f, a=s->t*0.35f;
    Vec3 eye=v3(cosf(a)*GRID*1.6f, GRID*0.5f+GRID*0.2f*sinf(a*0.5f), sinf(a)*GRID*1.6f);
    ctx.view=mat4_look_at(eye, v3(0,0,0), v3(0,1,0));
    ctx.proj=mat4_perspective(50.0f*CT_DEG2RAD,(f32)fb->w/fb->h,1.0f,GRID*6.0f);
    ctx.light_dir=v3_norm(v3(cosf(s->t*0.6f),0.7f,sinf(s->t*0.6f)));
    ctx.light_color=col3(1.0f,0.93f,0.82f); ctx.ambient=col3(0.1f,0.11f,0.16f);
    ctx.cam_pos=eye; ctx.specular=1.0f; ctx.shininess=80.0f; ctx.wireframe=0;
    ctx.model=mat4_translate(v3(-half,-half,-half));
    if (s->mesh->ntris>0) raster_mesh(fb, s->mesh, &ctx);
}

static CrtConfig cg_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void cg_destroy(Scene *sc){
    if(sc){ CsgState*s=sc->state;
        free(s->field); free(s->mcverts);
        if(s->mesh){ free(s->mesh->verts); free(s->mesh->tris); free(s->mesh); }
        free(s); free(sc);
    }
}

Scene *scene_csg_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="csg";
    sc->description="Constructive solid geometry: animated boolean solid (C++ CSG + marching cubes)";
    sc->state=calloc(1,sizeof(CsgState));
    sc->init=cg_init; sc->update=cg_update; sc->render=cg_render;
    sc->destroy=cg_destroy; sc->preferred_crt=cg_crt;
    return sc;
}
