/* ==========================================================================
 * scene_tesseract.c  -  rotating 4D polytopes (tesseract, 16-cell, 5-cell).
 *
 * Vertices live in 4-space. Each frame we rotate them by two simultaneous
 * 4D rotations (in the XW and YZ planes  -  genuinely 4-dimensional, not just a
 * spinning 3D shadow), perspective-project 4D→3D (divide by w-distance), then
 * 3D→2D (divide by z-distance), and draw the edges as glowing lines. The
 * "unfolding" you see  -  inner cube growing as it swings toward the 4D camera  - 
 * is the real hypercube rotation, impossible to fake with 3D alone.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct { f32 x,y,z,w; } Vec4d;
typedef struct { int a,b; } Edge;

typedef struct {
    Vec4d *verts; int nv;
    Edge  *edges; int ne;
    Color3 col;
    const char *name;
} Polytope;

typedef struct {
    Polytope poly[3];
    int which;
    i32 w,h; f32 t;
} TessState;

/* build the tesseract: 16 vertices (all ±1 in 4D), edge between vertices that
 * differ in exactly one coordinate. */
static void build_tesseract(Polytope *p){
    p->nv=16; p->verts=malloc(sizeof(Vec4d)*16);
    for (int i=0;i<16;++i){
        p->verts[i].x=(i&1)?1:-1; p->verts[i].y=(i&2)?1:-1;
        p->verts[i].z=(i&4)?1:-1; p->verts[i].w=(i&8)?1:-1;
    }
    p->edges=malloc(sizeof(Edge)*32); p->ne=0;
    for (int i=0;i<16;++i) for(int j=i+1;j<16;++j){
        int diff=i^j; if (diff && (diff&(diff-1))==0) p->edges[p->ne++]=(Edge){i,j}; /* one-bit diff */
    }
    p->col=col3(0.3f,0.8f,1.0f); p->name="tesseract";
}
/* 16-cell (hyper-octahedron): 8 vertices at ±1 on each axis; edges between all
 * non-antipodal pairs. */
static void build_16cell(Polytope *p){
    p->nv=8; p->verts=malloc(sizeof(Vec4d)*8);
    Vec4d v[8]={{1,0,0,0},{-1,0,0,0},{0,1,0,0},{0,-1,0,0},{0,0,1,0},{0,0,-1,0},{0,0,0,1},{0,0,0,-1}};
    memcpy(p->verts,v,sizeof(v));
    p->edges=malloc(sizeof(Edge)*24); p->ne=0;
    for (int i=0;i<8;++i)for(int j=i+1;j<8;++j){ if(i/2!=j/2) p->edges[p->ne++]=(Edge){i,j}; }
    p->col=col3(1.0f,0.5f,0.3f); p->name="16-cell";
}
/* 5-cell (4-simplex): 5 vertices, all pairs connected. */
static void build_5cell(Polytope *p){
    p->nv=5; p->verts=malloc(sizeof(Vec4d)*5);
    /* a symmetric 5-cell embedding */
    f32 s=1.0f/sqrtf(5.0f);
    Vec4d v[5]={
        { 1, 1, 1,-s},{ 1,-1,-1,-s},{-1, 1,-1,-s},{-1,-1, 1,-s},{0,0,0,4*s}
    };
    memcpy(p->verts,v,sizeof(v));
    p->edges=malloc(sizeof(Edge)*10); p->ne=0;
    for (int i=0;i<5;++i)for(int j=i+1;j<5;++j) p->edges[p->ne++]=(Edge){i,j};
    p->col=col3(0.6f,1.0f,0.4f); p->name="5-cell";
}

static void te_init(Scene *sc, i32 w, i32 h){
    TessState *s=sc->state; s->w=w; s->h=h; s->t=0; s->which=0;
    build_tesseract(&s->poly[0]); build_16cell(&s->poly[1]); build_5cell(&s->poly[2]);
}

/* rotate a 4D point in the XW and YZ planes */
static Vec4d rot4(Vec4d p, f32 axw, f32 ayz){
    f32 c1=cosf(axw), s1=sinf(axw);
    f32 nx=p.x*c1 - p.w*s1, nw=p.x*s1 + p.w*c1;
    f32 c2=cosf(ayz), s2=sinf(ayz);
    f32 ny=p.y*c2 - p.z*s2, nz=p.y*s2 + p.z*c2;
    return (Vec4d){nx,ny,nz,nw};
}

static void te_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((TessState*)sc->state)->t=t; }

static void glow_line(Framebuffer*fb, f32 x0,f32 y0,f32 x1,f32 y1, Color3 c){
    f32 dx=x1-x0, dy=y1-y0; int steps=(int)(fabsf(dx)+fabsf(dy)); if(steps<1)steps=1; if(steps>2000)steps=2000;
    for (int k=0;k<=steps;++k){ f32 tt=(f32)k/steps; fb_splat(fb, x0+dx*tt, y0+dy*tt, c); }
}

static void te_render(Scene *sc, Framebuffer *fb){
    TessState *s=sc->state; fb_clear(fb, col3(0.01f,0.01f,0.03f));
    Polytope *P=&s->poly[s->which];
    f32 t=s->t;
    /* project all verts to screen */
    f32 *sx=malloc(sizeof(f32)*P->nv), *sy=malloc(sizeof(f32)*P->nv);
    f32 w4dist=3.0f, z3dist=4.0f;
    f32 scale=(fb->h<fb->w?fb->h:fb->w)*0.32f;
    for (int i=0;i<P->nv;++i){
        Vec4d p=rot4(P->verts[i], t*0.5f, t*0.32f);
        /* 4D->3D perspective */
        f32 wf=1.0f/(w4dist - p.w);
        f32 x3=p.x*wf*w4dist, y3=p.y*wf*w4dist, z3=p.z*wf*w4dist;
        /* 3D->2D perspective */
        f32 zf=1.0f/(z3dist - z3);
        sx[i]=fb->w*0.5f + x3*zf*z3dist*scale*0.4f;
        sy[i]=fb->h*0.5f - y3*zf*z3dist*scale*0.4f;
    }
    for (int e=0;e<P->ne;++e){
        int a=P->edges[e].a, b=P->edges[e].b;
        glow_line(fb, sx[a],sy[a], sx[b],sy[b], col_scale(P->col,0.9f));
    }
    free(sx); free(sy);
}

static void te_key(Scene *sc, int key){
    TessState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->which=(s->which+1)%3;
}

static CrtConfig te_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.6f; c.persistence=0.4f; return c; }
static void te_destroy(Scene *sc){
    if(sc){ TessState*s=sc->state;
        for(int i=0;i<3;++i){ free(s->poly[i].verts); free(s->poly[i].edges); }
        free(s); free(sc);
    }
}

Scene *scene_tesseract_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="tesseract";
    sc->description="Rotating 4D polytopes (tesseract, 16-cell, 5-cell) projected 4D->2D";
    sc->state=calloc(1,sizeof(TessState));
    sc->init=te_init; sc->update=te_update; sc->render=te_render;
    sc->on_key=te_key; sc->destroy=te_destroy; sc->preferred_crt=te_crt;
    return sc;
}
