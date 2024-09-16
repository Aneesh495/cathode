/* test_noise.c  -  exercises the CATHODE noise module:
 *   (a) rng determinism             -  same seed reproduces the sequence
 *   (b) rng_f32 range               -  stays in [0,1) over 1e5 draws
 *   (c) rng_normal statistics       -  mean ~0, stddev ~1 over 1e5 draws
 *   (d) perlin2 continuity          -  tiny coord step => tiny output step
 *   (e) perlin/value range          -  within [-1.001, 1.001]
 *   (f) fbm2 finiteness             -  finite across a grid
 * plus sanity checks on perlin3, simplex2, fbm3, ridged2.
 */
#include "cathode/noise.h"
#include <stdio.h>
#include <math.h>

static int failures = 0;
static int checks   = 0;

static void ok(const char *name) {
    checks++;
    printf("  ok   %s\n", name);
}
static void fail(const char *name, const char *why) {
    checks++;
    failures++;
    printf("  FAIL %s: %s\n", name, why);
}
#define CHECK(cond, name, why) do { if (cond) ok(name); else fail(name, why); } while (0)

int main(void) {
    printf("== CATHODE noise module tests ==\n");

    /* ---- (a) RNG determinism ---------------------------------------- */
    {
        Rng a, b;
        rng_seed(&a, 0xDEADBEEFCAFEBABEULL);
        rng_seed(&b, 0xDEADBEEFCAFEBABEULL);
        int match = 1;
        for (int i = 0; i < 10000; ++i)
            if (rng_next(&a) != rng_next(&b)) { match = 0; break; }
        CHECK(match, "rng determinism (same seed)", "sequences diverged");

        /* Different seeds should (almost surely) differ. */
        Rng c;
        rng_seed(&c, 0xDEADBEEFCAFEBABEULL);
        Rng d;
        rng_seed(&d, 0x1234567890ABCDEFULL);
        int differ = 0;
        for (int i = 0; i < 100; ++i)
            if (rng_next(&c) != rng_next(&d)) { differ = 1; break; }
        CHECK(differ, "rng distinct seeds differ", "different seeds gave identical stream");

        /* State must not be all-zero even for seed 0 (xoshiro would stick). */
        Rng z;
        rng_seed(&z, 0);
        int nonzero = z.s[0] | z.s[1] | z.s[2] | z.s[3];
        CHECK(nonzero != 0, "rng seed 0 non-degenerate", "state is all-zero for seed 0");
    }

    /* ---- (b) rng_f32 range ------------------------------------------ */
    {
        Rng r; rng_seed(&r, 42);
        int in_range = 1;
        double sum = 0.0;
        const int N = 100000;
        for (int i = 0; i < N; ++i) {
            f32 u = rng_f32(&r);
            if (!(u >= 0.0f && u < 1.0f)) { in_range = 0; break; }
            sum += u;
        }
        CHECK(in_range, "rng_f32 in [0,1) over 1e5", "value outside [0,1)");
        double mean = sum / N;
        CHECK(fabs(mean - 0.5) < 0.01, "rng_f32 mean ~0.5", "uniform mean off");

        /* rng_range respects bounds. */
        int rng_ok = 1;
        for (int i = 0; i < 10000; ++i) {
            f32 v = rng_range(&r, -3.0f, 7.0f);
            if (!(v >= -3.0f && v < 7.0f)) { rng_ok = 0; break; }
        }
        CHECK(rng_ok, "rng_range within bounds", "value outside [lo,hi)");
    }

    /* ---- (c) rng_normal statistics ---------------------------------- */
    {
        Rng r; rng_seed(&r, 7);
        const int N = 100000;
        double sum = 0.0, sumsq = 0.0;
        int finite = 1;
        for (int i = 0; i < N; ++i) {
            f32 g = rng_normal(&r);
            if (!isfinite(g)) { finite = 0; break; }
            sum += g;
            sumsq += (double)g * g;
        }
        CHECK(finite, "rng_normal all finite", "produced NaN/Inf");
        double mean = sum / N;
        double var  = sumsq / N - mean * mean;
        double sd   = sqrt(var);
        CHECK(fabs(mean) < 0.02, "rng_normal mean ~0", "mean too far from 0");
        CHECK(fabs(sd - 1.0) < 0.03, "rng_normal stddev ~1", "stddev too far from 1");
    }

    /* ---- (d) perlin2 continuity ------------------------------------- */
    {
        int cont = 1;
        f32 worst = 0.0f;
        for (int i = 0; i < 500; ++i) {
            f32 x = (f32)i * 0.137f - 20.0f;
            f32 y = (f32)i * 0.091f + 3.0f;
            f32 d = fabsf(perlin2(x, y) - perlin2(x + 1e-4f, y));
            if (d > worst) worst = d;
            if (d > 1e-2f) { cont = 0; break; }
        }
        CHECK(cont, "perlin2 continuity (dx=1e-4)", "large jump for tiny step");
        printf("       (perlin2 worst delta over dx=1e-4: %.3e)\n", worst);
    }

    /* ---- (e) perlin/value ranges ------------------------------------ */
    {
        int rng_ok = 1;
        f32 pmin = 1e9f, pmax = -1e9f, vmin = 1e9f, vmax = -1e9f;
        for (int gx = 0; gx < 120; ++gx) {
            for (int gy = 0; gy < 120; ++gy) {
                f32 x = (f32)gx * 0.13f - 5.3f;
                f32 y = (f32)gy * 0.17f + 2.1f;
                f32 p = perlin2(x, y);
                f32 v = noise2(x, y);
                if (p < pmin) pmin = p; if (p > pmax) pmax = p;
                if (v < vmin) vmin = v; if (v > vmax) vmax = v;
                if (p < -1.001f || p > 1.001f) rng_ok = 0;
                if (v < -1.001f || v > 1.001f) rng_ok = 0;
            }
        }
        CHECK(rng_ok, "perlin2/value within [-1.001,1.001]", "out of expected range");
        printf("       (perlin2 range [%.4f, %.4f]; value range [%.4f, %.4f])\n",
               pmin, pmax, vmin, vmax);

        /* perlin3 + simplex2 range as well. */
        int p3ok = 1, s2ok = 1;
        f32 p3min=1e9f,p3max=-1e9f, s2min=1e9f,s2max=-1e9f;
        for (int gx = 0; gx < 60; ++gx)
            for (int gy = 0; gy < 60; ++gy)
                for (int gz = 0; gz < 6; ++gz) {
                    f32 x=(f32)gx*0.21f, y=(f32)gy*0.19f, z=(f32)gz*0.33f;
                    f32 p = perlin3(x, y, z);
                    if (p<p3min)p3min=p; if(p>p3max)p3max=p;
                    if (p < -1.001f || p > 1.001f) p3ok = 0;
                    f32 sx = simplex2(x, y);
                    if (sx<s2min)s2min=sx; if(sx>s2max)s2max=sx;
                    if (sx < -1.001f || sx > 1.001f) s2ok = 0;
                }
        CHECK(p3ok, "perlin3 within [-1.001,1.001]", "out of range");
        CHECK(s2ok, "simplex2 within [-1.001,1.001]", "out of range");
        printf("       (perlin3 range [%.4f, %.4f]; simplex2 range [%.4f, %.4f])\n",
               p3min, p3max, s2min, s2max);
    }

    /* ---- (f) fbm2/fbm3/ridged2 finiteness --------------------------- */
    {
        int finite = 1;
        f32 fmin=1e9f, fmax=-1e9f;
        for (int gx = 0; gx < 128; ++gx) {
            for (int gy = 0; gy < 128; ++gy) {
                f32 x = (f32)gx * 0.05f;
                f32 y = (f32)gy * 0.05f;
                f32 f = fbm2(x, y, 6, 2.0f, 0.5f);
                if (!isfinite(f)) { finite = 0; break; }
                if (f<fmin)fmin=f; if(f>fmax)fmax=f;
                if (!isfinite(fbm3(x, y, x - y, 5, 2.0f, 0.5f))) { finite = 0; break; }
                if (!isfinite(ridged2(x, y, 5, 2.0f, 0.5f)))     { finite = 0; break; }
            }
            if (!finite) break;
        }
        CHECK(finite, "fbm2/fbm3/ridged2 finite on grid", "produced NaN/Inf");
        printf("       (fbm2 range [%.4f, %.4f])\n", fmin, fmax);

        /* fbm2 should itself be bounded in ~[-1,1] since we normalize. */
        CHECK(fmin >= -1.001f && fmax <= 1.001f, "fbm2 bounded [-1,1]", "fbm2 exceeded normalized range");

        /* Coherence: fbm should be deterministic (pure function). */
        CHECK(fbm2(1.23f, 4.56f, 4, 2.0f, 0.5f) == fbm2(1.23f, 4.56f, 4, 2.0f, 0.5f),
              "fbm2 deterministic", "same input gave different output");

        /* ridged2 non-negative by construction. */
        int rge = 1;
        for (int i = 0; i < 2000; ++i) {
            f32 x = (f32)i * 0.037f, y = (f32)i * 0.053f;
            f32 rr = ridged2(x, y, 5, 2.0f, 0.5f);
            if (rr < -1e-4f || rr > 1.001f) { rge = 0; break; }
        }
        CHECK(rge, "ridged2 within [0,1]", "ridged out of range");
    }

    printf("\n%d checks, %d failures\n", checks, failures);
    if (failures == 0) printf("ALL PASS \xE2\x9C\x93\n");
    return failures ? 1 : 0;
}
