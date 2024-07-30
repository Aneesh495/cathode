/* test_project.c — batched NEON point projection vs C reference. */
#include "cathode/simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures=0, checks=0;
static float frand(void){ return (float)rand()/(float)RAND_MAX*2.0f-1.0f; }

int main(void){
    srand(2024);
    printf("== CATHODE project_points NEON vs C reference ==\n");
    unsigned long sizes[]={1,2,3,4,7,16,17,100,999};
    float vw=640.0f, vh=480.0f, wclip=1e-4f;
    for (unsigned si=0; si<sizeof(sizes)/sizeof(sizes[0]); ++si){
        unsigned long n=sizes[si];
        float *pts=malloc(n*3*4);
        float *o1=malloc(n*3*4), *o2=malloc(n*3*4);
        unsigned char *v1=malloc(n), *v2=malloc(n);
        for (unsigned long i=0;i<n*3;++i) pts[i]=frand()*5.0f;
        /* a realistic perspective*view matrix: build proj*translate */
        /* simple: perspective f=1.5, aspect 4:3, then translate z by -8 */
        float m[16]={0};
        float f=1.5f, aspect=vw/vh, zn=0.1f, zf=100.0f;
        m[0]=f/aspect; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.0f;
        m[14]=(2*zf*zn)/(zn-zf);
        /* fold in a translation of z-=8 by adjusting col3 (post-multiply-ish);
           just offset points instead so w varies: shift pts z by -8 */
        for (unsigned long i=0;i<n;++i) pts[i*3+2]-=8.0f;

        project_points_neon(o1,v1,pts,n,m,vw,vh,wclip);
        project_points_ref (o2,v2,pts,n,m,vw,vh,wclip);
        int bad=0;
        for (unsigned long i=0;i<n;++i){
            if (v1[i]!=v2[i]){ bad=1; break; }
            if (v1[i]){
                for(int k=0;k<3;++k){
                    float d=fabsf(o1[i*3+k]-o2[i*3+k]);
                    if (d>1e-3f*(1.0f+fabsf(o2[i*3+k]))){ bad=1; break; }
                }
            }
            if (bad) break;
        }
        checks++;
        if (bad){ printf("  FAIL n=%lu (screen/vis mismatch)\n",n); failures++; }
        else printf("  ok   n=%-4lu\n",n);
        free(pts);free(o1);free(o2);free(v1);free(v2);
    }

    /* known: a point on the -z axis at the center projects to screen center */
    {
        float pts[3]={0,0,-5}, o[3]; unsigned char v;
        float m[16]={0}; float f=1.5f;
        m[0]=f; m[5]=f; m[10]=-1.0f; m[11]=-1.0f;
        project_points_neon(o,&v,pts,1,m,vw,vh,1e-4f);
        checks++;
        if (v && fabsf(o[0]-vw*0.5f)<1.0f && fabsf(o[1]-vh*0.5f)<1.0f)
            printf("  ok   center point -> screen center (%.1f,%.1f)\n",o[0],o[1]);
        else { printf("  FAIL center point -> (%.1f,%.1f) vis=%d\n",o[0],o[1],v); failures++; }
    }

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
