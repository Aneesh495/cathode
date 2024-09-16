/* ==========================================================================
 * scene_softbody.c  -  bouncing pressurized soft-body blobs (C++ Verlet sim).
 *
 * Drives the C++ pressurized soft-body simulator (cpp_softbody_*): a few gooey
 * blobs fall under gravity, squash on the floor, and spring back  -  holding
 * their volume via the ideal-gas pressure force. We read each blob's ring of
 * world points, scanline-fill the polygon with a jelly color, and outline it
 * with a bright rim; a periodic "kick" tosses them so they keep wobbling.
 * Pure-C scene over the C++ soft-body ABI.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/cppcore.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NBLOBS 3
#define WORLD_W 24.0f
#define WORLD_H 20.0f
#define MAXPTS 48

typedef struct {
    i32 w, h; f32 t;
    CppSoftBody *blob[NBLOBS];
    Color3 col[NBLOBS];
    f32 *pts;                 /* scratch: 2*MAXPTS */
    f32 kick_timer;
    Rng rng;
} SoftState;

static void sb_spawn(SoftState *s, int i){
    if (s->blob[i]) cpp_softbody_destroy(s->blob[i]);
    f32 cx = 4.0f + rng_f32(&s->rng)*(WORLD_W-8.0f);
    f32 r  = 2.0f + rng_f32(&s->rng)*1.4f;
    int np = 16 + (int)(rng_f32(&s->rng)*12);
    if (np>MAXPTS) np=MAXPTS;
    s->blob[i]=cpp_softbody_create(np, cx, WORLD_H-4.0f, r, WORLD_W, WORLD_H);
    cpp_softbody_set_gravity(s->blob[i], 0.0f, -11.0f);
    cpp_softbody_set_pressure(s->blob[i], 1.0f);
    f32 hue=rng_f32(&s->rng);
    s->col[i]=col3(0.5f+0.5f*sinf(hue*6.28f), 0.5f+0.5f*sinf(hue*6.28f+2.09f),
                   0.5f+0.5f*sinf(hue*6.28f+4.18f));
}

static void sb_init(Scene *sc, i32 w, i32 h){
    SoftState *s=sc->state; s->w=w; s->h=h; s->t=0;
    rng_seed(&s->rng, 0x50F7B0D5ULL);
    s->pts=malloc(sizeof(f32)*2*MAXPTS);
    for (int i=0;i<NBLOBS;++i){ s->blob[i]=NULL; sb_spawn(s,i); }
    s->kick_timer=0;
}

static void sb_update(Scene *sc, f32 dt, f32 t){
    SoftState *s=sc->state; s->t=t;
    if (dt<=0) return; if (dt>0.04f) dt=0.04f;
    int steps=2; f32 h=dt/steps;
    for (int st=0; st<steps; ++st)
        for (int i=0;i<NBLOBS;++i) cpp_softbody_step(s->blob[i], h, 8);
    /* periodic kick to keep them lively; occasionally respawn one */
    s->kick_timer += dt;
    if (s->kick_timer > 2.2f){
        s->kick_timer=0;
        int i=(int)(rng_f32(&s->rng)*NBLOBS); if(i>=NBLOBS)i=NBLOBS-1;
        cpp_softbody_kick(s->blob[i], (rng_f32(&s->rng)-0.5f)*0.9f, 0.7f+rng_f32(&s->rng)*0.5f);
    }
}

/* scanline-fill a world-space polygon (already mapped to screen) */
static void fill_poly(Framebuffer *fb, const f32 *sx, const f32 *sy, int n, Color3 c){
    f32 miny=1e30f, maxy=-1e30f;
    for (int i=0;i<n;++i){ if(sy[i]<miny)miny=sy[i]; if(sy[i]>maxy)maxy=sy[i]; }
    int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
    if (y0<0)y0=0; if(y1>=fb->h)y1=fb->h-1;
    for (int y=y0;y<=y1;++y){
        f32 xs[MAXPTS]; int nx=0; f32 yc=(f32)y+0.5f;
        for (int i=0;i<n && nx<MAXPTS;++i){
            f32 ax=sx[i],ay=sy[i],bx=sx[(i+1)%n],by=sy[(i+1)%n];
            if ((ay<=yc&&by>yc)||(by<=yc&&ay>yc)) xs[nx++]=ax+(yc-ay)/(by-ay)*(bx-ax);
        }
        for (int i=1;i<nx;++i){ f32 v=xs[i]; int j=i-1; while(j>=0&&xs[j]>v){xs[j+1]=xs[j];j--;} xs[j+1]=v; }
        for (int i=0;i+1<nx;i+=2){
            int xa=(int)ceilf(xs[i]-0.5f), xb=(int)floorf(xs[i+1]-0.5f);
            if(xa<0)xa=0; if(xb>=fb->w)xb=fb->w-1;
            for (int x=xa;x<=xb;++x) fb_set(fb,x,y,c);
        }
    }
}

static void sb_render(Scene *sc, Framebuffer *fb){
    SoftState *s=sc->state; const i32 W=fb->w,H=fb->h;
    fb_clear(fb, col3(0.02f,0.02f,0.04f));
    /* floor line */
    f32 m=0.06f; f32 scx=(W*(1-2*m))/WORLD_W, scy=(H*(1-2*m))/WORLD_H;
    f32 sc_=scx<scy?scx:scy; f32 ox=W*m, oy=H*(1-m);
    for (int x=0;x<W;++x) fb_set(fb,x,(int)(oy),col3(0.25f,0.28f,0.35f));
    for (int i=0;i<NBLOBS;++i){
        int n=cpp_softbody_npoints(s->blob[i]);
        cpp_softbody_points(s->blob[i], s->pts);
        f32 px[MAXPTS], py[MAXPTS];
        for (int k=0;k<n && k<MAXPTS;++k){
            px[k]=ox + s->pts[2*k]*sc_;
            py[k]=oy - s->pts[2*k+1]*sc_;
        }
        Color3 body=s->col[i];
        fill_poly(fb, px, py, n<MAXPTS?n:MAXPTS, col_scale(body,0.6f));
        /* bright rim */
        Color3 rim=col3(body.r*1.4f+0.2f, body.g*1.4f+0.2f, body.b*1.4f+0.2f);
        for (int k=0;k<n && k<MAXPTS;++k){
            int a=k, b=(k+1)%n;
            int steps=(int)(fabsf(px[b]-px[a])+fabsf(py[b]-py[a]))+1;
            for (int t2=0;t2<=steps;++t2){ f32 f=(f32)t2/steps;
                fb_add(fb,(int)(px[a]+f*(px[b]-px[a])),(int)(py[a]+f*(py[b]-py[a])),rim); }
        }
        /* a little specular highlight dot near the top */
        fb_add(fb,(int)px[0],(int)py[0],col3(0.8f,0.8f,0.9f));
    }
}

static void sb_key(Scene *sc, int key){
    SoftState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) for(int i=0;i<NBLOBS;++i) sb_spawn(s,i);
    else if (key==KEY_SPACE) for(int i=0;i<NBLOBS;++i) cpp_softbody_kick(s->blob[i],0,1.2f);
}
static CrtConfig sb_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void sb_destroy(Scene *sc){
    if(sc){ SoftState*s=sc->state;
        for(int i=0;i<NBLOBS;++i) if(s->blob[i]) cpp_softbody_destroy(s->blob[i]);
        free(s->pts); free(s); free(sc);
    }
}

Scene *scene_softbody_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="softbody";
    sc->description="Bouncing pressurized soft-body blobs (C++ Verlet + gas pressure)";
    sc->state=calloc(1,sizeof(SoftState));
    sc->init=sb_init; sc->update=sb_update; sc->render=sb_render;
    sc->on_key=sb_key; sc->destroy=sb_destroy; sc->preferred_crt=sb_crt;
    return sc;
}
