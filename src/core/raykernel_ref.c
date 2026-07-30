/* raykernel_ref.c — portable C reference for the NEON ray-intersection kernels.
 * These define the exact semantics the NEON code must reproduce. */
#include "cathode/raykernel.h"
#include <math.h>

/* Ray-sphere: assume rd normalized (a=1). Solve |o + t d - c|^2 = r^2.
 *   oc = o - c;  b = dot(oc,d);  c2 = dot(oc,oc) - r^2;  disc = b^2 - c2.
 *   nearest positive root t = -b - sqrt(disc) (fall back to -b + sqrt if that
 *   is behind the origin). Miss (disc<0 or both roots behind) -> -1. */
void rk_ray4_spheres_ref(float *out4, const float *ro, const float *rd, const float *sph){
    for (int i=0;i<4;++i){
        float cx=sph[0+i], cy=sph[4+i], cz=sph[8+i], r=sph[12+i];
        float ox=ro[0]-cx, oy=ro[1]-cy, oz=ro[2]-cz;
        float b = ox*rd[0]+oy*rd[1]+oz*rd[2];
        float c2 = ox*ox+oy*oy+oz*oz - r*r;
        float disc = b*b - c2;
        if (disc < 0.0f){ out4[i] = -1.0f; continue; }
        float sq = sqrtf(disc);
        float t0 = -b - sq;
        float t1 = -b + sq;
        float t = (t0 > 1e-4f) ? t0 : ((t1 > 1e-4f) ? t1 : -1.0f);
        out4[i] = t;
    }
}

/* Ray-AABB slab test. rd may be un-normalized. Returns tnear if the ray
 * enters the box in front of the origin, else -1. */
void rk_ray4_aabb_ref(float *out4, const float *ro, const float *rd, const float *bb){
    for (int i=0;i<4;++i){
        float tmin = -1e30f, tmax = 1e30f;
        int miss = 0;
        for (int ax=0; ax<3; ++ax){
            float lo = bb[(ax)*4 + i];        /* min{x,y,z} */
            float hi = bb[(ax+3)*4 + i];      /* max{x,y,z} */
            float o = ro[ax], d = rd[ax];
            if (fabsf(d) < 1e-20f){
                if (o < lo || o > hi){ miss=1; break; }
            } else {
                float inv = 1.0f/d;
                float t1 = (lo - o)*inv;
                float t2 = (hi - o)*inv;
                if (t1 > t2){ float tmp=t1; t1=t2; t2=tmp; }
                if (t1 > tmin) tmin = t1;
                if (t2 < tmax) tmax = t2;
            }
        }
        if (miss || tmax < tmin || tmax < 0.0f){ out4[i] = -1.0f; continue; }
        float t = (tmin > 0.0f) ? tmin : tmax;  /* origin inside -> tmax */
        /* Guard against the -1e30/1e30 sentinels leaking when a ray is parallel
         * to (and inside) every slab it tests — treat an unbounded interval as
         * a miss rather than returning a huge bogus distance. */
        if (t > 1e29f || t < -1e29f){ out4[i] = -1.0f; continue; }
        out4[i] = t;
    }
}
