// ==========================================================================
// test_synth.cpp  -  unit tests for the headless polyphonic synth
// (cpp_synth_* in cppcore.h). Headless, deterministic, terminates in ms.
//
// Coverage (per the module spec):
//   (a) no notes  -> rendered block is near-silent (tiny RMS);
//   (b) note_on(p) -> non-zero RMS AND the dominant spectrum bin corresponds
//       to that pitch (within a small bin tolerance);
//   (c) different waveforms (sine vs square/saw) produce different spectra  - 
//       richer harmonic energy above the fundamental for the non-sine shapes;
//   (d) rendered samples are always finite and bounded (|s| < 4);
//   plus a couple of sanity checks (polyphony, note_off fade, sequencer runs).
// Prints PASS/FAIL per check and an ALL PASS / SOME FAILED summary.
// ==========================================================================
#include "cathode/cppcore.h"

#include <cstdio>
#include <cmath>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

// Record and print one boolean check.
static void check(const char *name, bool cond) {
    ++g_checks;
    if (cond) {
        std::printf("  PASS  %s\n", name);
    } else {
        std::printf("  FAIL  %s\n", name);
        ++g_failures;
    }
}

// Root-mean-square amplitude of a sample block.
static float rms(const float *x, int n) {
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += (double)x[i] * (double)x[i];
    return (float)std::sqrt(acc / (double)(n > 0 ? n : 1));
}

// All samples finite and within |s| < limit?
static bool finite_bounded(const float *x, int n, float limit) {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) return false;
        if (std::fabs(x[i]) >= limit) return false;
    }
    return true;
}

// MIDI note -> frequency (A4=69=440Hz). Mirrors the synth's own mapping so we
// can predict which spectrum bin should dominate.
static double midi_freq(int m) {
    return 440.0 * std::pow(2.0, (m - 69) / 12.0);
}

// Index of the largest-magnitude bin in [1, nbins) (skip DC bin 0).
static int argmax_bin(const float *mag, int nbins) {
    int best = 1;
    float bv = -1.0f;
    for (int k = 1; k < nbins; ++k) {
        if (mag[k] > bv) { bv = mag[k]; best = k; }
    }
    return best;
}

// Render `warmup` samples (in one call) so the ADSR settles into sustain and the
// analysis ring holds a steady-state block, then fetch the magnitude spectrum.
static void render_and_spectrum(CppSynth *s, int warmup,
                                std::vector<float> &mag, int nbins) {
    std::vector<float> buf(warmup);
    cpp_synth_render(s, buf.data(), warmup);
    mag.assign(nbins, 0.0f);
    cpp_synth_spectrum(s, mag.data(), nbins);
}

int main(void) {
    std::printf("== CATHODE audio synth ==\n");

    const int SR    = 44100;
    const int NBINS = 1024;                 // bin width = (SR/2)/NBINS ~= 21.5 Hz
    // Frequency-to-bin: f_k = k*(SR/2)/NBINS  ->  k = f * 2*NBINS / SR.
    auto freq_to_bin = [&](double f) {
        return (int)std::lround(f * 2.0 * (double)NBINS / (double)SR);
    };

    // ----------------------------------------------------------------------
    // (a) No notes -> near silence.
    // ----------------------------------------------------------------------
    {
        CppSynth *s = cpp_synth_create(SR);
        std::vector<float> buf(4096);
        cpp_synth_render(s, buf.data(), (int)buf.size());
        float r = rms(buf.data(), (int)buf.size());
        std::printf("  .. silence RMS = %.3e\n", r);
        check("silence: RMS is near zero", r < 1e-5f);
        check("silence: finite & bounded", finite_bounded(buf.data(), (int)buf.size(), 4.0f));
        cpp_synth_destroy(s);
    }

    // ----------------------------------------------------------------------
    // (b) note_on -> non-zero RMS and dominant bin at the played pitch.
    // ----------------------------------------------------------------------
    {
        CppSynth *s = cpp_synth_create(SR);
        cpp_synth_set_waveform(s, 0);        // sine: single clean partial
        const int note = 69;                 // A4 = 440 Hz
        cpp_synth_note_on(s, note, 1.0f);

        // Warm up past attack+decay so the ring is steady-state sustain.
        std::vector<float> mag;
        render_and_spectrum(s, 8192, mag, NBINS);

        // RMS over a fresh steady block.
        std::vector<float> buf(4096);
        cpp_synth_render(s, buf.data(), (int)buf.size());
        float r = rms(buf.data(), (int)buf.size());
        std::printf("  .. note RMS = %.4f\n", r);
        check("note_on: non-zero RMS", r > 0.05f);

        int dom = argmax_bin(mag.data(), NBINS);
        int exp = freq_to_bin(midi_freq(note));
        std::printf("  .. dominant bin = %d, expected ~%d\n", dom, exp);
        check("note_on: dominant bin matches pitch", std::abs(dom - exp) <= 2);

        check("note_on: finite & bounded", finite_bounded(buf.data(), (int)buf.size(), 4.0f));
        cpp_synth_destroy(s);
    }

    // ----------------------------------------------------------------------
    // (c) Different waveforms -> different spectra. A sine has essentially all
    //     energy in the fundamental; saw/square carry strong upper harmonics.
    //     Measure energy strictly ABOVE the fundamental band and compare.
    // ----------------------------------------------------------------------
    {
        const int note = 69;                 // 440 Hz
        int kf = freq_to_bin(midi_freq(note));
        int hi_start = kf * 2;               // ignore the fundamental's main lobe

        // Sum magnitudes in bins above ~2x the fundamental.
        auto upper_energy = [&](const std::vector<float> &mag) {
            double e = 0.0;
            for (int k = hi_start; k < NBINS; ++k) e += mag[k];
            return e;
        };

        // Open the filter wide so harmonics are not attenuated away.
        std::vector<float> mag_sine, mag_saw, mag_sq;

        CppSynth *a = cpp_synth_create(SR);
        cpp_synth_set_filter(a, 18000.0f, 0.0f);
        cpp_synth_set_waveform(a, 0);        // sine
        cpp_synth_note_on(a, note, 1.0f);
        render_and_spectrum(a, 8192, mag_sine, NBINS);
        cpp_synth_destroy(a);

        CppSynth *b = cpp_synth_create(SR);
        cpp_synth_set_filter(b, 18000.0f, 0.0f);
        cpp_synth_set_waveform(b, 1);        // saw
        cpp_synth_note_on(b, note, 1.0f);
        render_and_spectrum(b, 8192, mag_saw, NBINS);
        cpp_synth_destroy(b);

        CppSynth *c = cpp_synth_create(SR);
        cpp_synth_set_filter(c, 18000.0f, 0.0f);
        cpp_synth_set_waveform(c, 2);        // square
        cpp_synth_note_on(c, note, 1.0f);
        render_and_spectrum(c, 8192, mag_sq, NBINS);
        cpp_synth_destroy(c);

        double e_sine = upper_energy(mag_sine);
        double e_saw  = upper_energy(mag_saw);
        double e_sq   = upper_energy(mag_sq);
        std::printf("  .. upper-harmonic energy  sine=%.4f saw=%.4f square=%.4f\n",
                    e_sine, e_saw, e_sq);

        // Sine's above-fundamental energy should be tiny; the others substantial.
        check("sine has little upper-harmonic energy", e_sine < 0.05);
        check("saw has more harmonics than sine",   e_saw  > e_sine * 4.0 + 0.02);
        check("square has more harmonics than sine", e_sq  > e_sine * 4.0 + 0.02);

        // Saw and square differ from each other too (distinct harmonic series).
        double diff = 0.0;
        for (int k = 1; k < NBINS; ++k) diff += std::fabs(mag_saw[k] - mag_sq[k]);
        std::printf("  .. |saw - square| spectral distance = %.4f\n", diff);
        check("saw and square spectra differ", diff > 0.05);
    }

    // ----------------------------------------------------------------------
    // (d) Stress: many stacked voices stay finite and bounded (|s| < 4).
    // ----------------------------------------------------------------------
    {
        CppSynth *s = cpp_synth_create(SR);
        cpp_synth_set_waveform(s, 2);        // square: worst-case peaky sum
        cpp_synth_set_filter(s, 12000.0f, 0.9f); // high resonance -> stress filter
        for (int m = 48; m < 48 + 20; ++m)   // 20 note_ons (exceeds 16-voice pool)
            cpp_synth_note_on(s, m, 1.0f);

        std::vector<float> buf(8192);
        cpp_synth_render(s, buf.data(), (int)buf.size());
        float peak = 0.0f;
        for (float v : buf) peak = std::fmax(peak, std::fabs(v));
        std::printf("  .. stacked-voice peak = %.4f\n", peak);
        check("stacked voices finite & bounded", finite_bounded(buf.data(), (int)buf.size(), 4.0f));
        check("stacked voices non-silent", rms(buf.data(), (int)buf.size()) > 0.01f);
        cpp_synth_destroy(s);
    }

    // ----------------------------------------------------------------------
    // sanity: note_off makes a voice fade to silence within its release.
    // ----------------------------------------------------------------------
    {
        CppSynth *s = cpp_synth_create(SR);
        cpp_synth_set_waveform(s, 0);
        cpp_synth_note_on(s, 60, 1.0f);
        std::vector<float> buf(4096);
        cpp_synth_render(s, buf.data(), (int)buf.size());
        float loud = rms(buf.data(), (int)buf.size());
        cpp_synth_note_off(s, 60);
        // Render well past the release time; tail should decay to near silence.
        std::vector<float> tail(22050); // 0.5s >> release
        cpp_synth_render(s, tail.data(), (int)tail.size());
        float quiet = rms(tail.data() + 15000, (int)tail.size() - 15000);
        std::printf("  .. loud RMS=%.4f  post-release RMS=%.3e\n", loud, quiet);
        check("note_off releases voice to silence", loud > 0.05f && quiet < 1e-4f);
        cpp_synth_destroy(s);
    }

    // ----------------------------------------------------------------------
    // sanity: the generative sequencer auto-produces sound over time.
    // ----------------------------------------------------------------------
    {
        CppSynth *s = cpp_synth_create(SR);
        cpp_synth_set_waveform(s, 1);
        // Advance ~1 second of sequencer time in small steps, rendering as we go.
        std::vector<float> buf(2048);
        float total_rms = 0.0f;
        for (int i = 0; i < 30; ++i) {
            cpp_synth_sequencer_tick(s, 0.033f); // ~30 fps dt
            cpp_synth_render(s, buf.data(), (int)buf.size());
            total_rms += rms(buf.data(), (int)buf.size());
            if (!finite_bounded(buf.data(), (int)buf.size(), 4.0f)) { total_rms = -1.0f; break; }
        }
        std::printf("  .. sequencer accumulated RMS = %.4f\n", total_rms);
        check("sequencer auto-triggers audible notes", total_rms > 0.1f);
        cpp_synth_destroy(s);
    }

    // ---- reverb: adds a decaying tail after the note stops, stays bounded ----
    {
        const int N=4096;
        std::vector<float> buf(N);
        CppSynth* s = cpp_synth_create(44100);
        cpp_synth_set_reverb(s, 0.6f, 0.8f);
        cpp_synth_note_on(s, 69, 1.0f);
        for (int b=0;b<8;++b) cpp_synth_render(s, buf.data(), N);  // sustain
        cpp_synth_note_off(s, 69);
        // render a few blocks after release; the dry signal decays fast but the
        // reverb tail should keep some energy well after the note ends.
        float tail_rms=0; int blocks=6;
        for (int b=0;b<blocks;++b){
            cpp_synth_render(s, buf.data(), N);
            if (b>=3){ float e=0; for(int i=0;i<N;++i)e+=buf[i]*buf[i]; tail_rms+=std::sqrt(e/N); }
        }
        // bounded?
        bool bounded=true; for(int i=0;i<N;++i) if(std::fabs(buf[i])>4.0f) bounded=false;
        std::printf("  .. reverb tail RMS (post-release) = %.5f\n", tail_rms);
        check("reverb produces a decaying tail", tail_rms > 1e-4f);
        check("reverb output stays bounded", bounded);
        // wet=0 must be a true bypass (no tail)
        cpp_synth_set_reverb(s, 0.0f, 0.0f);
        cpp_synth_note_off(s, 69);
        for (int b=0;b<8;++b) cpp_synth_render(s, buf.data(), N);
        float silent=0; for(int i=0;i<N;++i) silent+=std::fabs(buf[i]);
        check("reverb wet=0 is silent bypass", silent < 1.0f);
        cpp_synth_destroy(s);
    }

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("SOME FAILED\n");
    return 1;
}
