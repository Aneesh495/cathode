/* ==========================================================================
 * scene_wireframe.c  -  neon vector-display "wireframe city", driven by the C++
 * retained-mode scene graph (src/cpp/scenegraph.cpp, contract in cppcore.h).
 *
 * We build a hierarchy: a ground grid plus a field of nested, rotating boxes
 * ("buildings"). Each frame the C++ side flattens the transformed hierarchy to
 * world-space line segments; we project them and draw anti-aliased-ish glowing
 * lines. With the CRT's bloom + phosphor this reads like an old vector monitor.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/cppcore.h"
#include <stdlib.h>
#include <math.h>

#define MAX_SEGS 4096
#define N_BUILDINGS 40

typedef struct {
    CppSceneGraph *sg;
    CppSegment *segs;
    i32 building_node[N_BUILDINGS];
    Vec3 building_pos[N_BUILDINGS];
    f32 building_spin[N_BUILDINGS];
    f32 building_h[N_BUILDINGS];
    i32 w,h; f32 t;
    int mode;
} WireState;

static void wf_init(Scene *sc, i32 w, i32 h){
    WireState *s=sc->state; s->w=w; s->h=h; s->t=0; s->mode=0;
    s->sg = cpp_sg_create();
    s->segs = malloc(sizeof(CppSegment)*MAX_SEGS);
    /* ground grid at the root */
    i32 groundn = cpp_sg_add_node(s->sg, -1);
    cpp_sg_set_transform(s->sg, groundn, mat4_identity());
    cpp_sg_add_grid(s->sg, groundn, 20, 24.0f, col3(0.1f,0.5f,0.7f));
    /* a field of "buildings": nested rotating boxes */
    unsigned seed=0x1234;
    for (int i=0;i<N_BUILDINGS;++i){
        seed = seed*1664525u + 1013904223u;
        f32 fx = ((seed>>16)&0xff)/255.0f - 0.5f;
        seed = seed*1664525u + 1013904223u;
        f32 fz = ((seed>>16)&0xff)/255.0f - 0.5f;
        f32 hgt = 0.8f + ((seed>>8&0xff)/255.0f)*3.5f;
        Vec3 pos = v3(fx*20.0f, 0.0f, fz*20.0f);
        s->building_pos[i]=pos; s->building_h[i]=hgt;
        s->building_spin[i]= (((seed>>3)&0xff)/255.0f - 0.5f)*0.6f;
        i32 n = cpp_sg_add_node(s->sg, -1);
        s->building_node[i]=n;
        Color3 c = col3(0.2f+0.8f*((i*37)%100)/100.0f, 0.4f, 0.9f-0.5f*((i*53)%100)/100.0f);
        cpp_sg_add_box(s->sg, n, v3(0.5f, hgt, 0.5f), c);
    }
}

static void wf_update(Scene *sc, f32 dt, f32 t){
    WireState *s=sc->state; (void)dt; s->t=t;
    for (int i=0;i<N_BUILDINGS;++i){
        /* each building sits at its pos, bobbing and spinning slowly */
        f32 bob = 0.15f*sinf(t*1.3f + i);
        Mat4 T = mat4_translate(v3(s->building_pos[i].x, s->building_h[i]+bob, s->building_pos[i].z));
        Mat4 R = mat4_rotate_y(t*s->building_spin[i]);
        cpp_sg_set_transform(s->sg, s->building_node[i], mat4_mul(T,R));
    }
}

/* additive glowing line via fb_splat along a Bresenham-ish walk */
static void glow_line(Framebuffer *fb, f32 x0,f32 y0,f32 x1,f32 y1, Color3 c){
    f32 dx=x1-x0, dy=y1-y0;
    int steps=(int)(fabsf(dx)+fabsf(dy)); if(steps<1)steps=1; if(steps>4000)steps=4000;
    for (int k=0;k<=steps;++k){ f32 tt=(f32)k/steps; fb_splat(fb, x0+dx*tt, y0+dy*tt, c); }
}

static void wf_render(Scene *sc, Framebuffer *fb){
    WireState *s=sc->state;
    fb_clear(fb, col3(0.01f,0.01f,0.03f));
    /* flatten the C++ scene graph to world-space segments */
    i32 nseg = cpp_sg_flatten(s->sg, mat4_identity(), s->segs, MAX_SEGS);
    /* camera flies over the city */
    f32 a=s->t*0.15f;
    Vec3 eye=v3(cosf(a)*16.0f, 6.0f+2.0f*sinf(a*0.5f), sinf(a)*16.0f);
    Mat4 view=mat4_look_at(eye, v3(0,1.5f,0), v3(0,1,0));
    Mat4 proj=mat4_perspective(58.0f*CT_DEG2RAD,(f32)fb->w/fb->h,0.1f,100.0f);
    Mat4 vp=mat4_mul(proj,view);
    for (i32 i=0;i<nseg;++i){
        Vec4 ca=mat4_mul_v4(vp, v4_from_v3(s->segs[i].a,1.0f));
        Vec4 cb=mat4_mul_v4(vp, v4_from_v3(s->segs[i].b,1.0f));
        if (ca.w<=1e-3f || cb.w<=1e-3f) continue;  /* skip if behind camera */
        f32 ia=1.0f/ca.w, ib=1.0f/cb.w;
        f32 ax=(ca.x*ia*0.5f+0.5f)*fb->w, ay=(1.0f-(ca.y*ia*0.5f+0.5f))*fb->h;
        f32 bx=(cb.x*ib*0.5f+0.5f)*fb->w, by=(1.0f-(cb.y*ib*0.5f+0.5f))*fb->h;
        glow_line(fb, ax,ay,bx,by, col_scale(s->segs[i].color, 0.9f));
    }
}

static void wf_key(Scene *sc, int key){ (void)sc; (void)key; }
static CrtConfig wf_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.6f; c.persistence=0.35f; return c; }
static void wf_destroy(Scene *sc){ if(sc){ WireState*s=sc->state; if(s->sg)cpp_sg_destroy(s->sg); free(s->segs); free(s); free(sc);} }

Scene *scene_wireframe_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="wireframe";
    sc->description="Neon vector-display wireframe city (C++ retained scene graph)";
    sc->state=calloc(1,sizeof(WireState));
    sc->init=wf_init; sc->update=wf_update; sc->render=wf_render;
    sc->on_key=wf_key; sc->destroy=wf_destroy; sc->preferred_crt=wf_crt;
    return sc;
}
