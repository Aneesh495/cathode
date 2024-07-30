/* ==========================================================================
 * nbody.c — gravitational N-body with a Barnes-Hut octree (O(N log N)).
 *
 * Each step:
 *   1. build an octree bounding all bodies; each node stores total mass and
 *      center of mass.
 *   2. for each body, traverse the tree: if a node is far enough
 *      (size/dist < theta) use its aggregate COM; else recurse. Plummer
 *      softening avoids singularities.
 *   3. kick-drift-kick leapfrog integration.
 * ========================================================================== */
#include "cathode/physics.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- octree node pool ---- */
typedef struct OctNode {
    Vec3 center;         /* cube center */
    f32  half;           /* half-size */
    Vec3 com;            /* center of mass */
    f32  mass;           /* total mass */
    i32  child[8];       /* indices into pool, -1 if empty */
    i32  body;           /* body index if leaf with a single body, else -1 */
    int  is_leaf;
} OctNode;

typedef struct {
    OctNode *nodes;
    i32 count, cap;
} Octree;

static i32 oct_new(Octree *ot, Vec3 c, f32 half){
    if (ot->count >= ot->cap){
        ot->cap = ot->cap ? ot->cap*2 : 1024;
        ot->nodes = (OctNode*)realloc(ot->nodes, sizeof(OctNode)*ot->cap);
    }
    i32 idx = ot->count++;
    OctNode *n = &ot->nodes[idx];
    n->center=c; n->half=half; n->com=v3(0,0,0); n->mass=0.0f;
    for(int i=0;i<8;++i) n->child[i]=-1;
    n->body=-1; n->is_leaf=1;
    return idx;
}

static int octant_of(Vec3 c, Vec3 p){
    int o=0; if(p.x>=c.x)o|=1; if(p.y>=c.y)o|=2; if(p.z>=c.z)o|=4; return o;
}
static Vec3 child_center(Vec3 c, f32 half, int o){
    f32 q=half*0.5f;
    return v3(c.x + ((o&1)?q:-q), c.y + ((o&2)?q:-q), c.z + ((o&4)?q:-q));
}

/* insert body b (pos/mass) into subtree rooted at node index ni */
static void oct_insert(Octree *ot, i32 ni, const Vec3 *pos, const f32 *mass, i32 b){
    OctNode *n = &ot->nodes[ni];
    /* update aggregate mass + COM incrementally */
    f32 m = mass[b];
    f32 newmass = n->mass + m;
    if (newmass > 0.0f){
        n->com = v3_scale(v3_add(v3_scale(n->com, n->mass), v3_scale(pos[b], m)), 1.0f/newmass);
    }
    n->mass = newmass;

    if (n->is_leaf && n->body < 0){
        n->body = b;   /* empty leaf: park the body here */
        return;
    }
    if (n->is_leaf && n->body >= 0){
        /* Two bodies in one leaf. If the cell is already tiny, coincident (or
         * near-coincident) bodies would recurse forever — keep them both
         * aggregated in this leaf (mass/COM already updated) rather than split.
         * Their mutual force is handled by softening. */
        if (n->half < 1e-5f){
            return;
        }
        /* split: push existing body down, then this one.
         * oct_new() may realloc the pool, invalidating `n`, so capture the
         * child params into locals first and write the result back through the
         * re-indexed node (ot->nodes[ni]) — never through the stale `n`. */
        i32 existing = n->body;
        n->body = -1; n->is_leaf = 0;
        int oe = octant_of(n->center, pos[existing]);
        if (n->child[oe] < 0){
            Vec3 cc = child_center(n->center, n->half, oe);
            f32  ch = n->half*0.5f;
            i32  nn = oct_new(ot, cc, ch);   /* may realloc pool */
            ot->nodes[ni].child[oe] = nn;    /* re-index after realloc */
        }
        oct_insert(ot, ot->nodes[ni].child[oe], pos, mass, existing);
    }
    /* internal node: descend into the right octant for b (re-fetch after the
     * recursive insert above, which can also grow the pool). */
    n = &ot->nodes[ni];
    int o = octant_of(n->center, pos[b]);
    if (n->child[o] < 0){
        Vec3 cc = child_center(n->center, n->half, o);
        f32  ch = n->half*0.5f;
        i32  nn = oct_new(ot, cc, ch);
        ot->nodes[ni].child[o] = nn;
    }
    i32 ci = ot->nodes[ni].child[o];
    oct_insert(ot, ci, pos, mass, b);
}

/* accumulate acceleration on body at position p from subtree ni */
static Vec3 oct_accel(const Octree *ot, i32 ni, Vec3 p, i32 self, f32 theta2,
                      f32 g, f32 eps2, const Vec3 *pos){
    const OctNode *n = &ot->nodes[ni];
    if (n->mass <= 0.0f) return v3(0,0,0);

    Vec3 d = v3_sub(n->com, p);
    f32 dist2 = d.x*d.x + d.y*d.y + d.z*d.z + eps2;

    if (n->is_leaf){
        if (n->body == self || n->body < 0) return v3(0,0,0);
        f32 inv = 1.0f/sqrtf(dist2);
        f32 f = g * n->mass * inv*inv*inv; /* = G m / (r^2+eps^2)^(3/2) */
        return v3_scale(d, f);
    }
    /* opening criterion: (size^2 / dist^2) < theta^2 => treat as aggregate */
    f32 size = 2.0f*n->half;
    if ( (size*size) < theta2 * dist2 ){
        f32 inv = 1.0f/sqrtf(dist2);
        f32 f = g * n->mass * inv*inv*inv;
        return v3_scale(d, f);
    }
    Vec3 a = v3(0,0,0);
    for (int i=0;i<8;++i)
        if (n->child[i] >= 0)
            a = v3_add(a, oct_accel(ot, n->child[i], p, self, theta2, g, eps2, pos));
    return a;
}

/* ---- public API ---- */
NBody *nbody_create(i32 capacity){
    NBody *nb = (NBody*)calloc(1,sizeof(NBody));
    nb->cap = capacity>0?capacity:1024;
    nb->pos=(Vec3*)calloc(nb->cap,sizeof(Vec3));
    nb->vel=(Vec3*)calloc(nb->cap,sizeof(Vec3));
    nb->mass=(f32*)calloc(nb->cap,sizeof(f32));
    nb->color=(Color3*)calloc(nb->cap,sizeof(Color3));
    nb->n=0;
    nb->theta=0.6f; nb->softening=0.05f; nb->g=1.0f;
    return nb;
}
void nbody_destroy(NBody *nb){
    if(!nb) return;
    free(nb->pos);free(nb->vel);free(nb->mass);free(nb->color);free(nb);
}
static void nbody_grow(NBody *nb){
    nb->cap*=2;
    nb->pos=(Vec3*)realloc(nb->pos,sizeof(Vec3)*nb->cap);
    nb->vel=(Vec3*)realloc(nb->vel,sizeof(Vec3)*nb->cap);
    nb->mass=(f32*)realloc(nb->mass,sizeof(f32)*nb->cap);
    nb->color=(Color3*)realloc(nb->color,sizeof(Color3)*nb->cap);
}
void nbody_add(NBody *nb, Vec3 pos, Vec3 vel, f32 mass, Color3 c){
    if (nb->n>=nb->cap) nbody_grow(nb);
    nb->pos[nb->n]=pos; nb->vel[nb->n]=vel; nb->mass[nb->n]=mass; nb->color[nb->n]=c;
    nb->n++;
}

void nbody_seed_galaxy(NBody *nb, i32 count, Vec3 center, f32 radius, f32 central_mass){
    Rng rng; rng_seed(&rng, 0x6A1A29ULL ^ (u64)count);
    /* central massive body */
    nbody_add(nb, center, v3(0,0,0), central_mass, col3(1.0f,0.95f,0.8f));
    for (i32 i=0;i<count;++i){
        /* sample radius with a falloff toward center */
        f32 u = rng_f32(&rng);
        f32 r = radius * powf(u, 0.5f) * 0.95f + 0.05f*radius;
        f32 ang = rng_f32(&rng)*CT_TAU;
        f32 thick = rng_normal(&rng)*radius*0.03f;
        Vec3 p = v3(center.x + r*cosf(ang), center.y + thick, center.z + r*sinf(ang));
        /* circular orbital velocity v = sqrt(G M / r), perpendicular in-plane */
        f32 v = sqrtf(nb->g * central_mass / r);
        Vec3 vel = v3(-sinf(ang)*v, 0, cosf(ang)*v);
        /* small dispersion */
        vel = v3_add(vel, v3(rng_normal(&rng)*v*0.03f, rng_normal(&rng)*v*0.02f, rng_normal(&rng)*v*0.03f));
        f32 rr = r/radius;
        Color3 col = col_lerp(col3(0.6f,0.75f,1.0f), col3(1.0f,0.7f,0.4f), rr);
        nbody_add(nb, p, vel, 1.0f, col);
    }
}

void nbody_seed_collision(NBody *nb, i32 per_galaxy){
    f32 cm = (f32)per_galaxy * 4.0f;
    /* galaxy A */
    Vec3 ca = v3(-3.5f, 0.3f, 0);
    nbody_seed_galaxy(nb, per_galaxy, ca, 2.2f, cm);
    /* give galaxy A a bulk velocity toward B */
    i32 a_start = 0, a_end = nb->n;
    for (i32 i=a_start;i<a_end;++i) nb->vel[i]=v3_add(nb->vel[i], v3(0.6f,0,0.2f));
    /* galaxy B */
    i32 b_start = nb->n;
    Vec3 cb = v3(3.5f,-0.3f,0);
    nbody_seed_galaxy(nb, per_galaxy, cb, 2.2f, cm);
    for (i32 i=b_start;i<nb->n;++i) nb->vel[i]=v3_add(nb->vel[i], v3(-0.6f,0,-0.2f));
}

/* compute accelerations for all bodies into out[] via a fresh Barnes-Hut tree */
static void compute_accel(NBody *nb, Vec3 *out){
    /* bounding cube */
    Vec3 lo=nb->pos[0], hi=nb->pos[0];
    for (i32 i=1;i<nb->n;++i){ lo=v3_min(lo,nb->pos[i]); hi=v3_max(hi,nb->pos[i]); }
    Vec3 center=v3_scale(v3_add(lo,hi),0.5f);
    Vec3 ext=v3_sub(hi,lo);
    f32 half=0.5f*ct_maxf(ext.x,ct_maxf(ext.y,ext.z))+1e-3f;

    Octree ot; ot.nodes=NULL; ot.count=0; ot.cap=0;
    i32 root=oct_new(&ot, center, half);
    for (i32 i=0;i<nb->n;++i) oct_insert(&ot, root, nb->pos, nb->mass, i);

    f32 theta2 = nb->theta*nb->theta;
    f32 eps2 = nb->softening*nb->softening;
    for (i32 i=0;i<nb->n;++i)
        out[i] = oct_accel(&ot, root, nb->pos[i], i, theta2, nb->g, eps2, nb->pos);

    free(ot.nodes);
}

void nbody_step(NBody *nb, f32 dt){
    if (nb->n==0) return;
    static Vec3 *acc = NULL; static i32 acc_cap = 0;
    if (acc_cap < nb->n){ acc_cap = nb->n; acc = (Vec3*)realloc(acc, sizeof(Vec3)*acc_cap); }
    /* kick-drift-kick leapfrog */
    compute_accel(nb, acc);
    for (i32 i=0;i<nb->n;++i){
        nb->vel[i]=v3_add(nb->vel[i], v3_scale(acc[i], dt*0.5f));   /* half kick */
        nb->pos[i]=v3_add(nb->pos[i], v3_scale(nb->vel[i], dt));    /* drift */
    }
    compute_accel(nb, acc);
    for (i32 i=0;i<nb->n;++i)
        nb->vel[i]=v3_add(nb->vel[i], v3_scale(acc[i], dt*0.5f));   /* half kick */
}

void nbody_render(const NBody *nb, Framebuffer *fb, Mat4 view, Mat4 proj){
    Mat4 vp = mat4_mul(proj, view);
    for (i32 i=0;i<nb->n;++i){
        Vec4 clip = mat4_mul_v4(vp, v4_from_v3(nb->pos[i],1.0f));
        if (clip.w <= 1e-4f) continue;
        f32 iw=1.0f/clip.w;
        f32 sx=(clip.x*iw*0.5f+0.5f)*fb->w;
        f32 sy=(1.0f-(clip.y*iw*0.5f+0.5f))*fb->h;
        if (sx<0||sx>=fb->w||sy<0||sy>=fb->h) continue;
        /* brightness by mass, gently dimmed by depth. Heavy central bodies
         * (mass >> 1) get a bright hot core plus an additive halo. */
        f32 depthdim = ct_clampf(2.5f/(1.0f+clip.w*0.04f),0.25f,1.4f);
        f32 heavy = ct_clampf(nb->mass[i]*0.02f, 0.0f, 1.0f);
        f32 b = (1.2f + 3.0f*heavy) * depthdim;
        Color3 c = col_scale(nb->color[i], b);
        fb_splat(fb, sx, sy, c);
        /* halo for massive cores so galactic centers glow */
        if (heavy > 0.2f){
            Color3 halo = col_scale(nb->color[i], b*0.25f);
            fb_splat(fb, sx+1, sy,   halo); fb_splat(fb, sx-1, sy,   halo);
            fb_splat(fb, sx,   sy+1, halo); fb_splat(fb, sx,   sy-1, halo);
        }
    }
}
