/* ==========================================================================
 * scene_ripple.c  -  2D wave-equation water ripples with caustic shading.
 *
 * Integrates the discrete wave equation on a height grid:
 *   h_next = 2h - h_prev + c^2 * laplacian(h),  with mild damping.
 * Moving droplets inject impulses; waves radiate, interfere, and reflect off
 * the borders. We shade it like a lit water surface: the height gradient
 * defines a normal, we do a cheap specular + refraction-tint, and add a bloom
 * pass with the NEON separable blur (blur.h) so highlights glow.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/blur.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    i32 gw, gh;
    f32 *h, *hp, *tmp;    /* current, previous, scratch height grids */
    f32 *lum, *bloom;     /* luminance + blurred bloom buffers */
    i32 w, hgt; f32 t;
    f32 c2, damp;
    int palette;
} RippleState;

static void rp_init(Scene *sc, i32 w, i32 h){
    RippleState *s=sc->state; s->w=w; s->hgt=h; s->t=0; s->palette=0;
    s->gw=w; s->gh=h;
    size_t n=(size_t)w*h;
    s->h=calloc(n,sizeof(f32)); s->hp=calloc(n,sizeof(f32)); s->tmp=calloc(n,sizeof(f32));
    s->lum=calloc(n,sizeof(f32)); s->bloom=calloc(n,sizeof(f32));
    s->c2=0.22f; s->damp=0.9985f;
}

static void drop(RippleState *s, i32 cx, i32 cy, f32 amp, i32 rad){
    for (i32 j=-rad;j<=rad;++j)for(i32 i=-rad;i<=rad;++i){
        i32 x=cx+i, y=cy+j; if((unsigned)x>=(unsigned)s->gw||(unsigned)y>=(unsigned)s->gh) continue;
        f32 r=sqrtf((f32)(i*i+j*j))/(f32)rad;
        if (r<1.0f) s->h[y*s->gw+x] += amp*(0.5f+0.5f*cosf(r*CT_PI));
    }
}

static void rp_update(Scene *sc, f32 dt, f32 t){
    RippleState *s=sc->state; (void)dt; s->t=t;
    i32 gw=s->gw, gh=s->gh;
    /* inject moving droplets on orbiting paths */
    if (((i32)(t*30.0f)) % 8 == 0){
        i32 dx=(i32)(gw*(0.5f+0.35f*sinf(t*1.3f)));
        i32 dy=(i32)(gh*(0.5f+0.35f*cosf(t*0.9f)));
        drop(s, dx, dy, 2.5f, 4);
    }
    /* wave equation step into tmp. Borders are set to 0 (a fixed/reflecting
     * boundary) EVERY step so no stale data survives the buffer rotation. */
    f32 *hc=s->h, *hn=s->tmp;
    for (i32 x=0;x<gw;++x){ hn[x]=0.0f; hn[(gh-1)*gw+x]=0.0f; }
    for (i32 y=0;y<gh;++y){ hn[y*gw]=0.0f; hn[y*gw+gw-1]=0.0f; }
    for (i32 y=1;y<gh-1;++y){
        for (i32 x=1;x<gw-1;++x){
            f32 lap = hc[(y-1)*gw+x]+hc[(y+1)*gw+x]+hc[y*gw+x-1]+hc[y*gw+x+1] - 4.0f*hc[y*gw+x];
            f32 v = 2.0f*hc[y*gw+x] - s->hp[y*gw+x] + s->c2*lap;
            hn[y*gw+x] = v*s->damp;
        }
    }
    /* rotate: hp <- old h, h <- new (tmp), tmp <- old hp (free scratch) */
    f32 *old_hp=s->hp; s->hp=s->h; s->h=s->tmp; s->tmp=old_hp;
}

static void rp_render(Scene *sc, Framebuffer *fb){
    RippleState *s=sc->state;
    i32 gw=s->gw, gh=s->gh;
    /* shade: normal from height gradient, specular highlight + depth tint */
    Vec3 L=v3_norm(v3(0.5f,0.6f,0.6f));
    for (i32 y=0;y<gh;++y){
        for (i32 x=0;x<gw;++x){
            i32 xm=x>0?x-1:x, xp=x<gw-1?x+1:x, ym=y>0?y-1:y, yp=y<gh-1?y+1:y;
            f32 dhx=s->h[y*gw+xp]-s->h[y*gw+xm];
            f32 dhy=s->h[yp*gw+x]-s->h[ym*gw+x];
            Vec3 nrm=v3_norm(v3(-dhx, -dhy, 1.0f));
            f32 diff=ct_maxf(0.0f, v3_dot(nrm,L));
            Vec3 V=v3(0,0,1); Vec3 H=v3_norm(v3_add(L,V));
            f32 spec=powf(ct_maxf(0.0f,v3_dot(nrm,H)), 40.0f);
            s->lum[y*gw+x]=spec;   /* stash highlight for bloom */
            /* deep-water blue tinted by height, plus specular */
            f32 hh=s->h[y*gw+x];
            Color3 water=col_lerp(col3(0.02f,0.08f,0.18f), col3(0.1f,0.35f,0.55f), 0.5f+0.25f*hh);
            Color3 c=col_add(col_scale(water, 0.4f+0.6f*diff), col3(spec,spec,spec));
            fb_set(fb,x,y,c);
        }
    }
    /* bloom the specular highlights with the NEON separable blur and add back.
     * We blur lum -> bloom (h pass) -> bloom again (v pass, in place-ish via a
     * second buffer). tmp is free scratch here (update() fully overwrites it). */
    float ker[16]; int r=blur_gaussian_kernel(ker,4,0.0f);
    blur_h_neon(s->tmp,   s->lum,  gw, gh, ker, r);
    blur_v_neon(s->bloom, s->tmp,  gw, gh, ker, r);
    for (i32 i=0;i<gw*gh;++i){
        f32 b=s->bloom[i]*1.5f;
        fb->px[i*3+0]+=b; fb->px[i*3+1]+=b*1.05f; fb->px[i*3+2]+=b*1.1f;
    }
}

static void rp_key(Scene *sc, int key){
    RippleState *s=sc->state;
    if (key==KEY_R){ memset(s->h,0,(size_t)s->gw*s->gh*sizeof(f32)); memset(s->hp,0,(size_t)s->gw*s->gh*sizeof(f32)); }
    else if (key==KEY_SPACE) drop(s, s->gw/2, s->gh/2, 4.0f, 6);
    else if (key==KEY_PLUS) s->c2=ct_minf(s->c2*1.1f,0.4f);
    else if (key==KEY_MINUS) s->c2*=0.9f;
}

static CrtConfig rp_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("trinitron"); c.bloom=0.35f; return c; }
static void rp_destroy(Scene *sc){ if(sc){ RippleState*s=sc->state; free(s->h);free(s->hp);free(s->tmp);free(s->lum);free(s->bloom); free(s); free(sc);} }

Scene *scene_ripple_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="ripple";
    sc->description="2D wave-equation water ripples (NEON-blurred caustic bloom)";
    sc->state=calloc(1,sizeof(RippleState));
    sc->init=rp_init; sc->update=rp_update; sc->render=rp_render;
    sc->on_key=rp_key; sc->destroy=rp_destroy; sc->preferred_crt=rp_crt;
    return sc;
}
