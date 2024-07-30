/* ==========================================================================
 * test_golden.c — golden-image regression test for every scene.
 *
 * Renders each scene deterministically (fixed size, fixed frame count, the
 * same dt/t sequence the capture tool uses) through the CRT chain, hashes the
 * tonemapped output, and compares against a stored golden hash. Any change that
 * alters a scene's rendering — intentional or a regression — shows up as a
 * hash mismatch, which is the point: you then eyeball the scene and, if the
 * change was intended, run with UPDATE=1 to rewrite the golden file.
 *
 * The golden file (test/golden.txt) is plain "scene hash" lines. On first run
 * (no file) it records all hashes and passes, printing a notice.
 *
 * usage: test_golden            # compare against golden.txt
 *        test_golden update     # (re)write golden.txt from current output
 * ========================================================================== */
#include "cathode/scene.h"
#include "cathode/crt.h"
#include "cathode/framebuffer.h"
#include "cathode/image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GW 96
#define GH 72
#define FRAMES 24
#define GOLDEN_PATH "test/golden.txt"

/* FNV-1a over the tonemapped 8-bit RGB — stable across runs, sensitive to any
 * pixel change. */
static u64 hash_scene(i32 idx){
    Scene *sc = scene_create(idx);
    if (!sc) return 0;
    if (sc->init) sc->init(sc, GW, GH);
    CrtConfig cfg = sc->preferred_crt ? sc->preferred_crt(sc) : crt_config_default();
    CrtState *crt = crt_create(GW, GH, &cfg);
    Framebuffer *a=fb_create(GW,GH), *b=fb_create(GW,GH);
    f32 t=0; const f32 dt=1.0f/60.0f;
    for (int f=0; f<FRAMES; ++f){
        if (sc->update) sc->update(sc, dt, t);
        if (sc->render) sc->render(sc, a);
        crt_process(crt, a, b);
        t += dt;
    }
    u8 *rgb=malloc((size_t)GW*GH*3);
    image_tonemap_srgb(b, rgb);
    u64 h=1469598103934665603ULL;
    for (size_t i=0;i<(size_t)GW*GH*3;++i){ h^=rgb[i]; h*=1099511628211ULL; }
    free(rgb);
    if (sc->destroy) sc->destroy(sc);
    crt_destroy(crt); fb_destroy(a); fb_destroy(b);
    return h;
}

int main(int argc, char**argv){
    scenes_register_all();
    int update = (argc>1 && strcmp(argv[1],"update")==0) || getenv("UPDATE");
    int n = scene_count();

    /* load existing golden hashes (name -> hash) */
    char names[64][32]; u64 golds[64]; int ng=0;
    FILE *gf=fopen(GOLDEN_PATH,"r");
    if (gf){
        while (ng<64 && fscanf(gf,"%31s %llu",names[ng],(unsigned long long*)&golds[ng])==2) ng++;
        fclose(gf);
    }

    printf("== CATHODE golden-image regression (%dx%d, %d frames) ==\n", GW,GH,FRAMES);
    int fails=0, newly=0;
    if (update){
        FILE *out=fopen(GOLDEN_PATH,"w");
        if (!out){ printf("cannot write %s\n",GOLDEN_PATH); return 1; }
        for (int i=0;i<n;++i){
            u64 h=hash_scene(i);
            fprintf(out,"%s %llu\n", scene_name_at(i), (unsigned long long)h);
        }
        fclose(out);
        printf("wrote %d golden hashes to %s\n", n, GOLDEN_PATH);
        return 0;
    }

    for (int i=0;i<n;++i){
        const char *nm=scene_name_at(i);
        u64 h=hash_scene(i);
        /* find stored golden for this name */
        u64 g=0; int found=0;
        for (int k=0;k<ng;++k) if (strcmp(names[k],nm)==0){ g=golds[k]; found=1; break; }
        if (!found){ printf("  NEW  %-12s %llu (no golden yet)\n", nm, (unsigned long long)h); newly++; }
        else if (g==h) printf("  ok   %-12s\n", nm);
        else { printf("  FAIL %-12s golden=%llu got=%llu\n", nm, (unsigned long long)g,(unsigned long long)h); fails++; }
    }

    if (ng==0){
        printf("\nNo golden file found. Run `test_golden update` (or UPDATE=1) to record baselines.\n");
        /* first-run: record and pass so CI can bootstrap */
        FILE *out=fopen(GOLDEN_PATH,"w");
        if (out){ for(int i=0;i<n;++i){ u64 h=hash_scene(i); fprintf(out,"%s %llu\n",scene_name_at(i),(unsigned long long)h);} fclose(out); printf("recorded %d baselines.\n",n); }
        return 0;
    }
    printf("\n%d scenes: %d ok, %d new, %d FAILED\n", n, n-fails-newly, newly, fails);
    if (!fails) printf("ALL PASS%s\n", newly?" (new scenes recorded on next update)":"");
    return fails?1:0;
}
