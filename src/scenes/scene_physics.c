/* ==========================================================================
 * scene_physics.c  -  2D rigid-body playground: polygons tumble and stack.
 *
 * Drives the impulse-based rigid-body solver (src/physics/rigidbody.c): a
 * static angled ramp and floor, with boxes and regular polygons spawned above
 * that fall, collide, tumble, and settle into a pile. Bodies are filled with a
 * flat color and outlined by drawing their edges into the framebuffer; the CRT
 * chain gives them glow. New bodies spawn periodically until the world is full,
 * then it resets  -  a hypnotic, always-moving physics toy.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/physics.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WORLD_W 24.0f
#define WORLD_H 30.0f
#define MAXBODIES 40

typedef struct {
    i32 w, h; f32 t;
    RigidWorld *world;
    Rng rng;
    f32 spawn_timer;
    Color3 colors[MAXBODIES];
    int ncolors;
} PhysState;

static void spawn_static(PhysState *s){
    /* floor is implicit (walls in the solver); add a couple of angled ledges */
    int a=rb_add_box(s->world, WORLD_W*0.35f, 9.0f, 5.0f, 0.5f, 0.35f, 0.0f);
    int b=rb_add_box(s->world, WORLD_W*0.70f, 5.0f, 4.5f, 0.5f, -0.30f, 0.0f);
    if (a>=0) s->colors[a]=col3(0.3f,0.35f,0.45f);
    if (b>=0) s->colors[b]=col3(0.3f,0.35f,0.45f);
    s->ncolors = rb_count(s->world);
}

static void ph_reset(PhysState *s){
    if (s->world) rb_destroy(s->world);
    s->world = rb_create(WORLD_W, WORLD_H, MAXBODIES);
    rb_set_gravity(s->world, 0.0f, -12.0f);
    s->ncolors=0;
    spawn_static(s);
    s->spawn_timer=0;
}

static void ph_init(Scene *sc, i32 w, i32 h){
    PhysState *s=sc->state; s->w=w; s->h=h; s->t=0;
    rng_seed(&s->rng, 0xB0D1E5ULL);
    s->world=NULL;
    ph_reset(s);
}

static Color3 rand_hue(Rng *r){
    f32 hue=rng_f32(r);
    return col3(0.5f+0.5f*sinf(hue*6.28f), 0.5f+0.5f*sinf(hue*6.28f+2.09f),
                0.5f+0.5f*sinf(hue*6.28f+4.18f));
}

static void ph_update(Scene *sc, f32 dt, f32 t){
    PhysState *s=sc->state; s->t=t;
    if (dt<=0) return; if (dt>0.05f) dt=0.05f;
    /* spawn a new body every ~0.7s until full, then reset after a pause */
    s->spawn_timer += dt;
    if (s->spawn_timer > 0.7f){
        s->spawn_timer=0;
        if (rb_count(s->world) < MAXBODIES-1){
            f32 x = 4.0f + rng_f32(&s->rng)*(WORLD_W-8.0f);
            f32 ang = rng_range(&s->rng, -0.5f, 0.5f);
            int id;
            if (rng_f32(&s->rng) < 0.5f){
                f32 hx=rng_range(&s->rng,0.7f,1.3f), hy=rng_range(&s->rng,0.7f,1.3f);
                id=rb_add_box(s->world, x, WORLD_H-3.0f, hx, hy, ang, 1.0f);
            } else {
                int sides=3+(int)(rng_f32(&s->rng)*5);   /* 3..7 */
                f32 rr=rng_range(&s->rng,0.8f,1.4f);
                id=rb_add_ngon(s->world, x, WORLD_H-3.0f, sides, rr, ang, 1.0f);
            }
            if (id>=0){ s->colors[id]=rand_hue(&s->rng); if(id>=s->ncolors)s->ncolors=id+1; }
        } else if (s->spawn_timer==0) {
            /* full  -  will reset below */
        }
    }
    /* when full and settled a while, reset for a fresh pile */
    if (rb_count(s->world) >= MAXBODIES-1 && rb_total_energy(s->world) < 2.0f)
        ph_reset(s);

    /* fixed-step the solver for stability */
    int steps=2; f32 h=dt/steps;
    for (int i=0;i<steps;++i) rb_step(s->world, h, 10);
}

/* draw a filled convex polygon (scanline) + bright edge, world->screen mapped */
static void draw_body(Framebuffer *fb, const f32 *xy, int n, Color3 fill,
                      f32 sx, f32 sy, f32 ox, f32 oy){
    /* transform to screen */
    f32 px[8], py[8];
    f32 miny=1e30f, maxy=-1e30f;
    for (int i=0;i<n;++i){
        px[i]=ox + xy[2*i]*sx;
        py[i]=oy - xy[2*i+1]*sy;   /* flip y (screen down) */
        if (py[i]<miny)miny=py[i]; if(py[i]>maxy)maxy=py[i];
    }
    int y0=(int)floorf(miny), y1=(int)ceilf(maxy);
    if (y0<0)y0=0; if (y1>=fb->h)y1=fb->h-1;
    for (int y=y0;y<=y1;++y){
        /* find span intersections of scanline y with polygon edges */
        f32 xs[16]; int nx=0;
        f32 yc=(f32)y+0.5f;
        for (int i=0;i<n && nx<16;++i){
            f32 ax=px[i], ay=py[i], bx=px[(i+1)%n], by=py[(i+1)%n];
            if ((ay<=yc && by>yc) || (by<=yc && ay>yc)){
                f32 tt=(yc-ay)/(by-ay);
                xs[nx++]=ax+tt*(bx-ax);
            }
        }
        /* sort intersections (small n, insertion) */
        for (int i=1;i<nx;++i){ f32 v=xs[i]; int j=i-1; while(j>=0&&xs[j]>v){xs[j+1]=xs[j];j--;} xs[j+1]=v; }
        for (int i=0;i+1<nx;i+=2){
            int xa=(int)ceilf(xs[i]-0.5f), xb=(int)floorf(xs[i+1]-0.5f);
            if (xa<0)xa=0; if(xb>=fb->w)xb=fb->w-1;
            for (int x=xa;x<=xb;++x) fb_set(fb, x, y, fill);
        }
    }
    /* bright edges */
    Color3 edge=col3(fill.r*1.6f+0.2f, fill.g*1.6f+0.2f, fill.b*1.6f+0.2f);
    for (int i=0;i<n;++i){
        f32 ax=px[i], ay=py[i], bx=px[(i+1)%n], by=py[(i+1)%n];
        int steps=(int)(fabsf(bx-ax)+fabsf(by-ay))+1;
        for (int k=0;k<=steps;++k){
            f32 tt=(f32)k/steps;
            fb_set(fb, (int)(ax+tt*(bx-ax)), (int)(ay+tt*(by-ay)), edge);
        }
    }
}

static void ph_render(Scene *sc, Framebuffer *fb){
    PhysState *s=sc->state;
    fb_clear(fb, col3(0.02f,0.025f,0.04f));
    /* world [0,WORLD_W]x[0,WORLD_H] -> screen, fit with margin */
    f32 margin=0.06f;
    f32 sx=(fb->w*(1-2*margin))/WORLD_W;
    f32 sy=(fb->h*(1-2*margin))/WORLD_H;
    f32 sc_=sx<sy?sx:sy;
    f32 ox=fb->w*margin;
    f32 oy=fb->h*(1-margin);   /* world y=0 maps near bottom */
    for (int b=0;b<rb_count(s->world);++b){
        f32 xy[16]; i32 n; f32 cx,cy,ang;
        rb_body_poly(s->world, b, xy, 8, &n, &cx,&cy,&ang);
        Color3 c = (b<s->ncolors)? s->colors[b] : col3(0.6f,0.6f,0.6f);
        draw_body(fb, xy, n, c, sc_, sc_, ox, oy);
    }
}

static void ph_key(Scene *sc, int key){
    PhysState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) ph_reset(s);
}
static CrtConfig ph_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void ph_destroy(Scene *sc){ if(sc){ PhysState*s=sc->state; if(s->world)rb_destroy(s->world); free(s); free(sc);} }

Scene *scene_physics_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="physics";
    sc->description="2D rigid-body playground: polygons tumble, collide, and stack";
    sc->state=calloc(1,sizeof(PhysState));
    sc->init=ph_init; sc->update=ph_update; sc->render=ph_render;
    sc->on_key=ph_key; sc->destroy=ph_destroy; sc->preferred_crt=ph_crt;
    return sc;
}
