/* ==========================================================================
 * cathode/scene.h — the demo scene interface + registry.
 * Each scene is a self-contained module implementing this vtable.
 * ========================================================================== */
#ifndef CATHODE_SCENE_H
#define CATHODE_SCENE_H

#include "cathode/framebuffer.h"
#include "cathode/crt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Scene Scene;
struct Scene {
    const char *name;
    const char *description;
    void  (*init)(Scene *s, i32 w, i32 h);
    void  (*update)(Scene *s, f32 dt, f32 t);
    void  (*render)(Scene *s, Framebuffer *fb);   /* write linear RGB */
    void  (*on_key)(Scene *s, int key);           /* Key enum value */
    void  (*destroy)(Scene *s);
    CrtConfig (*preferred_crt)(Scene *s);         /* scene picks a CRT look */
    void  *state;
};

/* Registry — scenes register themselves; app iterates them. */
typedef Scene *(*SceneFactory)(void);
void   scene_register(const char *name, SceneFactory f);
i32    scene_count(void);
Scene *scene_create(i32 index);
const char *scene_name_at(i32 index);

/* Factories implemented by the scene modules. */
Scene *scene_starfield_create(void);
Scene *scene_galaxy_create(void);
Scene *scene_raymarch_create(void);
Scene *scene_solids_create(void);
Scene *scene_fluid_create(void);
Scene *scene_tunnel_create(void);
Scene *scene_plasma_create(void);
Scene *scene_terrain_create(void);
/* Phase-2 scenes (pure-C, self-contained). */
Scene *scene_mandelbrot_create(void);
Scene *scene_life_create(void);
Scene *scene_boids_create(void);
Scene *scene_attractor_create(void);
/* Phase-3 emergent-systems scenes (pure-C). */
Scene *scene_slime_create(void);
Scene *scene_lenia_create(void);
Scene *scene_flow_create(void);
Scene *scene_julia_create(void);      /* NEON fractal kernel */
Scene *scene_ripple_create(void);     /* 2D wave eqn + NEON blur bloom */
Scene *scene_cells_create(void);      /* Worley/Voronoi cellular texture */
Scene *scene_flame_create(void);      /* fractal flame (chaos game) */
Scene *scene_ifs_create(void);        /* iterated function systems (fern etc.) */
Scene *scene_sph_create(void);        /* smoothed-particle hydrodynamics fluid */
Scene *scene_buddhabrot_create(void); /* Buddhabrot / Nebulabrot density */
Scene *scene_tesseract_create(void);  /* rotating 4D polytopes */
Scene *scene_pendulum_create(void);   /* double-pendulum chaos array */
Scene *scene_bz_create(void);         /* excitable-media spiral waves */
Scene *scene_orbital_create(void);    /* volume-rendered hydrogen orbitals */
Scene *scene_wireworld_create(void);  /* Wireworld electron CA */
Scene *scene_observatory_create(void);/* combined: terrain + N-body + NEON projector */
Scene *scene_brain_create(void);      /* Brian's Brain 3-state CA */
Scene *scene_parametric_create(void); /* parametric surfaces (Mobius/Klein/knot) */
Scene *scene_demoscene_create(void);  /* copper bars + sine-scroller cracktro */
Scene *scene_bootscreen_create(void); /* retro POST/boot sequence with cursor */
Scene *scene_credits_create(void);    /* perspective credits crawl (font in 3D) */
Scene *scene_hyperbolic_create(void); /* {p,q} hyperbolic tilings (Poincare disk) */
Scene *scene_qjulia_create(void);     /* ray-marched quaternion Julia (4D fractal) */
Scene *scene_chiptune_create(void);   /* live tracker UI + synth + FFT (audio stack) */
Scene *scene_physics_create(void);    /* 2D rigid-body playground (impulse solver) */
/* Phase-2 scenes backed by polyglot subsystems (Rust/C++). */
Scene *scene_reaction_create(void);   /* Rust reaction-diffusion */
Scene *scene_cloth_create(void);      /* Rust Verlet cloth */
Scene *scene_dla_create(void);        /* Rust diffusion-limited aggregation */
Scene *scene_wfc_create(void);        /* Rust wave function collapse */
Scene *scene_pathtrace_create(void);  /* C++ BVH path tracer */
Scene *scene_wireframe_create(void);  /* C++ scene graph */
Scene *scene_audioviz_create(void);   /* C++ synth visualizer */
Scene *scene_metaballs_create(void);  /* C++ marching cubes + rasterizer */
Scene *scene_spectrogram_create(void);/* Rust FFT spectrogram */
Scene *scene_maze_create(void);       /* Rust maze gen + BFS flood-fill solve */
Scene *scene_lsystem_create(void);    /* Rust L-system turtle-graphics plants */
Scene *scene_csg_create(void);        /* C++ CSG boolean solid + marching cubes */
Scene *scene_softbody_create(void);   /* C++ pressurized soft-body blobs */
Scene *scene_planet_create(void);     /* procedurally-textured rotating planet */
Scene *scene_voxel_create(void);      /* Comanche-style voxel-heightmap terrain */
Scene *scene_truchet_create(void);    /* animated Truchet quarter-arc tilings */
Scene *scene_chladni_create(void);    /* Chladni cymatics nodal-line sand sim */
Scene *scene_magnetic_create(void);   /* magnetic-pendulum fractal basins */
Scene *scene_fire_create(void);       /* Doom-PSX fire effect + burning text */

/* Called once at startup to populate the registry. */
void   scenes_register_all(void);
/* Registers polyglot (Rust/C++)-backed scenes; guarded so the pure-C build
 * works before those subsystems are wired in. */
void   scenes_register_polyglot(void);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_SCENE_H */
