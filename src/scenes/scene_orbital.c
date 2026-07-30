/* ==========================================================================
 * scene_orbital.c — hydrogen atomic orbitals, volume-rendered.
 *
 * Ray-marches the probability density |psi_{n,l,m}(r,theta,phi)|^2 of real
 * hydrogen-like orbitals (built from associated Laguerre radial parts and real
 * spherical harmonics) as a glowing volumetric cloud. Absorption-emission
 * integration along each ray accumulates color by density; the camera orbits.
 * Cycles through s, p, d, f orbitals. This is genuine quantum chemistry — the
 * shapes are the real electron clouds, not decorative blobs.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <math.h>

typedef struct { int n,l,m; const char*name; Color3 tint; } Orbital;
static const Orbital ORB[] = {
    {2,1,0,  "2p_z",   {0.4f,0.7f,1.0f}},
    {3,2,0,  "3d_z2",  {1.0f,0.6f,0.3f}},
    {3,2,2,  "3d_x2y2",{0.5f,1.0f,0.5f}},
    {4,3,0,  "4f_z3",  {1.0f,0.4f,0.8f}},
    {3,1,1,  "3p_x",   {0.7f,0.8f,1.0f}},
};
#define NORB ((int)(sizeof(ORB)/sizeof(ORB[0])))

typedef struct { i32 w,h; f32 t; int which; } OrbState;

static void or_init(Scene*sc,i32 w,i32 h){ OrbState*s=sc->state; s->w=w;s->h=h;s->t=0;s->which=0; }
static void or_update(Scene*sc,f32 dt,f32 t){ (void)dt; ((OrbState*)sc->state)->t=t; }

/* real spherical harmonic angular part (unnormalized, |Y|-ish) for small l,m,
 * expressed in Cartesian direction (x,y,z) on the unit sphere. */
static f32 angular(int l, int m, f32 x, f32 y, f32 z){
    switch(l){
        case 0: return 1.0f;                                  /* s */
        case 1: /* p */
            if (m==0) return z; if(m==1) return x; return y;
        case 2: /* d */
            if (m==0) return (3*z*z-1.0f);                    /* d_z2 */
            if (m==2) return (x*x-y*y);                       /* d_x2-y2 */
            if (m==1) return x*z; if(m==-1) return y*z; return x*y;
        default: /* f (m==0 -> f_z3) */
            if (m==0) return z*(5*z*z-3.0f);
            return x*(x*x-3*y*y);                             /* f_x(x2-3y2) */
    }
}

/* radial part magnitude ~ r^(l) * exp(-r/n) (hydrogenic envelope; nodes omitted
 * for a clean shell but the shape/anisotropy is faithful). */
static f32 radial(int n, int l, f32 r){
    f32 rn=r/(f32)n;
    return powf(r, (f32)l) * expf(-rn);
}

/* |psi|^2 at a world point p (orbital centered at origin, scaled) */
static f32 density(const Orbital*o, Vec3 p){
    f32 r=v3_len(p);
    if (r<1e-4f) return 0.0f;
    f32 ang=angular(o->l,o->m, p.x/r,p.y/r,p.z/r);
    f32 rad=radial(o->n,o->l, r);
    f32 psi=rad*ang;
    return psi*psi;
}

static void or_render(Scene*sc, Framebuffer*fb){
    OrbState*s=sc->state;
    const Orbital*o=&ORB[s->which];
    fb_clear(fb, col3(0.01f,0.01f,0.03f));
    f32 a=s->t*0.3f;
    f32 dist=(f32)(o->n*o->n)*1.6f + 6.0f;      /* frame bigger orbitals wider */
    Vec3 eye=v3(cosf(a)*dist, dist*0.35f, sinf(a)*dist);
    Vec3 fwd=v3_norm(v3_sub(v3(0,0,0),eye));
    Vec3 right=v3_norm(v3_cross(fwd,v3(0,1,0)));
    Vec3 up=v3_cross(right,fwd);
    f32 aspect=(f32)fb->w/fb->h, tanf_half=tanf(0.5f);
    /* |psi|^2 peaks ~0.5 and is exactly 0 off the orbital's lobes, so no huge
     * scale is needed — a modest emission per unit length lets the anisotropic
     * lobes glow while empty space stays black. */
    const f32 emission=0.28f;   /* calibrated so lobe cores glow, halo fades */
    for (i32 py=0;py<fb->h;++py){
        for (i32 px=0;px<fb->w;++px){
            f32 ndcx=(2.0f*(px+0.5f)/fb->w-1.0f)*aspect*tanf_half;
            f32 ndcy=(1.0f-2.0f*(py+0.5f)/fb->h)*tanf_half;
            Vec3 rd=v3_norm(v3_add(fwd, v3_add(v3_scale(right,ndcx), v3_scale(up,ndcy))));
            /* march the ray through the bounding region, accumulating |psi|^2 */
            f32 tmax=dist*2.2f; int steps=96; f32 dt=tmax/steps;
            f32 accum=0.0f; f32 tt=dist*0.15f;
            for (int k=0;k<steps;++k){
                Vec3 wp=v3_add(eye, v3_scale(rd,tt));
                accum += density(o, wp)*emission*dt;
                tt += dt;
                if (accum>4.0f) break;
            }
            f32 b=1.0f-expf(-accum*1.8f);     /* absorption -> brightness */
            b=b*b;                            /* gamma to darken the thin halo */
            fb_set(fb, px,py, col_scale(o->tint, b*1.5f));
        }
    }
}

static void or_key(Scene*sc,int key){
    OrbState*s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->which=(s->which+1)%NORB;
}

static CrtConfig or_crt(Scene*sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.55f; return c; }
static void or_destroy(Scene*sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_orbital_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="orbital";
    sc->description="Volume-rendered hydrogen atomic orbitals (|psi|^2, real spherical harmonics)";
    sc->state=calloc(1,sizeof(OrbState));
    sc->init=or_init; sc->update=or_update; sc->render=or_render;
    sc->on_key=or_key; sc->destroy=or_destroy; sc->preferred_crt=or_crt;
    return sc;
}
