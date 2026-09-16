/* ==========================================================================
 * scene_maze.c  -  animated maze generation + BFS flood-fill solve (Rust core).
 *
 * Uses the Rust maze module (recursive-backtracker generation + BFS solve):
 * we draw the maze walls, then animate a flood filling outward from the start
 * cell colored by BFS distance (a spreading "dye"), and finally trace the
 * unique shortest path from start to goal in a bright contrasting color. When
 * the animation completes it holds briefly, then regenerates a fresh maze.
 *
 * The maze is drawn at 2 framebuffer pixels per cell plus wall pixels, scaled
 * to fill the display; everything is linear RGB so the CRT chain glows.
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/tui.h"
#include "cathode/framebuffer.h"
#include "cathode/rustcore.h"
#include "cathode/vec.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* wall-open bits (mirror the Rust module) */
#define WN 1
#define WE 2
#define WS 4
#define WW 8

typedef struct {
    i32 w, h; f32 t;
    RustMaze *mz;
    i32 mw, mh;          /* maze dimensions in cells */
    u8  *walls;          /* mw*mh */
    i32 *dist;           /* mw*mh */
    i32 *path; i32 path_len;
    i32 max_dist;
    f32 anim;            /* flood-fill wavefront distance */
    u64 seed;
    f32 hold;            /* post-solve hold timer */
} MazeState;

static void maze_build(MazeState *s){
    if (s->mz) rust_maze_destroy(s->mz);
    s->mz = rust_maze_create(s->mw, s->mh, s->seed);
    rust_maze_walls(s->mz, s->walls);
    rust_maze_dist(s->mz, s->dist);
    s->max_dist = rust_maze_max_dist(s->mz);
    s->path_len = rust_maze_path(s->mz, s->path, s->mw*s->mh);
    s->anim = 0;
    s->hold = 0;
}

static void mz_init(Scene *sc, i32 w, i32 h){
    MazeState *s=sc->state; s->w=w; s->h=h; s->t=0;
    /* choose a cell grid so each cell is ~10px (walls + path stay legible) */
    s->mw = w/10; if (s->mw<6) s->mw=6; if (s->mw>48) s->mw=48;
    s->mh = h/10; if (s->mh<6) s->mh=6; if (s->mh>48) s->mh=48;
    s->walls = malloc((size_t)s->mw*s->mh);
    s->dist  = malloc((size_t)s->mw*s->mh*sizeof(i32));
    s->path  = malloc((size_t)s->mw*s->mh*sizeof(i32));
    s->mz=NULL; s->seed=0x51EEDULL;
    maze_build(s);
}

static void mz_update(Scene *sc, f32 dt, f32 t){
    MazeState *s=sc->state; s->t=t;
    if (dt<=0) return;
    if (s->anim < s->max_dist + 1){
        s->anim += dt * (s->max_dist * 0.5f + 8.0f);  /* fill in ~2s regardless of size */
    } else {
        s->hold += dt;
        if (s->hold > 2.5f){ s->seed = s->seed*6364136223846793005ULL + 1442695040888963407ULL; maze_build(s); }
    }
}

static Color3 dist_color(f32 d, f32 maxd){
    f32 u = maxd>0? d/maxd : 0; if(u>1)u=1;
    /* deep blue -> cyan -> green -> yellow heat ramp */
    return col3(0.15f+0.85f*u, 0.3f+0.6f*sinf(u*3.14159f), 0.9f-0.7f*u);
}

static void mz_render(Scene *sc, Framebuffer *fb){
    MazeState *s=sc->state;
    const i32 W=fb->w, H=fb->h;
    fb_clear(fb, col3(0.01f,0.01f,0.02f));

    /* cell size in pixels (leave a 1px border); each cell is cs x cs, walls
     * drawn on the shared edges */
    i32 cs_x = (W-2)/s->mw, cs_y=(H-2)/s->mh;
    i32 cs = cs_x<cs_y?cs_x:cs_y; if (cs<2) cs=2;
    i32 ox = (W - cs*s->mw)/2, oy=(H - cs*s->mh)/2;
    Color3 wall = col3(0.55f,0.6f,0.75f);

    for (i32 cy=0; cy<s->mh; ++cy){
        for (i32 cx=0; cx<s->mw; ++cx){
            i32 ci = cy*s->mw+cx;
            i32 x0 = ox + cx*cs, y0 = oy + cy*cs;
            /* fill cell interior colored by BFS distance if the flood reached it */
            f32 d=(f32)s->dist[ci];
            Color3 fill = col3(0.03f,0.03f,0.05f);
            if (d>=0 && d <= s->anim) fill = dist_color(d, (f32)s->max_dist);
            for (i32 yy=y0; yy<y0+cs; ++yy)
                for (i32 xx=x0; xx<x0+cs; ++xx)
                    fb_set(fb, xx, yy, fill);
            /* draw closed walls on N and W edges (E/S handled by neighbor) */
            u8 ob = s->walls[ci];
            if (!(ob & WN)) for (i32 xx=x0; xx<x0+cs; ++xx) fb_set(fb, xx, y0, wall);
            if (!(ob & WW)) for (i32 yy=y0; yy<y0+cs; ++yy) fb_set(fb, x0, yy, wall);
            /* outer border: draw S/E on the last row/col */
            if (cy==s->mh-1 && !(ob & WS)) for (i32 xx=x0; xx<x0+cs; ++xx) fb_set(fb, xx, y0+cs-1, wall);
            if (cx==s->mw-1 && !(ob & WE)) for (i32 yy=y0; yy<y0+cs; ++yy) fb_set(fb, x0+cs-1, yy, wall);
        }
    }

    /* once fully flooded, trace the solution path bright */
    if (s->anim >= s->max_dist && s->path_len>0){
        f32 pulse = 0.6f+0.4f*sinf(s->t*5.0f);
        Color3 pc = col3(1.0f*pulse, 0.95f*pulse, 0.3f*pulse);
        for (i32 i=0;i<s->path_len;++i){
            i32 ci=s->path[i]; i32 cx=ci%s->mw, cy=ci/s->mw;
            i32 x0=ox+cx*cs, y0=oy+cy*cs;
            i32 mx=x0+cs/2, my=y0+cs/2;
            for (i32 yy=my-cs/4; yy<=my+cs/4; ++yy)
                for (i32 xx=mx-cs/4; xx<=mx+cs/4; ++xx)
                    fb_add(fb, xx, yy, pc);
        }
    }
}

static void mz_key(Scene *sc, int key){
    MazeState *s=sc->state;
    if (key==KEY_TAB||key==KEY_ENTER){ s->seed = s->seed*2862933555777941757ULL + 3037000493ULL; maze_build(s); }
}
static CrtConfig mz_crt(Scene *sc){ (void)sc; return crt_config_preset("trinitron"); }
static void mz_destroy(Scene *sc){
    if(sc){ MazeState*s=sc->state;
        if(s->mz) rust_maze_destroy(s->mz);
        free(s->walls); free(s->dist); free(s->path); free(s); free(sc);
    }
}

Scene *scene_maze_create(void){
    Scene *sc=calloc(1,sizeof(Scene));
    sc->name="maze";
    sc->description="Maze generation + BFS flood-fill solve (Rust recursive-backtracker)";
    sc->state=calloc(1,sizeof(MazeState));
    sc->init=mz_init; sc->update=mz_update; sc->render=mz_render;
    sc->on_key=mz_key; sc->destroy=mz_destroy; sc->preferred_crt=mz_crt;
    return sc;
}
