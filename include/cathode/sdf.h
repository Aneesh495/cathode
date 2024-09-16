/* ==========================================================================
 * cathode/sdf.h  -  signed-distance-field ray marcher.
 * Renders analytic scenes (spheres, boxes, tori, mandelbulb) with soft
 * shadows, ambient occlusion, and normals via gradient of the field.
 * ========================================================================== */
#ifndef CATHODE_SDF_H
#define CATHODE_SDF_H

#include "cathode/framebuffer.h"
#include "cathode/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A scene is a callback returning the signed distance to the nearest surface
 * and, via out_mat, a material id. time animates the field. */
typedef f32 (*SdfFn)(Vec3 p, f32 time, i32 *out_mat, void *user);

typedef struct {
    SdfFn  field;
    void  *user;
    Vec3   cam_pos, cam_target;
    f32    fov;             /* radians */
    Vec3   light_dir;       /* normalized */
    Color3 sky_top, sky_bottom;
    Color3 mat_albedo[8];   /* per material id */
    f32    time;
    i32    max_steps;
    f32    max_dist;
    f32    epsilon;
    i32    aa;              /* sqrt of samples per pixel (1,2) */
} SdfScene;

/* Built-in fields (pass as scene.field). */
f32 sdf_scene_primitives(Vec3 p, f32 t, i32 *mat, void *user);
f32 sdf_scene_mandelbulb(Vec3 p, f32 t, i32 *mat, void *user);
f32 sdf_scene_infinite  (Vec3 p, f32 t, i32 *mat, void *user);

/* Render a horizontal band [y0,y1) of fb  -  enables tiled threading. */
void sdf_render_band(Framebuffer *fb, const SdfScene *sc, i32 y0, i32 y1);
void sdf_render(Framebuffer *fb, const SdfScene *sc);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_SDF_H */
