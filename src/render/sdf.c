/* ==========================================================================
 * sdf.c — signed-distance-field sphere tracer.
 *
 *   * primitives: sphere, box, torus, plane
 *   * operators: union, smooth-union (polynomial smin), subtract, repeat
 *   * marcher: sphere tracing with normal via central differences,
 *     soft shadows (penumbra), ambient occlusion, sky gradient
 *   * scenes: animated primitives, power-8 mandelbulb, infinite repetition
 * ========================================================================== */
#include "cathode/sdf.h"
#include <math.h>

/* ----- primitives (all return signed distance) ----- */
static inline f32 sd_sphere(Vec3 p, f32 r){ return v3_len(p) - r; }
static inline f32 sd_box(Vec3 p, Vec3 b){
    Vec3 d = v3_sub(v3_abs(p), b);
    Vec3 dm = v3_max(d, v3(0,0,0));
    return v3_len(dm) + ct_minf(ct_maxf(d.x,ct_maxf(d.y,d.z)), 0.0f);
}
static inline f32 sd_torus(Vec3 p, f32 R, f32 r){
    f32 qx = sqrtf(p.x*p.x + p.z*p.z) - R;
    return sqrtf(qx*qx + p.y*p.y) - r;
}
static inline f32 sd_plane(Vec3 p, f32 y){ return p.y - y; }

/* ----- operators ----- */
static inline f32 op_smin(f32 a, f32 b, f32 k){
    f32 h = ct_clampf(0.5f + 0.5f*(b-a)/k, 0.0f, 1.0f);
    return ct_lerpf(b, a, h) - k*h*(1.0f-h);
}
static inline Vec3 op_repeat(Vec3 p, Vec3 c){
    /* domain repetition: wrap each axis to [-c/2, c/2] */
    return v3(p.x - c.x*roundf(p.x/c.x),
              p.y - c.y*roundf(p.y/c.y),
              p.z - c.z*roundf(p.z/c.z));
}

/* ============================ scene fields ============================ */

f32 sdf_scene_primitives(Vec3 p, f32 t, i32 *mat, void *user){
    (void)user;
    f32 ground = sd_plane(p, -1.2f);
    i32 gm = 0;

    /* animated blobby cluster above the plane */
    Vec3 c1 = v3(sinf(t)*1.2f, 0.2f+0.3f*sinf(t*1.3f), cosf(t)*1.2f);
    f32 s1 = sd_sphere(v3_sub(p, c1), 0.7f);
    Vec3 bpos = v3_sub(p, v3(0, 0.1f, 0));
    /* rotate box slowly around Y */
    f32 ca=cosf(t*0.7f), sa=sinf(t*0.7f);
    Vec3 br = v3(ca*bpos.x - sa*bpos.z, bpos.y, sa*bpos.x + ca*bpos.z);
    f32 s2 = sd_box(br, v3(0.6f,0.6f,0.6f));
    f32 s3 = sd_torus(v3_sub(p, v3(cosf(t*0.9f)*1.4f, 0.3f, sinf(t*0.9f)*1.4f)), 0.6f, 0.22f);

    f32 blob = op_smin(s1, s2, 0.5f);
    blob = op_smin(blob, s3, 0.4f);

    f32 d; i32 m;
    if (ground < blob) { d = ground; m = gm; }
    else { d = blob; m = 1 + ((int)(t)) % 3; /* cycle material */ m = 1; }
    /* choose material more stably: nearest surface */
    if (ground < blob) m = 0;
    else if (s1 <= s2 && s1 <= s3) m = 1;
    else if (s2 <= s3) m = 2;
    else m = 3;
    if (mat) *mat = m;
    return d;
}

f32 sdf_scene_mandelbulb(Vec3 p, f32 t, i32 *mat, void *user){
    (void)user;
    /* rotate the point slowly for animation */
    f32 ca=cosf(t*0.25f), sa=sinf(t*0.25f);
    Vec3 z0 = v3(ca*p.x - sa*p.z, p.y, sa*p.x + ca*p.z);
    Vec3 z = z0;
    f32 dr = 1.0f;
    f32 r = 0.0f;
    f32 power = 8.0f + 1.5f*sinf(t*0.2f);
    int iters = 8;
    for (int i=0;i<iters;++i){
        r = v3_len(z);
        if (r > 2.0f) break;
        /* to polar */
        f32 theta = acosf(ct_clampf(z.y/r,-1.0f,1.0f));
        f32 phi = atan2f(z.z, z.x);
        dr = powf(r, power-1.0f)*power*dr + 1.0f;
        /* scale and rotate */
        f32 zr = powf(r, power);
        theta *= power; phi *= power;
        z = v3_scale(v3(sinf(theta)*cosf(phi), cosf(theta), sinf(theta)*sinf(phi)), zr);
        z = v3_add(z, z0);
    }
    if (mat) *mat = 4;
    return 0.5f*logf(ct_maxf(r,1e-6f))*r/dr;   /* distance estimator */
}

f32 sdf_scene_infinite(Vec3 p, f32 t, i32 *mat, void *user){
    (void)user;
    f32 ground = sd_plane(p, -1.0f);
    Vec3 q = op_repeat(p, v3(4.0f,0.0f,4.0f));  /* repeat on x,z only */
    /* bob the spheres */
    q.y -= 0.3f*sinf(t + p.x*0.2f + p.z*0.2f);
    f32 balls = sd_sphere(q, 0.8f);
    f32 pillars = sd_box(v3(q.x, p.y, q.z), v3(0.25f,3.0f,0.25f));
    f32 obj = op_smin(balls, pillars, 0.3f);
    i32 m;
    f32 d;
    if (ground < obj){ d=ground; m=0; } else { d=obj; m = (balls<pillars)?5:6; }
    if (mat) *mat = m;
    return d;
}

/* ============================ marcher ============================ */

static f32 field(const SdfScene *sc, Vec3 p, i32 *mat){
    return sc->field(p, sc->time, mat, sc->user);
}

static Vec3 calc_normal(const SdfScene *sc, Vec3 p){
    const f32 e = 0.0006f;
    i32 dummy;
    f32 dx = field(sc, v3(p.x+e,p.y,p.z), &dummy) - field(sc, v3(p.x-e,p.y,p.z), &dummy);
    f32 dy = field(sc, v3(p.x,p.y+e,p.z), &dummy) - field(sc, v3(p.x,p.y-e,p.z), &dummy);
    f32 dz = field(sc, v3(p.x,p.y,p.z+e), &dummy) - field(sc, v3(p.x,p.y,p.z-e), &dummy);
    return v3_norm(v3(dx,dy,dz));
}

/* soft shadow: march toward light, track closest approach for penumbra */
static f32 soft_shadow(const SdfScene *sc, Vec3 ro, Vec3 rd, f32 mint, f32 maxt, f32 k){
    f32 res = 1.0f;
    f32 t = mint;
    i32 m;
    for (int i=0;i<48 && t<maxt;++i){
        f32 h = field(sc, v3_add(ro, v3_scale(rd,t)), &m);
        if (h < 0.001f) return 0.0f;
        res = ct_minf(res, k*h/t);
        t += ct_clampf(h, 0.01f, 0.3f);
    }
    return ct_clampf(res, 0.0f, 1.0f);
}

/* ambient occlusion: sample field along normal */
static f32 calc_ao(const SdfScene *sc, Vec3 p, Vec3 n){
    f32 occ = 0.0f, sca = 1.0f;
    i32 m;
    for (int i=0;i<5;++i){
        f32 hr = 0.01f + 0.12f*(f32)i/4.0f;
        f32 d = field(sc, v3_add(p, v3_scale(n,hr)), &m);
        occ += (hr - d)*sca;
        sca *= 0.85f;
    }
    return ct_clampf(1.0f - 1.5f*occ, 0.0f, 1.0f);
}

static Color3 sky(const SdfScene *sc, Vec3 rd){
    f32 tt = ct_clampf(rd.y*0.5f+0.5f, 0.0f, 1.0f);
    return col_lerp(sc->sky_bottom, sc->sky_top, tt);
}

/* trace one primary ray, return shaded color */
static Color3 trace(const SdfScene *sc, Vec3 ro, Vec3 rd){
    f32 t = 0.0f;
    i32 mat = 0;
    int hit = 0;
    for (int i=0;i<sc->max_steps;++i){
        Vec3 p = v3_add(ro, v3_scale(rd,t));
        f32 d = field(sc, p, &mat);
        if (d < sc->epsilon){ hit=1; break; }
        t += d;
        if (t > sc->max_dist) break;
    }
    if (!hit) return sky(sc, rd);

    Vec3 p = v3_add(ro, v3_scale(rd,t));
    Vec3 n = calc_normal(sc, p);
    Vec3 L = sc->light_dir;
    f32 ndl = ct_maxf(0.0f, v3_dot(n,L));
    f32 sh = soft_shadow(sc, v3_add(p, v3_scale(n,0.02f)), L, 0.02f, 8.0f, 16.0f);
    f32 ao = calc_ao(sc, p, n);

    Color3 albedo = sc->mat_albedo[mat & 7];
    Color3 sky_amb = sky(sc, n);
    Color3 c = col_scale(albedo, 0.0f);
    /* ambient from sky * AO */
    c = col_add(c, col_mul(albedo, col_scale(sky_amb, 0.35f*ao)));
    /* diffuse sun */
    Color3 sun = col3(1.0f,0.95f,0.85f);
    c = col_add(c, col_mul(albedo, col_scale(sun, ndl*sh)));
    /* specular */
    Vec3 V = v3_neg(rd);
    Vec3 H = v3_norm(v3_add(L,V));
    f32 spec = powf(ct_maxf(0.0f,v3_dot(n,H)), 32.0f) * sh;
    c = col_add(c, col_scale(sun, spec*0.4f));
    /* distance fog toward sky */
    f32 fog = 1.0f - expf(-t*0.03f);
    c = col_lerp(c, sky(sc, rd), ct_clampf(fog,0.0f,1.0f));
    return c;
}

void sdf_render_band(Framebuffer *fb, const SdfScene *sc, i32 y0, i32 y1){
    f32 aspect = (f32)fb->w / (f32)fb->h;
    /* camera basis */
    Vec3 fwd = v3_norm(v3_sub(sc->cam_target, sc->cam_pos));
    Vec3 right = v3_norm(v3_cross(fwd, v3(0,1,0)));
    Vec3 up = v3_cross(right, fwd);
    f32 tanf_half = tanf(sc->fov*0.5f);
    int aa = sc->aa < 1 ? 1 : sc->aa;

    for (i32 y=y0; y<y1; ++y){
        for (i32 x=0; x<fb->w; ++x){
            Color3 acc = col3(0,0,0);
            for (int sy=0; sy<aa; ++sy) for (int sx=0; sx<aa; ++sx){
                f32 ox = (sx+0.5f)/aa, oy = (sy+0.5f)/aa;
                f32 ndcx = (2.0f*(x+ox)/fb->w - 1.0f)*aspect*tanf_half;
                f32 ndcy = (1.0f - 2.0f*(y+oy)/fb->h)*tanf_half;
                Vec3 rd = v3_norm(v3_add(fwd, v3_add(v3_scale(right,ndcx), v3_scale(up,ndcy))));
                acc = col_add(acc, trace(sc, sc->cam_pos, rd));
            }
            f32 inv = 1.0f/(aa*aa);
            fb_set(fb, x, y, col_scale(acc, inv));
        }
    }
}

void sdf_render(Framebuffer *fb, const SdfScene *sc){
    sdf_render_band(fb, sc, 0, fb->h);
}
