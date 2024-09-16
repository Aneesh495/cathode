/* ==========================================================================
 * sph.c  -  2D Smoothed Particle Hydrodynamics (Müller et al. 2003).
 *
 * Per step:
 *   1. build a uniform spatial hash (cell size = smoothing radius h) so each
 *      particle only checks its 3x3 neighboring cells  -  near-O(N).
 *   2. density_i = sum_j m * W_poly6(r_ij, h);  pressure_i = k*(density-rho0).
 *   3. force_i   = -pressure grad (spiky kernel) + viscosity (laplacian kernel)
 *                  + gravity.
 *   4. integrate (semi-implicit Euler) and resolve domain-boundary collisions.
 *
 * Kernels (2D-normalized):
 *   W_poly6(r,h)      = 4/(pi h^8) (h^2 - r^2)^3            (density)
 *   grad W_spiky(r,h) = -30/(pi h^5) (h - r)^2  (rhat)      (pressure force)
 *   lap W_visc(r,h)   = 40/(pi h^5) (h - r)                 (viscosity)
 * ========================================================================== */
#include "cathode/physics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct SphSim {
    f32 *px,*py, *vx,*vy, *ax,*ay;   /* SoA particle state */
    f32 *rho, *prs;                  /* density, pressure */
    i32 n, cap;
    f32 domw, domh;
    f32 gx, gy;
    /* SPH params */
    f32 h;          /* smoothing radius */
    f32 mass;
    f32 rho0;       /* rest density */
    f32 stiff;      /* pressure stiffness k */
    f32 visc;       /* viscosity mu */
    /* spatial hash */
    i32 *cell_head; /* per-cell first-particle index, -1 if empty */
    i32 *next;      /* linked-list next per particle */
    i32 ncx, ncy;   /* grid dims */
    f32 cellsz;
};

static f32 *falloc(i32 n){ return (f32*)calloc((size_t)n,sizeof(f32)); }

SphSim *sph_create(i32 capacity, f32 w, f32 h){
    if (capacity<16) capacity=16;
    SphSim *s=(SphSim*)calloc(1,sizeof(SphSim));
    s->cap=capacity; s->domw=w; s->domh=h;
    s->px=falloc(capacity); s->py=falloc(capacity);
    s->vx=falloc(capacity); s->vy=falloc(capacity);
    s->ax=falloc(capacity); s->ay=falloc(capacity);
    s->rho=falloc(capacity); s->prs=falloc(capacity);
    s->gx=0.0f; s->gy=-9.8f;
    /* choose smoothing radius so a modest number of particles interact */
    s->h=1.1f; s->mass=1.0f; s->rho0=1.6f; s->stiff=25.0f; s->visc=1.2f;
    s->cellsz=s->h;
    s->ncx=(i32)(w/s->cellsz)+2; s->ncy=(i32)(h/s->cellsz)+2;
    if (s->ncx<2)s->ncx=2; if(s->ncy<2)s->ncy=2;
    s->cell_head=malloc(sizeof(i32)*s->ncx*s->ncy);
    s->next=malloc(sizeof(i32)*capacity);
    return s;
}

void sph_destroy(SphSim *s){
    if(!s) return;
    free(s->px);free(s->py);free(s->vx);free(s->vy);free(s->ax);free(s->ay);
    free(s->rho);free(s->prs);free(s->cell_head);free(s->next);free(s);
}

void sph_add(SphSim *s, f32 x, f32 y){
    if (s->n>=s->cap) return;
    i32 i=s->n++;
    s->px[i]=x; s->py[i]=y; s->vx[i]=0; s->vy[i]=0;
}

void sph_add_block(SphSim *s, f32 x0,f32 y0,f32 x1,f32 y1,f32 spacing){
    if (spacing<=0.0f) spacing=s->h*0.6f;
    for (f32 y=y0;y<=y1;y+=spacing)
        for (f32 x=x0;x<=x1;x+=spacing){
            /* tiny jitter avoids a perfectly-symmetric lattice locking up */
            sph_add(s, x+0.01f*((x*13.0f)-floorf(x*13.0f)-0.5f), y);
        }
}

void sph_set_gravity(SphSim *s, f32 gx, f32 gy){ s->gx=gx; s->gy=gy; }
i32 sph_count(const SphSim *s){ return s->n; }
f32 sph_domain_w(const SphSim *s){ return s->domw; }
f32 sph_domain_h(const SphSim *s){ return s->domh; }

static inline i32 cell_of(SphSim *s, f32 x, f32 y){
    i32 cx=(i32)(x/s->cellsz); i32 cy=(i32)(y/s->cellsz);
    if (cx<0)cx=0; if(cy<0)cy=0; if(cx>=s->ncx)cx=s->ncx-1; if(cy>=s->ncy)cy=s->ncy-1;
    return cy*s->ncx+cx;
}

static void build_hash(SphSim *s){
    i32 ncell=s->ncx*s->ncy;
    for (i32 c=0;c<ncell;++c) s->cell_head[c]=-1;
    for (i32 i=0;i<s->n;++i){
        i32 c=cell_of(s,s->px[i],s->py[i]);
        s->next[i]=s->cell_head[c];
        s->cell_head[c]=i;
    }
}

void sph_step(SphSim *s, f32 dt){
    if (s->n==0) return;
    const f32 h=s->h, h2=h*h;
    const f32 PI=3.14159265358979f;
    const f32 poly6=4.0f/(PI*powf(h,8));
    const f32 spiky=-30.0f/(PI*powf(h,5));
    const f32 viscl=40.0f/(PI*powf(h,5));
    build_hash(s);

    /* --- density & pressure --- */
    for (i32 i=0;i<s->n;++i){
        f32 rho=0.0f;
        i32 cx=(i32)(s->px[i]/s->cellsz), cy=(i32)(s->py[i]/s->cellsz);
        for (i32 oy=-1;oy<=1;++oy)for(i32 ox=-1;ox<=1;++ox){
            i32 nx=cx+ox, ny=cy+oy; if(nx<0||ny<0||nx>=s->ncx||ny>=s->ncy)continue;
            for (i32 j=s->cell_head[ny*s->ncx+nx]; j>=0; j=s->next[j]){
                f32 dx=s->px[i]-s->px[j], dy=s->py[i]-s->py[j];
                f32 r2=dx*dx+dy*dy;
                if (r2<h2){ f32 d=h2-r2; rho += s->mass*poly6*d*d*d; }
            }
        }
        s->rho[i]=rho>1e-6f?rho:1e-6f;
        s->prs[i]=s->stiff*(s->rho[i]-s->rho0);
        if (s->prs[i]<0.0f) s->prs[i]=0.0f;  /* clamp: no negative (tensile) pressure */
    }

    /* --- forces --- */
    for (i32 i=0;i<s->n;++i){
        f32 fx=0.0f, fy=0.0f;
        i32 cx=(i32)(s->px[i]/s->cellsz), cy=(i32)(s->py[i]/s->cellsz);
        for (i32 oy=-1;oy<=1;++oy)for(i32 ox=-1;ox<=1;++ox){
            i32 nx=cx+ox, ny=cy+oy; if(nx<0||ny<0||nx>=s->ncx||ny>=s->ncy)continue;
            for (i32 j=s->cell_head[ny*s->ncx+nx]; j>=0; j=s->next[j]){
                if (j==i) continue;
                f32 dx=s->px[i]-s->px[j], dy=s->py[i]-s->py[j];
                f32 r2=dx*dx+dy*dy;
                if (r2<h2 && r2>1e-9f){
                    f32 r=sqrtf(r2);
                    /* pressure force (symmetric) */
                    f32 pterm = -s->mass*(s->prs[i]+s->prs[j])/(2.0f*s->rho[j]) * spiky*(h-r)*(h-r)/r;
                    fx += pterm*dx; fy += pterm*dy;
                    /* viscosity force */
                    f32 vterm = s->visc*s->mass/s->rho[j]*viscl*(h-r);
                    fx += vterm*(s->vx[j]-s->vx[i]);
                    fy += vterm*(s->vy[j]-s->vy[i]);
                }
            }
        }
        /* gravity (per unit mass) */
        s->ax[i]=fx/s->rho[i]+s->gx;
        s->ay[i]=fy/s->rho[i]+s->gy;
    }

    /* --- integrate + boundary --- */
    const f32 rest=0.4f;   /* wall restitution */
    for (i32 i=0;i<s->n;++i){
        s->vx[i]+=s->ax[i]*dt; s->vy[i]+=s->ay[i]*dt;
        s->px[i]+=s->vx[i]*dt; s->py[i]+=s->vy[i]*dt;
        f32 m=s->h*0.5f;
        if (s->px[i]<m){ s->px[i]=m; s->vx[i]*=-rest; }
        if (s->px[i]>s->domw-m){ s->px[i]=s->domw-m; s->vx[i]*=-rest; }
        if (s->py[i]<m){ s->py[i]=m; s->vy[i]*=-rest; }
        if (s->py[i]>s->domh-m){ s->py[i]=s->domh-m; s->vy[i]*=-rest; }
    }
}

void sph_positions(const SphSim *s, f32 *out_xy, f32 *out_speed){
    for (i32 i=0;i<s->n;++i){
        if (out_xy){ out_xy[i*2]=s->px[i]; out_xy[i*2+1]=s->py[i]; }
        if (out_speed) out_speed[i]=sqrtf(s->vx[i]*s->vx[i]+s->vy[i]*s->vy[i]);
    }
}
