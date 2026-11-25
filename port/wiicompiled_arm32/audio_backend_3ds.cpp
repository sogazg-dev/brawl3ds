// The runtime's audio output on the 3DS: the console's DSP (ndsp) directly, in place of runtime/src/audio_backend.cpp
// (SDL). port/headless/CMakeLists.txt builds this file instead of that one.
//
// The game's audio comes out a block per game frame (the runtime's deterministic clock), so a game running at a
// fraction of its speed produces that fraction of the samples a second. Played at the Wii's rate they would end in
// a fraction of the time: gaps eight times out of nine. Here the DSP channel's rate follows the game's speed
// (main_3ds.cpp, Brawl3dsAudioSpeed), so the sound is continuous and as slow as the picture; the DSP does the
// resampling, the CPU none. At full speed it is the Wii's sound.
//
// Needs sdmc:/3ds/dspfirm.cdc (the DSP firmware, dumped once with the Homebrew Launcher's DSP1 or by Luma); without
// it ndspInit fails and the game stays silent, with one line in stderr.txt.
#include "audio_backend.h"

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" float Brawl3dsAudioSpeed();  // main_3ds.cpp: the game's speed, 1 = 60 frames a second
extern "C" int Brawl3dsAudioWanted();   // main_3ds.cpp: play mode, and not switched off

namespace {
constexpr int kChannel = 0;
constexpr uint32_t kBuffers = 12;
constexpr uint32_t kBufferFrames = 1024;  // stereo frames: 32 ms at 32 kHz
ndspWaveBuf g_wave[kBuffers];
int16_t* g_memory = nullptr;  // linear memory: kBuffers * kBufferFrames stereo frames
uint32_t g_fill = 0;          // the buffer being filled
uint32_t g_fillFrames = 0;
float g_rate = 0.0f;
bool g_failed = false;
uint64_t g_dropped = 0;

bool Free(const ndspWaveBuf& wave) { return wave.status == NDSP_WBUF_FREE || wave.status == NDSP_WBUF_DONE; }

// The channel plays as fast as the game makes sound: frames queued a second, measured over half-second windows.
// (The game's frame rate is not the measure: on the console the runtime produced several times the audio the
// frame rate predicted, and 120,000 frames of a short session were dropped at a full queue.) The first window
// uses the frame rate's estimate. Never above the Wii's own rate.
uint64_t g_windowStart = 0;
uint32_t g_windowFrames = 0;

void FollowProduction(uint32_t count, uint32_t sampleRate)
{
    const uint64_t now = svcGetSystemTick();
    float rate = g_rate;
    if (g_rate == 0.0f) {
        rate = static_cast<float>(sampleRate) * std::clamp(Brawl3dsAudioSpeed(), 0.05f, 1.0f);
        g_windowStart = now;
        g_windowFrames = 0;
    }
    g_windowFrames += count;
    const uint64_t elapsed = now - g_windowStart;
    if (elapsed >= SYSCLOCK_ARM11 / 2) {
        const float measured = static_cast<float>(g_windowFrames) * static_cast<float>(SYSCLOCK_ARM11) /
                               static_cast<float>(elapsed);
        rate = g_rate * 0.5f + measured * 0.5f;
        g_windowStart = now;
        g_windowFrames = 0;
    }
    rate = std::clamp(rate, 1500.0f, static_cast<float>(sampleRate));
    if (g_rate == 0.0f || rate > g_rate * 1.03f || rate < g_rate * 0.97f) {
        g_rate = rate;
        ndspChnSetRate(kChannel, rate);
    }
}

// Interleaved left/right frames into the wave buffers; a full queue drops the rest (as the runtime's SDL queue did).
void Queue(const int16_t* frames, uint32_t count, uint32_t sampleRate)
{
    FollowProduction(count, sampleRate);
    while (count != 0) {
        ndspWaveBuf& wave = g_wave[g_fill];
        if (g_fillFrames == 0 && !Free(wave)) {
            g_dropped += count;
            return;
        }
        const uint32_t take = std::min(count, kBufferFrames - g_fillFrames);
        std::memcpy(const_cast<int16_t*>(wave.data_pcm16) + g_fillFrames * 2, frames, take * 2 * sizeof(int16_t));
        g_fillFrames += take;
        frames += take * 2;
        count -= take;
        if (g_fillFrames == kBufferFrames) {
            wave.nsamples = kBufferFrames;
            DSP_FlushDataCache(wave.data_pcm16, kBufferFrames * 2 * sizeof(int16_t));
            ndspChnWaveBufAdd(kChannel, &wave);
            g_fill = (g_fill + 1) % kBuffers;
            g_fillFrames = 0;
        }
    }
}
}  // namespace

AudioBackend& AudioBackend::Instance()
{
    static AudioBackend instance;
    return instance;
}

float AudioBackend::EffectiveGainLocked() const { return m_muted ? 0.0f : m_masterVolume; }

void AudioBackend::ApplyGainLocked()
{
    if (!m_initialized) return;
    float mix[12] = {};
    mix[0] = mix[1] = EffectiveGainLocked();
    ndspChnSetMix(kChannel, mix);
}

void AudioBackend::SetMasterVolume(float volume)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_masterVolume = std::clamp(volume, 0.0f, 1.0f);
    ApplyGainLocked();
}

void AudioBackend::SetMuted(bool muted)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_muted = muted;
    ApplyGainLocked();
}

bool AudioBackend::EnsureInitializedLocked(uint32_t sampleRate, uint32_t channels)
{
    if (m_initialized) {
        if (m_sampleRate != sampleRate) {
            m_sampleRate = sampleRate;
            g_rate = 0.0f;
        }
        return true;
    }
    if (g_failed || !Brawl3dsAudioWanted()) return false;
    const Result result = ndspInit();
    if (R_FAILED(result)) {
        g_failed = true;
        std::fprintf(stderr, "[audio] ndspInit failed (%08lX): no sound. It needs sdmc:/3ds/dspfirm.cdc\n",
                     static_cast<unsigned long>(result));
        return false;
    }
    g_memory = static_cast<int16_t*>(linearAlloc(kBuffers * kBufferFrames * 2 * sizeof(int16_t)));
    if (!g_memory) {
        g_failed = true;
        ndspExit();
        std::fprintf(stderr, "[audio] no linear memory for the wave buffers: no sound\n");
        return false;
    }
    std::memset(g_memory, 0, kBuffers * kBufferFrames * 2 * sizeof(int16_t));
    std::memset(g_wave, 0, sizeof(g_wave));
    for (uint32_t i = 0; i < kBuffers; ++i) {
        g_wave[i].data_pcm16 = g_memory + i * kBufferFrames * 2;
        g_wave[i].status = NDSP_WBUF_FREE;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(kChannel);
    ndspChnSetInterp(kChannel, NDSP_INTERP_LINEAR);
    ndspChnSetFormat(kChannel, NDSP_FORMAT_STEREO_PCM16);
    m_sampleRate = sampleRate;
    m_channels = channels;
    m_initialized = true;
    g_rate = 0.0f;
    FollowProduction(0, sampleRate);
    ApplyGainLocked();
    std::fprintf(stderr, "[audio] ndsp output: %lu Hz, %lu channels\n", static_cast<unsigned long>(sampleRate),
                 static_cast<unsigned long>(channels));
    return true;
}

bool AudioBackend::Init(uint32_t sampleRate, uint32_t channels)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return EnsureInitializedLocked(sampleRate, channels);
}

void AudioBackend::Shutdown()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) {
        ndspChnWaveBufClear(kChannel);
        ndspExit();
        linearFree(g_memory);
        g_memory = nullptr;
        if (g_dropped) std::fprintf(stderr, "[audio] %llu frames dropped (queue full)\n", static_cast<unsigned long long>(g_dropped));
    }
    m_initialized = false;
    m_sampleRate = 0;
    m_channels = 0;
    g_fill = g_fillFrames = 0;
}

uint32_t AudioBackend::QueueLimitBytesLocked() const { return kBuffers * kBufferFrames * 2 * sizeof(int16_t); }
bool AudioBackend::QueueHasCapacityLocked(int) { return m_initialized; }

bool AudioBackend::PushWiiAiSamplesBE16(const uint8_t* data, size_t bytes)
{
    if (!data || bytes < 4) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return false;
    // Wii AI frames are big-endian, right then left.
    const size_t frames = bytes / 4;
    if (m_convertBuffer.size() < frames * 2) m_convertBuffer.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        m_convertBuffer[i * 2] = static_cast<int16_t>(data[i * 4 + 2] << 8 | data[i * 4 + 3]);
        m_convertBuffer[i * 2 + 1] = static_cast<int16_t>(data[i * 4] << 8 | data[i * 4 + 1]);
    }
    Queue(m_convertBuffer.data(), static_cast<uint32_t>(frames), m_sampleRate);
    return true;
}

bool AudioBackend::PushSamplesLE16(const int16_t* samples, size_t sampleCount)
{
    if (!samples || sampleCount < 2) return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized) return false;
    if (m_channels == 1) {  // mono: both sides
        if (m_convertBuffer.size() < sampleCount * 2) m_convertBuffer.resize(sampleCount * 2);
        for (size_t i = 0; i < sampleCount; ++i) m_convertBuffer[i * 2] = m_convertBuffer[i * 2 + 1] = samples[i];
        Queue(m_convertBuffer.data(), static_cast<uint32_t>(sampleCount), m_sampleRate);
    } else {
        Queue(samples, static_cast<uint32_t>(sampleCount / 2), m_sampleRate);
    }
    return true;
}
