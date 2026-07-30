/* ==========================================================================
 * scene_cells.c — animated Voronoi / cellular texture from Worley noise.
 *
 * Layers Worley noise (noise.h worley2 / worley2_f2f1) into an organic living
 * texture: F1 gives cell-body shading, F2-F1 gives glowing membrane edges. We
 * scroll and warp the domain over time (and modulate with fbm) so the cells
 * pulse and drift like a microscope slide of living tissue. A palette maps it
 * to bioluminescent colors; the CRT bloom lights the membranes.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>

typedef struct { i32 w,h; f32 t; f32 scale; int palette; } CellState;

static void ce_init(Scene *sc, i32 w, i32 h){ CellState*s=sc->state; s->w=w;s->h=h;s->t=0;s->scale=6.0f;s->palette=0; }
static void ce_update(Scene *sc, f32 dt, f32 t){ (void)dt; ((CellState*)sc->state)->t=t; }

static Color3 cell_color(int pal, f32 body, f32 edge){
    /* body: 0 (cell center) .. ~1 (far);  edge: membrane strength */
    switch(pal){
        case 0: { /* bioluminescent teal cells, cyan membranes */
            Color3 c = col_lerp(col3(0.05f,0.25f,0.3f), col3(0.02f,0.06f,0.12f), body);
            return col_add(c, col_scale(col3(0.3f,1.0f,0.9f), edge));
        }
        case 1: { /* molten: dark rock, orange cracks */
            Color3 c = col_lerp(col3(0.25f,0.12f,0.05f), col3(0.03f,0.02f,0.02f), body);
            return col_add(c, col_scale(col3(1.0f,0.55f,0.1f), edge*1.4f));
        }
        default:{ /* violet tissue, magenta membranes */
            Color3 c = col_lerp(col3(0.2f,0.1f,0.3f), col3(0.04f,0.02f,0.08f), body);
            return col_add(c, col_scale(col3(1.0f,0.3f,0.8f), edge));
        }
    }
}

static void ce_render(Scene *sc, Framebuffer *fb){
    CellState *s=sc->state; f32 t=s->t;
    for (i32 py=0;py<fb->h;++py){
        for (i32 px=0;px<fb->w;++px){
            f32 u=(f32)px/fb->h*s->scale;   /* /h keeps cells square-ish */
            f32 v=(f32)py/fb->h*s->scale;
            /* domain warp with slow fbm so cells breathe and drift */
            f32 wx=u + 0.6f*fbm2(u*0.5f+t*0.05f, v*0.5f, 3, 2.0f, 0.5f) + t*0.15f;
            f32 wy=v + 0.6f*fbm2(u*0.5f, v*0.5f-t*0.04f, 3, 2.0f, 0.5f);
            f32 f1=worley2(wx,wy);
            f32 edge=worley2_f2f1(wx,wy);
            f32 body=ct_clampf(f1,0.0f,1.0f);
            /* membrane: bright where F2-F1 is small (near a cell border) */
            f32 mem=ct_clampf(1.0f - edge*4.0f, 0.0f, 1.0f);
            mem=mem*mem;
            fb_set(fb,px,py, cell_color(s->palette, body, mem));
        }
    }
}

static void ce_key(Scene *sc, int key){
    CellState *s=sc->state;
    if (key==KEY_TAB) s->palette=(s->palette+1)%3;
    else if (key==KEY_PLUS) s->scale*=1.15f;
    else if (key==KEY_MINUS) s->scale/=1.15f;
    s->scale=ct_clampf(s->scale,2.0f,20.0f);
}

static CrtConfig ce_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("broadcast"); c.bloom=0.5f; return c; }
static void ce_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_cells_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="cells";
    sc->description="Living Voronoi cell texture from Worley noise (domain-warped)";
    sc->state=calloc(1,sizeof(CellState));
    sc->init=ce_init; sc->update=ce_update; sc->render=ce_render;
    sc->on_key=ce_key; sc->destroy=ce_destroy; sc->preferred_crt=ce_crt;
    return sc;
}
