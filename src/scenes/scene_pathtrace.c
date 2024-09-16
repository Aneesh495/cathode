/* ==========================================================================
 * scene_pathtrace.c  -  real-time Monte-Carlo path tracing via the C++ tracer.
 *
 * The C++ BVH path tracer (src/cpp/tracer.cpp, contract in cppcore.h) renders
 * a small scene of spheres (diffuse / metal / glass / emissive) with global
 * illumination. Because path tracing is expensive, we render at a modest
 * internal resolution and accumulate a few samples per frame; when the camera
 * moves we reset the accumulator. The CRT chain adds period-appropriate grain,
 * which conveniently masks path-tracer noise.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/cppcore.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    CppTracer *tr;
    i32 w, h;
    f32 t;
    int spp_per_frame;
    int moving;         /* orbit the camera (resets accum) vs let it refine */
} PtState;

static void pt_scene(PtState *s){
    cpp_tracer_clear_scene(s->tr);
    /* ground plane */
    cpp_tracer_add_plane(s->tr, v3(0,-0.5f,0), v3(0,1,0), CPPMAT_LAMBERT, col3(0.55f,0.55f,0.6f), 0);
    /* a row of spheres, one per material */
    cpp_tracer_add_sphere(s->tr, v3(-1.6f,0,-1.0f), 0.5f, CPPMAT_LAMBERT,    col3(0.85f,0.25f,0.25f), 0);
    cpp_tracer_add_sphere(s->tr, v3(-0.5f,0,-1.0f), 0.5f, CPPMAT_METAL,      col3(0.9f,0.75f,0.4f), 0.02f);
    cpp_tracer_add_sphere(s->tr, v3( 0.6f,0,-1.0f), 0.5f, CPPMAT_METAL,      col3(0.85f,0.85f,0.9f), 0.25f);
    cpp_tracer_add_sphere(s->tr, v3( 1.7f,0,-1.0f), 0.5f, CPPMAT_DIELECTRIC, col3(1,1,1), 1.5f);
    /* a small bright diffuse ball tucked behind */
    cpp_tracer_add_sphere(s->tr, v3(0.0f,-0.15f,-0.1f), 0.35f, CPPMAT_LAMBERT, col3(0.3f,0.5f,0.9f), 0);
    /* overhead area light (big emissive sphere) */
    cpp_tracer_add_sphere(s->tr, v3(0.0f,4.0f,-1.0f), 1.5f, CPPMAT_EMISSIVE, col3(1.0f,0.95f,0.85f), 5.0f);
    cpp_tracer_set_sky(s->tr, col3(0.4f,0.55f,0.85f), col3(0.85f,0.87f,0.95f));
    cpp_tracer_build(s->tr);
}

static void pt_init(Scene *sc, i32 w, i32 h){
    PtState *s=sc->state; s->w=w; s->h=h; s->t=0; s->spp_per_frame=3; s->moving=1;
    s->tr = cpp_tracer_create(w, h);
    pt_scene(s);
    cpp_tracer_reset_accum(s->tr);
}

static void pt_update(Scene *sc, f32 dt, f32 t){
    PtState *s=sc->state; (void)dt; s->t=t;
    if (s->moving){
        /* orbit camera; reset accumulation since the view changed */
        f32 a=t*0.25f;
        Vec3 eye=v3(cosf(a)*3.6f, 1.1f+0.4f*sinf(a*0.7f), 2.6f + sinf(a)*0.5f);
        cpp_tracer_set_camera(s->tr, eye, v3(0,-0.05f,-1.0f), 1.0f, 0.02f);
        cpp_tracer_reset_accum(s->tr);
    }
    cpp_tracer_render(s->tr, s->spp_per_frame, 8);
}

static void pt_render(Scene *sc, Framebuffer *fb){
    PtState *s=sc->state;
    cpp_tracer_resolve(s->tr, fb);   /* averaged linear HDR into fb */
}

static void pt_key(Scene *sc, int key){
    PtState *s=sc->state;
    if (key==KEY_SPACE || key==KEY_TAB) s->moving=!s->moving; /* freeze to refine */
    else if (key==KEY_PLUS) s->spp_per_frame = s->spp_per_frame<16?s->spp_per_frame+1:16;
    else if (key==KEY_MINUS) s->spp_per_frame = s->spp_per_frame>1?s->spp_per_frame-1:1;
    else if (key==KEY_R) cpp_tracer_reset_accum(s->tr);
}

static CrtConfig pt_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.noise=0.01f; return c; }
static void pt_destroy(Scene *sc){ if(sc){ PtState*s=sc->state; if(s->tr)cpp_tracer_destroy(s->tr); free(s); free(sc);} }

Scene *scene_pathtrace_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="pathtrace";
    sc->description="Monte-Carlo path tracer with BVH (C++), progressive accumulation";
    sc->state=calloc(1,sizeof(PtState));
    sc->init=pt_init; sc->update=pt_update; sc->render=pt_render;
    sc->on_key=pt_key; sc->destroy=pt_destroy; sc->preferred_crt=pt_crt;
    return sc;
}
