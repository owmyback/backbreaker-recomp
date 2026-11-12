#pragma once
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdint>
#include <string>

namespace kernel
{
    // Copy the XEX header into guest memory and create the module handle.
    void loadXexHeader(const std::string& path);
    uint32_t moduleHandle();

    // Map guest devices (game:, d:, cache:) to host directories.
    void initFileSystem(const std::string& discRoot, const std::string& cacheRoot);

    // Patch variable and function-pointer import slots in the loaded image.
    void patchImports();

    // Guest-visible thread state (PCR + KTHREAD + stack) for the calling host thread.
    struct GuestThread
    {
        uint32_t pcr;
        uint32_t kthread;
        uint32_t stackBase;  // highest address
        uint32_t stackLimit; // lowest address
        uint32_t tlsSlots;   // guest array of 64 u32 slots
        uint32_t id;
    };

    GuestThread* createGuestThread(PPCContext& ctx, uint32_t stackSize);
    void setCurrentThread(GuestThread* t);
    GuestThread* currentThread();

    void printTrace();
    void printSummary();
}
