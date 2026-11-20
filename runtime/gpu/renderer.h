#pragma once
// Vulkan renderer for the Xenos command stream (roadmap phase 4): executes the game's draws on
// the host GPU with translated shaders, keeps EDRAM render targets as Vulkan images, performs
// resolves, and hands the resolved front buffer to the presenter at each swap.
// All hooks are called on the command processor thread.
#include <cstdint>
#include <string>

namespace gpu::render
{
    void disable();                         // --no-render: command stream side effects only
    void requestScreenshot(uint64_t frame); // write screenshots/frame_<N>.png when swap N is rendered
    // --screenshot-at: write screenshots/t_<label>.png at the first swap at or after that many
    // seconds since launch (hid::secondsSinceLaunch, the --press clock)
    void requestScreenshotAt(double seconds, const std::string& label);
    // Internal resolution multiplier, 1-4 (2 = 2560x1440, 3 = 3840x2160 for the game's 1280x720)
    void setResolutionScale(uint32_t scale);
    // --anisotropy N (2-16): at least N:1 anisotropic filtering on textures the game minifies with
    // linear filtering and mips (the ground at a shallow angle stays sharp). Default: what the
    // texture fetch constant asks for (mostly none).
    void setAnisotropy(uint32_t n);
    // --no-shader-cache: compile every shader with shaderc instead of reusing compiled SPIR-V from
    // the disk cache ($XDG_CACHE_HOME/backbreaker-recomp/spirv)
    void setShaderCache(bool on);

    void onShaderLoad(uint32_t type, const uint32_t* bigEndianWords, uint32_t dwords, uint64_t hash); // hash: shader_hash.h
    // indexBase / indexSizeWord: DRAW_INDX DMA index buffer (physical address, VGT_DMA_SIZE)
    void onDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexSizeWord);
    void onSwap(uint32_t frontBuffer, uint32_t width, uint32_t height, uint64_t frameNumber);
    // Resolves are written to guest memory by the GPU, when the recorded commands run. Before the
    // command processor lets the CPU see its progress (a fence or other memory write, an
    // interrupt, the read pointer), this waits until the writebacks recorded so far have landed.
    void syncGuestMemory(const char* why = "renderer"); // `why`: for the BB_PERF line
    // The command processor let the guest see how far the GPU is (a fence or other memory write, a
    // scratch register, the read pointer, an interrupt): the guest may now rewrite what earlier
    // draws read, so copies made before are not reused (#121). Any thread.
    void guestSawProgress();
    void addCommandWaitMs(double ms);
    void submitBeforeWait();                // the CP is about to wait for the game: let the GPU start on what is recorded       // BB_PERF: time the CP spent polling in WAIT_REG_MEM

    // Asynchronous guest fences (#73). fenceSerial(): 0 if no GPU write is waiting to reach guest
    // memory (a fence can be written now); otherwise it submits what is recorded and returns the
    // submit the GPU must finish first. gpuDone(): copies to guest memory the writes of every
    // finished submit and returns the highest one finished. waitGpu(): blocks until that submit is
    // done. All three are for the command processor thread; asyncFences() is false without
    // timeline semaphores or with the renderer off, and then syncGuestMemory() is the only way.
    bool asyncFences();
    uint64_t fenceSerial();
    uint64_t gpuDone();
    void waitGpu(uint64_t serial);
    // BB_PERF (#174): a fence held back for the GPU was written, `heldMs` after it was queued;
    // `serial` is the submit it waited for, whose round trip is counted once
    void fenceWritten(uint64_t serial, double heldMs);
}
