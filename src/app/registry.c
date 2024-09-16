/* ==========================================================================
 * registry.c  -  the scene registry. Scenes register a name + factory here;
 * the app enumerates and instantiates them by index.
 * ========================================================================== */
#include "cathode/scene.h"
#include <string.h>
#include <stdio.h>

#define MAX_SCENES 128   /* generous headroom; scene_register silently drops past this */

typedef struct { const char *name; SceneFactory factory; } RegEntry;
static RegEntry g_scenes[MAX_SCENES];
static i32      g_nscenes = 0;

void scene_register(const char *name, SceneFactory f) {
    if (!f) return;
    if (g_nscenes >= MAX_SCENES) {
        /* Don't fail silently  -  a dropped scene is an invisible bug (this bit
         * us once when the cap was 32). Warn loudly; bump MAX_SCENES to fix. */
        fprintf(stderr, "cathode: scene registry full (MAX_SCENES=%d), dropping '%s'\n",
                MAX_SCENES, name ? name : "?");
        return;
    }
    /* de-dup by name so repeated scenes_register_all() calls are harmless */
    for (i32 i = 0; i < g_nscenes; ++i)
        if (g_scenes[i].name && name && strcmp(g_scenes[i].name, name) == 0) {
            g_scenes[i].factory = f;
            return;
        }
    g_scenes[g_nscenes].name = name;
    g_scenes[g_nscenes].factory = f;
    g_nscenes++;
}

i32 scene_count(void) { return g_nscenes; }

Scene *scene_create(i32 index) {
    if (index < 0 || index >= g_nscenes || !g_scenes[index].factory) return NULL;
    return g_scenes[index].factory();
}

const char *scene_name_at(i32 index) {
    if (index < 0 || index >= g_nscenes) return "";
    return g_scenes[index].name;
}

/* Register every scene module. Each factory is declared in scene.h and
 * implemented by a Layer-C module. Guarded so missing scenes (if a module
 * failed) can be commented out at integration without touching callers. */
void scenes_register_all(void) {
    scene_register("starfield", scene_starfield_create);
    scene_register("galaxy",    scene_galaxy_create);
    scene_register("raymarch",  scene_raymarch_create);
    scene_register("solids",    scene_solids_create);
    scene_register("fluid",     scene_fluid_create);
    scene_register("tunnel",    scene_tunnel_create);
    scene_register("plasma",    scene_plasma_create);
    scene_register("terrain",   scene_terrain_create);
    /* Phase-2 pure-C scenes. */
    scene_register("mandelbrot", scene_mandelbrot_create);
    scene_register("life",       scene_life_create);
    scene_register("boids",      scene_boids_create);
    scene_register("attractor",  scene_attractor_create);
    /* Phase-3 emergent-systems scenes. */
    scene_register("slime",      scene_slime_create);
    scene_register("lenia",      scene_lenia_create);
    scene_register("flow",       scene_flow_create);
    scene_register("julia",      scene_julia_create);
    scene_register("ripple",     scene_ripple_create);
    scene_register("cells",      scene_cells_create);
    scene_register("flame",      scene_flame_create);
    scene_register("ifs",        scene_ifs_create);
    scene_register("sph",        scene_sph_create);
    scene_register("buddhabrot", scene_buddhabrot_create);
    scene_register("tesseract",  scene_tesseract_create);
    scene_register("pendulum",   scene_pendulum_create);
    scene_register("bz",         scene_bz_create);
    scene_register("orbital",    scene_orbital_create);
    scene_register("wireworld",  scene_wireworld_create);
    scene_register("observatory",scene_observatory_create);
    scene_register("brain",      scene_brain_create);
    scene_register("parametric", scene_parametric_create);
    scene_register("demoscene",  scene_demoscene_create);
    scene_register("bootscreen", scene_bootscreen_create);
    scene_register("credits",    scene_credits_create);
    scene_register("hyperbolic", scene_hyperbolic_create);
    scene_register("qjulia",     scene_qjulia_create);
    scene_register("physics",    scene_physics_create);
    scene_register("planet",     scene_planet_create);
    scene_register("voxel",      scene_voxel_create);
    scene_register("truchet",    scene_truchet_create);
    scene_register("chladni",    scene_chladni_create);
    scene_register("magnetic",   scene_magnetic_create);
    scene_register("fire",       scene_fire_create);
    /* Phase-2 polyglot-backed scenes are registered by scenes_register_polyglot()
     * once their Rust/C++ subsystems are linked in (see below). */
    scenes_register_polyglot();
}

/* Registered separately so the pure-C build stays independent of the polyglot
 * subsystems during incremental bring-up. Each factory is weakly guarded: if a
 * subsystem isn't linked yet the factory simply isn't referenced. */
void scenes_register_polyglot(void) {
#ifdef CATHODE_HAVE_POLYGLOT_SCENES
    scene_register("reaction",  scene_reaction_create);
    scene_register("cloth",     scene_cloth_create);
    scene_register("dla",       scene_dla_create);
    scene_register("wfc",       scene_wfc_create);
    scene_register("pathtrace", scene_pathtrace_create);
    scene_register("wireframe", scene_wireframe_create);
    scene_register("audioviz",  scene_audioviz_create);
    scene_register("metaballs", scene_metaballs_create);
    scene_register("spectrogram", scene_spectrogram_create);
    scene_register("chiptune",  scene_chiptune_create);
    scene_register("maze",      scene_maze_create);
    scene_register("csg",       scene_csg_create);
    scene_register("lsystem",   scene_lsystem_create);
    scene_register("softbody",  scene_softbody_create);
#endif
}
