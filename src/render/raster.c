/* ==========================================================================
 * raster.c  -  AArch64 NEON CPU triangle rasterizer.
 *
 *  model -> world -> view -> clip  (MVP via NEON mat4_transform / mat4_mul)
 *  perspective divide -> NDC -> viewport
 *  barycentric edge-function fill, perspective-correct attribute interp,
 *  z-buffer test, Blinn-Phong shading (ambient + diffuse + specular).
 *  Backface culling by screen-space winding. Optional wireframe.
 *
 * Hot path: hand-written NEON mat4 (~4x vs portable C reference at -O3).
 * Headless timing drives scenes that use this rasterizer at 1440p with a
 * documented <2 ms/frame gate (scene + CRT); see docs/BENCHMARKS.md.
 * ========================================================================== */
#include "cathode/raster.h"
#include "cathode/simd.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

/* A vertex after MVP transform: clip-space position + world attrs for shading. */
typedef struct {
    Vec4 clip;
    Vec3 world_pos;
    Vec3 world_norm;
    Color3 color;
    Vec2 uv;
} VOut;

static inline Vec3 mat4_point(const Mat4 *m, Vec3 p) {
    /* transform a point (w=1) using NEON, return xyz */
    f32 in[4] = {p.x,p.y,p.z,1.0f}, out[4];
    mat4_transform_neon(out, m->m, in);
    return v3(out[0],out[1],out[2]);
}
static inline Vec4 mat4_point4(const Mat4 *m, Vec3 p) {
    f32 in[4] = {p.x,p.y,p.z,1.0f}, out[4];
    mat4_transform_neon(out, m->m, in);
    return v4(out[0],out[1],out[2],out[3]);
}
static inline Vec3 mat4_dir(const Mat4 *m, Vec3 d) {
    /* transform a direction (w=0); good enough for rigid+uniform-scale models */
    f32 in[4] = {d.x,d.y,d.z,0.0f}, out[4];
    mat4_transform_neon(out, m->m, in);
    return v3(out[0],out[1],out[2]);
}

static void draw_line(Framebuffer *fb, int x0,int y0,int x1,int y1, Color3 c) {
    int dx=abs(x1-x0), sx=x0<x1?1:-1;
    int dy=-abs(y1-y0), sy=y0<y1?1:-1;
    int err=dx+dy;
    for (;;) {
        fb_set(fb, x0, y0, c);
        if (x0==x1 && y0==y1) break;
        int e2=2*err;
        if (e2>=dy){ err+=dy; x0+=sx; }
        if (e2<=dx){ err+=dx; y0+=sy; }
    }
}

static inline f32 edge(f32 ax,f32 ay,f32 bx,f32 by,f32 cx,f32 cy){
    return (cx-ax)*(by-ay) - (cy-ay)*(bx-ax);
}

/* Rasterize one triangle already in screen space with clip.w kept for
 * perspective-correct interpolation. */
static void fill_tri(Framebuffer *fb, VOut A, VOut B, VOut C, const RasterCtx *ctx) {
    /* screen coords (already viewport-mapped in .clip.x/.y; .z = ndc depth,
     * .w = clip w for perspective correction) */
    f32 ax=A.clip.x, ay=A.clip.y, bx=B.clip.x, by=B.clip.y, cx=C.clip.x, cy=C.clip.y;

    f32 area = edge(ax,ay,bx,by,cx,cy);
    if (area == 0.0f) return;
    /* backface cull: keep counter-clockwise (area>0) unless wireframe */
    if (!ctx->wireframe && area < 0.0f) return;
    f32 inv_area = 1.0f/area;

    int minx=(int)floorf(ct_minf(ax,ct_minf(bx,cx)));
    int maxx=(int)ceilf (ct_maxf(ax,ct_maxf(bx,cx)));
    int miny=(int)floorf(ct_minf(ay,ct_minf(by,cy)));
    int maxy=(int)ceilf (ct_maxf(ay,ct_maxf(by,cy)));
    if (minx<0) minx=0; if (miny<0) miny=0;
    if (maxx>fb->w-1) maxx=fb->w-1; if (maxy>fb->h-1) maxy=fb->h-1;

    f32 iwa=1.0f/A.clip.w, iwb=1.0f/B.clip.w, iwc=1.0f/C.clip.w;

    for (int y=miny;y<=maxy;++y){
        for (int x=minx;x<=maxx;++x){
            f32 px=x+0.5f, py=y+0.5f;
            f32 w0=edge(bx,by,cx,cy,px,py);
            f32 w1=edge(cx,cy,ax,ay,px,py);
            f32 w2=edge(ax,ay,bx,by,px,py);
            /* inside test (allow either winding when wireframe off already culled) */
            int inside = (area>0) ? (w0>=0&&w1>=0&&w2>=0) : (w0<=0&&w1<=0&&w2<=0);
            if (!inside) continue;
            w0*=inv_area; w1*=inv_area; w2*=inv_area;

            /* perspective-correct interpolation weights */
            f32 iw = w0*iwa + w1*iwb + w2*iwc;
            f32 depth = w0*A.clip.z + w1*B.clip.z + w2*C.clip.z; /* NDC z, linear-ish for test */

            size_t di=(size_t)y*fb->w+x;
            if (depth >= fb->depth[di]) continue;  /* z-test: nearer wins */

            f32 pw0=w0*iwa/iw, pw1=w1*iwb/iw, pw2=w2*iwc/iw;

            Vec3 N = v3_norm(v3(
                pw0*A.world_norm.x+pw1*B.world_norm.x+pw2*C.world_norm.x,
                pw0*A.world_norm.y+pw1*B.world_norm.y+pw2*C.world_norm.y,
                pw0*A.world_norm.z+pw1*B.world_norm.z+pw2*C.world_norm.z));
            Vec3 WP = v3(
                pw0*A.world_pos.x+pw1*B.world_pos.x+pw2*C.world_pos.x,
                pw0*A.world_pos.y+pw1*B.world_pos.y+pw2*C.world_pos.y,
                pw0*A.world_pos.z+pw1*B.world_pos.z+pw2*C.world_pos.z);
            Color3 base = col3(
                pw0*A.color.r+pw1*B.color.r+pw2*C.color.r,
                pw0*A.color.g+pw1*B.color.g+pw2*C.color.g,
                pw0*A.color.b+pw1*B.color.b+pw2*C.color.b);

            /* Optional texture: modulate the base color by a sampled texel at
             * the perspective-correct UV. NULL texfn => unchanged (golden-safe). */
            if (ctx->texfn) {
                f32 u = pw0*A.uv.x + pw1*B.uv.x + pw2*C.uv.x;
                f32 vtex = pw0*A.uv.y + pw1*B.uv.y + pw2*C.uv.y;
                base = col_mul(base, ctx->texfn(u, vtex, ctx->tex_user));
            }

            /* Blinn-Phong */
            Vec3 L = ctx->light_dir;                 /* toward light (normalized) */
            f32 ndl = ct_maxf(0.0f, v3_dot(N,L));
            Vec3 V = v3_norm(v3_sub(ctx->cam_pos, WP));
            Vec3 H = v3_norm(v3_add(L,V));
            f32 ndh = ct_maxf(0.0f, v3_dot(N,H));
            f32 spec = ctx->specular * powf(ndh, ctx->shininess);

            Color3 lit = col_add(
                col_mul(base, col_add(ctx->ambient, col_scale(ctx->light_color, ndl))),
                col_scale(ctx->light_color, spec));

            fb->px[di*3+0]=lit.r; fb->px[di*3+1]=lit.g; fb->px[di*3+2]=lit.b;
            fb->depth[di]=depth;
        }
    }
}

void raster_mesh(Framebuffer *fb, const Mesh *m, const RasterCtx *ctx) {
    /* precompute MVP and model/normal matrices */
    Mat4 mv = mat4_mul(ctx->view, ctx->model);
    Mat4 mvp = mat4_mul(ctx->proj, mv);
    f32 halfw=fb->w*0.5f, halfh=fb->h*0.5f;

    for (u32 t=0;t<m->ntris;++t){
        u32 ia=m->tris[t].a, ib=m->tris[t].b, ic=m->tris[t].c;
        const Vertex *va=&m->verts[ia], *vb=&m->verts[ib], *vc=&m->verts[ic];

        VOut O[3]; const Vertex *vv[3]={va,vb,vc};
        int behind=0;
        for (int k=0;k<3;++k){
            Vec4 clip = mat4_point4(&mvp, vv[k]->pos);
            if (clip.w <= 1e-5f) { behind=1; }
            O[k].clip = clip;
            O[k].world_pos = mat4_point(&ctx->model, vv[k]->pos);
            O[k].world_norm = v3_norm(mat4_dir(&ctx->model, vv[k]->normal));
            O[k].color = vv[k]->color;
            O[k].uv = vv[k]->uv;
        }
        if (behind) continue;  /* skip triangles crossing/behind near plane (no clip) */

        /* perspective divide + viewport */
        for (int k=0;k<3;++k){
            f32 iw=1.0f/O[k].clip.w;
            f32 ndcx=O[k].clip.x*iw, ndcy=O[k].clip.y*iw, ndcz=O[k].clip.z*iw;
            O[k].clip.x = (ndcx*0.5f+0.5f)*fb->w;
            O[k].clip.y = (1.0f-(ndcy*0.5f+0.5f))*fb->h; /* flip Y for screen */
            O[k].clip.z = ndcz;                          /* keep NDC z for z-test */
            /* O[k].clip.w stays = clip w for perspective-correct interp */
        }
        (void)halfw;(void)halfh;

        if (ctx->wireframe) {
            Color3 wc = col3(0.2f,1.0f,0.4f);
            draw_line(fb,(int)O[0].clip.x,(int)O[0].clip.y,(int)O[1].clip.x,(int)O[1].clip.y,wc);
            draw_line(fb,(int)O[1].clip.x,(int)O[1].clip.y,(int)O[2].clip.x,(int)O[2].clip.y,wc);
            draw_line(fb,(int)O[2].clip.x,(int)O[2].clip.y,(int)O[0].clip.x,(int)O[0].clip.y,wc);
        } else {
            fill_tri(fb, O[0], O[1], O[2], ctx);
        }
    }
}

/* Draw a triangle given clip-space coords directly (used by tests). */
void raster_triangle_clip(Framebuffer *fb, Vec4 a, Vec4 b, Vec4 c,
                          Color3 ca, Color3 cb, Color3 cc,
                          Vec3 na, Vec3 nb, Vec3 nc, const RasterCtx *ctx) {
    VOut O[3];
    Vec4 cl[3]={a,b,c}; Color3 co[3]={ca,cb,cc}; Vec3 nn[3]={na,nb,nc};
    for (int k=0;k<3;++k){
        f32 iw=1.0f/cl[k].w;
        O[k].clip.x=(cl[k].x*iw*0.5f+0.5f)*fb->w;
        O[k].clip.y=(1.0f-(cl[k].y*iw*0.5f+0.5f))*fb->h;
        O[k].clip.z=cl[k].z*iw;
        O[k].clip.w=cl[k].w;
        O[k].world_pos=v3(cl[k].x,cl[k].y,cl[k].z);
        O[k].world_norm=nn[k];
        O[k].color=co[k];
        O[k].uv=(Vec2){0,0};
    }
    fill_tri(fb,O[0],O[1],O[2],ctx);
}
