/* ==========================================================================
 * cathode/raster.h  -  CPU triangle rasterizer with a full transform pipeline.
 * Barycentric, perspective-correct, z-buffered, with Phong-ish shading.
 * ========================================================================== */
#ifndef CATHODE_RASTER_H
#define CATHODE_RASTER_H

#include "cathode/framebuffer.h"
#include "cathode/vec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { Vec3 pos; Vec3 normal; Color3 color; Vec2 uv; } Vertex;
typedef struct { u32 a, b, c; } Tri;

typedef struct {
    Vertex *verts; u32 nverts;
    Tri    *tris;  u32 ntris;
} Mesh;

/* Optional texture sampler: given perspective-correct UV (any range; wrap or
 * clamp as you like) return a color that MODULATES the interpolated base color.
 * NULL disables texturing (identical output to the untextured path). */
typedef Color3 (*TexFn)(f32 u, f32 v, void *user);

typedef struct {
    Mat4  model, view, proj;
    Vec3  light_dir;      /* normalized, world space */
    Color3 light_color;
    Color3 ambient;
    Vec3  cam_pos;
    f32   specular;       /* specular strength */
    f32   shininess;
    int   wireframe;
    TexFn texfn;          /* optional; NULL = untextured */
    void *tex_user;       /* passed to texfn */
} RasterCtx;

/* Procedural mesh generators (return heap meshes; free with mesh_free). */
Mesh *mesh_cube(f32 size);
Mesh *mesh_sphere(f32 radius, int stacks, int slices);
Mesh *mesh_torus(f32 R, f32 r, int nmaj, int nmin);
Mesh *mesh_icosphere(f32 radius, int subdiv);
Mesh *mesh_from_heightfield(const f32 *h, int nx, int ny, f32 scale, f32 zscale);
void  mesh_free(Mesh *m);
void  mesh_compute_normals(Mesh *m);

/* Draw a mesh into fb using ctx. Depth-tested. */
void  raster_mesh(Framebuffer *fb, const Mesh *m, const RasterCtx *ctx);
/* Draw a single triangle in already-clip-space coords (used internally + tests). */
void  raster_triangle_clip(Framebuffer *fb, Vec4 a, Vec4 b, Vec4 c,
                           Color3 ca, Color3 cb, Color3 cc,
                           Vec3 na, Vec3 nb, Vec3 nc, const RasterCtx *ctx);

#ifdef __cplusplus
}
#endif
#endif /* CATHODE_RASTER_H */
