#pragma once
// Kernel dispatcher objects (events, semaphores, threads) shared by the Nt* (handle)
// and Ke* (guest pointer) imports.
#include "kernel.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace kernel
{
    struct Object
    {
        enum class Type { Event, Semaphore, Thread, Timer, Other };
        Type type;
        uint32_t guest = 0; // guest DISPATCHER_HEADER address
        explicit Object(Type t) : type(t) {}
        virtual ~Object() = default;
    };

    struct Event : Object
    {
        bool manualReset;
        bool signaled;
        Event(bool manual, bool initial) : Object(Type::Event), manualReset(manual), signaled(initial) {}
    };

    struct Semaphore : Object
    {
        int32_t count, maximum;
        Semaphore(int32_t c, int32_t m) : Object(Type::Semaphore), count(c), maximum(m) {}
    };

    struct Thread : Object
    {
        GuestThread* guestThread = nullptr;
        PPCContext* ctx = nullptr;
        bool exited = false;
        uint32_t exitCode = 0;
        int suspendCount = 0;
        Thread() : Object(Type::Thread) {}
    };

    // All dispatcher state is protected by one lock; any state change notifies all waiters.
    extern std::mutex g_dispatchLock;
    extern std::condition_variable g_dispatchCv;

    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFF;

    // Registration (call with g_dispatchLock NOT held)
    uint32_t registerObject(Object* o); // returns a new handle
    uint32_t duplicateHandle(uint32_t handle);
    void closeHandle(uint32_t handle);
    Object* objectFromHandle(uint32_t handle);
    // Look up (or lazily create from its header) the object at a guest address.
    Object* objectFromGuest(uint32_t guest);

    Thread* currentThreadObject();
    Thread* adoptCurrentThread(GuestThread* gt, PPCContext* ctx); // main thread
    void adoptExistingThread(Thread* t);                        // threads from ExCreateThread

    // Returns STATUS_WAIT_0 + index, or STATUS_TIMEOUT.
    using Deadline = std::optional<std::chrono::steady_clock::time_point>;
    uint32_t waitForObjects(const std::vector<Object*>& objects, bool waitAll, Deadline deadline);
    // Guest timeout pointer (100 ns units; negative = relative, positive = absolute, null = infinite)
    Deadline deadlineFromGuest(uint32_t pTimeout);

    void setEvent(Event* e);
    void signalObjectsChanged(); // notify after modifying state under g_dispatchLock
}
