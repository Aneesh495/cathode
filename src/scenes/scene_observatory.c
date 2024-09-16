/* ==========================================================================
 * scene_observatory.c  -  a subsystem-combining showcase.
 *
 * Renders, through ONE camera and into one framebuffer:
 *   • a fractal terrain horizon   -  the C rasterizer + noise heightfield,
 *   • a live Barnes-Hut N-body star swarm orbiting overhead  -  the C physics
 *     module, its particles projected with the hand-written NEON batched
 *     projector (project_points_neon),
 *   • a graded night sky.
 * It exists to prove the pieces compose: rasterizer + physics + assembly +
 * CRT all cooperating in a single coherent shot  -  a galaxy rising over
 * mountains at night.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/raster.h"
#include "cathode/physics.h"
#include "cathode/noise.h"
#include "cathode/simd.h"     /* project_points_neon */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TGRID 72

typedef struct {
    Mesh *terrain;
    f32  *heights;
    NBody *nb;
    f32  *ptxyz;      /* packed particle positions for the NEON projector */
    f32  *scr;        /* projected screen coords (n*3) */
    unsigned char *vis;
    i32 w, h; f32 t;
} ObsState;

static void ob_build_terrain(ObsState *s){
    for (int j=0;j<TGRID;++j)for(int i=0;i<TGRID;++i){
        f32 x=(f32)i*0.16f, z=(f32)j*0.16f;
        s->heights[j*TGRID+i]=ridged2(x*0.5f, z*0.5f, 5, 2.0f, 0.5f)*1.4f
                             + fbm2(x*0.3f, z*0.3f, 4, 2.0f, 0.5f)*0.4f;
    }
    if (s->terrain) mesh_free(s->terrain);
    s->terrain=mesh_from_heightfield(s->heights, TGRID, TGRID, 20.0f, 3.0f);
}

static void ob_init(Scene *sc, i32 w, i32 h){
    ObsState *s=sc->state; s->w=w; s->h=h; s->t=0;
    s->heights=malloc(sizeof(f32)*TGRID*TGRID);
    s->terrain=NULL; ob_build_terrain(s);
    /* a compact galaxy hovering above the ridge */
    s->nb=nbody_create(700); s->nb->g=1.0f; s->nb->theta=0.7f; s->nb->softening=0.15f;
    nbody_seed_galaxy(s->nb, 600, v3(0, 7.0f, 0), 4.0f, 1800.0f);
    s->ptxyz=malloc(sizeof(f32)*s->nb->n*3);
    s->scr=malloc(sizeof(f32)*s->nb->n*3);
    s->vis=malloc(s->nb->n);
}

static void ob_update(Scene *sc, f32 dt, f32 t){
    ObsState *s=sc->state; s->t=t;
    f32 step=dt<0.04f?dt:0.04f;
    nbody_step(s->nb, step*0.5f);
}

static void ob_render(Scene *sc, Framebuffer *fb){
    ObsState *s=sc->state;
    /* --- night-sky gradient --- */
    for (i32 y=0;y<fb->h;++y){
        f32 v=(f32)y/fb->h;
        Color3 c=col_lerp(col3(0.02f,0.02f,0.08f), col3(0.10f,0.06f,0.16f), v*0.8f);
        for (i32 x=0;x<fb->w;++x) fb_set(fb,x,y,c);
    }
    fb_clear_depth(fb, 1e30f);

    /* shared camera: slowly pan across the ridge, galaxy overhead */
    f32 a=s->t*0.08f;
    Vec3 eye=v3(sinf(a)*3.0f, 4.5f, 13.0f);
    Vec3 tgt=v3(0.0f, 3.0f, 0.0f);
    Mat4 view=mat4_look_at(eye, tgt, v3(0,1,0));
    Mat4 proj=mat4_perspective(55.0f*CT_DEG2RAD, (f32)fb->w/fb->h, 0.1f, 200.0f);

    /* --- N-body swarm FIRST (behind/above the ridge), via NEON projector --- */
    Mat4 vp=mat4_mul(proj, view);
    for (i32 i=0;i<s->nb->n;++i){
        s->ptxyz[i*3+0]=s->nb->pos[i].x; s->ptxyz[i*3+1]=s->nb->pos[i].y; s->ptxyz[i*3+2]=s->nb->pos[i].z;
    }
    project_points_neon(s->scr, s->vis, s->ptxyz, (unsigned long)s->nb->n,
                        vp.m, (f32)fb->w, (f32)fb->h, 1e-3f);
    for (i32 i=0;i<s->nb->n;++i){
        if (!s->vis[i]) continue;
        f32 sx=s->scr[i*3+0], sy=s->scr[i*3+1];
        Color3 c=col_scale(s->nb->color[i], 1.2f);
        fb_splat(fb, sx, sy, c);
    }

    /* --- terrain ridge in front, depth-tested so it occludes low stars --- */
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    ctx.view=view; ctx.proj=proj;
    ctx.light_dir=v3_norm(v3(0.3f,0.5f,0.4f));
    ctx.light_color=col3(0.5f,0.55f,0.8f);      /* cool moonlight */
    ctx.ambient=col3(0.06f,0.07f,0.12f);
    ctx.cam_pos=eye; ctx.specular=0.1f; ctx.shininess=8.0f; ctx.wireframe=0;
    ctx.model=mat4_translate(v3(0,-1.5f,0));
    raster_mesh(fb, s->terrain, &ctx);
}

static void ob_key(Scene *sc, int key){
    ObsState *s=sc->state;
    if (key==KEY_R){ ob_build_terrain(s); nbody_destroy(s->nb);
        s->nb=nbody_create(700); s->nb->g=1.0f; s->nb->theta=0.7f; s->nb->softening=0.15f;
        nbody_seed_galaxy(s->nb, 600, v3(0,7,0), 4.0f, 1800.0f); }
}

static CrtConfig ob_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.5f; c.persistence=0.35f; return c; }
static void ob_destroy(Scene *sc){
    if(sc){ ObsState*s=sc->state;
        if(s->terrain)mesh_free(s->terrain); if(s->nb)nbody_destroy(s->nb);
        free(s->heights);free(s->ptxyz);free(s->scr);free(s->vis); free(s); free(sc);
    }
}

Scene *scene_observatory_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="observatory";
    sc->description="Galaxy rising over fractal mountains (rasterizer + N-body + NEON projector)";
    sc->state=calloc(1,sizeof(ObsState));
    sc->init=ob_init; sc->update=ob_update; sc->render=ob_render;
    sc->on_key=ob_key; sc->destroy=ob_destroy; sc->preferred_crt=ob_crt;
    return sc;
}
