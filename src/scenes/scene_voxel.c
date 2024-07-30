/* ==========================================================================
 * scene_voxel.c — Comanche-style voxel-heightmap terrain (column ray-caster).
 *
 * The early-90s "voxel space" technique (Novalogic's Comanche): no polygons —
 * for each screen column, march a ray forward across a heightmap from front to
 * back, and for each step project the terrain height to a screen y; wherever it
 * rises above the highest column drawn so far, paint that vertical span with the
 * terrain's color. Painting front-to-back with a per-column "y-buffer" gives
 * correct occlusion for free. The heightmap and color map are generated from
 * fbm/Perlin noise (no assets). A moving camera flies over an endless,
 * wrapping landscape under a graded sky with distance haze.
 *
 * Pure C over the framebuffer + noise library; no new subsystems.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/noise.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAP 256                 /* heightmap is MAP x MAP, wraps toroidally */

typedef struct {
    i32 w, h; f32 t;
    f32 *height;                /* MAP*MAP in [0,1] */
    Color3 *color;              /* MAP*MAP terrain albedo */
    f32 *ybuf;                  /* per-column lowest painted y (reused each frame) */
    int preset;
} VoxState;

static inline f32 hmap(VoxState *s, int x, int y){
    x &= (MAP-1); y &= (MAP-1);
    return s->height[y*MAP+x];
}
static inline Color3 cmap(VoxState *s, int x, int y){
    x &= (MAP-1); y &= (MAP-1);
    return s->color[y*MAP+x];
}

static void vx_generate(VoxState *s){
    f32 off = s->preset * 13.7f;   /* domain shift => a fresh landscape per preset */
    for (int y=0;y<MAP;++y){
        for (int x=0;x<MAP;++x){
            f32 fx=(f32)x*0.028f + off, fy=(f32)y*0.028f + off*0.53f;
            /* ridged/fbm mix for mountainous terrain */
            f32 e = fbm2(fx, fy, 6, 2.0f, 0.5f);       /* [-1,1]-ish */
            e = 0.5f + 0.5f*e;
            f32 ridge = 1.0f - fabsf(fbm2(fx*1.7f+31.0f, fy*1.7f+17.0f, 4, 2.0f, 0.5f));
            e = 0.6f*e + 0.4f*ridge*ridge;
            if (e<0)e=0; if(e>1)e=1;
            s->height[y*MAP+x]=e;
            /* color ramp: water -> sand -> grass -> rock -> snow */
            Color3 c;
            if (e < 0.30f)      c=col3(0.05f,0.18f,0.38f);              /* water */
            else if (e < 0.36f) c=col3(0.55f,0.52f,0.32f);             /* sand */
            else if (e < 0.60f) c=col3(0.15f+0.2f*(e-0.36f)/0.24f, 0.42f, 0.14f); /* grass */
            else if (e < 0.80f) c=col3(0.36f,0.30f,0.24f);             /* rock */
            else                c=col3(0.9f,0.92f,0.97f);              /* snow */
            /* subtle noise dither on albedo */
            f32 d=0.9f+0.1f*noise2(fx*6.0f, fy*6.0f);
            s->color[y*MAP+x]=col_scale(c,d);
        }
    }
}

static void vx_init(Scene *sc, i32 w, i32 h){
    VoxState *s=sc->state; s->w=w; s->h=h; s->t=0; s->preset=0;
    s->height=malloc(sizeof(f32)*MAP*MAP);
    s->color=malloc(sizeof(Color3)*MAP*MAP);
    s->ybuf=malloc(sizeof(f32)*(w>0?w:1));
    vx_generate(s);
}
static void vx_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((VoxState*)sc->state)->t=t; }

static void vx_render(Scene *sc, Framebuffer *fb){
    VoxState *s=sc->state; const i32 W=fb->w, H=fb->h;

    /* graded sky: horizon warm, zenith blue */
    for (i32 y=0;y<H;++y){
        f32 v=(f32)y/H;
        Color3 sky=col_lerp(col3(0.55f,0.68f,0.92f), col3(0.86f,0.80f,0.70f), v*v);
        f32 *row=&fb->px[(size_t)y*W*3];
        for (i32 x=0;x<W;++x){ row[3*x+0]=sky.r; row[3*x+1]=sky.g; row[3*x+2]=sky.b; }
    }

    /* camera: fly forward over the map, gently turning */
    const f32 HSCALE = 260.0f;                      /* terrain vertical amplitude */
    f32 t=s->t;
    f32 camx = t*22.0f;
    f32 camy = t*6.0f + 40.0f*sinf(t*0.15f);
    f32 dir  = 0.35f*sinf(t*0.2f);                  /* heading */
    /* keep the camera a fixed height above the ground it's over */
    f32 camh = hmap(s, (int)camx, (int)camy)*HSCALE + 90.0f;
    f32 horizon = H*0.30f;                           /* screen row of the horizon */
    f32 scale_h = (f32)H*2.6f;                       /* height projection scale */

    const f32 sind=sinf(dir), cosd=cosf(dir);
    const int zfar=320;
    const f32 fov=0.9f;

    /* init per-column y-buffer to the bottom of the screen */
    for (i32 x=0;x<W;++x) s->ybuf[x]=(f32)H;

    /* march front-to-back; step grows with distance (LOD) for speed */
    f32 dz=1.0f, z=1.0f;
    while (z < zfar){
        /* the left/right edges of the view frustum at distance z (camera space) */
        f32 halfw = fov*z;
        f32 lx = -halfw, rx = halfw;
        /* world-space endpoints of this scanline (rotated by heading) */
        f32 plx =  lx*cosd - z*sind + camx;
        f32 ply =  lx*sind + z*cosd + camy;
        f32 prx =  rx*cosd - z*sind + camx;
        f32 pry =  rx*sind + z*cosd + camy;
        f32 stepx=(prx-plx)/W, stepy=(pry-ply)/W;
        f32 invz = 1.0f/z;
        /* distance haze factor */
        f32 haze = z/(f32)zfar; if (haze>1)haze=1;
        Color3 hazecol=col3(0.78f,0.80f,0.85f);
        f32 wx=plx, wy=ply;
        for (i32 x=0;x<W;++x){
            f32 hgt = hmap(s,(int)wx,(int)wy)*HSCALE;
            /* project: screen y of this terrain height */
            f32 sy = (camh - hgt)*invz*scale_h*0.01f + horizon;
            if (sy < s->ybuf[x]){
                Color3 c=cmap(s,(int)wx,(int)wy);
                c=col_lerp(c, hazecol, haze*0.75f);
                /* simple lambert-ish shading from the slope in +x */
                f32 hgx = hmap(s,(int)wx+1,(int)wy)*HSCALE;
                f32 sh=0.75f + 0.5f*(hgt-hgx)*0.05f; if(sh<0.35f)sh=0.35f; if(sh>1.3f)sh=1.3f;
                c=col_scale(c,sh);
                int y0=(int)sy; if(y0<0)y0=0;
                int y1=(int)s->ybuf[x]; if(y1>H)y1=H;
                f32 *col=&fb->px[0];
                for (int y=y0;y<y1;++y){
                    size_t di=((size_t)y*W+x)*3;
                    col[di+0]=c.r; col[di+1]=c.g; col[di+2]=c.b;
                }
                s->ybuf[x]=sy;
            }
            wx+=stepx; wy+=stepy;
        }
        z += dz;
        dz *= 1.012f;   /* geometric LOD stepping */
    }
}

static void vx_key(Scene *sc, int key){
    VoxState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->preset++; vx_generate(s); }  /* fresh landscape */
}
static CrtConfig vx_crt(Scene *sc){ (void)sc; return crt_config_preset("vhs"); }
static void vx_destroy(Scene *sc){ if(sc){ VoxState*s=sc->state; free(s->height); free(s->color); free(s->ybuf); free(s); free(sc);} }

Scene *scene_voxel_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="voxel";
    sc->description="Comanche-style voxel-heightmap terrain flyover (column ray-caster, no polygons)";
    sc->state=calloc(1,sizeof(VoxState));
    sc->init=vx_init; sc->update=vx_update; sc->render=vx_render;
    sc->on_key=vx_key; sc->destroy=vx_destroy; sc->preferred_crt=vx_crt;
    return sc;
}
