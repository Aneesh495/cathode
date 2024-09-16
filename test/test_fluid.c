/* test_fluid.c  -  stable fluids solver properties. */
#include "cathode/physics.h"
#include "cathode/vec.h"
#include "cathode/framebuffer.h"
#include <stdio.h>
#include <math.h>

static int failures=0, checks=0;
static void ok(const char*n,int c){ checks++; if(c) printf("  ok   %s\n",n); else {printf("  FAIL %s\n",n);failures++;} }

/* We can't see internals directly; test through the public API + a small
 * reimplementation of divergence on the rendered/observable behavior.
 * For divergence we add a known divergent velocity via add_velocity and then
 * check that after a step the dye transport is bounded/finite. Structural
 * properties (mass conservation, spreading) are observable via render. */

static f32 total_density(FluidSim*f, Framebuffer*fb){
    fluid_render(f,fb);
    f32 s=0; for(int i=0;i<fb->w*fb->h*3;++i) s+=fb->px[i]; return s;
}

int main(void){
    printf("== CATHODE fluid (stable Navier-Stokes) ==\n");
    int NX=48,NY=48;
    FluidSim*f=fluid_create(NX,NY);
    Framebuffer*fb=fb_create(96,96);

    ok("dims", fluid_nx(f)==NX && fluid_ny(f)==NY);

    /* (a) add a density blob at center; a previously-zero neighbor becomes >0
       after stepping with some velocity (spreading). */
    fluid_add_density(f, NX/2, NY/2, 50.0f, col3(1,1,1));
    fluid_add_velocity(f, NX/2, NY/2, 2.0f, 0.5f);
    /* record a far neighbor's dye (rendered) before */
    for(int s=0;s<8;++s) fluid_step(f, 0.1f, 0.0f, 0.0f);
    /* after stepping, density should have moved: check the field is finite and
       total density is conserved-ish (no sources after initial) */
    f32 tot=total_density(f,fb);
    ok("density present after steps", tot>0.5f);

    int finite=1; for(int i=0;i<fb->w*fb->h*3;++i){ float v=fb->px[i]; if(!(v==v)||v<0||v>1e6f){finite=0;break;} }
    ok("fields finite", finite);

    /* (b) mass conservation (advection only): seed, no further sources,
       zero viscosity/diffusion, compare total dye over steps (allow a few %). */
    FluidSim*g=fluid_create(48,48);
    Framebuffer*gb=fb_create(96,96);
    for(int j=20;j<28;++j)for(int i=20;i<28;++i) fluid_add_density(g,i,j,10.0f,col3(1,0.5f,0.2f));
    /* gentle swirl */
    for(int j=1;j<=48;++j)for(int i=1;i<=48;++i) fluid_add_velocity(g,i,j, 0.3f*sinf(j*0.2f), 0.3f*cosf(i*0.2f));
    /* sum raw dye by rendering is nonlinear (tonemap); instead compare rendered
       total at step 2 vs step 12  -  should not blow up or vanish. */
    for(int s=0;s<2;++s) fluid_step(g,0.05f,0.0f,0.0f);
    f32 t_early=total_density(g,gb);
    for(int s=0;s<10;++s) fluid_step(g,0.05f,0.0f,0.0f);
    f32 t_late=total_density(g,gb);
    printf("  dye rendered total early=%.2f late=%.2f ratio=%.3f\n", t_early,t_late,t_late/t_early);
    ok("advection roughly conserves dye (0.5..1.6x)", t_late>0.5f*t_early && t_late<1.6f*t_early);

    /* (c) projection keeps things stable under strong divergent input */
    FluidSim*h=fluid_create(32,32);
    Framebuffer*hb=fb_create(64,64);
    fluid_add_density(h,16,16,30.0f,col3(0.2f,0.6f,1.0f));
    for(int j=1;j<=32;++j)for(int i=1;i<=32;++i) fluid_add_velocity(h,i,j,(i-16)*0.3f,(j-16)*0.3f); /* radially outward = divergent */
    int stable=1;
    for(int s=0;s<20;++s){ fluid_step(h,0.08f,0.0001f,0.0001f); }
    fluid_render(h,hb);
    for(int i=0;i<hb->w*hb->h*3;++i){ float v=hb->px[i]; if(!(v==v)||v>1e6f){stable=0;break;} }
    ok("stable under divergent forcing", stable);

    fluid_destroy(f);fluid_destroy(g);fluid_destroy(h);
    fb_destroy(fb);fb_destroy(gb);fb_destroy(hb);
    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
