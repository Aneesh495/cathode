/* ==========================================================================
 * scene_cloth.c  -  a waving flag: Rust Verlet cloth + C rasterizer.
 *
 * The cloth physics run in Rust (rustsrc/src/cloth.rs). Each frame we pull the
 * node positions and triangle indices across the FFI boundary, build a Mesh,
 * recompute normals, and render it with the existing Phong rasterizer under a
 * moving light. Wind gusts are animated. Demonstrates Rust-computed geometry
 * flowing into the C render pipeline and out through the CRT.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/raster.h"
#include "cathode/rustcore.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define CW 34
#define CH 24

typedef struct {
    RustCloth *cloth;
    Mesh *mesh;              /* rebuilt each frame from cloth state */
    f32 *xyz;                /* node position scratch (node_count*3) */
    u32 *idx;                /* triangle index scratch */
    i32 ntris, nnodes;
    i32 w, h; f32 t;
    int wind_on;
} ClothState;

static void cl_build_cloth(ClothState *s){
    if (s->cloth) rust_cloth_destroy(s->cloth);
    s->cloth = rust_cloth_create(CW, CH, 0.22f);
    /* pin the top edge (the flag's hoist side) */
    for (i32 j=0;j<CH;++j) rust_cloth_pin(s->cloth, 0, j);
    rust_cloth_set_gravity(s->cloth, 4.0f);
    s->nnodes = rust_cloth_node_count(s->cloth);
}

static void cl_init(Scene *sc, i32 w, i32 h){
    ClothState *s=sc->state;
    s->w=w; s->h=h; s->t=0; s->wind_on=1;
    s->cloth=NULL;
    cl_build_cloth(s);
    s->xyz = malloc((size_t)s->nnodes*3*sizeof(f32));
    i32 maxtris = (CW-1)*(CH-1)*2;
    s->idx = malloc((size_t)maxtris*3*sizeof(u32));
    /* mesh with a fixed vertex/tri count; we rewrite positions each frame */
    s->mesh = calloc(1,sizeof(Mesh));
    s->mesh->nverts = s->nnodes;
    s->mesh->verts = calloc(s->nnodes, sizeof(Vertex));
    s->ntris = rust_cloth_indices(s->cloth, s->idx, maxtris);
    s->mesh->ntris = s->ntris;
    s->mesh->tris = calloc(s->ntris, sizeof(Tri));
    for (i32 t=0;t<s->ntris;++t){
        s->mesh->tris[t].a=s->idx[t*3+0];
        s->mesh->tris[t].b=s->idx[t*3+1];
        s->mesh->tris[t].c=s->idx[t*3+2];
    }
}

static void cl_update(Scene *sc, f32 dt, f32 t){
    ClothState *s=sc->state; s->t=t;
    if (dt>0.033f) dt=0.033f;
    if (s->wind_on){
        /* gusty wind: a base breeze plus turbulent variation */
        f32 wx = 3.5f + 2.5f*sinf(t*1.3f) + 1.5f*sinf(t*3.7f+1.0f);
        f32 wz = 1.5f*sinf(t*0.8f);
        rust_cloth_set_wind(s->cloth, wx, 0.4f*sinf(t*2.1f), wz);
    } else {
        rust_cloth_set_wind(s->cloth, 0,0,0);
    }
    rust_cloth_step(s->cloth, dt, 6);
    /* pull positions across FFI into our mesh */
    rust_cloth_positions(s->cloth, s->xyz);
    for (i32 i=0;i<s->nnodes;++i){
        s->mesh->verts[i].pos = v3(s->xyz[i*3+0], s->xyz[i*3+1], s->xyz[i*3+2]);
        /* color gradient across the flag */
        f32 u = (f32)(i / CH)/(CW-1);
        s->mesh->verts[i].color = col_lerp(col3(0.9f,0.15f,0.15f), col3(0.95f,0.85f,0.2f), u);
    }
    mesh_compute_normals(s->mesh);
}

static void cl_render(Scene *sc, Framebuffer *fb){
    ClothState *s=sc->state;
    fb_clear(fb, col3(0.03f,0.04f,0.07f));
    fb_clear_depth(fb, 1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    /* Frame the whole flag. It's pinned along x=0, spans ~CW*spacing in x and
     * hangs down in -y from the top row (y≈0), blowing toward +x. Center the
     * camera on the flag's midpoint and pull back enough to see all of it. */
    f32 span_x = CW*0.22f;   /* cloth width  */
    f32 span_y = CH*0.22f;   /* cloth height */
    Vec3 center = v3(span_x*0.45f, -span_y*0.5f, 0.0f);
    Vec3 eye = v3(center.x, center.y, span_x*1.6f + 3.0f);
    ctx.view=mat4_look_at(eye, center, v3(0,1,0));
    ctx.proj=mat4_perspective(52.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    ctx.light_dir=v3_norm(v3(cosf(s->t*0.6f), 0.5f, 0.8f));
    ctx.light_color=col3(1.0f,0.96f,0.9f);
    ctx.ambient=col3(0.18f,0.19f,0.26f);
    ctx.cam_pos=eye; ctx.specular=0.35f; ctx.shininess=24.0f; ctx.wireframe=0;
    ctx.model=mat4_identity();
    raster_mesh(fb, s->mesh, &ctx);
}

static void cl_key(Scene *sc, int key){
    ClothState *s=sc->state;
    if (key==KEY_TAB) s->wind_on=!s->wind_on;
    else if (key==KEY_R) { /* re-drop the cloth */
        rust_cloth_positions(s->cloth, s->xyz);
        cl_build_cloth(s);
    }
}

static CrtConfig cl_crt(Scene *sc){ (void)sc; return crt_config_preset("broadcast"); }
static void cl_destroy(Scene *sc){
    if(sc){ ClothState*s=sc->state;
        if(s->cloth) rust_cloth_destroy(s->cloth);
        if(s->mesh){ free(s->mesh->verts); free(s->mesh->tris); free(s->mesh); }
        free(s->xyz); free(s->idx); free(s); free(sc);
    }
}

Scene *scene_cloth_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="cloth";
    sc->description="Waving flag: Rust Verlet cloth simulation, C Phong rasterizer";
    sc->state=calloc(1,sizeof(ClothState));
    sc->init=cl_init; sc->update=cl_update; sc->render=cl_render;
    sc->on_key=cl_key; sc->destroy=cl_destroy; sc->preferred_crt=cl_crt;
    return sc;
}
