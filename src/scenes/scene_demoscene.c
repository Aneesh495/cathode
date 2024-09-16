/* ==========================================================================
 * scene_demoscene.c  -  a classic early-90s cracktro / demo screen.
 *
 * Three layered effects, all pure C into the linear-RGB framebuffer, then run
 * through the NTSC/CRT chain (so the raster bars bloom and the text bleeds
 * chroma exactly like a real Amiga/C64 demo on a CRT):
 *
 *   1. copper raster bars  -  horizontal color bands whose vertical position is
 *      driven by stacked sines (the "copper list" look), additively blended.
 *   2. starfield parallax  -  three depth layers of drifting points.
 *   3. sine-scroller        -  a greeting scrolled right-to-left, each glyph
 *      vertically displaced by a travelling sine wave and hue-cycled, the
 *      signature demoscene text effect.
 *
 * Everything is deterministic given (t), so it golden-tests cleanly.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/text.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NSTARS 220

typedef struct { f32 x, y, z; } Star;

typedef struct {
    i32 w, h;
    f32 t;
    Star stars[NSTARS];
    u32 rng;
    int palette;   /* which bar color scheme */
} DemoState;

static const char SCROLL_TEXT[] =
    "   CATHODE PRESENTS ... A CPU-ONLY GRAPHICS ENGINE WITH SOFTWARE NTSC/CRT "
    "EMULATION, HAND-WRITTEN AARCH64 NEON ASSEMBLY, AND A POLYGLOT C / RUST / "
    "C++ BACKEND.   GREETINGS TO EVERYONE STILL RENDERING PIXELS THE HARD WAY.   "
    "WRAP AROUND AND DO IT AGAIN ...        ";

static inline u32 xs32(u32 *s){ u32 x=*s; x^=x<<13; x^=x>>17; x^=x<<5; *s=x; return x; }
static inline f32 frnd(u32 *s){ return (f32)(xs32(s)>>8) * (1.0f/16777216.0f); }

/* HSV->RGB (h in [0,1)) for the hue-cycled text + bars. */
static Color3 hsv(f32 h, f32 s, f32 v){
    h = h - floorf(h);
    f32 r=0,g=0,b=0; f32 i=floorf(h*6.0f); f32 f=h*6.0f-i;
    f32 p=v*(1-s), q=v*(1-s*f), t=v*(1-s*(1-f));
    switch(((int)i)%6){
        case 0: r=v;g=t;b=p;break; case 1: r=q;g=v;b=p;break;
        case 2: r=p;g=v;b=t;break; case 3: r=p;g=q;b=v;break;
        case 4: r=t;g=p;b=v;break; default:r=v;g=p;b=q;break;
    }
    return col3(r,g,b);
}

static void dm_init(Scene *sc, i32 w, i32 h){
    DemoState *s=sc->state; s->w=w; s->h=h; s->t=0; s->rng=0xC0FFEEu; s->palette=0;
    for (int i=0;i<NSTARS;++i){
        s->stars[i].x = frnd(&s->rng)*w;
        s->stars[i].y = frnd(&s->rng)*h;
        s->stars[i].z = 0.3f + frnd(&s->rng)*2.7f;   /* depth => speed + brightness */
    }
}
static void dm_update(Scene *sc, f32 dt, f32 t){
    DemoState *s=sc->state; s->t=t;
    for (int i=0;i<NSTARS;++i){
        s->stars[i].x -= (10.0f + s->stars[i].z*26.0f)*dt;
        if (s->stars[i].x < 0){ s->stars[i].x += s->w; s->stars[i].y = frnd(&s->rng)*s->h; }
    }
}

static void dm_render(Scene *sc, Framebuffer *fb){
    DemoState *s=sc->state;
    const i32 w=fb->w, h=fb->h; const f32 t=s->t;
    fb_clear(fb, col3(0.01f,0.01f,0.03f));

    /* ---- 1. copper raster bars ---- */
    /* Several bars; each has a base hue and a sine-driven center row. A bar is
     * a smooth vertical gaussian band added into every column. */
    const int NBARS=5;
    for (int b=0;b<NBARS;++b){
        f32 phase = t*(0.6f+0.13f*b) + b*1.7f;
        f32 cy = (0.5f + 0.42f*sinf(phase)) * h;
        f32 hue = (s->palette==0)
                    ? (0.02f*t + b*0.14f)          /* rainbow drift */
                    : (0.58f + 0.06f*sinf(phase));  /* icy blue band */
        Color3 base = hsv(hue, 0.85f, 1.0f);
        f32 half = 3.5f;   /* bar half-height in pixels */
        int y0 = (int)(cy-half*2)-1, y1=(int)(cy+half*2)+1;
        if (y0<0) y0=0; if (y1>=h) y1=h-1;
        for (int y=y0;y<=y1;++y){
            f32 d=((f32)y-cy)/half;
            f32 inten=expf(-d*d)*0.9f;
            if (inten<0.004f) continue;
            Color3 c=col_scale(base,inten);
            f32 *row=&fb->px[(size_t)y*w*3];
            for (int x=0;x<w;++x){ row[3*x+0]+=c.r; row[3*x+1]+=c.g; row[3*x+2]+=c.b; }
        }
    }

    /* ---- 2. parallax starfield (additive) ---- */
    for (int i=0;i<NSTARS;++i){
        f32 b = 0.25f + s->stars[i].z*0.28f;
        fb_add(fb, (i32)s->stars[i].x, (i32)s->stars[i].y, col3(b,b,b));
    }

    /* ---- 3. sine-scroller ---- */
    /* choose glyph scale from height; keep readable but chunky */
    i32 scale = h/40; if (scale<1) scale=1; if (scale>4) scale=4;
    i32 gw = (FONT_W+1)*scale;             /* per-glyph advance */
    i32 baseline = (i32)(h*0.62f);
    f32 speed = 34.0f*scale;               /* px/sec leftward */
    i32 nchars = (i32)(sizeof(SCROLL_TEXT)-1);
    f32 total = (f32)nchars*gw;
    /* leftmost pen position wraps modulo total, starting off the right edge */
    f32 scroll = w - fmodf(t*speed, total + w);
    for (i32 i=0;i<nchars;++i){
        f32 gx = scroll + (f32)i*gw;
        if (gx < -gw || gx > w) continue;   /* offscreen cull */
        f32 wob = sinf(t*3.0f + (f32)i*0.45f) * (h*0.12f);
        i32 gy = baseline + (i32)wob;
        Color3 c = hsv(0.03f*t + i*0.03f, 0.7f, 1.0f);
        text_char(fb, (i32)gx, gy, SCROLL_TEXT[i], c, scale, 1 /*additive*/);
    }

    /* ---- static title line (overwrite so it stays crisp) ---- */
    {
        const char *title="* CATHODE DEMO *";
        i32 tscale = h/56; if(tscale<1)tscale=1; if(tscale>3)tscale=3;
        i32 tw = text_width(title, tscale, 1);
        f32 pulse = 0.6f+0.4f*sinf(t*4.0f);
        text_draw_add(fb, (w-tw)/2, (i32)(h*0.10f), title,
                      col_scale(hsv(0.0f, 0.0f, 1.0f), pulse), tscale, 1);
    }
}

static void dm_key(Scene *sc, int key){
    DemoState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER) s->palette=!s->palette;
}
static CrtConfig dm_crt(Scene *sc){ (void)sc; return crt_config_preset("broadcast"); }
static void dm_destroy(Scene *sc){ if(sc){ free(sc->state); free(sc);} }

Scene *scene_demoscene_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="demoscene";
    sc->description="Copper raster bars + parallax stars + sine-scroller (cracktro homage)";
    sc->state=calloc(1,sizeof(DemoState));
    sc->init=dm_init; sc->update=dm_update; sc->render=dm_render;
    sc->on_key=dm_key; sc->destroy=dm_destroy; sc->preferred_crt=dm_crt;
    return sc;
}
