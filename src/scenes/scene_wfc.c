/* ==========================================================================
 * scene_wfc.c — Wave Function Collapse procedural generation (Rust-backed).
 *
 * Watches the WFC solver (rust_wfc_* in rustcore.h) fill a grid cell-by-cell:
 * each frame it performs a batch of collapses, and we draw the current state —
 * collapsed cells in their tile color, still-superposed cells dark. When the
 * grid is solved (or hits a contradiction) it pauses, then reseeds with the
 * next ruleset (pipes → circuit → maze). The emergent coherent patterns are
 * mesmerizing as they crystallize.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/vec.h"
#include "cathode/rustcore.h"
#include <stdlib.h>
#include <math.h>

typedef struct {
    RustWFC *wfc;
    i32 *tiles;
    f32 *pal;        /* ntiles*3 */
    i32 gw, gh, ntiles;
    i32 ruleset;
    u64 seed;
    i32 w, h; f32 t;
    f32 hold;        /* seconds to hold a finished pattern before reseed */
    int done;
} WfcState;

static void wfc_setup(WfcState *s){
    if (s->wfc) rust_wfc_destroy(s->wfc);
    s->wfc = rust_wfc_create(s->gw, s->gh, s->ruleset, s->seed);
    s->ntiles = rust_wfc_ntiles(s->wfc);
    free(s->pal); s->pal = malloc((size_t)s->ntiles*3*sizeof(f32));
    rust_wfc_palette(s->wfc, s->pal);
    s->done=0; s->hold=0;
}

static void wf_init(Scene *sc, i32 w, i32 h){
    WfcState *s=sc->state; s->w=w; s->h=h; s->t=0; s->ruleset=0; s->seed=0x1234ABCDULL;
    /* coarse grid: WFC cells are big blocks on screen */
    s->gw = w/6 < 16 ? 16 : (w/6 > 90 ? 90 : w/6);
    s->gh = h/6 < 12 ? 12 : (h/6 > 70 ? 70 : h/6);
    s->tiles=malloc((size_t)s->gw*s->gh*sizeof(i32));
    s->pal=NULL; s->wfc=NULL;
    wfc_setup(s);
}

static void wf_update(Scene *sc, f32 dt, f32 t){
    WfcState *s=sc->state; s->t=t;
    if (!s->done){
        /* collapse a batch per frame; scale so a grid solves over a few seconds */
        i32 batch = 3 + s->gw*s->gh/120;
        i32 status = rust_wfc_step(s->wfc, batch);
        if (status==1){ s->done=1; s->hold=0; }
        else if (status==-1){
            /* contradiction: reseed and retry (WFC can dead-end) */
            s->seed = s->seed*6364136223846793005ULL + 1442695040888963407ULL;
            rust_wfc_reset(s->wfc, s->seed);
        }
    } else {
        s->hold += dt;
        if (s->hold > 2.5f){
            /* advance to next ruleset with a fresh seed */
            s->ruleset = (s->ruleset+1)%3;
            s->seed = s->seed*2862933555777941757ULL + 3037000493ULL;
            wfc_setup(s);
        }
    }
}

static void wf_render(Scene *sc, Framebuffer *fb){
    WfcState *s=sc->state;
    rust_wfc_tiles(s->wfc, s->tiles);
    for (i32 py=0;py<fb->h;++py){
        i32 gy=(i32)((f32)py/(fb->h)*s->gh); if(gy>=s->gh)gy=s->gh-1;
        for (i32 px=0;px<fb->w;++px){
            i32 gx=(i32)((f32)px/(fb->w)*s->gw); if(gx>=s->gw)gx=s->gw-1;
            i32 tile=s->tiles[gy*s->gw+gx];
            Color3 c;
            if (tile<0) c=col3(0.02f,0.02f,0.04f);        /* uncollapsed */
            else {
                c=col3(s->pal[tile*3],s->pal[tile*3+1],s->pal[tile*3+2]);
                /* subtle grid lines between cells for a "tiled" feel */
                f32 fx=(f32)px/(fb->w)*s->gw - gx, fy=(f32)py/(fb->h)*s->gh - gy;
                if (fx<0.06f||fy<0.06f) c=col_scale(c,0.6f);
            }
            fb_set(fb,px,py,c);
        }
    }
}

static void wf_key(Scene *sc, int key){
    WfcState *s=sc->state;
    if (key==KEY_TAB){ s->ruleset=(s->ruleset+1)%3; wfc_setup(s); }
    else if (key==KEY_R){ s->seed=s->seed*2862933555777941757ULL+1; wfc_setup(s); }
}

static CrtConfig wf_crt(Scene *sc){ (void)sc; return crt_config_preset("arcade"); }
static void wf_destroy(Scene *sc){ if(sc){ WfcState*s=sc->state; if(s->wfc)rust_wfc_destroy(s->wfc); free(s->tiles); free(s->pal); free(s); free(sc);} }

Scene *scene_wfc_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="wfc";
    sc->description="Wave Function Collapse procedural tiles (Rust, live solve)";
    sc->state=calloc(1,sizeof(WfcState));
    sc->init=wf_init; sc->update=wf_update; sc->render=wf_render;
    sc->on_key=wf_key; sc->destroy=wf_destroy; sc->preferred_crt=wf_crt;
    return sc;
}
