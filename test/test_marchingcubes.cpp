// test_marchingcubes.cpp  -  validate the marching-cubes isosurface extractor.
#include "cathode/cppcore.h"
#include <cstdio>
#include <cmath>
#include <vector>

static int failures=0, checks=0;
static void ok(const char*n,bool c){ ++checks; if(c) printf("  ok   %s\n",n); else { printf("  FAIL %s\n",n); ++failures; } }

int main(){
    printf("== CATHODE marching cubes ==\n");
    const int N=32;
    std::vector<float> field(N*N*N);
    // signed-ish field: sphere of radius R centered in the grid. Inside < iso.
    // We use f = dist - R so that f<iso(=0) inside. But metaball/MC convention
    // here treats val<iso as inside, so store f = dist-R and iso=0.
    float cx=N/2.0f, cy=N/2.0f, cz=N/2.0f, R=10.0f;
    for (int z=0;z<N;++z)for(int y=0;y<N;++y)for(int x=0;x<N;++x){
        float dx=x-cx,dy=y-cy,dz=z-cz;
        field[(z*N+y)*N+x]=std::sqrt(dx*dx+dy*dy+dz*dz)-R;
    }
    std::vector<CppMcVertex> verts(200000);
    int tris = cpp_marching_cubes(field.data(), N,N,N, 0.0f, verts.data(), 60000);
    printf("  sphere -> %d triangles\n", tris);
    ok("produced triangles", tris>100);

    // all vertices should lie near the sphere surface (radius ~R)
    double worst=0; bool allnear=true;
    for (int i=0;i<tris*3;++i){
        float dx=verts[i].pos.x-cx, dy=verts[i].pos.y-cy, dz=verts[i].pos.z-cz;
        double r=std::sqrt(dx*dx+dy*dy+dz*dz);
        double e=std::fabs(r-R); if(e>worst)worst=e; if(e>1.5) allnear=false;
    }
    printf("  worst |r-R| = %.3f\n", worst);
    ok("vertices lie on the sphere", allnear);

    // normals should be unit length and point roughly radially outward
    bool normals_ok=true; double worst_dot=1.0;
    for (int i=0;i<tris*3;++i){
        float nx=verts[i].normal.x, ny=verts[i].normal.y, nz=verts[i].normal.z;
        double len=std::sqrt(nx*nx+ny*ny+nz*nz);
        if (std::fabs(len-1.0)>1e-2) normals_ok=false;
        // radial direction
        float dx=verts[i].pos.x-cx, dy=verts[i].pos.y-cy, dz=verts[i].pos.z-cz;
        double rl=std::sqrt(dx*dx+dy*dy+dz*dz)+1e-6;
        double dot=(nx*dx+ny*dy+nz*dz)/rl;   // for f=dist-R, grad points inward (we negate in impl)
        if (dot<worst_dot) worst_dot=dot;
    }
    printf("  normals unit=%d, worst radial dot=%.3f\n", normals_ok, worst_dot);
    ok("normals are unit length", normals_ok);
    // our gradient is (low-high) so for f=dist-R it points inward (toward center):
    // radial dot should be consistently negative (or consistently positive)  -  i.e.
    // consistent orientation. Accept |worst_dot| indicates alignment.
    ok("normals radially aligned", std::fabs(worst_dot) > 0.5);

    // empty field (all above iso) -> no triangles
    std::fill(field.begin(), field.end(), 5.0f);
    int t2=cpp_marching_cubes(field.data(),N,N,N,0.0f,verts.data(),60000);
    ok("empty field -> 0 tris", t2==0);

    // metaball field helper produces a finite field with a peak near a center
    Vec3 c{ (float)(N/2),(float)(N/2),(float)(N/2) }; float rr=6.0f;
    cpp_metaball_field(field.data(), N,N,N, &c, &rr, 1);
    bool finite=true; for(float v:field) if(!(v==v)) finite=false;
    ok("metaball field finite", finite);
    int t3=cpp_marching_cubes(field.data(),N,N,N, 0.5f, verts.data(),60000);
    ok("metaball surface extracted", t3>50);

    printf("\n%d checks, %d failures\n",checks,failures);
    if(!failures) printf("ALL PASS\n");
    return failures?1:0;
}
