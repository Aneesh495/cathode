/* fastmath_ref.c  -  libm reference for the NEON transcendental kernels. */
#include "cathode/fastmath.h"
#include <math.h>

void fm_sin4_ref(float *out, const float *in, unsigned long n){
    for (unsigned long i=0;i<n;++i) out[i]=sinf(in[i]);
}
void fm_cos4_ref(float *out, const float *in, unsigned long n){
    for (unsigned long i=0;i<n;++i) out[i]=cosf(in[i]);
}
void fm_exp4_ref(float *out, const float *in, unsigned long n){
    for (unsigned long i=0;i<n;++i) out[i]=expf(in[i]);
}
void fm_log4_ref(float *out, const float *in, unsigned long n){
    for (unsigned long i=0;i<n;++i) out[i]=logf(in[i]);
}
