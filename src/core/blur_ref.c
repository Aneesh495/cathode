/* blur_ref.c  -  C reference + kernel builder for the NEON separable blur. */
#include "cathode/blur.h"
#include <math.h>
#include <stddef.h>

int blur_gaussian_kernel(float *ker, int r, float sigma){
    if (r<0) r=0; if (r>15) r=15;
    if (sigma<=0.0f) sigma=(float)r*0.5f+0.5f;
    float sum=0.0f;
    for (int k=0;k<=r;++k){
        ker[k]=expf(-(float)(k*k)/(2.0f*sigma*sigma));
        sum += (k==0)?ker[k]:2.0f*ker[k];   /* symmetric: each side counted */
    }
    for (int k=0;k<=r;++k) ker[k]/=sum;
    return r;
}

void blur_h_ref(float *dst, const float *src, int w, int h, const float *ker, int r){
    for (int y=0;y<h;++y){
        const float *row=&src[(size_t)y*w];
        float *out=&dst[(size_t)y*w];
        for (int x=0;x<w;++x){
            float acc=ker[0]*row[x];
            for (int k=1;k<=r;++k){
                int lo=x-k; if(lo<0)lo=0;
                int hi=x+k; if(hi>w-1)hi=w-1;
                acc += ker[k]*(row[lo]+row[hi]);
            }
            out[x]=acc;
        }
    }
}

void blur_v_ref(float *dst, const float *src, int w, int h, const float *ker, int r){
    for (int y=0;y<h;++y){
        for (int x=0;x<w;++x){
            float acc=ker[0]*src[(size_t)y*w+x];
            for (int k=1;k<=r;++k){
                int lo=y-k; if(lo<0)lo=0;
                int hi=y+k; if(hi>h-1)hi=h-1;
                acc += ker[k]*(src[(size_t)lo*w+x]+src[(size_t)hi*w+x]);
            }
            dst[(size_t)y*w+x]=acc;
        }
    }
}
