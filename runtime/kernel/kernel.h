#pragma once
#include <ppc_config.h>
#include <ppc_context.h>
#include <atomic>
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

    // Map guest devices (game:, d:, cache:) to host directories. Saves (XAM content packages)
    // live under savesRoot.
    void initFileSystem(const std::string& discRoot, const std::string& cacheRoot, const std::string& savesRoot);
    // Achievements (xam_achievements.cpp): reads the title's list from the loaded image and the
    // unlocks earned so far from dir (game/achievements). After the image and XEX header load.
    void initAchievements(const std::string& dir);
    void printAchievements(); // --achievements

    // For the in-window achievements overlay (runtime/video/overlay.cpp, #101)
    struct AchievementView
    {
        uint16_t id = 0, gamerscore = 0;
        std::string label, description, unachieved; // UTF-8
        bool unlocked = false, secret = false;      // secret: description hidden while locked
        uint64_t unlockTime = 0;                    // unix seconds
        const uint8_t* image = nullptr;             // PNG in the SPA (the loaded image), or null
        uint32_t imageSize = 0;
    };
    std::vector<AchievementView> achievementList();
    uint64_t achievementUnlockCount();                 // new unlocks so far this run (cheap: an atomic)
    std::vector<uint16_t> unlockedSince(uint64_t index); // ids of this run's unlocks from that index on, in order
    const std::string& savesRoot();
    // Runtime mounts, used by XAM content packages: device path prefix such as
    // "\Device\Content\1" -> host directory, and symbolic links such as "save:" -> device path.
    void addMount(const std::string& devicePrefix, const std::string& hostRoot, bool writable);
    void removeMount(const std::string& devicePrefix);
    void addSymbolicLink(const std::string& name, const std::string& target);
    void removeSymbolicLink(const std::string& name);

    // Complete an XOVERLAPPED (sets result/length/extended error, signals its event); returns
    // ERROR_IO_PENDING, or `result` itself when there is no overlapped.
    uint32_t completeOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

    // Patch variable and function-pointer import slots in the loaded image.
    void patchImports();

    // Windows FILETIME: 100 ns intervals since 1601-01-01. Used by KeQuerySystemTime, the
    // KeTimeStampBundle updater and absolute-timeout waits, so they all agree on "now".
    uint64_t systemTime();

    // KeTimeStampBundle: guest address of the bundle (0 when the game does not import it) and the
    // host thread that keeps it current. Call startTimeStampThread() after patchImports().
    uint32_t timeStampBundleAddress();
    void startTimeStampThread();

    // Guest-visible thread state (PCR + KTHREAD + stack) for the calling host thread.
    struct GuestThread
    {
        uint32_t pcr;
        uint32_t kthread;
        uint32_t stackBase;  // highest address
        uint32_t stackLimit; // lowest address
        uint32_t stackSize;  // 0 once the stack has been returned to the heap
        uint32_t tlsSlots;   // guest array of 64 u32 slots
        uint32_t id;
        // Ring of recent profiler scope names (guest string addresses), see scopes.cpp
        uint32_t scopes[16] = {};
        uint32_t scopePos = 0;
        // Host thread running it (for the thread sampler); valid from setCurrentThread until exit
        unsigned long hostThread = 0; // pthread_t
        std::atomic<bool> hostRunning{false};
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
