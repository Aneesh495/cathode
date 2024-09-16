/* ==========================================================================
 * fluid.c  -  2D stable fluid solver (Jos Stam, "Stable Fluids" / "Real-Time
 * Fluid Dynamics for Games").
 *
 * Grid with a 1-cell border. Velocity (u,v) + colored dye density (r,g,b).
 * Step = velocity{ diffuse -> project -> advect -> project }
 *       then density{ diffuse -> advect } .
 * Poisson solves via Gauss-Seidel; advection is semi-Lagrangian (backtrace +
 * bilinear sample), which is unconditionally stable.
 * ========================================================================== */
#include "cathode/physics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct FluidSim {
    i32 nx, ny;        /* interior size; arrays are (nx+2)*(ny+2) */
    i32 W, H;          /* W=nx+2, H=ny+2 */
    f32 *u, *v, *u0, *v0;
    f32 *dr,*dg,*db, *dr0,*dg0,*db0;   /* dye channels + scratch */
    i32 iters;         /* Gauss-Seidel iterations */
};

#define IX(f,i,j) ((i) + (f)->W*(j))

static f32 *fcalloc(i32 n){ return (f32*)calloc((size_t)n,sizeof(f32)); }

FluidSim *fluid_create(i32 nx, i32 ny){
    FluidSim *f=(FluidSim*)calloc(1,sizeof(FluidSim));
    if (nx<4)nx=4; if(ny<4)ny=4;
    f->nx=nx; f->ny=ny; f->W=nx+2; f->H=ny+2;
    i32 N=f->W*f->H;
    f->u=fcalloc(N); f->v=fcalloc(N); f->u0=fcalloc(N); f->v0=fcalloc(N);
    f->dr=fcalloc(N); f->dg=fcalloc(N); f->db=fcalloc(N);
    f->dr0=fcalloc(N); f->dg0=fcalloc(N); f->db0=fcalloc(N);
    f->iters=20;
    return f;
}
void fluid_destroy(FluidSim*f){
    if(!f)return;
    free(f->u);free(f->v);free(f->u0);free(f->v0);
    free(f->dr);free(f->dg);free(f->db);free(f->dr0);free(f->dg0);free(f->db0);
    free(f);
}
i32 fluid_nx(const FluidSim*f){return f->nx;}
i32 fluid_ny(const FluidSim*f){return f->ny;}

/* boundary conditions. b: 0 scalar (Neumann), 1 u (reflect x), 2 v (reflect y). */
static void set_bnd(FluidSim*f, int b, f32*x){
    i32 nx=f->nx, ny=f->ny;
    for (i32 i=1;i<=nx;++i){
        x[IX(f,i,0)]    = b==2 ? -x[IX(f,i,1)]  : x[IX(f,i,1)];
        x[IX(f,i,ny+1)] = b==2 ? -x[IX(f,i,ny)] : x[IX(f,i,ny)];
    }
    for (i32 j=1;j<=ny;++j){
        x[IX(f,0,j)]    = b==1 ? -x[IX(f,1,j)]  : x[IX(f,1,j)];
        x[IX(f,nx+1,j)] = b==1 ? -x[IX(f,nx,j)] : x[IX(f,nx,j)];
    }
    /* corners = average of neighbors */
    x[IX(f,0,0)]        =0.5f*(x[IX(f,1,0)]        + x[IX(f,0,1)]);
    x[IX(f,0,ny+1)]     =0.5f*(x[IX(f,1,ny+1)]     + x[IX(f,0,ny)]);
    x[IX(f,nx+1,0)]     =0.5f*(x[IX(f,nx,0)]       + x[IX(f,nx+1,1)]);
    x[IX(f,nx+1,ny+1)]  =0.5f*(x[IX(f,nx,ny+1)]    + x[IX(f,nx+1,ny)]);
}

/* Gauss-Seidel linear solve for (I - a*Laplacian) style relaxation. */
static void lin_solve(FluidSim*f, int b, f32*x, const f32*x0, f32 a, f32 c){
    f32 invc=1.0f/c;
    for (int k=0;k<f->iters;++k){
        for (i32 j=1;j<=f->ny;++j)
            for (i32 i=1;i<=f->nx;++i){
                x[IX(f,i,j)] = ( x0[IX(f,i,j)] + a*( x[IX(f,i-1,j)]+x[IX(f,i+1,j)]
                                                    +x[IX(f,i,j-1)]+x[IX(f,i,j+1)] ) )*invc;
            }
        set_bnd(f,b,x);
    }
}

static void diffuse(FluidSim*f, int b, f32*x, const f32*x0, f32 diff, f32 dt){
    f32 a = dt*diff*(f32)f->nx*(f32)f->ny;
    lin_solve(f,b,x,x0,a,1.0f+4.0f*a);
}

static void advect(FluidSim*f, int b, f32*d, const f32*d0, const f32*u, const f32*v, f32 dt){
    i32 nx=f->nx, ny=f->ny;
    f32 dt0x=dt*(f32)nx, dt0y=dt*(f32)ny;
    for (i32 j=1;j<=ny;++j)
        for (i32 i=1;i<=nx;++i){
            f32 x = (f32)i - dt0x*u[IX(f,i,j)];
            f32 y = (f32)j - dt0y*v[IX(f,i,j)];
            if (x<0.5f)x=0.5f; if(x>nx+0.5f)x=nx+0.5f;
            if (y<0.5f)y=0.5f; if(y>ny+0.5f)y=ny+0.5f;
            i32 i0=(i32)x, i1=i0+1;
            i32 j0=(i32)y, j1=j0+1;
            f32 s1=x-i0, s0=1-s1, t1=y-j0, t0=1-t1;
            d[IX(f,i,j)] = s0*(t0*d0[IX(f,i0,j0)] + t1*d0[IX(f,i0,j1)])
                         + s1*(t0*d0[IX(f,i1,j0)] + t1*d0[IX(f,i1,j1)]);
        }
    set_bnd(f,b,d);
}

/* make velocity field mass-conserving (divergence-free) via pressure projection */
static void project(FluidSim*f, f32*u, f32*v, f32*p, f32*div){
    i32 nx=f->nx, ny=f->ny;
    f32 h = 1.0f/(f32)nx; /* grid spacing (assume square-ish) */
    for (i32 j=1;j<=ny;++j)
        for (i32 i=1;i<=nx;++i){
            div[IX(f,i,j)] = -0.5f*h*( u[IX(f,i+1,j)]-u[IX(f,i-1,j)]
                                      +v[IX(f,i,j+1)]-v[IX(f,i,j-1)] );
            p[IX(f,i,j)]=0.0f;
        }
    set_bnd(f,0,div); set_bnd(f,0,p);
    lin_solve(f,0,p,div,1.0f,4.0f);
    for (i32 j=1;j<=ny;++j)
        for (i32 i=1;i<=nx;++i){
            u[IX(f,i,j)] -= 0.5f*(p[IX(f,i+1,j)]-p[IX(f,i-1,j)])/h;
            v[IX(f,i,j)] -= 0.5f*(p[IX(f,i,j+1)]-p[IX(f,i,j-1)])/h;
        }
    set_bnd(f,1,u); set_bnd(f,2,v);
}

void fluid_add_density(FluidSim*f, i32 x, i32 y, f32 amount, Color3 c){
    if (x<1||x>f->nx||y<1||y>f->ny) return;
    i32 idx=IX(f,x,y);
    f->dr[idx]+=amount*c.r; f->dg[idx]+=amount*c.g; f->db[idx]+=amount*c.b;
}
void fluid_add_velocity(FluidSim*f, i32 x, i32 y, f32 vx, f32 vy){
    if (x<1||x>f->nx||y<1||y>f->ny) return;
    i32 idx=IX(f,x,y);
    f->u[idx]+=vx; f->v[idx]+=vy;
}

void fluid_step(FluidSim*f, f32 dt, f32 viscosity, f32 diffusion){
    i32 N=f->W*f->H;
    /* --- velocity --- */
    /* diffuse (u0/v0 hold current as source) */
    memcpy(f->u0,f->u,N*sizeof(f32));
    memcpy(f->v0,f->v,N*sizeof(f32));
    diffuse(f,1,f->u,f->u0,viscosity,dt);
    diffuse(f,2,f->v,f->v0,viscosity,dt);
    project(f,f->u,f->v,f->u0,f->v0);
    memcpy(f->u0,f->u,N*sizeof(f32));
    memcpy(f->v0,f->v,N*sizeof(f32));
    advect(f,1,f->u,f->u0,f->u0,f->v0,dt);
    advect(f,2,f->v,f->v0,f->u0,f->v0,dt);
    project(f,f->u,f->v,f->u0,f->v0);
    /* --- density (3 channels) --- */
    f32 *chans[3]={f->dr,f->dg,f->db}, *chans0[3]={f->dr0,f->dg0,f->db0};
    for (int ch=0; ch<3; ++ch){
        memcpy(chans0[ch], chans[ch], N*sizeof(f32));
        diffuse(f,0,chans[ch],chans0[ch],diffusion,dt);
        memcpy(chans0[ch], chans[ch], N*sizeof(f32));
        advect(f,0,chans[ch],chans0[ch],f->u,f->v,dt);
    }
}

void fluid_render(const FluidSim*f, Framebuffer*fb){
    /* map grid dye to fb via bilinear upscale; tonemap softly */
    for (i32 py=0; py<fb->h; ++py){
        f32 gy = 1.0f + (f32)py/(fb->h-1)*(f->ny-1);
        i32 j0=(i32)gy; i32 j1=j0+1<=f->ny?j0+1:j0; f32 tj=gy-j0;
        for (i32 px=0; px<fb->w; ++px){
            f32 gx = 1.0f + (f32)px/(fb->w-1)*(f->nx-1);
            i32 i0=(i32)gx; i32 i1=i0+1<=f->nx?i0+1:i0; f32 ti=gx-i0;
            f32 rr=0,gg=0,bb=0;
            const f32 *R=f->dr,*G=f->dg,*B=f->db;
            #define SAMP(A) ((1-ti)*(1-tj)*A[IX(f,i0,j0)] + ti*(1-tj)*A[IX(f,i1,j0)] \
                            + (1-ti)*tj*A[IX(f,i0,j1)] + ti*tj*A[IX(f,i1,j1)])
            rr=SAMP(R); gg=SAMP(G); bb=SAMP(B);
            #undef SAMP
            /* soft tonemap so dense dye glows */
            fb_set(fb, px, py, col3(rr/(1.0f+rr), gg/(1.0f+gg), bb/(1.0f+bb)));
        }
    }
}
