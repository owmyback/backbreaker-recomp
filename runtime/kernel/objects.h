#pragma once
// Kernel dispatcher objects (events, semaphores, threads) shared by the Nt* (handle)
// and Ke* (guest pointer) imports. NetDll sockets (xnet.cpp) are registered here too, so they get
// handles from the same table; they carry no dispatcher state and are never signaled.
#include "kernel.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace kernel
{
    // A thread blocked in waitForObjects, on the waiter list of every object it waits for: a change
    // to one of those wakes it, and only those (signalObject)
    struct Waiter
    {
        std::condition_variable cv;
        bool woken = false;
    };

    struct Object
    {
        enum class Type { Event, Semaphore, Thread, Timer, Socket, Other };
        Type type;
        uint32_t guest = 0; // guest DISPATCHER_HEADER address
        std::vector<Waiter*> waiters; // threads waiting for it (under g_dispatchLock)
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

    // All dispatcher state is protected by one lock. A change to an object wakes the threads waiting
    // for that object (signalObject). It used to wake every waiting thread through one condition
    // variable: with the game's job threads signalling thousands of events a second at a new scene,
    // that was a storm of wake-ups, each through the kernel, and on Windows the 100-250 ms frames
    // of #174. g_dispatchCv is left for a thread created suspended, waiting for its resume.
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
    // After making `o` signaled (or more so) under g_dispatchLock: wakes the threads waiting for it
    void signalObject(Object* o);
    // After a resume under g_dispatchLock: wakes threads waiting on g_dispatchCv to start
    void signalObjectsChanged();
}
