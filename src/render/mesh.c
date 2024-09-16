/* ==========================================================================
 * mesh.c  -  procedural mesh generators + normal computation.
 * ========================================================================== */
#include "cathode/raster.h"
#include "cathode/noise.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

static Mesh *mesh_alloc(u32 nverts, u32 ntris) {
    Mesh *m = (Mesh *)calloc(1, sizeof(Mesh));
    m->verts = (Vertex *)calloc(nverts, sizeof(Vertex));
    m->tris  = (Tri *)calloc(ntris, sizeof(Tri));
    m->nverts = nverts; m->ntris = ntris;
    return m;
}

void mesh_free(Mesh *m) {
    if (!m) return;
    free(m->verts); free(m->tris); free(m);
}

void mesh_compute_normals(Mesh *m) {
    for (u32 i = 0; i < m->nverts; ++i) m->verts[i].normal = v3(0,0,0);
    for (u32 t = 0; t < m->ntris; ++t) {
        u32 ia=m->tris[t].a, ib=m->tris[t].b, ic=m->tris[t].c;
        Vec3 a=m->verts[ia].pos, b=m->verts[ib].pos, c=m->verts[ic].pos;
        Vec3 fn = v3_cross(v3_sub(b,a), v3_sub(c,a));  /* area-weighted */
        m->verts[ia].normal = v3_add(m->verts[ia].normal, fn);
        m->verts[ib].normal = v3_add(m->verts[ib].normal, fn);
        m->verts[ic].normal = v3_add(m->verts[ic].normal, fn);
    }
    for (u32 i = 0; i < m->nverts; ++i) {
        Vec3 n = m->verts[i].normal;
        if (v3_len2(n) < 1e-12f) {
            /* Degenerate accumulation (e.g. a pole vertex whose surrounding
             * face normals cancel). Fall back to the direction from the mesh
             * origin, which is correct for star-shaped/convex meshes. */
            n = v3_len2(m->verts[i].pos) > 1e-12f ? m->verts[i].pos : v3(0,1,0);
        }
        m->verts[i].normal = v3_norm(n);
    }
}

static Color3 palette(f32 t) { /* pleasant hue ramp for default vertex colors */
    return col3(0.5f+0.5f*sinf(t*CT_TAU+0.0f),
                0.5f+0.5f*sinf(t*CT_TAU+2.094f),
                0.5f+0.5f*sinf(t*CT_TAU+4.188f));
}

Mesh *mesh_cube(f32 s) {
    Mesh *m = mesh_alloc(24, 12);
    /* 6 faces, 4 verts each, so per-face normals are crisp */
    Vec3 n[6] = { v3(0,0,1), v3(0,0,-1), v3(1,0,0), v3(-1,0,0), v3(0,1,0), v3(0,-1,0) };
    Vec3 fc[6][4] = {
        {v3(-s,-s, s),v3( s,-s, s),v3( s, s, s),v3(-s, s, s)}, /* +z */
        {v3( s,-s,-s),v3(-s,-s,-s),v3(-s, s,-s),v3( s, s,-s)}, /* -z */
        {v3( s,-s, s),v3( s,-s,-s),v3( s, s,-s),v3( s, s, s)}, /* +x */
        {v3(-s,-s,-s),v3(-s,-s, s),v3(-s, s, s),v3(-s, s,-s)}, /* -x */
        {v3(-s, s, s),v3( s, s, s),v3( s, s,-s),v3(-s, s,-s)}, /* +y */
        {v3(-s,-s,-s),v3( s,-s,-s),v3( s,-s, s),v3(-s,-s, s)}, /* -y */
    };
    u32 v=0, t=0;
    for (int f=0; f<6; ++f) {
        u32 base=v;
        for (int k=0;k<4;++k){
            m->verts[v].pos=fc[f][k];
            m->verts[v].normal=n[f];
            m->verts[v].color=palette((f+0.0f)/6.0f);
            m->verts[v].uv=(Vec2){(f32)(k==1||k==2),(f32)(k>=2)};
            v++;
        }
        m->tris[t++] = (Tri){base+0,base+1,base+2};
        m->tris[t++] = (Tri){base+0,base+2,base+3};
    }
    return m;
}

Mesh *mesh_sphere(f32 radius, int stacks, int slices) {
    if (stacks<2) stacks=2; if (slices<3) slices=3;
    u32 nv = (u32)(stacks+1)*(slices+1);
    u32 nt = (u32)stacks*slices*2;
    Mesh *m = mesh_alloc(nv, nt);
    u32 v=0;
    for (int i=0;i<=stacks;++i){
        f32 phi = CT_PI * (f32)i/stacks;         /* 0..pi */
        f32 sp=sinf(phi), cp=cosf(phi);
        for (int j=0;j<=slices;++j){
            f32 th = CT_TAU * (f32)j/slices;
            f32 st=sinf(th), ct=cosf(th);
            Vec3 nrm = v3(sp*ct, cp, sp*st);
            m->verts[v].pos = v3_scale(nrm, radius);
            m->verts[v].normal = nrm;
            m->verts[v].color = palette((f32)i/stacks);
            m->verts[v].uv = (Vec2){(f32)j/slices,(f32)i/stacks};
            v++;
        }
    }
    u32 t=0, row=slices+1;
    for (int i=0;i<stacks;++i)
        for (int j=0;j<slices;++j){
            u32 a=(u32)i*row+j, b=a+row;
            m->tris[t++]=(Tri){a,b,a+1};
            m->tris[t++]=(Tri){a+1,b,b+1};
        }
    return m;
}

Mesh *mesh_torus(f32 R, f32 r, int nmaj, int nmin) {
    if (nmaj<3) nmaj=3; if (nmin<3) nmin=3;
    u32 nv=(u32)(nmaj+1)*(nmin+1);
    u32 nt=(u32)nmaj*nmin*2;
    Mesh *m=mesh_alloc(nv,nt);
    u32 v=0;
    for (int i=0;i<=nmaj;++i){
        f32 u=CT_TAU*(f32)i/nmaj; f32 cu=cosf(u),su=sinf(u);
        for (int j=0;j<=nmin;++j){
            f32 vv=CT_TAU*(f32)j/nmin; f32 cv=cosf(vv),sv=sinf(vv);
            Vec3 pos=v3((R+r*cv)*cu,(R+r*cv)*su,r*sv);
            Vec3 nrm=v3(cv*cu,cv*su,sv);
            m->verts[v].pos=pos; m->verts[v].normal=nrm;
            m->verts[v].color=palette((f32)j/nmin);
            m->verts[v].uv=(Vec2){(f32)i/nmaj,(f32)j/nmin};
            v++;
        }
    }
    u32 t=0,row=nmin+1;
    for (int i=0;i<nmaj;++i)
        for (int j=0;j<nmin;++j){
            u32 a=(u32)i*row+j,b=a+row;
            m->tris[t++]=(Tri){a,b,a+1};
            m->tris[t++]=(Tri){a+1,b,b+1};
        }
    return m;
}

/* Icosphere: start from a regular icosahedron, subdivide, project to sphere. */
Mesh *mesh_icosphere(f32 radius, int subdiv) {
    const f32 t=(1.0f+sqrtf(5.0f))*0.5f;
    Vec3 base[12]={
        v3(-1,t,0),v3(1,t,0),v3(-1,-t,0),v3(1,-t,0),
        v3(0,-1,t),v3(0,1,t),v3(0,-1,-t),v3(0,1,-t),
        v3(t,0,-1),v3(t,0,1),v3(-t,0,-1),v3(-t,0,1)
    };
    u32 baseidx[20][3]={
        {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},
        {1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
        {3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},
        {4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
    };
    /* working arrays; grow generously */
    u32 cap_v = 12; u32 cap_t = 20;
    for (int s=0;s<subdiv;++s){ cap_t*=4; cap_v = cap_v + cap_t/2; }
    cap_v += 64;
    Vec3 *pos=malloc(sizeof(Vec3)*cap_v);
    u32 (*tri)[3]=malloc(sizeof(u32)*3*cap_t*2);
    u32 nv=12,nt=20;
    for (int i=0;i<12;++i) pos[i]=v3_norm(base[i]);
    for (int i=0;i<20;++i){tri[i][0]=baseidx[i][0];tri[i][1]=baseidx[i][1];tri[i][2]=baseidx[i][2];}

    for (int s=0;s<subdiv;++s){
        /* midpoint cache via simple linear search table (small meshes) */
        u32 (*ntri)[3]=malloc(sizeof(u32)*3*nt*4);
        u32 nnt=0;
        /* edge->midpoint memo */
        typedef struct{u32 a,b,m;} Edge; Edge *edges=malloc(sizeof(Edge)*nt*3); u32 ne=0;
        for (u32 f=0; f<nt; ++f){
            u32 v0=tri[f][0],v1=tri[f][1],v2=tri[f][2];
            u32 mids[3]; u32 pair[3][2]={{v0,v1},{v1,v2},{v2,v0}};
            for (int e=0;e<3;++e){
                u32 aa=pair[e][0],bb=pair[e][1];
                u32 lo=aa<bb?aa:bb, hi=aa<bb?bb:aa; u32 found=0xffffffff;
                for (u32 k=0;k<ne;++k) if(edges[k].a==lo&&edges[k].b==hi){found=edges[k].m;break;}
                if (found==0xffffffff){
                    Vec3 mp=v3_norm(v3_scale(v3_add(pos[aa],pos[bb]),0.5f));
                    pos[nv]=mp; found=nv++; edges[ne].a=lo;edges[ne].b=hi;edges[ne].m=found;ne++;
                }
                mids[e]=found;
            }
            ntri[nnt][0]=v0;      ntri[nnt][1]=mids[0]; ntri[nnt][2]=mids[2]; nnt++;
            ntri[nnt][0]=v1;      ntri[nnt][1]=mids[1]; ntri[nnt][2]=mids[0]; nnt++;
            ntri[nnt][0]=v2;      ntri[nnt][1]=mids[2]; ntri[nnt][2]=mids[1]; nnt++;
            ntri[nnt][0]=mids[0]; ntri[nnt][1]=mids[1]; ntri[nnt][2]=mids[2]; nnt++;
        }
        free(edges); free(tri); tri=ntri; nt=nnt;
    }
    Mesh *m=mesh_alloc(nv,nt);
    for (u32 i=0;i<nv;++i){
        m->verts[i].pos=v3_scale(pos[i],radius);
        m->verts[i].normal=pos[i];
        f32 hh=0.5f+0.5f*pos[i].y;
        m->verts[i].color=palette(hh);
        m->verts[i].uv=(Vec2){0.5f+atan2f(pos[i].z,pos[i].x)/CT_TAU, 0.5f-pos[i].y*0.5f};
    }
    for (u32 i=0;i<nt;++i){ m->tris[i].a=tri[i][0];m->tris[i].b=tri[i][1];m->tris[i].c=tri[i][2]; }
    free(pos); free(tri);
    return m;
}

Mesh *mesh_from_heightfield(const f32 *hgt, int nx, int ny, f32 scale, f32 zscale) {
    if (nx<2) nx=2; if (ny<2) ny=2;
    u32 nv=(u32)nx*ny, nt=(u32)(nx-1)*(ny-1)*2;
    Mesh *m=mesh_alloc(nv,nt);
    for (int j=0;j<ny;++j)for(int i=0;i<nx;++i){
        f32 hh=hgt[j*nx+i];
        f32 wx=((f32)i/(nx-1)-0.5f)*scale;
        f32 wz=((f32)j/(ny-1)-0.5f)*scale;
        u32 idx=(u32)j*nx+i;
        m->verts[idx].pos=v3(wx, hh*zscale, wz);
        m->verts[idx].normal=v3(0,1,0);
        m->verts[idx].uv=(Vec2){(f32)i/(nx-1),(f32)j/(ny-1)};
        /* altitude palette: water->grass->rock->snow */
        f32 a=hh*0.5f+0.5f;
        Color3 c;
        if (a<0.35f) c=col3(0.1f,0.25f,0.55f);
        else if (a<0.5f) c=col3(0.75f,0.7f,0.4f);
        else if (a<0.7f) c=col3(0.2f,0.5f,0.18f);
        else if (a<0.85f) c=col3(0.4f,0.35f,0.3f);
        else c=col3(0.95f,0.95f,1.0f);
        m->verts[idx].color=c;
    }
    u32 t=0;
    for (int j=0;j<ny-1;++j)for(int i=0;i<nx-1;++i){
        u32 a=(u32)j*nx+i,b=a+1,c=a+nx,d=c+1;
        m->tris[t++]=(Tri){a,c,b};
        m->tris[t++]=(Tri){b,c,d};
    }
    mesh_compute_normals(m);
    return m;
}
