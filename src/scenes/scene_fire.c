/* ==========================================================================
 * scene_fire.c  -  the classic "Doom PSX" fire effect.
 *
 * The famous 1990s fire algorithm: a heat grid whose bottom row is held at
 * maximum. Each cell above cools by a small random amount as heat propagates
 * upward, with a random horizontal wind so the flames flicker and lean. A
 * palette maps heat (0..1) to the black→red→orange→yellow→white fire ramp. It's
 * astonishingly convincing for how little it computes  -  and a perfect fit for
 * the CRT chain's bloom.
 *
 * We also let the fire spell CATHODE: a masked set of hot "emitter" cells in
 * the shape of text (drawn with the bitmap font) seeds extra heat, so letters
 * burn upward out of the flames. Pure C over the framebuffer + noise RNG.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/text.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    i32 w, h;              /* framebuffer size */
    i32 gw, gh;            /* fire grid size (downscaled for chunky pixels) */
    u8 *heat;              /* gw*gh, 0..255 */
    u8 *emit;              /* gw*gh, persistent emitter mask (the text) */
    f32 t; u32 rng; int mode;
} FireState;

static inline u32 xr(u32 *s){ u32 x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }

/* fire palette: heat 0..255 -> color along black->red->orange->yellow->white */
static Color3 fire_color(u8 h){
    f32 t=h/255.0f;
    /* piecewise ramp */
    f32 r = t<0.5f ? t*2.0f : 1.0f;
    f32 g = t<0.4f ? 0.0f : (t<0.8f ? (t-0.4f)/0.4f : 1.0f);
    f32 b = t<0.8f ? 0.0f : (t-0.8f)/0.2f;
    /* HDR boost at the top so the hottest cells bloom */
    f32 boost = 1.0f + t*t*0.8f;
    return col3(r*boost, g*boost, b*boost);
}

static void fire_stamp_text(FireState *s){
    memset(s->emit, 0, (size_t)s->gw*s->gh);
    /* draw "CATHODE" into a temporary 1-bit mask via a small framebuffer trick:
     * we render the font into a scratch RGB buffer, then threshold. Simpler:
     * use text into a scratch Framebuffer sized to the grid. */
    Framebuffer *tmp=fb_create(s->gw, s->gh);
    fb_clear(tmp, col3(0,0,0));
    const char *msg="CATHODE";
    i32 scale = s->gw/64; if (scale<1) scale=1;
    i32 tw = text_width(msg, scale, 1);
    text_draw(tmp, (s->gw-tw)/2, s->gh/2, msg, col3(1,1,1), scale, 1);
    for (i32 y=0;y<s->gh;++y) for (i32 x=0;x<s->gw;++x){
        const f32 *p=&tmp->px[((size_t)y*s->gw+x)*3];
        if (p[0]+p[1]+p[2] > 0.5f) s->emit[y*s->gw+x]=1;
    }
    fb_destroy(tmp);
}

static void fire_init(Scene *sc, i32 w, i32 h){
    FireState *s=sc->state; s->w=w; s->h=h; s->t=0; s->rng=0xF1EE00Du; s->mode=0;
    s->gw = w; s->gh = h;
    s->heat=calloc((size_t)s->gw*s->gh,1);
    s->emit=calloc((size_t)s->gw*s->gh,1);
    fire_stamp_text(s);
}

static void fire_update(Scene *sc, f32 dt, f32 t){
    FireState *s=sc->state; (void)dt; s->t=t;
    const i32 gw=s->gw, gh=s->gh;
    /* seed the bottom row hot (flicker a little) */
    for (i32 x=0;x<gw;++x){
        u32 r=xr(&s->rng);
        s->heat[(gh-1)*gw+x] = 220 + (r%36);
    }
    /* emitter cells (the text) inject heat wherever the mask is set */
    if (s->mode==0){
        for (i32 i=0;i<gw*gh;++i) if (s->emit[i]) s->heat[i]=255;
    }
    /* propagate upward: each cell = cell below cooled by a random amount, with
     * a random horizontal source offset (wind + flicker). Classic Doom fire. */
    for (i32 y=0;y<gh-1;++y){
        for (i32 x=0;x<gw;++x){
            u32 r=xr(&s->rng);
            int decay = r & 3;                    /* cool by 0..3 */
            int src_x = x + (int)((r>>2)&3) - 1;  /* wind: -1..+1 */
            if (src_x<0) src_x=0; if (src_x>=gw) src_x=gw-1;
            int below = s->heat[(y+1)*gw+src_x];
            int nh = below - decay;
            if (nh<0) nh=0;
            s->heat[y*gw + x] = (u8)nh;
        }
    }
}

static void fire_render(Scene *sc, Framebuffer *fb){
    FireState *s=sc->state;
    for (i32 y=0;y<fb->h;++y){
        f32 *row=&fb->px[(size_t)y*fb->w*3];
        i32 gy = y;  /* grid == fb size */
        for (i32 x=0;x<fb->w;++x){
            Color3 c=fire_color(s->heat[gy*s->gw + x]);
            row[3*x+0]=c.r; row[3*x+1]=c.g; row[3*x+2]=c.b;
        }
    }
}

static void fire_key(Scene *sc, int key){
    FireState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->mode^=1;   /* toggle the burning text */
}
static CrtConfig fire_crt(Scene *sc){ (void)sc; CrtConfig c=crt_config_preset("arcade"); c.bloom=0.6f; c.persistence=0.4f; return c; }
static void fire_destroy(Scene *sc){ if(sc){ FireState*s=sc->state; free(s->heat); free(s->emit); free(s); free(sc);} }

Scene *scene_fire_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="fire";
    sc->description="Classic Doom-PSX fire effect, with burning CATHODE text (TAB toggles)";
    sc->state=calloc(1,sizeof(FireState));
    sc->init=fire_init; sc->update=fire_update; sc->render=fire_render;
    sc->on_key=fire_key; sc->destroy=fire_destroy; sc->preferred_crt=fire_crt;
    return sc;
}
