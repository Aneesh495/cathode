/* ==========================================================================
 * scene_parametric.c  -  rotating parametric surfaces (Möbius, Klein, trefoil,
 * Boy's-ish), mesh-generated and Phong-shaded via the rasterizer.
 *
 * Builds a triangle mesh by sampling a parametric map (u,v) -> R^3 over a grid,
 * computing normals by finite differences of the surface, and rendering it with
 * the CPU rasterizer under a moving light. Cycles through several classic
 * surfaces from topology. Each is a genuine embedding/immersion, not a fake.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/raster.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NU 120
#define NV 40

typedef struct { int kind; Mesh *mesh; i32 w,h; f32 t; } ParamState;

/* parametric maps: u in [0,2pi], v in [-1,1] or [0,2pi] depending on surface */
static Vec3 surf(int kind, f32 u, f32 v){
    switch(kind){
        case 0: { /* Möbius strip  -  wide ribbon so the single-sided twist reads */
            f32 hu=u*0.5f;
            f32 r=2.4f + v*1.1f*cosf(hu);
            return v3(r*cosf(u), r*sinf(u), v*1.1f*sinf(hu));
        }
        case 1: { /* trefoil knot tube */
            Vec3 c=v3(sinf(u)+2*sinf(2*u), cosf(u)-2*cosf(2*u), -sinf(3*u));
            /* tube around the curve: approximate frame via derivative */
            Vec3 d=v3(cosf(u)+4*cosf(2*u), -sinf(u)+4*sinf(2*u), -3*cosf(3*u));
            Vec3 t=v3_norm(d);
            Vec3 n=v3_norm(v3_cross(t, v3(0,0,1)));
            Vec3 b=v3_cross(t,n);
            f32 tr=0.55f;
            return v3_add(v3_scale(c,1.05f),
                   v3_add(v3_scale(n, tr*cosf(v)), v3_scale(b, tr*sinf(v))));
        }
        case 2: { /* Klein bottle (figure-8 immersion) */
            f32 hu=u*0.5f;
            f32 cx=(2.0f + cosf(hu)*sinf(v) - sinf(hu)*sinf(2*v))*cosf(u);
            f32 cy=(2.0f + cosf(hu)*sinf(v) - sinf(hu)*sinf(2*v))*sinf(u);
            f32 cz=sinf(hu)*sinf(v)+cosf(hu)*sinf(2*v);
            return v3(cx*1.15f, cy*1.15f, cz*1.5f);
        }
        default: { /* twisted torus (2-fold twist) */
            f32 R=2.4f, rr=0.95f;
            f32 tw=v + 2.0f*u;   /* twist */
            return v3((R+rr*cosf(tw))*cosf(u), (R+rr*cosf(tw))*sinf(u), rr*sinf(tw));
        }
    }
}

static void pa_build(ParamState *s){
    int uwrap=1, vwrap=(s->kind==1||s->kind==3);   /* v wraps for tubes */
    f32 umax=CT_TAU, vmin, vmax;
    if (s->kind==0){ vmin=-1; vmax=1; }
    else if (s->kind==2){ vmin=0; vmax=CT_TAU; }
    else { vmin=0; vmax=CT_TAU; }
    int NUv=NU, NVv=NV;
    u32 nv=(u32)(NUv)*(NVv);
    Mesh *m=calloc(1,sizeof(Mesh));
    m->nverts=nv; m->verts=calloc(nv,sizeof(Vertex));
    m->ntris=(u32)NUv*NVv*2; m->tris=calloc(m->ntris,sizeof(Tri));
    for (int i=0;i<NUv;++i){
        f32 u=umax*(f32)i/NUv;
        for (int j=0;j<NVv;++j){
            f32 v=vmin+(vmax-vmin)*(f32)j/(NVv-1);
            int idx=i*NVv+j;
            m->verts[idx].pos=surf(s->kind,u,v);
            f32 hue=(f32)j/NVv;
            m->verts[idx].color=col3(0.5f+0.5f*sinf(hue*CT_TAU),0.5f+0.5f*sinf(hue*CT_TAU+2.09f),0.5f+0.5f*sinf(hue*CT_TAU+4.18f));
            /* UVs run along the (u,v) parameter grid so a checker texture reveals
             * the surface's intrinsic parameterization (and its twist). */
            m->verts[idx].uv=(Vec2){ (f32)i/NUv * 8.0f, (f32)j/(NVv-1) * 2.0f };
        }
    }
    u32 t=0;
    for (int i=0;i<NUv;++i){
        int ni=(i+1)%NUv;
        for (int j=0;j<NVv-1;++j){
            int a=i*NVv+j, b=ni*NVv+j, c=i*NVv+j+1, d=ni*NVv+j+1;
            m->tris[t++]=(Tri){(u32)a,(u32)b,(u32)c};
            m->tris[t++]=(Tri){(u32)c,(u32)b,(u32)d};
        }
    }
    m->ntris=t;
    mesh_compute_normals(m);
    if (s->mesh) mesh_free(s->mesh);
    s->mesh=m;
    (void)uwrap;(void)vwrap;
}

static void pa_init(Scene *sc, i32 w, i32 h){
    ParamState *s=sc->state; s->w=w; s->h=h; s->t=0; s->kind=0; s->mesh=NULL;
    pa_build(s);
}
static void pa_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((ParamState*)sc->state)->t=t; }

/* procedural checker texture: darkens alternating UV cells so the mesh's
 * parameterization (and the Mobius half-twist) reads clearly. */
static Color3 checker_tex(f32 u, f32 v, void *user){
    (void)user;
    int cu = (int)floorf(u), cv = (int)floorf(v);
    return ((cu + cv) & 1) ? col3(1.0f,1.0f,1.0f) : col3(0.30f,0.32f,0.38f);
}

static void pa_render(Scene *sc, Framebuffer *fb){
    ParamState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.02f,0.05f)); fb_clear_depth(fb,1e30f);
    RasterCtx ctx; memset(&ctx,0,sizeof(ctx));
    f32 a=s->t*0.5f;
    Vec3 eye=v3(cosf(a)*6.0f, 2.5f+1.5f*sinf(a*0.6f), sinf(a)*6.0f);
    ctx.view=mat4_look_at(eye, v3(0,0,0), v3(0,1,0));
    ctx.proj=mat4_perspective(52.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    ctx.light_dir=v3_norm(v3(cosf(s->t*0.7f),0.6f,sinf(s->t*0.7f)));
    ctx.light_color=col3(1.0f,0.95f,0.9f); ctx.ambient=col3(0.12f,0.13f,0.2f);
    ctx.cam_pos=eye; ctx.specular=0.7f; ctx.shininess=40.0f; ctx.wireframe=0;
    ctx.model=mat4_rotate_y(s->t*0.3f);
    ctx.texfn=checker_tex;   /* modulate per-strip color by a UV checker */
    raster_mesh(fb, s->mesh, &ctx);
}

static void pa_key(Scene *sc, int key){
    ParamState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->kind=(s->kind+1)%4; pa_build(s); }
}

static CrtConfig pa_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void pa_destroy(Scene *sc){ if(sc){ ParamState*s=sc->state; if(s->mesh)mesh_free(s->mesh); free(s); free(sc);} }

Scene *scene_parametric_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="parametric";
    sc->description="Rotating parametric surfaces: Mobius, trefoil knot, Klein bottle, twisted torus";
    sc->state=calloc(1,sizeof(ParamState));
    sc->init=pa_init; sc->update=pa_update; sc->render=pa_render;
    sc->on_key=pa_key; sc->destroy=pa_destroy; sc->preferred_crt=pa_crt;
    return sc;
}
