#pragma once
#include <ppc_config.h>
#include <ppc_context.h>
#include <cstdint>
#include <string>
#include <vector>

namespace kernel
{
    // Copy the XEX header into guest memory and create the module handle.
    void loadXexHeader(const std::string& path);
    uint32_t moduleHandle();
    // RtlImageXexHeaderField on the game module: value (key & 0xFF == 0) or guest pointer, 0 if absent
    uint32_t xexHeaderField(uint32_t key);

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
        // Ring of recent profiler scope names (guest string addresses), see scopes.cpp
        uint32_t scopes[16] = {};
        uint32_t scopePos = 0;
    };

    GuestThread* createGuestThread(PPCContext& ctx, uint32_t stackSize);
    void setCurrentThread(GuestThread* t);
    GuestThread* currentThread();
    std::vector<GuestThread*> allThreads();

    void fillVideoMode(uint32_t guestMode);

    void printTrace();
    void printTrace(uint32_t tid, size_t limit);
    // Most recent profiler scope names entered on the calling thread (from the game's markers)
    void printScopes(const GuestThread* t);
    void printSummary();
}
