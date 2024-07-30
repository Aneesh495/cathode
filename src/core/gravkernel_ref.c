/* gravkernel_ref.c — C reference for the NEON gravitational force kernel. */
#include "cathode/gravkernel.h"
#include <math.h>

void grav_accum_ref(float *out3, float px, float py, float pz,
                    const float *sx, const float *sy, const float *sz,
                    const float *sm, unsigned long n, float g, float eps2){
    float ax=0.0f, ay=0.0f, az=0.0f;
    for (unsigned long j=0;j<n;++j){
        float dx=sx[j]-px, dy=sy[j]-py, dz=sz[j]-pz;
        float r2=dx*dx+dy*dy+dz*dz+eps2;
        float inv=1.0f/sqrtf(r2);
        float f=g*sm[j]*inv*inv*inv;    /* G m / (r^2+eps^2)^(3/2) */
        ax+=dx*f; ay+=dy*f; az+=dz*f;
    }
    out3[0]=ax; out3[1]=ay; out3[2]=az;
}
