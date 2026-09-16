/* ==========================================================================
 * rigidbody.c  -  2D impulse-based rigid-body dynamics over convex polygons.
 *
 * A compact sequential-impulse solver in the Box2D-lite tradition:
 *
 *   1. Integrate forces: v += g*dt for every dynamic body.
 *   2. Broad+narrow phase: for each body pair (and each body vs the static
 *      floor/walls), run the Separating-Axis Test on the two convex hulls; if
 *      they overlap, generate up to two contact points with a penetration depth
 *      and a contact normal (clipped-incident-edge, the standard method).
 *   3. Solve: for `iterations` passes, apply normal impulses (with restitution)
 *      and Coulomb-clamped friction impulses at each contact, using the
 *      relative velocity at the contact and the bodies' inverse mass/inertia.
 *      A Baumgarte positional bias removes residual penetration without adding
 *      energy explosively.
 *   4. Integrate velocities into positions and angles.
 *
 * Everything is 2D; a body's orientation is a single angle, inertia a scalar.
 * The world is a box [0,w] x [0,h] with a floor at y=0 and vertical side walls,
 * modeled as immovable half-planes (infinite mass) so bodies pile up on them.
 * ========================================================================== */
#include "cathode/physics.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAXV 8          /* max polygon vertices per body */

typedef struct {
    Vec2 v[MAXV]; int nv;   /* local-space vertices (centered on the CoM) */
    Vec2 pos; f32 ang;      /* world position + orientation */
    Vec2 vel; f32 w;        /* linear + angular velocity */
    f32  inv_m, inv_I;      /* inverse mass + inertia (0 => static) */
    f32  restitution, friction;
    int  is_static;
} Body;

struct RigidWorld {
    Body *b; int n, cap;
    f32 W, H;
    Vec2 gravity;
};

/* --- 2D vector helpers (Vec2 lives in types.h/vec.h) --- */
static inline Vec2 v2(f32 x, f32 y){ Vec2 r={x,y}; return r; }
static inline Vec2 v2add(Vec2 a, Vec2 b){ return v2(a.x+b.x,a.y+b.y); }
static inline Vec2 v2sub(Vec2 a, Vec2 b){ return v2(a.x-b.x,a.y-b.y); }
static inline Vec2 v2scale(Vec2 a, f32 s){ return v2(a.x*s,a.y*s); }
static inline f32  v2dot(Vec2 a, Vec2 b){ return a.x*b.x+a.y*b.y; }
static inline f32  v2cross(Vec2 a, Vec2 b){ return a.x*b.y - a.y*b.x; }   /* scalar */
static inline Vec2 v2crosssv(f32 s, Vec2 v){ return v2(-s*v.y, s*v.x); }  /* s × v */
static inline f32  v2len(Vec2 a){ return sqrtf(a.x*a.x+a.y*a.y); }
static inline Vec2 v2norm(Vec2 a){ f32 l=v2len(a); return l>1e-9f? v2scale(a,1.0f/l):a; }
static inline Vec2 v2rot(Vec2 a, f32 c, f32 s){ return v2(a.x*c-a.y*s, a.x*s+a.y*c); }

RigidWorld *rb_create(f32 w, f32 h, i32 capacity){
    if (w<=0||h<=0||capacity<=0) return NULL;
    RigidWorld *rw=calloc(1,sizeof(RigidWorld));
    if(!rw) return NULL;
    rw->b=calloc((size_t)capacity,sizeof(Body));
    if(!rw->b){ free(rw); return NULL; }
    rw->cap=capacity; rw->n=0; rw->W=w; rw->H=h;
    rw->gravity=v2(0.0f,-9.8f);
    return rw;
}
void rb_destroy(RigidWorld *rw){ if(rw){ free(rw->b); free(rw); } }
void rb_set_gravity(RigidWorld *rw, f32 gx, f32 gy){ if(rw) rw->gravity=v2(gx,gy); }
i32  rb_count(const RigidWorld *rw){ return rw? rw->n:0; }
f32  rb_domain_w(const RigidWorld *rw){ return rw? rw->W:0; }
f32  rb_domain_h(const RigidWorld *rw){ return rw? rw->H:0; }

/* Compute polygon area, centroid, and inertia (about centroid) for unit
 * density; used to derive mass/inertia and to re-center vertices on the CoM. */
static void poly_mass(const Vec2 *v, int n, f32 density,
                      f32 *mass, f32 *inertia, Vec2 *centroid){
    Vec2 c=v2(0,0); f32 area=0, I=0;
    for (int i=0;i<n;++i){
        Vec2 p1=v[i], p2=v[(i+1)%n];
        f32 cr=v2cross(p1,p2);
        f32 a=0.5f*cr; area+=a;
        c=v2add(c, v2scale(v2add(p1,p2), cr*(1.0f/3.0f)));
        f32 intx2 = p1.x*p1.x + p1.x*p2.x + p2.x*p2.x;
        f32 inty2 = p1.y*p1.y + p1.y*p2.y + p2.y*p2.y;
        I += (0.25f/3.0f)*cr*(intx2+inty2);
    }
    if (fabsf(area)<1e-9f) area=1e-9f;
    c=v2scale(c, 1.0f/area);
    *centroid=c;
    *mass = density*area;
    /* shift inertia to centroid (parallel axis) */
    f32 Ic = density*I - (*mass)*(c.x*c.x+c.y*c.y);
    *inertia = fabsf(Ic);
}

static i32 add_body(RigidWorld *rw, const Vec2 *verts, int nv, f32 x, f32 y,
                    f32 ang, f32 density){
    if (!rw || rw->n>=rw->cap || nv<3 || nv>MAXV) return -1;
    Body *bd=&rw->b[rw->n];
    memset(bd,0,sizeof(*bd));
    f32 mass, inertia; Vec2 cen;
    poly_mass(verts, nv, density>0?density:1.0f, &mass, &inertia, &cen);
    /* store vertices relative to centroid */
    for (int i=0;i<nv;++i) bd->v[i]=v2sub(verts[i],cen);
    bd->nv=nv;
    bd->pos=v2(x+cen.x, y+cen.y);
    bd->ang=ang;
    bd->vel=v2(0,0); bd->w=0;
    bd->restitution=0.15f; bd->friction=0.4f;
    if (density<=0.0f){ bd->is_static=1; bd->inv_m=0; bd->inv_I=0; }
    else { bd->inv_m=1.0f/mass; bd->inv_I= inertia>1e-9f? 1.0f/inertia:0.0f; }
    return rw->n++;
}

i32 rb_add_box(RigidWorld *rw, f32 x, f32 y, f32 hx, f32 hy, f32 ang, f32 density){
    Vec2 v[4]={ v2(-hx,-hy), v2(hx,-hy), v2(hx,hy), v2(-hx,hy) };
    return add_body(rw, v, 4, x, y, ang, density);
}
i32 rb_add_ngon(RigidWorld *rw, f32 x, f32 y, i32 sides, f32 r, f32 ang, f32 density){
    if (sides<3) sides=3; if (sides>MAXV) sides=MAXV;
    Vec2 v[MAXV];
    for (int i=0;i<sides;++i){
        f32 a=CT_TAU*(f32)i/sides;
        v[i]=v2(r*cosf(a), r*sinf(a));
    }
    return add_body(rw, v, sides, x, y, ang, density);
}

/* world-space vertex i of body b */
static Vec2 world_vert(const Body *b, int i){
    f32 c=cosf(b->ang), s=sinf(b->ang);
    return v2add(b->pos, v2rot(b->v[i], c, s));
}

void rb_body_poly(const RigidWorld *rw, i32 i, f32 *out_xy, i32 maxv,
                  i32 *out_n, f32 *cx, f32 *cy, f32 *ang){
    if (!rw||i<0||i>=rw->n) { if(out_n)*out_n=0; return; }
    const Body *b=&rw->b[i];
    int n=b->nv<maxv?b->nv:maxv;
    for (int k=0;k<n;++k){ Vec2 w=world_vert(b,k); out_xy[2*k]=w.x; out_xy[2*k+1]=w.y; }
    if(out_n)*out_n=n; if(cx)*cx=b->pos.x; if(cy)*cy=b->pos.y; if(ang)*ang=b->ang;
}

f32 rb_total_energy(const RigidWorld *rw){
    f32 e=0;
    for (int i=0;i<rw->n;++i){
        const Body *b=&rw->b[i];
        if (b->is_static) continue;
        f32 m = b->inv_m>0? 1.0f/b->inv_m:0;
        f32 I = b->inv_I>0? 1.0f/b->inv_I:0;
        e += 0.5f*m*v2dot(b->vel,b->vel) + 0.5f*I*b->w*b->w;
    }
    return e;
}

/* ---- SAT between two convex polys; returns 1 + fills normal/depth ---- */
/* project poly onto axis */
static void proj(const Vec2 *v, int n, Vec2 ax, f32 *mn, f32 *mx){
    f32 d=v2dot(v[0],ax); *mn=*mx=d;
    for (int i=1;i<n;++i){ d=v2dot(v[i],ax); if(d<*mn)*mn=d; if(d>*mx)*mx=d; }
}
/* Over all of A's edge normals, find the axis of MINIMUM overlap between the
 * two polygons' projections. Returns that minimum overlap and its axis. If any
 * axis shows a gap (overlap < 0) we return that negative value immediately  - 
 * the shapes are separated and no contact should be generated. */
static f32 axis_least_pen(const Vec2 *a,int na,const Vec2 *b,int nb, Vec2 *axis){
    f32 best=1e30f; Vec2 bestax=v2(0,0);
    for (int i=0;i<na;++i){
        Vec2 e=v2sub(a[(i+1)%na], a[i]);
        Vec2 nrm=v2norm(v2(e.y,-e.x));   /* outward normal (CCW winding) */
        f32 amn,amx,bmn,bmx;
        proj(a,na,nrm,&amn,&amx); proj(b,nb,nrm,&bmn,&bmx);
        /* signed overlap of the two intervals along this axis */
        f32 ov = (amx<bmx?amx:bmx) - (amn>bmn?amn:bmn);
        if (ov < 0){ *axis=nrm; return ov; }   /* a separating axis exists */
        if (ov < best){ best=ov; bestax=nrm; }
    }
    *axis=bestax; return best;
}

typedef struct { Vec2 point; f32 depth; } Contact;

/* Full SAT: returns number of contacts (0,1,2), normal points from A to B. */
static int collide(const Body *A, const Body *B, Vec2 *normal, Contact *cts){
    Vec2 wa[MAXV], wb[MAXV];
    for (int i=0;i<A->nv;++i) wa[i]=world_vert(A,i);
    for (int i=0;i<B->nv;++i) wb[i]=world_vert(B,i);

    Vec2 axA, axB;
    f32 penA=axis_least_pen(wa,A->nv,wb,B->nv,&axA);
    if (penA < 0) return 0;   /* separated */
    f32 penB=axis_least_pen(wb,B->nv,wa,A->nv,&axB);
    if (penB < 0) return 0;

    /* choose the reference face (smaller penetration => that separating dir) */
    Vec2 n;
    f32 pen;
    if (penA <= penB){ n=axA; pen=penA; }
    else { n=v2scale(axB,-1.0f); pen=penB; }   /* flip so normal is A->B */
    /* ensure normal points from A center to B center */
    Vec2 d=v2sub(B->pos, A->pos);
    if (v2dot(d,n) < 0) n=v2scale(n,-1.0f);
    *normal=n;

    /* crude but effective contact point: the deepest vertex of B into A and of
     * A into B along -n / n. Two points give stacking stability. */
    int nc=0;
    /* vertex of B most opposite to n (deepest into A) */
    f32 best=1e30f; int bi=0;
    for (int i=0;i<B->nv;++i){ f32 dd=v2dot(wb[i],n); if(dd<best){best=dd;bi=i;} }
    cts[nc].point=wb[bi]; cts[nc].depth=pen; nc++;
    /* also its neighbor if nearly as deep (edge contact => stable stack) */
    int bn=(bi+1)%B->nv, bp=(bi-1+B->nv)%B->nv;
    f32 dn=v2dot(wb[bn],n), dp=v2dot(wb[bp],n);
    int other = dn<dp? bn:bp;
    if (fabsf((dn<dp?dn:dp) - best) < 0.15f*(1.0f+fabsf(best))){
        cts[nc].point=wb[other]; cts[nc].depth=pen; nc++;
    }
    return nc;
}

/* apply an impulse P at world point p on body b */
static void apply_impulse(Body *b, Vec2 P, Vec2 r){
    if (b->is_static) return;
    b->vel=v2add(b->vel, v2scale(P, b->inv_m));
    b->w += b->inv_I * v2cross(r, P);
}

/* Restitution is only meaningful for genuine impacts. Below this approach
 * speed a contact is "resting" and must not bounce, or a stack pumps energy. */
#define REST_VN_THRESHOLD 1.0f

/* Velocity solve for one contact: restitution impulse + Coulomb friction. No
 * positional bias here  -  penetration is fixed by a separate position pass, so
 * this loop can only ever REMOVE kinetic energy and is unconditionally stable. */
static void solve_contact(Body *A, Body *B, Vec2 n, Contact ct, f32 dt, f32 inv_dt){
    (void)dt; (void)inv_dt;
    Vec2 rA=v2sub(ct.point, A->pos);
    Vec2 rB=v2sub(ct.point, B->pos);
    Vec2 vA=v2add(A->vel, v2crosssv(A->w, rA));
    Vec2 vB=v2add(B->vel, v2crosssv(B->w, rB));
    Vec2 rv=v2sub(vB, vA);
    f32 vn=v2dot(rv,n);
    if (vn > 0) return;   /* separating */

    f32 rnA=v2cross(rA,n), rnB=v2cross(rB,n);
    f32 kn = A->inv_m+B->inv_m + A->inv_I*rnA*rnA + B->inv_I*rnB*rnB;
    if (kn<1e-9f) return;
    f32 e = A->restitution<B->restitution?A->restitution:B->restitution;
    if (vn > -REST_VN_THRESHOLD) e = 0.0f;   /* resting contact: no bounce */
    f32 jn = -(1.0f+e)*vn/kn;
    if (jn < 0) jn = 0;
    Vec2 Pn=v2scale(n, jn);
    apply_impulse(A, v2scale(Pn,-1.0f), rA);
    apply_impulse(B, Pn, rB);

    /* friction along the tangent, Coulomb-clamped to mu*jn */
    Vec2 t=v2(-n.y, n.x);
    vA=v2add(A->vel, v2crosssv(A->w, rA));
    vB=v2add(B->vel, v2crosssv(B->w, rB));
    rv=v2sub(vB,vA);
    f32 vt=v2dot(rv,t);
    f32 rtA=v2cross(rA,t), rtB=v2cross(rB,t);
    f32 kt=A->inv_m+B->inv_m + A->inv_I*rtA*rtA + B->inv_I*rtB*rtB;
    if (kt<1e-9f) return;
    f32 jt=-vt/kt;
    f32 mu=sqrtf(A->friction*B->friction);
    f32 maxf=mu*jn;
    if (jt> maxf) jt=maxf; if (jt<-maxf) jt=-maxf;
    Vec2 Pt=v2scale(t,jt);
    apply_impulse(A, v2scale(Pt,-1.0f), rA);
    apply_impulse(B, Pt, rB);
}

/* Position correction: directly translate the two bodies out of penetration
 * along the normal, split by inverse mass. Editing position (not velocity)
 * adds no kinetic energy, so stacks settle instead of exploding. */
static void correct_positions(Body *A, Body *B, Vec2 n, f32 depth){
    const f32 slop=0.01f, beta=0.25f;
    f32 corr = fmaxf(depth-slop, 0.0f) * beta;
    if (corr <= 0) return;
    f32 im = A->inv_m + B->inv_m;
    if (im < 1e-9f) return;
    Vec2 c = v2scale(n, corr/im);
    A->pos = v2sub(A->pos, v2scale(c, A->inv_m));
    B->pos = v2add(B->pos, v2scale(c, B->inv_m));
}

/* Resolve a dynamic body against a static half-plane (floor/wall) with outward
 * normal n and plane offset: points with v2dot(p,n) < limit are penetrating. */
static void solve_wall(Body *b, Vec2 n, f32 limit, f32 inv_dt){
    if (b->is_static) return;
    f32 c=cosf(b->ang), s=sinf(b->ang);
    for (int i=0;i<b->nv;++i){
        Vec2 p=v2add(b->pos, v2rot(b->v[i],c,s));
        f32 d=v2dot(p,n)-limit;
        if (d >= 0) continue;             /* outside the wall */
        Vec2 r=v2sub(p, b->pos);
        Vec2 v=v2add(b->vel, v2crosssv(b->w, r));
        f32 vn=v2dot(v,n);
        f32 rn=v2cross(r,n);
        f32 kn=b->inv_m + b->inv_I*rn*rn;
        if (kn<1e-9f) continue;
        f32 e = b->restitution;
        if (vn > -REST_VN_THRESHOLD) e = 0.0f;   /* resting: no bounce */
        f32 jn=0;
        if (vn<0) jn = -(1.0f+e)*vn/kn;
        if (jn<0) jn=0;
        Vec2 P=v2scale(n,jn);
        b->vel=v2add(b->vel, v2scale(P,b->inv_m));
        b->w += b->inv_I*v2cross(r,P);
        /* energy-safe positional correction along the wall normal */
        const f32 slop=0.01f, beta=0.25f;
        f32 corr=fmaxf(-d-slop,0.0f)*beta;
        if (corr>0) b->pos=v2add(b->pos, v2scale(n, corr));
        /* friction */
        Vec2 t=v2(-n.y,n.x);
        v=v2add(b->vel, v2crosssv(b->w,r));
        f32 vt=v2dot(v,t);
        f32 rt=v2cross(r,t);
        f32 kt=b->inv_m + b->inv_I*rt*rt;
        if (kt<1e-9f) continue;
        f32 jt=-vt/kt; f32 mu=b->friction; f32 mx=mu*jn;
        if (jt>mx)jt=mx; if(jt<-mx)jt=-mx;
        Vec2 Pt=v2scale(t,jt);
        b->vel=v2add(b->vel, v2scale(Pt,b->inv_m));
        b->w += b->inv_I*v2cross(r,Pt);
    }
}

void rb_step(RigidWorld *rw, f32 dt, i32 iterations){
    if (!rw || dt<=0) return;
    if (iterations<1) iterations=8;
    f32 inv_dt = 1.0f/dt;

    /* 1. integrate forces */
    for (int i=0;i<rw->n;++i){
        Body *b=&rw->b[i];
        if (b->is_static) continue;
        b->vel=v2add(b->vel, v2scale(rw->gravity, dt));
    }

    /* 2+3. detect + solve contacts iteratively */
    for (int it=0; it<iterations; ++it){
        /* body-body */
        for (int i=0;i<rw->n;++i){
            for (int j=i+1;j<rw->n;++j){
                Body *A=&rw->b[i], *B=&rw->b[j];
                if (A->is_static && B->is_static) continue;
                Vec2 n; Contact cts[2];
                int nc=collide(A,B,&n,cts);
                for (int k=0;k<nc;++k) solve_contact(A,B,n,cts[k],dt,inv_dt);
            }
        }
        /* body-walls: floor y=0 (n=+y, limit 0), left x=0 (n=+x), right x=W (n=-x),
         * ceiling omitted so things can be tossed above the top. */
        for (int i=0;i<rw->n;++i){
            Body *b=&rw->b[i];
            if (b->is_static) continue;
            solve_wall(b, v2(0,1), 0.0f, inv_dt);       /* floor */
            solve_wall(b, v2(1,0), 0.0f, inv_dt);       /* left wall */
            solve_wall(b, v2(-1,0), -rw->W, inv_dt);    /* right wall (n=-x, limit=-W) */
        }
    }

    /* 3b. positional correction pass: push overlapping body pairs apart along
     * the contact normal (energy-safe  -  edits position, not velocity). A few
     * passes converge stacks without the bias-in-velocity energy pumping. */
    for (int it=0; it<iterations/2+1; ++it){
        for (int i=0;i<rw->n;++i){
            for (int j=i+1;j<rw->n;++j){
                Body *A=&rw->b[i], *B=&rw->b[j];
                if (A->is_static && B->is_static) continue;
                Vec2 n; Contact cts[2];
                int nc=collide(A,B,&n,cts);
                for (int k=0;k<nc;++k) correct_positions(A,B,n,cts[k].depth);
            }
        }
    }

    /* 4. integrate velocities into positions */
    for (int i=0;i<rw->n;++i){
        Body *b=&rw->b[i];
        if (b->is_static) continue;
        b->pos=v2add(b->pos, v2scale(b->vel, dt));
        b->ang += b->w*dt;
        /* mild global damping keeps stacks from jittering forever */
        b->vel=v2scale(b->vel, 0.999f);
        b->w *= 0.999f;
    }
}
