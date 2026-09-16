/* ==========================================================================
 * scene_qjulia.c  -  ray-marched quaternion Julia set (a true 4D fractal).
 *
 * The quaternion Julia set is the filled Julia set of z -> z^2 + c over the
 * quaternions H (a 4D division algebra). We fix the 4D point's w-slice and
 * march rays through the remaining 3D slice, using the classic Hart-Sandin
 * distance estimator:
 *
 *     DE(z0) = 0.5 * |z_n| * ln|z_n| / |z'_n|
 *
 * where z_{k+1} = z_k^2 + c and the running derivative z'_{k+1} = 2 z_k z'_k
 * (both quaternion products). This gives an unbiased lower bound on the
 * distance to the set, so sphere tracing converges. Normals come from the
 * gradient of the (approximate) potential via central differences of the
 * escape radius. The Julia constant c animates on a slow path through
 * quaternion space, morphing the fractal continuously.
 *
 * Everything is per-pixel CPU work, split across cores by the thread pool.
 * Output is linear HDR so the CRT bloom lights the rim.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "threadpool.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define QJ_ITERS 12          /* Julia iterations for the distance estimate */

typedef struct { f32 x, y, z, w; } Quat4;

typedef struct {
    i32 w, h; f32 t;
    Quat4 c;                 /* animating Julia constant */
    int preset;
    ThreadPool *pool;
} QJState;

/* quaternion product a*b (Hamilton) */
static inline Quat4 qmul(Quat4 a, Quat4 b){
    return (Quat4){
        a.x*b.x - a.y*b.y - a.z*b.z - a.w*b.w,
        a.x*b.y + a.y*b.x + a.z*b.w - a.w*b.z,
        a.x*b.z - a.y*b.w + a.z*b.x + a.w*b.y,
        a.x*b.w + a.y*b.z - a.z*b.y + a.w*b.x
    };
}
static inline Quat4 qadd(Quat4 a, Quat4 b){ return (Quat4){a.x+b.x,a.y+b.y,a.z+b.z,a.w+b.w}; }
static inline f32   qdot(Quat4 a){ return a.x*a.x+a.y*a.y+a.z*a.z+a.w*a.w; }

/* Distance estimate for the quaternion Julia set at 3D point p (w=fixed). */
static f32 qjulia_de(Quat4 z, Quat4 c){
    Quat4 zp = {1,0,0,0};    /* running derivative z' */
    f32 md2 = 1.0f;          /* |z'|^2 */
    f32 mz2 = qdot(z);
    for (int i=0;i<QJ_ITERS;++i){
        /* z' = 2 * z * z' ; z = z^2 + c */
        zp = qmul(z, zp);
        zp.x *= 2; zp.y *= 2; zp.z *= 2; zp.w *= 2;
        z = qadd(qmul(z, z), c);
        md2 = qdot(zp);
        mz2 = qdot(z);
        if (mz2 > 16.0f) break;   /* escaped */
    }
    /* DE = 0.5 * |z| * ln|z| / |z'| */
    f32 mz = sqrtf(mz2);
    f32 md = sqrtf(md2);
    if (md < 1e-12f) md = 1e-12f;
    return 0.5f * mz * logf(mz > 1.0f ? mz : 1.0001f) / md;
}

static void qj_init(Scene *sc, i32 w, i32 h){
    QJState *s=sc->state; s->w=w; s->h=h; s->t=0; s->preset=0;
    if (!s->pool) s->pool = tp_create(0);
}
static void qj_update(Scene *sc, f32 dt, f32 t){
    (void)dt; QJState *s=sc->state; s->t=t;
    /* animate c on a gentle Lissajous path inside the interesting region */
    f32 a=t*0.15f;
    /* classic quaternion-Julia constants, gently modulated so the fractal
     * breathes without leaving its interesting regime */
    switch (s->preset){
        case 1: s->c=(Quat4){ -0.291f, -0.399f+0.03f*sinf(a), 0.339f, 0.437f+0.03f*cosf(a) }; break;
        case 2: s->c=(Quat4){ -0.2f, 0.4f+0.04f*sinf(a), -0.4f, -0.4f }; break;
        default:s->c=(Quat4){ -0.45f+0.04f*cosf(a), -0.3f+0.05f*sinf(a*1.1f), -0.15f, 0.2f+0.03f*cosf(a*0.7f) }; break;
    }
}

/* central-difference normal of the DE field */
static Vec3 qj_normal(Vec3 p, Quat4 c, f32 wslice){
    const f32 e=0.0015f;
    Quat4 px1={p.x+e,p.y,p.z,wslice}, px0={p.x-e,p.y,p.z,wslice};
    Quat4 py1={p.x,p.y+e,p.z,wslice}, py0={p.x,p.y-e,p.z,wslice};
    Quat4 pz1={p.x,p.y,p.z+e,wslice}, pz0={p.x,p.y,p.z-e,wslice};
    Vec3 n = { qjulia_de(px1,c)-qjulia_de(px0,c),
               qjulia_de(py1,c)-qjulia_de(py0,c),
               qjulia_de(pz1,c)-qjulia_de(pz0,c) };
    return v3_norm(n);
}

typedef struct { QJState *s; Framebuffer *fb; } QJJob;

static void qj_band(void *ctx, i32 y0, i32 y1){
    QJJob *j=ctx; QJState *s=j->s; Framebuffer *fb=j->fb;
    const i32 w=fb->w, h=fb->h;
    const f32 aspect=(f32)w/h;
    const Quat4 c=s->c;
    const f32 wslice=0.0f;

    /* orbiting camera */
    f32 ca=s->t*0.25f;
    Vec3 eye = { cosf(ca)*2.6f, 1.1f+0.5f*sinf(ca*0.5f), sinf(ca)*2.6f };
    Vec3 tgt = {0,0,0};
    Vec3 fwd = v3_norm(v3_sub(tgt, eye));
    Vec3 right = v3_norm(v3_cross(fwd, v3(0,1,0)));
    Vec3 up = v3_cross(right, fwd);
    Vec3 ldir = v3_norm(v3(0.5f,0.8f,0.3f));

    for (i32 py=y0; py<y1; ++py){
        f32 *row=&fb->px[(size_t)py*w*3];
        for (i32 px=0; px<w; ++px){
            f32 u=(2.0f*((f32)px+0.5f)/w - 1.0f)*aspect;
            f32 v=1.0f - 2.0f*((f32)py+0.5f)/h;
            Vec3 rd = v3_norm(v3_add(fwd, v3_add(v3_scale(right,u*0.6f), v3_scale(up,v*0.6f))));

            /* sphere-trace */
            f32 tt=0.0f; int hit=0; Vec3 pos={0,0,0};
            for (int step=0; step<90; ++step){
                pos = v3_add(eye, v3_scale(rd, tt));
                if (qdot((Quat4){pos.x,pos.y,pos.z,wslice}) > 12.0f && tt>0.5f) break; /* left bounds */
                Quat4 z={pos.x,pos.y,pos.z,wslice};
                f32 d = qjulia_de(z, c);
                if (d < 0.0008f){ hit=1; break; }
                tt += d*0.9f;
                if (tt > 8.0f) break;
            }

            Color3 col;
            if (hit){
                Vec3 nrm = qj_normal(pos, c, wslice);
                f32 diff = v3_dot(nrm, ldir); if (diff<0) diff=0;
                Vec3 h_ = v3_norm(v3_sub(ldir, rd));
                f32 spec = powf(fmaxf(v3_dot(nrm,h_),0.0f), 32.0f);
                /* color by surface orientation + distance for depth */
                f32 hue = 0.6f + 0.15f*nrm.y + 0.1f*sinf(s->t*0.3f);
                Color3 base = col3(0.5f+0.5f*sinf(hue*6.28f),
                                   0.5f+0.5f*sinf(hue*6.28f+2.09f),
                                   0.5f+0.5f*sinf(hue*6.28f+4.18f));
                f32 ao = 1.0f - (f32)0.0f; /* (kept simple) */
                col = col3(base.r*(0.15f+0.85f*diff)+spec,
                           base.g*(0.15f+0.85f*diff)+spec,
                           base.b*(0.15f+0.85f*diff)+spec);
                col = col_scale(col, ao);
                /* rim glow toward silhouette for HDR bloom */
                f32 rim = powf(1.0f - fmaxf(v3_dot(nrm, v3_neg(rd)),0.0f), 3.0f);
                col = col_add(col, col_scale(base, rim*0.8f));
            } else {
                /* background: soft vertical gradient */
                f32 g=0.03f+0.05f*(1.0f-(f32)py/h);
                col = col3(g*0.4f, g*0.5f, g*0.9f);
            }
            row[3*px+0]=col.r; row[3*px+1]=col.g; row[3*px+2]=col.b;
        }
    }
}

static void qj_render(Scene *sc, Framebuffer *fb){
    QJState *s=sc->state;
    QJJob job={s,fb};
    if (s->pool) tp_run_bands(s->pool, fb->h, qj_band, &job);
    else qj_band(&job, 0, fb->h);
}

static void qj_key(Scene *sc, int key){
    QJState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->preset=(s->preset+1)%3;
}
static CrtConfig qj_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void qj_destroy(Scene *sc){ if(sc){ QJState*s=sc->state; if(s->pool)tp_destroy(s->pool); free(s); free(sc);} }

Scene *scene_qjulia_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="qjulia";
    sc->description="Ray-marched quaternion Julia set (4D fractal, distance-estimated)";
    sc->state=calloc(1,sizeof(QJState));
    sc->init=qj_init; sc->update=qj_update; sc->render=qj_render;
    sc->on_key=qj_key; sc->destroy=qj_destroy; sc->preferred_crt=qj_crt;
    return sc;
}
