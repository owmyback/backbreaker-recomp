// Audio (XAudio render driver): the game registers a render-driver client (FMOD's output), whose
// callback asks for one frame of 256 samples at 48 kHz; the game answers with
// XAudioSubmitRenderDriverFrame. Submitted frames go to an SDL3 audio stream on the default
// playback device and, with --audio-dump, to a WAV file.
//
// Pacing: the callback runs on a guest thread every 5.33 ms (one frame). With a device open, the
// stream's fill level steers it: a callback is skipped while the stream holds kHighWater frames or
// more, and an extra one runs when it holds fewer than kLowWater, so the game's audio clock
// follows the device clock instead of drifting against it. Without a device (headless, --mute,
// or no audio server) the timer alone paces it, as before.
//
// Signatures follow Xenia's xboxkrnl_audio.cc; the frame layout is in frame_convert.h.
#include "apu.h"
#include "frame_convert.h"
#include "system_sound.h"
#include "../kernel/import_support.h"
#include "../kernel/objects.h"
#include "../log.h"
#include "../debug/thread_times.h"
#include "../paths.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mem;

namespace
{
    constexpr uint32_t kMaxClients = 8;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;
    constexpr uint32_t kRate = 48000;
    constexpr int kLowWater = 4;   // frames (21 ms): below this, run an extra callback
    constexpr int kHighWater = 12; // frames (64 ms): at or above this, skip a callback

    // The callback receives a pointer to a dword holding its argument (as on the console)
    struct Client { uint32_t callback = 0, argPtr = 0; bool active = false; };
    std::mutex g_lock;
    Client g_clients[kMaxClients];
    std::atomic<bool> g_started{false};

    kernel::GuestThread* g_audioThread = nullptr;
    kernel::Thread* g_audioThreadObj = nullptr;
    PPCContext* g_audioCtx = nullptr;

    // ---- options (set from main before the game starts) ----
    bool g_wantDevice = true;
    std::string g_dumpPath;

    // ---- host output ----
    SDL_AudioStream* g_stream = nullptr;
    int g_streamChannels = 0; // 2 (stereo downmix) or 6 (SDL converts 5.1 to the device layout)
    int g_hostFrameBytes = 0;

    // ---- statistics (printed every 5 s and in the exit summary) ----
    std::atomic<uint64_t> g_callbacks{0}, g_submitted{0}, g_audible{0};
    std::atomic<uint64_t> g_skipped{0}, g_extra{0}, g_underruns{0}, g_starvedBytes{0};
    std::atomic<bool> g_primed{false}; // the stream reached kLowWater once (underruns count from then)
    std::mutex g_statsLock;
    float g_peak[apu::frame::kChannels] = {};
    double g_energy[apu::frame::kChannels] = {};

    // ---- the runtime's own sounds (the unlock chime, #105), mixed over the game's ----
    std::vector<float> g_chime;            // made once, before the game starts (initAudio)
    std::atomic<int64_t> g_chimePos{-1};   // next sample to play, -1 = silent

    // Adds the next 256 samples of the chime to FL/FR of the host buffers that this frame
    // produces (channels = 2 or 6, interleaved). A restart from another thread wins over the
    // advance (compare-exchange).
    // A sound of the player's own replaces the chime: game/sounds/achievement.wav (any WAV SDL
    // reads; gitignored with the rest of game/, never committed). Converted to 48 kHz mono float
    // and scaled to the chime's peak, so it sits at the same level over the game. At most 10 s.
    bool loadUserUnlockSound(const char* path)
    {
        SDL_AudioSpec spec{};
        Uint8* data = nullptr;
        Uint32 length = 0;
        if (!SDL_LoadWAV(path, &spec, &data, &length))
            return false; // no file (the usual case) or not a WAV SDL reads
        const SDL_AudioSpec want{ SDL_AUDIO_F32, 1, int(kRate) };
        Uint8* out = nullptr;
        int outLength = 0;
        bool ok = SDL_ConvertAudioSamples(&spec, data, int(length), &want, &out, &outLength);
        SDL_free(data);
        if (!ok)
        {
            warn("[apu] %s: could not convert it: %s", path, SDL_GetError());
            return false;
        }
        size_t n = std::min<size_t>(size_t(outLength) / sizeof(float), size_t(kRate) * 10);
        std::vector<float> sound(reinterpret_cast<float*>(out), reinterpret_cast<float*>(out) + n);
        SDL_free(out);
        float peak = 0;
        for (float v : sound) peak = std::max(peak, std::fabs(v));
        if (n == 0 || peak <= 0)
        {
            warn("[apu] %s is silent: keeping the built-in chime", path);
            return false;
        }
        for (float& v : sound) v *= apu::sound::kChimePeak / peak;
        g_chime = std::move(sound);
        info("[apu] unlock sound: %s (%.2f s, %d Hz %d ch in the file)", path, double(n) / kRate, spec.freq, spec.channels);
        return true;
    }

    // Adds the unlock sound to a frame and turns the game down under it (sound::duckGain), from
    // its start until the ducking has come back up after it.
    void mixChime(float* stereo, float* six)
    {
        int64_t pos = g_chimePos.load(std::memory_order_acquire);
        if (pos < 0 || g_chime.empty()) return;
        for (uint32_t i = 0; i < apu::frame::kSamples; i++)
        {
            size_t s = size_t(pos) + i;
            float v = s < g_chime.size() ? g_chime[s] : 0.0f;
            float g = apu::sound::duckGain(s, g_chime.size(), kRate);
            if (stereo)
            {
                stereo[i * 2] = stereo[i * 2] * g + v;
                stereo[i * 2 + 1] = stereo[i * 2 + 1] * g + v;
            }
            if (six)
            {
                for (uint32_t c = 0; c < apu::frame::kChannels; c++) six[i * apu::frame::kChannels + c] *= g;
                six[i * apu::frame::kChannels + apu::frame::FL] += v;
                six[i * apu::frame::kChannels + apu::frame::FR] += v;
            }
        }
        const int64_t span = int64_t(g_chime.size() + apu::sound::duckTail(kRate));
        int64_t next = pos + apu::frame::kSamples >= span ? -1 : pos + apu::frame::kSamples;
        g_chimePos.compare_exchange_strong(pos, next);
    }

    // ---- WAV dump: stereo 32-bit float, the stereo downmix of every submitted frame ----
    std::mutex g_dumpLock;
    FILE* g_dump = nullptr;
    uint64_t g_dumpFrames = 0;

    void put32(FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); } // WAV is little-endian, as the host
    void put16(FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }

    void writeWavHeader(FILE* f, uint64_t frames)
    {
        uint32_t dataBytes = uint32_t(std::min<uint64_t>(frames * apu::frame::kSamples * 8, 0xFFFFFF00u));
        std::fseek(f, 0, SEEK_SET);
        std::fwrite("RIFF", 1, 4, f);
        put32(f, 36 + dataBytes);
        std::fwrite("WAVEfmt ", 1, 8, f);
        put32(f, 16);
        put16(f, 3); // WAVE_FORMAT_IEEE_FLOAT
        put16(f, 2);
        put32(f, kRate);
        put32(f, kRate * 8);
        put16(f, 8);
        put16(f, 32);
        std::fwrite("data", 1, 4, f);
        put32(f, dataBytes);
        std::fseek(f, 0, SEEK_END);
    }

    void dumpFrame(const float* stereo)
    {
        std::lock_guard guard(g_dumpLock);
        if (!g_dump) return;
        std::fwrite(stereo, 8, apu::frame::kSamples, g_dump);
        // Keep the header's sizes current about once a second, so the file is valid even if the
        // run ends in a crash or a kill -9.
        if (++g_dumpFrames % (kRate / apu::frame::kSamples) == 0)
        {
            writeWavHeader(g_dump, g_dumpFrames);
            std::fflush(g_dump);
        }
    }

    void accumulate(const uint8_t* frame)
    {
        float peak[apu::frame::kChannels] = {};
        double energy[apu::frame::kChannels] = {};
        for (uint32_t c = 0; c < apu::frame::kChannels; c++)
            for (uint32_t i = 0; i < apu::frame::kSamples; i++)
            {
                float s = apu::frame::sample(frame, c, i);
                peak[c] = std::max(peak[c], std::fabs(s));
                energy[c] += double(s) * s;
            }
        bool audible = false;
        std::lock_guard guard(g_statsLock);
        for (uint32_t c = 0; c < apu::frame::kChannels; c++)
        {
            g_peak[c] = std::max(g_peak[c], peak[c]);
            g_energy[c] += energy[c];
            audible |= peak[c] > 1.0f / 32768;
        }
        if (audible) g_audible++;
    }

    // Called by SDL from its device thread when the device wants data. additional > 0 means the
    // stream holds less than the device asked for, and SDL pads the rest with silence: an underrun.
    void SDLCALL onDeviceRequest(void*, SDL_AudioStream*, int additional, int)
    {
        if (additional > 0 && g_primed.load(std::memory_order_relaxed))
        {
            g_underruns++;
            g_starvedBytes += uint64_t(additional);
        }
    }

    void runCallbacks()
    {
        Client snapshot[kMaxClients];
        {
            std::lock_guard guard(g_lock);
            for (uint32_t i = 0; i < kMaxClients; i++) snapshot[i] = g_clients[i];
        }
        for (auto& c : snapshot)
        {
            if (!c.active) continue;
            PPCContext& ctx = *g_audioCtx;
            ctx.r1.u64 = g_audioThread->stackBase - 0x100;
            ctx.r3.u64 = c.argPtr;
            ctx.lr = 0;
            (PPC_LOOKUP_FUNC(base, c.callback))(ctx, base);
            g_callbacks++;
        }
    }

    void report()
    {
        static uint64_t lastSubmitted = 0, lastAudible = 0, lastPassNs = 0, lastDecodeNs = 0;
        static auto lastTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double seconds = std::max(1e-3, std::chrono::duration<double>(now - lastTime).count());
        uint64_t s = g_submitted, a = g_audible;
        int queued = g_stream ? SDL_GetAudioStreamQueued(g_stream) / g_hostFrameBytes : -1;
        apu::XmaCounters x = apu::xmaCounters();
        info("[apu] %llu frames (+%llu, %llu audible), queue %d, underruns %llu, skipped %llu, extra %llu; "
             "xma kicks %llu, frames %llu decoded %llu silent, errors %llu decode %llu parse, %.1f ms/s in passes (%.1f decoding)",
             (unsigned long long)s, (unsigned long long)(s - lastSubmitted), (unsigned long long)(a - lastAudible),
             queued, (unsigned long long)g_underruns.load(), (unsigned long long)g_skipped.load(),
             (unsigned long long)g_extra.load(), (unsigned long long)x.kicks, (unsigned long long)x.decoded,
             (unsigned long long)x.silent, (unsigned long long)x.decodeErrors, (unsigned long long)x.parseErrors,
             double(x.passNs - lastPassNs) / 1e6 / seconds, double(x.decodeNs - lastDecodeNs) / 1e6 / seconds);
        lastSubmitted = s;
        lastAudible = a;
        lastPassNs = x.passNs;
        lastDecodeNs = x.decodeNs;
        lastTime = now;
    }

    void audioWorker()
    {
        kernel::setCurrentThread(g_audioThread);
        kernel::adoptExistingThread(g_audioThreadObj);
        debug::threadtimes::registerRole("audio", "bb-audio");
        using clock = std::chrono::steady_clock;
        const auto period = std::chrono::nanoseconds(1000000000ll * apu::frame::kSamples / kRate);
        auto next = clock::now();
        auto lastReport = next;
        for (;;)
        {
            next += period;
            std::this_thread::sleep_until(next);
            auto now = clock::now();
            if (now - next > std::chrono::milliseconds(50))
                next = now; // a long stall (debugger, swapping): don't burst to catch up
            int calls = 1;
            if (g_stream)
            {
                int queued = SDL_GetAudioStreamQueued(g_stream) / g_hostFrameBytes;
                if (queued >= kLowWater) g_primed.store(true, std::memory_order_relaxed);
                if (queued >= kHighWater) { calls = 0; g_skipped++; }
                else if (queued < kLowWater && g_submitted > 0) { calls = 2; g_extra++; }
            }
            for (int k = 0; k < calls; k++)
                runCallbacks();
            if (now - lastReport >= std::chrono::seconds(5))
            {
                report();
                lastReport = now;
            }
        }
    }

    void startWorker()
    {
        if (g_started.exchange(true)) return;
        g_audioCtx = new PPCContext{};
        g_audioCtx->fpscr.loadFromHost();
        g_audioThread = kernel::createGuestThread(*g_audioCtx, 0x40000);
        g_audioThreadObj = new kernel::Thread();
        g_audioThreadObj->guestThread = g_audioThread;
        g_audioThreadObj->ctx = g_audioCtx;
        g_audioThreadObj->guest = g_audioThread->kthread;
        std::thread(audioWorker).detach();
    }

    void openDevice()
    {
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // SIGINT/SIGTERM end the run (main.cpp)
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_STREAM_NAME, "Backbreaker");
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            warn("[apu] no audio output: SDL audio init failed: %s", SDL_GetError());
            return;
        }
        // 5.1 goes to the device as 5.1 when it has more than two channels (SDL converts to its
        // layout); a stereo or mono device gets our downmix, which keeps front-only content at
        // full level (SDL's own 5.1 -> stereo matrix attenuates it).
        SDL_AudioSpec device{};
        int channels = 2;
        if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &device, nullptr) && device.channels > 2)
            channels = 6;
        SDL_AudioSpec spec{ SDL_AUDIO_F32, channels, int(kRate) };
        g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, onDeviceRequest, nullptr);
        if (!g_stream)
        {
            warn("[apu] no audio output: %s", SDL_GetError());
            return;
        }
        g_streamChannels = channels;
        g_hostFrameBytes = int(apu::frame::kSamples * 4 * channels);
        SDL_ResumeAudioStreamDevice(g_stream);
        info("[apu] output: %s, device %d ch %d Hz, stream %d ch float",
             SDL_GetCurrentAudioDriver(), device.channels, device.freq, channels);
    }
}

namespace apu
{
    void configureAudio(bool device, const std::string& dumpPath)
    {
        g_wantDevice = device;
        g_dumpPath = dumpPath;
    }

    void playUnlockSound()
    {
        g_chimePos.store(0, std::memory_order_release);
        info("[apu] unlock sound");
    }

    void initAudio()
    {
        if (!loadUserUnlockSound((paths::user() + "/sounds/achievement.wav").c_str()))
            g_chime = sound::unlockChime(kRate);
        // Debug aid: BB_DEBUG_CHIME=<seconds> plays the unlock sound at that time (checks the mix
        // in an --audio-dump without earning an achievement)
        if (const char* at = std::getenv("BB_DEBUG_CHIME"))
        {
            double seconds = std::atof(at);
            std::thread([seconds] {
                std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
                apu::playUnlockSound();
            }).detach();
        }
        if (!g_dumpPath.empty())
        {
            g_dump = std::fopen(g_dumpPath.c_str(), "wb");
            if (!g_dump)
                warn("[apu] cannot write %s", g_dumpPath.c_str());
            else
            {
                writeWavHeader(g_dump, 0);
                info("[apu] dumping the output to %s (stereo, 32-bit float, %u Hz)", g_dumpPath.c_str(), kRate);
            }
        }
        if (g_wantDevice)
            openDevice();
        else
            info("[apu] no audio device (headless or --mute): frames are paced by a timer%s",
                 g_dump ? " and only dumped" : " and discarded");
    }

    void printAudioSummary()
    {
        {
            std::lock_guard guard(g_dumpLock);
            if (g_dump)
            {
                writeWavHeader(g_dump, g_dumpFrames);
                std::fflush(g_dump);
            }
        }
        double frames = double(std::max<uint64_t>(g_submitted, 1)) * frame::kSamples;
        std::lock_guard guard(g_statsLock);
        std::fprintf(stderr, "[apu] callbacks %llu, frames %llu (%llu audible, %.1f s), skipped %llu, extra %llu, "
                             "underruns %llu (%.1f ms starved)\n",
                     (unsigned long long)g_callbacks.load(), (unsigned long long)g_submitted.load(),
                     (unsigned long long)g_audible.load(), double(g_submitted.load()) * frame::kSamples / kRate,
                     (unsigned long long)g_skipped.load(), (unsigned long long)g_extra.load(),
                     (unsigned long long)g_underruns.load(),
                     g_streamChannels ? double(g_starvedBytes.load()) / (4.0 * g_streamChannels * kRate) * 1000 : 0.0);
        static const char* names[] = { "FL", "FR", "FC", "LFE", "BL", "BR" };
        std::fprintf(stderr, "[apu] per channel peak / rms dBFS:");
        for (uint32_t c = 0; c < frame::kChannels; c++)
        {
            double rms = std::sqrt(g_energy[c] / frames);
            std::fprintf(stderr, " %s %.3f / %.1f", names[c], g_peak[c], rms > 0 ? 20 * std::log10(rms) : -999.0);
        }
        std::fprintf(stderr, "\n");
    }
}

// u32 XAudioGetSpeakerConfig(u32* Config)
GUEST_IMPORT(XAudioGetSpeakerConfig)
{
    write32(ctx.r3.u32, 0x00010001); // stereo, digital
    ctx.r3.u64 = 0;
}

// u32 XAudioGetVoiceCategoryVolume(u32 Category, float* Volume)
GUEST_IMPORT(XAudioGetVoiceCategoryVolume)
{
    write32(ctx.r4.u32, 0x3F800000); // 1.0f
    ctx.r3.u64 = 0;
}

// u32 XAudioRegisterRenderDriverClient(u32* CallbackInfo /* {callback, arg} */, u32* Driver)
GUEST_IMPORT(XAudioRegisterRenderDriverClient)
{
    uint32_t info = ctx.r3.u32, pDriver = ctx.r4.u32;
    uint32_t callback = read32(info), arg = read32(info + 4);
    if (!callback) { ctx.r3.u64 = X_E_INVALIDARG; return; }
    std::lock_guard guard(g_lock);
    for (uint32_t i = 0; i < kMaxClients; i++)
    {
        if (g_clients[i].active) continue;
        uint32_t argPtr = kernelAlloc(4);
        write32(argPtr, arg);
        g_clients[i] = { callback, argPtr, true };
        write32(pDriver, 0x41550000 | i);
        info("[apu] render client %u: callback %08X arg %08X", i, callback, arg);
        startWorker();
        ctx.r3.u64 = 0;
        return;
    }
    ctx.r3.u64 = X_E_INVALIDARG;
}

// u32 XAudioUnregisterRenderDriverClient(u32 Driver)
GUEST_IMPORT(XAudioUnregisterRenderDriverClient)
{
    uint32_t i = ctx.r3.u32 & 0xFFFF;
    std::lock_guard guard(g_lock);
    if (i < kMaxClients) g_clients[i].active = false;
    ctx.r3.u64 = 0;
}

// u32 XAudioSubmitRenderDriverFrame(u32 Driver, float* Samples)
GUEST_IMPORT(XAudioSubmitRenderDriverFrame)
{
    const uint8_t* frame = (const uint8_t*)host(ctx.r4.u32);
    g_submitted++;
    accumulate(frame);
    float stereo[apu::frame::kSamples * 2];
    float six[apu::frame::kSamples * apu::frame::kChannels];
    bool needStereo = g_dump || g_streamChannels == 2, needSix = g_stream && g_streamChannels == 6;
    if (needStereo)
        apu::frame::toStereo(frame, stereo);
    if (needSix)
        apu::frame::toInterleaved6(frame, six);
    mixChime(needStereo ? stereo : nullptr, needSix ? six : nullptr);
    if (g_stream)
    {
        if (g_streamChannels == 2)
            SDL_PutAudioStreamData(g_stream, stereo, sizeof(stereo));
        else
            SDL_PutAudioStreamData(g_stream, six, sizeof(six));
    }
    if (g_dump)
        dumpFrame(stereo);
    ctx.r3.u64 = 0;
}
