#pragma once
// The geometry cache (#146): copies of the guest's vertices and decoded indices that last across
// frames, in a ring at the start of the renderer's upload buffer.
//
// Every draw used to copy its vertices and indices into the frame's upload buffer. Copies were
// reused within a frame only until the guest saw GPU progress (#121), which a match signals about
// 300 times a frame, and never across frames. On the PC's Exhibition route 70% of the vertex bytes
// copied, and nearly all of the index bytes, were the same as the last copy of the same range. On
// the Steam Deck that copying was a third of the command processor thread's work.
//
// A cached copy is good for as long as the guest memory it came from is unchanged, and the
// renderer knows that from the write watch (mem::watch, runtime/memory.cpp), as it does for
// textures: the range's pages are protected before they are read, and any write afterwards faults
// and stamps the page. A range is watched only once it has looked the same for a few frames in a
// row, because a write fault costs the game's thread about 90 us (#119) and some buffers are
// rewritten every frame.
//
// Header-only, no Vulkan, so tests/geometry_cache_test.cpp checks it without game files. For the
// renderer thread only.
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace gpu::geometry
{
    constexpr uint64_t kNone = UINT64_MAX;

    // A ring over `size` bytes of buffer, by virtual position: the bytes at position p live at
    // offset p % size, and the ring has written every position below written().
    //
    // Safety: a frame's draws read what they used until the GPU has run that frame. The renderer
    // keeps two frames in flight, and before it records frame F+2 it waits for frame F's fence
    // (submitAndAdvance), so between a use in frame F and the GPU finishing F the ring writes at
    // most what the rest of F and all of F+1 write: two frame budgets, size/4 together. A position
    // is used only while it is at most size/2 behind written() (usable); older data is copied
    // forward on its next use. So data in use is overwritten only after size/2 more bytes, never
    // within those two budgets.
    class Ring
    {
    public:
        explicit Ring(uint64_t size) : m_size(size) {}

        uint64_t size() const { return m_size; }
        uint64_t frameBudget() const { return m_size / 8; } // what one frame may write
        uint64_t maxEntry() const { return m_size / 16; }   // larger copies go to the frame's buffer

        // A position for `bytes` at a multiple of `align` (which divides size), or kNone: too big,
        // or this frame's budget is spent. An entry never wraps: when the rest of the ring is too
        // short, it starts again at offset 0, and the skipped tail counts as written.
        uint64_t alloc(uint64_t bytes, uint64_t align)
        {
            if (bytes == 0 || bytes > maxEntry()) return kNone;
            uint64_t pos = (m_written + align - 1) / align * align;
            const uint64_t off = pos % m_size;
            if (off + bytes > m_size) pos += m_size - off;
            if (pos + bytes - m_frameStart > frameBudget()) return kNone;
            m_written = pos + bytes;
            return pos;
        }

        uint64_t offset(uint64_t pos) const { return pos % m_size; }
        bool usable(uint64_t pos) const { return pos != kNone && pos <= m_written && m_written - pos <= m_size / 2; }

        // A new frame (its frame context restarted, after the wait for the frame before last)
        void beginFrame() { m_frameStart = m_written; }
        uint64_t written() const { return m_written; }

    private:
        uint64_t m_size;
        uint64_t m_written = 0;    // the next free position
        uint64_t m_frameStart = 0; // written() when this frame began
    };

    // What the renderer remembers about one guest range (vertices of one slot, or one draw's
    // indices), from frame to frame
    struct History
    {
        uint64_t fingerprint = 0;
        uint64_t lastSeen = 0;      // the frame it was last copied or used in
        uint32_t stableFrames = 0;  // frames in a row it was copied with the same fingerprint
        uint32_t backoffs = 0;      // times it was found written while watched
        uint64_t unwatchedUntil = 0;
        // The cached copy: ring position (kNone if none), the watch stamp taken before it was read,
        // and the renderer's GPU write serial then
        uint64_t pos = kNone;
        uint64_t watchStamp = 0;
        uint64_t gpuWrites = 0;
    };

    constexpr uint32_t kStableFrames = 3;      // copied the same this many frames in a row: watch it
    constexpr uint64_t kMinBytes = 1024;       // smaller copies cost less than watching them
    constexpr uint64_t kBackoffFrames = 300;   // found written: not watched for 10 s, then 20, 40, 80
    constexpr uint32_t kMaxBackoffShift = 3;
    constexpr uint64_t kForgetFrames = 30;     // not seen for this long (and not backing off): dropped
    constexpr uint64_t kSeenGap = 2;           // "in a row" allows a frame without it (alternate passes)

    // A copy of the range was made in frame `frame` and its bytes have this fingerprint. Once per
    // frame: later copies in the same frame are the same bytes.
    inline void observe(History& h, uint64_t fingerprint, uint64_t frame)
    {
        if (h.lastSeen == frame && h.stableFrames) return;
        const bool consecutive = h.stableFrames && frame > h.lastSeen && frame - h.lastSeen <= kSeenGap;
        h.stableFrames = consecutive && fingerprint == h.fingerprint ? h.stableFrames + 1 : 1;
        h.fingerprint = fingerprint;
        h.lastSeen = frame;
    }

    // May the range be watched and cached now?
    inline bool mayWatch(const History& h, uint64_t frame, uint64_t bytes)
    {
        return h.stableFrames >= kStableFrames && frame >= h.unwatchedUntil && bytes >= kMinBytes;
    }

    // The cached copy's memory was written (by the guest, a file read, or a GPU result): drop the
    // copy and leave the range unwatched for a while, longer each time
    inline void foundWritten(History& h, uint64_t frame)
    {
        h.pos = kNone;
        h.stableFrames = 0;
        h.unwatchedUntil = frame + (kBackoffFrames << std::min(h.backoffs, kMaxBackoffShift));
        h.backoffs++;
    }

    // Drop the ranges not seen for kForgetFrames: a dynamic buffer's addresses change from frame to
    // frame, and would pile up. Once that long unseen a range is no longer "in a row" anyway, and its
    // cached copy (if any) has aged out of use; one still backing off is kept, so it isn't watched
    // again early.
    template <class Map> void prune(Map& map, uint64_t frame)
    {
        for (auto it = map.begin(); it != map.end();)
            if (frame - it->second.lastSeen > kForgetFrames && frame >= it->second.unwatchedUntil) it = map.erase(it);
            else ++it;
    }

    // A sample of the range's bytes: 64 at the start of every 4 KB, and the last 64. Read right after
    // the copy, so it comes from the CPU's cache. It only decides when a range is worth watching;
    // whether a cached copy is still good is the write watch's call.
    inline uint64_t fingerprint(const uint8_t* p, uint64_t bytes)
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ bytes;
        auto mix = [&](const uint8_t* q, uint64_t n) {
            uint64_t i = 0;
            for (; i + 8 <= n; i += 8)
            {
                uint64_t w;
                std::memcpy(&w, q + i, 8);
                h = (h ^ w) * 0xFF51AFD7ED558CCDull;
                h ^= h >> 29;
            }
            for (; i < n; i++) h = (h ^ q[i]) * 0x100000001B3ull;
        };
        for (uint64_t at = 0; at < bytes; at += 4096) mix(p + at, std::min<uint64_t>(64, bytes - at));
        if (bytes > 64) mix(p + bytes - 64, 64);
        return h;
    }
}
