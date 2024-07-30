// ==========================================================================
// audio_out.cpp — real audio device output for the synth, via macOS
// AudioQueue (AudioToolbox). Optional and headless-safe: if CATHODE_AUDIO is
// not defined at build time, these become no-ops that report "no device" so
// the audioviz scene still runs silently in CI / over SSH.
//
// Design: a small ring of AudioQueue buffers is primed and enqueued; the
// AudioQueue calls our C callback on a background thread whenever a buffer is
// free, and we fill it by pulling mono float samples from the synth
// (cpp_synth_render) and converting to interleaved stereo. The synth must
// outlive the stream (the scene owns it and stops audio in destroy()).
// ==========================================================================
#include "cathode/cppcore.h"

#ifdef CATHODE_AUDIO
#include <AudioToolbox/AudioToolbox.h>
#include <cstring>
#include <atomic>

namespace {
constexpr int kNumBuffers = 3;
constexpr int kFramesPerBuffer = 2048;

struct AudioState {
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kNumBuffers] = {};
    CppSynth *synth = nullptr;
    std::atomic<bool> running{false};
    float mono[kFramesPerBuffer];
};
AudioState g;

// AudioQueue output callback: fill `buf` with interleaved stereo float32.
void fill_cb(void * /*user*/, AudioQueueRef q, AudioQueueBufferRef buf) {
    if (!g.running.load()) return;
    int frames = (int)(buf->mAudioDataBytesCapacity / (sizeof(float) * 2));
    if (frames > kFramesPerBuffer) frames = kFramesPerBuffer;
    // pull mono from the synth, duplicate to L/R
    cpp_synth_render(g.synth, g.mono, frames);
    float *out = (float *)buf->mAudioData;
    for (int i = 0; i < frames; ++i) {
        float s = g.mono[i];
        if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
        out[i * 2] = s;
        out[i * 2 + 1] = s;
    }
    buf->mAudioDataByteSize = (UInt32)(frames * sizeof(float) * 2);
    AudioQueueEnqueueBuffer(q, buf, 0, nullptr);
}
} // namespace

extern "C" int cpp_audio_start(CppSynth *s, i32 sample_rate) {
    if (g.running.load()) return 1;      // already running
    if (!s) return 0;
    g.synth = s;

    AudioStreamBasicDescription fmt = {};
    fmt.mSampleRate = sample_rate > 0 ? sample_rate : 44100;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2;
    fmt.mBitsPerChannel = 32;
    fmt.mBytesPerFrame = fmt.mChannelsPerFrame * sizeof(float);
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerPacket = fmt.mBytesPerFrame;

    OSStatus st = AudioQueueNewOutput(&fmt, fill_cb, nullptr, nullptr, nullptr, 0, &g.queue);
    if (st != noErr || !g.queue) { g.queue = nullptr; return 0; }

    g.running.store(true);
    UInt32 bytes = kFramesPerBuffer * fmt.mBytesPerFrame;
    for (int i = 0; i < kNumBuffers; ++i) {
        if (AudioQueueAllocateBuffer(g.queue, bytes, &g.buffers[i]) != noErr) { cpp_audio_stop(); return 0; }
        // prime with silence then enqueue via the callback path
        std::memset(g.buffers[i]->mAudioData, 0, bytes);
        g.buffers[i]->mAudioDataByteSize = bytes;
        fill_cb(nullptr, g.queue, g.buffers[i]);
    }
    if (AudioQueueStart(g.queue, nullptr) != noErr) { cpp_audio_stop(); return 0; }
    return 1;
}

extern "C" void cpp_audio_stop(void) {
    g.running.store(false);
    if (g.queue) {
        AudioQueueStop(g.queue, true);
        AudioQueueDispose(g.queue, true);
        g.queue = nullptr;
    }
    g.synth = nullptr;
}

extern "C" int cpp_audio_running(void) { return g.running.load() ? 1 : 0; }

#else  // ---- no audio backend: headless-safe no-ops ----

extern "C" int  cpp_audio_start(CppSynth * /*s*/, i32 /*sr*/) { return 0; }
extern "C" void cpp_audio_stop(void) {}
extern "C" int  cpp_audio_running(void) { return 0; }

#endif
