// ==========================================================================
// synth.cpp  -  headless polyphonic synthesizer (cpp_synth_* in cppcore.h)
//
// A subtractive polyphonic synth that NEVER touches an audio device: it only
// renders sample buffers on request, so it is safe to run in a terminal / CI.
// A scene turns the rendered blocks (and their magnitude spectrum) into an
// audio-reactive CRT visualizer.
//
// Signal chain, per rendered sample:
//
//     for each sounding voice:
//         osc(waveform, phase) * ADSR_envelope * velocity   -->  mix
//     mix --> resonant state-variable low-pass filter --> master gain
//         --> tanh soft-clip  (guarantees |out| < 1)
//
// Design notes:
//   * Up to MAX_VOICES (16) voices; note_on steals the quietest voice when
//     the pool is full so a busy sequencer never silently drops notes.
//   * One global waveform + one global filter shared by all voices (classic
//     paraphonic-ish layout; matches the flat cpp_synth_set_* ABI).
//   * The filter is Andrew Simper's "Cytomic" TPT state-variable filter  - 
//     unconditionally stable, zero-delay-feedback, cheap per sample.
//   * A rolling history ring stores the most recent CAP output samples so
//     cpp_synth_spectrum can run a windowed DFT over "the last block" at any
//     bin count without re-rendering.
//   * The generative sequencer is a seeded arpeggiator walking a pentatonic
//     scale; sequencer_tick advances wall-clock time and auto note_on/off's.
//
// The C++ side uses STL/RAII internally; the boundary stays extern "C" POD.
// ==========================================================================
#include "cathode/cppcore.h"

#include <vector>
#include <cmath>
#include <cstdint>

// When linked into the full engine we also have the Rust compute core, whose
// radix-2 FFT computes the magnitude spectrum in O(n log n) instead of the
// naive O(bins*window) DFT below. The standalone `test_synth` compiles this
// file WITHOUT the Rust staticlib, so the fast path is guarded and falls back
// to the reference DFT there  -  both produce the same windowed magnitudes, so
// the test's "dominant bin matches pitch" assertions hold either way.
#ifdef CATHODE_HAVE_RUST_FFT
#include "cathode/rustcore.h"
#endif

namespace {

// --------------------------------------------------------------------------
// Tunables
// --------------------------------------------------------------------------
constexpr int    MAX_VOICES  = 16;     // polyphony cap
constexpr int    HIST_CAP    = 2048;   // analysis-window ring capacity (samples)
constexpr int    DFT_MAXWIN  = 2048;   // hard cap on DFT window length (bounded cost)
constexpr int    DFT_MAXBINS = 4096;   // hard cap on spectrum bins (bounded cost)
constexpr float  MASTER_GAIN = 0.30f;  // pre-clip headroom for summed voices

// ADSR times, in seconds (linear ramps  -  deterministic and click-free enough).
constexpr float  ATT_SEC = 0.006f;     // attack
constexpr float  DEC_SEC = 0.120f;     // decay
constexpr float  SUS_LVL = 0.70f;      // sustain level (0..1)
constexpr float  REL_SEC = 0.220f;     // release

// Generative sequencer timing (seconds).
constexpr double STEP_LEN = 0.150;     // one arpeggiator step (~100 BPM 1/16ths)
constexpr double GATE_LEN = 0.095;     // how long each stepped note is held

// --------------------------------------------------------------------------
// Envelope
// --------------------------------------------------------------------------
enum class EnvStage { Idle, Attack, Decay, Sustain, Release };

// Per-voice ADSR state. Levels are in [0,1]; the increments are per-sample and
// are derived once from the sample rate (see Synth ctor).
struct Env {
    EnvStage stage = EnvStage::Idle;
    float    level = 0.0f;   // current envelope amplitude
    float    rel_step = 0.0f; // per-sample decrement during Release (set at note_off)
};

// --------------------------------------------------------------------------
// Voice
// --------------------------------------------------------------------------
struct Voice {
    bool   active = false; // occupies a slot (still audible or ramping)
    int    midi   = -1;    // MIDI note this voice was triggered with
    double phase  = 0.0;   // oscillator phase in [0,1)
    double inc    = 0.0;   // phase increment per sample = freq / sample_rate
    float  vel    = 0.0f;  // note velocity in [0,1]
    Env    env;
};

// --------------------------------------------------------------------------
// TPT state-variable low-pass filter (Cytomic / Andrew Simper).
// Stores integrator states; coefficients are recomputed on set_filter.
// --------------------------------------------------------------------------
struct SVF {
    float ic1 = 0.0f, ic2 = 0.0f;      // integrator memories
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    // cutoff in Hz, resonance in [0,1] (0 = flat, ->1 = self-oscillating-ish).
    void configure(float cutoff_hz, float resonance, float sr) {
        // Keep cutoff strictly inside (0, Nyquist) so tan() stays finite.
        float fc = ct_clampf(cutoff_hz, 10.0f, sr * 0.45f);
        float r  = ct_clampf(resonance, 0.0f, 1.0f);
        // Pre-warped integrator gain for the bilinear transform.
        float g  = std::tan(CT_PI * fc / sr);
        // Damping k = 1/Q. Map resonance 0..1 -> k 2.0..0.10 (never 0 => stable).
        float k  = 2.0f - 1.9f * r;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    // Process one sample, returning the low-pass output.
    inline float lp(float x) {
        float v3 = x - ic2;
        float v1 = a1 * ic1 + a2 * v3;
        float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2; // band 2 == low-pass
    }
};

// --------------------------------------------------------------------------
// Oscillator: evaluate the selected naive waveform at phase in [0,1).
// Naive (non-band-limited) is fine here: the low-pass tames the harshest
// harmonics and the fundamental always remains the dominant partial.
// --------------------------------------------------------------------------
inline float osc(int wave, double phase) {
    switch (wave) {
        case 1: // saw: ramp -1..+1
            return (float)(2.0 * phase - 1.0);
        case 2: // square: +1 first half, -1 second half
            return phase < 0.5 ? 1.0f : -1.0f;
        case 3: // triangle: |.|-folded ramp, range -1..+1
            return (float)(4.0 * std::fabs(phase - 0.5) - 1.0);
        case 0: // sine
        default:
            return std::sin((float)(CT_TAU * phase));
    }
}

} // namespace

// --------------------------------------------------------------------------
// Opaque synth object exposed to C.
// --------------------------------------------------------------------------
// ---- Schroeder reverb: 4 parallel feedback-comb filters summed, then 2
// series allpass filters. Classic Freeverb-lineage topology. Delay lengths are
// mutually-prime-ish (in samples at 44.1k) to avoid ringing. Mono. ----
struct Comb {
    std::vector<float> buf; int idx=0; float fb=0.7f, damp=0.2f, store=0.0f;
    void init(int len){ buf.assign(len>1?len:1, 0.0f); idx=0; store=0.0f; }
    inline float process(float x){
        float y=buf[idx];
        store = y*(1.0f-damp) + store*damp;      // one-pole damping in the loop
        buf[idx]=x + store*fb;
        if(++idx>=(int)buf.size()) idx=0;
        return y;
    }
};
struct Allpass {
    std::vector<float> buf; int idx=0; float fb=0.5f;
    void init(int len){ buf.assign(len>1?len:1, 0.0f); idx=0; }
    inline float process(float x){
        float y=buf[idx];
        float out=-x + y;
        buf[idx]=x + y*fb;
        if(++idx>=(int)buf.size()) idx=0;
        return out;
    }
};
struct Reverb {
    Comb comb[4];
    Allpass ap[2];
    float wet=0.0f;
    void init(int sr){
        // tuning lengths scaled from the classic 44.1k Freeverb constants
        double sc = sr/44100.0;
        int cl[4]={1557,1617,1491,1422};
        int al[2]={225,556};
        for(int i=0;i<4;++i) comb[i].init((int)(cl[i]*sc));
        for(int i=0;i<2;++i) ap[i].init((int)(al[i]*sc));
    }
    void set(float w, float room){
        wet = w<0?0:(w>1?1:w);
        float fb = 0.7f + 0.28f*(room<0?0:(room>1?1:room));  // 0.70..0.98
        for(int i=0;i<4;++i) comb[i].fb=fb;
    }
    inline float process(float x){
        if (wet<=0.0f) return x;
        float acc=0.0f;
        for(int i=0;i<4;++i) acc+=comb[i].process(x);
        acc*=0.25f;
        acc=ap[0].process(acc);
        acc=ap[1].process(acc);
        return x*(1.0f-wet) + acc*wet;
    }
};

struct CppSynth {
    int   sample_rate = 44100;
    int   waveform    = 0;          // 0 sine 1 saw 2 square 3 tri
    float gain        = MASTER_GAIN;

    Voice voices[MAX_VOICES];
    SVF   filter;
    Reverb reverb;

    // Envelope rates (per-sample), derived from the sample rate once.
    float att_step = 0.0f;          // Attack: level += att_step
    float dec_step = 0.0f;          // Decay:  level -= dec_step toward SUS_LVL

    // Rolling history of rendered output for the spectrum analyzer.
    std::vector<float> hist;        // size HIST_CAP ring
    int   hist_head = 0;            // next write index
    int   hist_len  = 0;            // valid samples (<= HIST_CAP)

    // Generative sequencer state.
    double seq_time     = 0.0;      // accumulated wall-clock seconds
    long   seq_step     = -1;       // index of last-triggered step (-1 = none yet)
    bool   seq_gate     = false;    // is the sequencer note currently held?
    int    seq_note     = -1;       // MIDI note the sequencer is holding
    double seq_gate_off = 0.0;      // absolute time to release the held note
    uint32_t rng        = 0;        // xorshift32 state (seed)

    CppSynth(int sr) {
        sample_rate = (sr > 0) ? sr : 44100;
        float srf = (float)sample_rate;
        // A per-sample step of 1/(t*sr) crosses the full 0..1 range in t seconds.
        att_step = 1.0f / ct_maxf(1.0f, ATT_SEC * srf);
        dec_step = (1.0f - SUS_LVL) / ct_maxf(1.0f, DEC_SEC * srf);
        filter.configure(8000.0f, 0.10f, srf); // musical default: bright, mild Q
        reverb.init(sample_rate);
        hist.assign(HIST_CAP, 0.0f);
        // Seed the arpeggiator RNG; must be nonzero for xorshift.
        rng = 0x1234567u ^ (uint32_t)sample_rate;
        if (rng == 0) rng = 0xA5A5A5A5u;
    }

    // xorshift32 PRNG  -  fast, deterministic, good enough for note choice.
    inline uint32_t rand_u32() {
        uint32_t x = rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        rng = x;
        return x;
    }

    // MIDI note -> frequency (A4=69=440Hz), then -> phase increment/sample.
    inline double note_to_inc(int midi) const {
        double f = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        return f / (double)sample_rate;
    }

    // Find a slot for a new note: prefer an idle voice, else steal the one with
    // the lowest envelope level (least audible). Always returns a valid index.
    int alloc_voice() {
        int best = 0;
        float best_lvl = 1e30f;
        for (int i = 0; i < MAX_VOICES; ++i) {
            if (!voices[i].active) return i;         // free slot wins outright
            float l = voices[i].env.level;
            if (l < best_lvl) { best_lvl = l; best = i; }
        }
        return best; // steal quietest
    }

    void note_on(int midi, float vel) {
        if (midi < 0 || midi > 127) return;
        int i = alloc_voice();
        Voice& v = voices[i];
        v.active = true;
        v.midi   = midi;
        v.phase  = 0.0;                          // reset for a clean attack
        v.inc    = note_to_inc(midi);
        v.vel    = ct_clampf(vel, 0.0f, 1.0f);
        v.env.stage = EnvStage::Attack;
        v.env.level = 0.0f;
    }

    void note_off(int midi) {
        // Release EVERY matching, still-sounding voice (handles retriggers).
        for (int i = 0; i < MAX_VOICES; ++i) {
            Voice& v = voices[i];
            if (v.active && v.midi == midi && v.env.stage != EnvStage::Release &&
                v.env.stage != EnvStage::Idle) {
                v.env.stage = EnvStage::Release;
                // Ramp from wherever we are down to 0 over REL_SEC.
                float rel_samps = ct_maxf(1.0f, REL_SEC * (float)sample_rate);
                v.env.rel_step = v.env.level / rel_samps;
            }
        }
    }

    // Advance one voice's envelope by a single sample; returns its amplitude.
    // Frees the voice when a Release ramp reaches zero.
    inline float env_step(Voice& v) {
        Env& e = v.env;
        switch (e.stage) {
            case EnvStage::Attack:
                e.level += att_step;
                if (e.level >= 1.0f) { e.level = 1.0f; e.stage = EnvStage::Decay; }
                break;
            case EnvStage::Decay:
                e.level -= dec_step;
                if (e.level <= SUS_LVL) { e.level = SUS_LVL; e.stage = EnvStage::Sustain; }
                break;
            case EnvStage::Sustain:
                break;                            // hold until note_off
            case EnvStage::Release:
                e.level -= e.rel_step;
                if (e.level <= 0.0f) {            // fully faded -> reclaim slot
                    e.level = 0.0f;
                    e.stage = EnvStage::Idle;
                    v.active = false;
                }
                break;
            case EnvStage::Idle:
            default:
                e.level = 0.0f;
                break;
        }
        return e.level;
    }

    // Push one finished output sample into the history ring.
    inline void hist_push(float s) {
        hist[hist_head] = s;
        hist_head = (hist_head + 1) % HIST_CAP;
        if (hist_len < HIST_CAP) ++hist_len;
    }

    void render(float* out, int n) {
        for (int s = 0; s < n; ++s) {
            float mix = 0.0f;
            for (int i = 0; i < MAX_VOICES; ++i) {
                Voice& v = voices[i];
                if (!v.active) continue;
                float amp = env_step(v);          // advance + read envelope
                if (!v.active) continue;          // may have just been freed
                float w = osc(waveform, v.phase); // waveform sample
                mix += w * amp * v.vel;
                v.phase += v.inc;                 // advance & wrap oscillator
                if (v.phase >= 1.0) v.phase -= std::floor(v.phase);
            }
            float filtered = filter.lp(mix);      // resonant low-pass
            // Master gain + tanh soft-clip: bounds output to (-1,1) no matter
            // how many voices stack, and adds a touch of analog-ish warmth.
            float y = std::tanh(filtered * gain);
            y = reverb.process(y);                 // spatial tail (mono, no-op if wet=0)
            out[s] = y;
            hist_push(y);
        }
    }

    // Windowed DFT magnitude spectrum over the most recent samples.
    // Bin k maps linearly to frequency f_k = k * (sample_rate/2) / nbins, so the
    // full [0, Nyquist] range is covered by `nbins` bins. A Hann window reduces
    // spectral leakage, sharpening the dominant peak.
    void spectrum(float* out_mag, int nbins) {
        if (nbins <= 0) return;
        int bins = (nbins < DFT_MAXBINS) ? nbins : DFT_MAXBINS;
        for (int k = 0; k < nbins; ++k) out_mag[k] = 0.0f; // zero any tail past cap

        int W = hist_len;                          // available samples
        if (W > DFT_MAXWIN) W = DFT_MAXWIN;
        if (W < 2) return;                         // nothing meaningful yet

        // Gather the last W samples in chronological order into a scratch buffer,
        // applying a Hann window as we go.
        std::vector<float> win(W);
        int start = (hist_head - W) % HIST_CAP;
        if (start < 0) start += HIST_CAP;
        float wnorm = 0.0f;
        for (int n = 0; n < W; ++n) {
            float h = 0.5f * (1.0f - std::cos(CT_TAU * (float)n / (float)(W - 1)));
            win[n] = hist[(start + n) % HIST_CAP] * h;
            wnorm += h;
        }
        if (wnorm <= 0.0f) wnorm = (float)W;

        // Target bin k evaluates the windowed signal at digital frequency
        // omega_k = pi*k/nbins rad/sample (so f_k = k*(sr/2)/nbins spans [0,Nyquist)).
#ifdef CATHODE_HAVE_RUST_FFT
        // Fast path: one radix-2 FFT of the zero-padded windowed frame, then read
        // off the bins. FFT bin j sits at 2*pi*j/N rad/sample, so it coincides
        // with omega_k when j = k*N/(2*nbins). For the analyzer's steady-state
        // window (W = 2048, a power of two) this is an exact integer map, so the
        // FFT sum equals the reference DFT sum bin-for-bin  -  same magnitudes,
        // O(N log N) instead of O(bins*W). We reuse the identical 2/wnorm gain.
        {
            int N = 1; while (N < W) N <<= 1;          // next power of two >= W
            std::vector<float> data((size_t)N * 2, 0.0f);
            for (int n = 0; n < W; ++n) data[(size_t)2 * n] = win[n];  // re; im=0
            rust_fft_complex(data.data(), (unsigned)N, 0);
            const double scale = 2.0 / (double)wnorm;
            const int half = N / 2;
            for (int k = 0; k < bins; ++k) {
                // nearest FFT bin; exact when N == 2*nbins (the common case)
                int j = (int)(((long long)k * N + nbins) / (2 * nbins));  // round
                if (j > half) j = half;
                double re = data[(size_t)2 * j], im = data[(size_t)2 * j + 1];
                out_mag[k] = (float)(std::sqrt(re * re + im * im) * scale);
            }
            return;
        }
#else
        // Reference path: evaluate the real-input DFT directly at each omega_k.
        const double inv = 1.0 / (double)nbins;
        for (int k = 0; k < bins; ++k) {
            double w0 = CT_PI * (double)k * inv;   // radians advanced per sample
            double re = 0.0, im = 0.0;
            for (int n = 0; n < W; ++n) {
                double ang = w0 * (double)n;
                re += win[n] * std::cos(ang);
                im -= win[n] * std::sin(ang);
            }
            // Normalize by the window's coherent gain so magnitudes are ~linear
            // in amplitude and comparable across window lengths.
            out_mag[k] = (float)(std::sqrt(re * re + im * im) * 2.0 / (double)wnorm);
        }
#endif
    }

    // Pick the next arpeggiator note: a minor-pentatonic degree over a two-octave
    // span above a fixed root. Deterministic given the seed.
    int seq_pick() {
        static const int SCALE[5] = {0, 3, 5, 7, 10}; // minor pentatonic
        constexpr int ROOT = 57;                       // A3
        int degree = (int)(rand_u32() % 5u);
        int octave = (int)(rand_u32() % 2u);           // 0 or 1 octave up
        int note = ROOT + SCALE[degree] + 12 * octave;
        return ct_maxi(0, ct_mini(127, note));
    }

    // Advance the generative sequencer by dt seconds, auto-triggering notes.
    // Bounded: at most a fixed number of step transitions are processed per call.
    void sequencer_tick(float dt) {
        if (dt <= 0.0f) return;
        seq_time += (double)dt;

        // Which step does the clock currently sit in?
        long cur = (long)std::floor(seq_time / STEP_LEN);

        // Advance through any steps we crossed (bounded by a guard so a huge dt
        // can never spin forever).
        int guard = 0;
        while (seq_step < cur && guard < 512) {
            ++seq_step;
            ++guard;
            if (seq_gate) { note_off(seq_note); seq_gate = false; } // end prev
            seq_note = seq_pick();
            note_on(seq_note, 0.85f);
            seq_gate = true;
            seq_gate_off = (double)seq_step * STEP_LEN + GATE_LEN;
        }

        // Within the current step, release the note once its gate time passes.
        if (seq_gate && seq_time >= seq_gate_off) {
            note_off(seq_note);
            seq_gate = false;
        }
    }
};

// --------------------------------------------------------------------------
// C ABI
// --------------------------------------------------------------------------
extern "C" {

CppSynth *cpp_synth_create(i32 sample_rate) {
    return new CppSynth((int)sample_rate);
}

void cpp_synth_destroy(CppSynth *s) {
    delete s; // RAII: vectors + POD members freed
}

void cpp_synth_note_on(CppSynth *s, i32 midi_note, f32 velocity) {
    if (!s) return;
    s->note_on((int)midi_note, velocity);
}

void cpp_synth_note_off(CppSynth *s, i32 midi_note) {
    if (!s) return;
    s->note_off((int)midi_note);
}

void cpp_synth_set_waveform(CppSynth *s, i32 wave) {
    if (!s) return;
    // Clamp to the four supported shapes; unknown values fall back to sine.
    s->waveform = (wave >= 0 && wave <= 3) ? (int)wave : 0;
}

void cpp_synth_set_filter(CppSynth *s, f32 cutoff_hz, f32 resonance) {
    if (!s) return;
    s->filter.configure(cutoff_hz, resonance, (float)s->sample_rate);
}

void cpp_synth_set_reverb(CppSynth *s, f32 wet, f32 roomsize) {
    if (!s) return;
    s->reverb.set(wet, roomsize);
}

void cpp_synth_render(CppSynth *s, f32 *out, i32 n) {
    if (!s || !out || n <= 0) return;
    s->render(out, (int)n);
}

void cpp_synth_spectrum(CppSynth *s, f32 *out_mag, i32 nbins) {
    if (!s || !out_mag || nbins <= 0) return;
    s->spectrum(out_mag, (int)nbins);
}

void cpp_synth_sequencer_tick(CppSynth *s, f32 dt) {
    if (!s) return;
    s->sequencer_tick(dt);
}

} // extern "C"
