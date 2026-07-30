/* ==========================================================================
 * scene_hyperbolic.c — animated {p,q} tilings of the hyperbolic plane, drawn
 * in the Poincaré disk model.
 *
 * The hyperbolic plane can be tiled by regular p-gons meeting q at a vertex
 * whenever 1/p + 1/q < 1/2 (e.g. {5,4}, {7,3}, {6,4}). We render such a tiling
 * directly, per pixel, by *folding* each disk point into the fundamental
 * triangle of the (2,p,q) reflection group:
 *
 *   1. reflect across the two diameters at angles 0 and π/p (dihedral fold)
 *      until the point lies in the wedge [0, π/p];
 *   2. if it lies inside the "edge" geodesic — a circle orthogonal to the unit
 *      disk — invert it back out across that circle;
 *   repeat until stable. The number of reflections used gives the tile identity
 *   (parity → checkerboard, inversion depth → hue), which is exactly how the
 *   tessellation's combinatorics fall out of the group action.
 *
 * The whole disk is pushed through an animating hyperbolic translation (a real
 * Möbius map of the disk), so the tiling appears to glide past the viewer while
 * staying a perfect tessellation — the hallmark "infinite descent into the
 * boundary" look. Runs on the thread pool: rows are split across cores.
 *
 * The reflecting circle for {p,q}: centered at (cx,0), radius rc, orthogonal to
 * the unit circle (cx² = rc² + 1) and meeting the π/p diameter at angle π/q:
 *     rc = sin(π/p) / sqrt(cos²(π/q) − sin²(π/p))
 *     cx = cos(π/q) / sqrt(cos²(π/q) − sin²(π/p))
 * (the radicand is positive precisely when 1/p + 1/q < 1/2).
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "threadpool.h"      /* app-internal: multithreaded band rendering */
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* a small selection of valid hyperbolic Schläfli symbols {p,q} */
static const int PQ[][2] = { {5,4}, {7,3}, {6,4}, {8,3}, {5,5}, {4,6} };
enum { NPQ = (int)(sizeof(PQ)/sizeof(PQ[0])) };

typedef struct {
    i32 w, h; f32 t; int which;
    /* cached geometry for the current {p,q} */
    int p, q;
    f32 cx, rc;          /* reflecting circle */
    f32 wedge;           /* π/p */
    ThreadPool *pool;
} HypState;

static void hy_geometry(HypState *s){
    s->p = PQ[s->which][0]; s->q = PQ[s->which][1];
    f32 sp = sinf((f32)CT_PI/s->p), cq = cosf((f32)CT_PI/s->q);
    f32 rad = cq*cq - sp*sp;
    if (rad < 1e-4f) rad = 1e-4f;
    f32 root = sqrtf(rad);
    s->rc = sp / root;
    s->cx = cq / root;
    s->wedge = (f32)CT_PI/s->p;
}

static void hy_init(Scene *sc, i32 w, i32 h){
    HypState *s=sc->state; s->w=w; s->h=h; s->t=0; s->which=0;
    hy_geometry(s);
    if (!s->pool) s->pool = tp_create(0);
}

static void hy_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((HypState*)sc->state)->t=t; }

/* HSV->RGB helper (h in turns). */
static Color3 hsv(f32 h, f32 s, f32 v){
    h -= floorf(h);
    f32 i=floorf(h*6.0f), f=h*6.0f-i;
    f32 p=v*(1-s), q=v*(1-s*f), tt=v*(1-s*(1-f));
    switch(((int)i)%6){
        case 0: return col3(v,tt,p); case 1: return col3(q,v,p);
        case 2: return col3(p,v,tt); case 3: return col3(p,q,v);
        case 4: return col3(tt,p,v); default: return col3(v,p,q);
    }
}

/* Fold (x,y) into the fundamental triangle; returns reflection count, and the
 * inversion depth via *depth. Sets *edge to a [0,1] proximity-to-nearest-mirror
 * value for drawing crisp tile boundaries. */
static int fold(const HypState *s, f32 x, f32 y, int *depth, f32 *edge){
    int refl = 0, inv = 0;
    f32 near_edge = 1.0f;
    const f32 twowedge = 2.0f*s->wedge;
    for (int it=0; it<48; ++it){
        /* dihedral fold into [0, wedge] */
        f32 r = sqrtf(x*x+y*y);
        if (r > 0.9999f) { *depth=inv; *edge=0.0f; return refl; } /* at boundary */
        f32 a = atan2f(y, x);
        /* bring angle into [0, 2*wedge) */
        a = fmodf(a, twowedge); if (a < 0) a += twowedge;
        int did = 0;
        if (a > s->wedge) { a = twowedge - a; refl++; did=1; }  /* reflect across π/p line */
        /* also count the fold across the x-axis implicitly done by the modulo */
        x = r*cosf(a); y = r*sinf(a);

        /* distance from the π/p mirror line and x-axis for edge shading */
        f32 dl = fabsf(a) ;                 /* to x-axis (angle 0) */
        f32 dr = fabsf(s->wedge - a);       /* to π/p line */
        f32 dm = dl < dr ? dl : dr;
        if (dm*r < near_edge) near_edge = dm*r;

        /* invert across the edge circle if inside it */
        f32 ddx = x - s->cx, ddy = y;
        f32 d2 = ddx*ddx + ddy*ddy;
        f32 rc2 = s->rc*s->rc;
        /* proximity to the circle arc (for edges) */
        f32 dc = fabsf(sqrtf(d2) - s->rc);
        if (dc < near_edge) near_edge = dc;
        if (d2 < rc2 - 1e-6f){
            /* circle inversion: z' = C + rc^2 (z-C)/|z-C|^2 */
            f32 k = rc2 / (d2 > 1e-9f ? d2 : 1e-9f);
            x = s->cx + ddx*k;
            y = ddy*k;
            inv++; refl++;
            did = 1;
        }
        if (!did) break;   /* stable: inside the fundamental triangle */
    }
    *depth = inv;
    *edge = near_edge;
    return refl;
}

/* Animating hyperbolic translation of the disk (real Möbius map):
 *   w = (z + a) / (1 + a z),  a in (-1,1) real, plus a slow rotation. */
static void animate(f32 t, f32 x, f32 y, f32 *ox, f32 *oy){
    /* rotate first */
    f32 rot = t*0.15f;
    f32 cr=cosf(rot), sr=sinf(rot);
    f32 rx = x*cr - y*sr, ry = x*sr + y*cr;
    /* hyperbolic translation along x by a(t) */
    f32 a = 0.55f*sinf(t*0.35f);
    /* w = (z + a)/(1 + a z), z=(rx,ry), a real */
    f32 nx = rx + a, ny = ry;
    f32 dx = 1.0f + a*rx, dy = a*ry;
    f32 den = dx*dx + dy*dy; if (den < 1e-9f) den = 1e-9f;
    *ox = (nx*dx + ny*dy)/den;
    *oy = (ny*dx - nx*dy)/den;
}

typedef struct { HypState *s; Framebuffer *fb; } HyJob;

static void hy_band(void *ctx, i32 y0, i32 y1){
    HyJob *j = ctx; HypState *s=j->s; Framebuffer *fb=j->fb;
    const i32 w=fb->w, h=fb->h;
    const f32 cx=(w-1)*0.5f, cy=(h-1)*0.5f;
    const f32 R = 0.94f * (cx < cy ? cx : cy);   /* disk radius in pixels */
    const f32 invR = 1.0f/R;
    for (i32 py=y0; py<y1; ++py){
        f32 *row=&fb->px[(size_t)py*w*3];
        for (i32 px=0; px<w; ++px){
            f32 dx=((f32)px-cx)*invR, dy=((f32)py-cy)*invR;
            f32 rr=dx*dx+dy*dy;
            if (rr >= 1.0f){ row[3*px+0]=0.01f; row[3*px+1]=0.01f; row[3*px+2]=0.02f; continue; }
            f32 zx, zy; animate(s->t, dx, dy, &zx, &zy);
            int depth; f32 edge;
            int refl = fold(s, zx, zy, &depth, &edge);
            /* base color: hue from inversion depth, shade by parity */
            f32 hue = 0.58f + 0.045f*depth - 0.03f*s->t;
            f32 val = (refl & 1) ? 0.85f : 0.55f;
            Color3 c = hsv(hue, 0.62f, val);
            /* darken tile boundaries (small edge distance => near a mirror) */
            f32 e = edge*22.0f; if (e>1) e=1;
            f32 line = 0.25f + 0.75f*e;   /* 0.25 at the edge, 1 in the interior */
            c = col_scale(c, line);
            /* vignette toward the disk boundary for a lens feel */
            f32 fade = 1.0f - rr*rr*0.5f;
            row[3*px+0]=c.r*fade; row[3*px+1]=c.g*fade; row[3*px+2]=c.b*fade;
        }
    }
}

static void hy_render(Scene *sc, Framebuffer *fb){
    HypState *s=sc->state;
    HyJob job = { s, fb };
    if (s->pool) tp_run_bands(s->pool, fb->h, hy_band, &job);
    else hy_band(&job, 0, fb->h);
}

static void hy_key(Scene *sc, int key){
    HypState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->which=(s->which+1)%NPQ; hy_geometry(s); }
}
static CrtConfig hy_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void hy_destroy(Scene *sc){
    if(sc){ HypState*s=sc->state; if(s->pool) tp_destroy(s->pool); free(s); free(sc);} }

Scene *scene_hyperbolic_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="hyperbolic";
    sc->description="Animated {p,q} tilings of the hyperbolic plane (Poincare disk)";
    sc->state=calloc(1,sizeof(HypState));
    sc->init=hy_init; sc->update=hy_update; sc->render=hy_render;
    sc->on_key=hy_key; sc->destroy=hy_destroy; sc->preferred_crt=hy_crt;
    return sc;
}
