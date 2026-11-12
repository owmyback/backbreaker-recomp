// Audio (XAudio render driver + XMA contexts), headless: the render callback is driven at the
// hardware rate (256 samples @ 48 kHz) and submitted frames are discarded.
// Signatures follow Xenia's xboxkrnl_audio.cc / xboxkrnl_audio_xma.cc.
#include "../kernel/import_support.h"
#include "../kernel/objects.h"
#include "../log.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

using namespace mem;

namespace
{
    constexpr uint32_t kMaxClients = 8;
    constexpr uint32_t X_E_INVALIDARG = 0x80070057;

    // The callback receives a pointer to a dword holding its argument (as on the console)
    struct Client { uint32_t callback = 0, argPtr = 0; bool active = false; };
    std::mutex g_lock;
    Client g_clients[kMaxClients];
    std::atomic<uint64_t> g_submitted{0};
    std::atomic<bool> g_started{false};

    kernel::GuestThread* g_audioThread = nullptr;
    kernel::Thread* g_audioThreadObj = nullptr;
    PPCContext* g_audioCtx = nullptr;

    void audioWorker()
    {
        kernel::setCurrentThread(g_audioThread);
        kernel::adoptExistingThread(g_audioThreadObj);
        using clock = std::chrono::steady_clock;
        auto period = std::chrono::nanoseconds(1000000000ll * 256 / 48000);
        auto next = clock::now();
        for (;;)
        {
            next += period;
            std::this_thread::sleep_until(next);
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
    g_submitted++;
    ctx.r3.u64 = 0;
}

