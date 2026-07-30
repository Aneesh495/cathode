/* ==========================================================================
 * scene_planet.c — a procedurally-textured rotating planet.
 *
 * An icosphere Phong-shaded by the CPU rasterizer, its surface colored by a
 * procedural texture sampled through the rasterizer's new UV texture hook
 * (RasterCtx.texfn): layered fbm noise defines continents vs ocean, with a
 * latitude-based ice-cap tint and an fbm "cloud" band modulating brightness.
 * The whole thing spins under a moving sun. Demonstrates perspective-correct
 * texture mapping over a real mesh, all on the CPU.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/raster.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { i32 w,h; f32 t; Mesh *sphere; int style; } PlanetState;

static void pl_init(Scene *sc, i32 w, i32 h){
    PlanetState *s=sc->state; s->w=w; s->h=h; s->t=0; s->style=0;
    s->sphere=mesh_icosphere(1.4f, 4);   /* dense enough for smooth UV texturing */
}
static void pl_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((PlanetState*)sc->state)->t=t; }

/* Earth-like: fbm elevation -> ocean/land/mountain/ice ramp. UVs are the
 * icosphere's lat/long (u in [0,1] around, v in [0,1] pole-to-pole). */
static Color3 planet_tex(f32 u, f32 v, void *user){
    (void)user;
    /* wrap u for seamless longitude; map to a sphere-ish sample domain */
    f32 x = u * 6.2831853f;
    /* sample fbm on a cylinder to avoid the seam being too obvious */
    f32 nx = cosf(x), nz = sinf(x);
    f32 e = fbm3(nx*2.2f, v*3.4f, nz*2.2f, 6, 2.0f, 0.5f);   /* [-1,1]-ish */
    e = 0.5f + 0.5f*e;                                        /* [0,1] */
    /* latitude 0..1 -> distance from equator for ice caps */
    f32 lat = fabsf(v - 0.5f) * 2.0f;
    Color3 c;
    if (e < 0.48f){
        /* ocean: deep to shallow blue */
        f32 d = e/0.48f;
        c = col3(0.02f+0.05f*d, 0.10f+0.35f*d, 0.35f+0.45f*d);
    } else if (e < 0.62f){
        /* coast/lowland green */
        f32 d=(e-0.48f)/0.14f;
        c = col3(0.15f+0.25f*d, 0.45f+0.20f*d, 0.12f+0.10f*d);
    } else if (e < 0.80f){
        /* highland brown */
        f32 d=(e-0.62f)/0.18f;
        c = col3(0.40f+0.25f*d, 0.32f+0.12f*d, 0.16f+0.06f*d);
    } else {
        /* mountain rock -> snow */
        f32 d=(e-0.80f)/0.20f;
        c = col3(0.55f+0.4f*d, 0.5f+0.45f*d, 0.48f+0.5f*d);
    }
    /* polar ice caps blend in toward the poles */
    if (lat > 0.72f){
        f32 icy=(lat-0.72f)/0.28f; if(icy>1)icy=1;
        c = col_lerp(c, col3(0.92f,0.95f,1.0f), icy);
    }
    return c;
}

static void pl_render(Scene *sc, Framebuffer *fb){
    PlanetState *s=sc->state;
    fb_clear(fb, col3(0.01f,0.01f,0.03f)); fb_clear_depth(fb,1e30f);
    /* a few background stars (deterministic) */
    for (int i=0;i<80;++i){
        int hx=(i*2654435761u)%fb->w, hy=(i*40503u)%fb->h;
        f32 b=0.3f+0.5f*((i*7u)%10)/10.0f;
        fb_add(fb, hx, hy, col3(b,b,b));
    }
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    f32 a=s->t*0.15f;
    Vec3 eye=v3(cosf(a)*4.2f, 0.9f, sinf(a)*4.2f);
    ctx.view=mat4_look_at(eye, v3(0,0,0), v3(0,1,0));
    ctx.proj=mat4_perspective(46.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    /* moving sun */
    ctx.light_dir=v3_norm(v3(cosf(s->t*0.4f), 0.35f, sinf(s->t*0.4f)));
    ctx.light_color=col3(1.0f,0.97f,0.9f); ctx.ambient=col3(0.05f,0.06f,0.1f);
    ctx.cam_pos=eye; ctx.specular=0.35f; ctx.shininess=16.0f; ctx.wireframe=0;
    ctx.model=mat4_rotate_y(s->t*0.25f);   /* planet spin */
    ctx.texfn=planet_tex;
    raster_mesh(fb, s->sphere, &ctx);
}

static CrtConfig pl_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void pl_destroy(Scene *sc){ if(sc){ PlanetState*s=sc->state; if(s->sphere)mesh_free(s->sphere); free(s); free(sc);} }

Scene *scene_planet_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="planet";
    sc->description="Procedurally-textured rotating planet (fbm continents, ice caps, UV texture mapping)";
    sc->state=calloc(1,sizeof(PlanetState));
    sc->init=pl_init; sc->update=pl_update; sc->render=pl_render;
    sc->destroy=pl_destroy; sc->preferred_crt=pl_crt;
    return sc;
}
