// Checks runtime/gpu/geometry_cache.h (issue #146): the ring the renderer keeps vertex and index
// copies in across frames, and when a range gets watched. No game files, no generated headers:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/geometry_cache_test.cpp -o build/geometry_cache_test
//
// The important check is the simulation at the end: frames with two in flight, as the renderer
// runs them, and no byte that a frame the GPU hasn't finished reads is ever written again.
#include "../runtime/gpu/geometry_cache.h"

#include <cstdio>
#include <random>
#include <unordered_map>
#include <vector>

using namespace gpu::geometry;

static int failures = 0, checks = 0;

static void check(bool ok, const char* what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what);
    }
}

int main()
{
    constexpr uint64_t S = 1 << 20; // a 1 MB ring: budget 128 KB a frame, entries up to 64 KB
    {
        Ring r(S);
        check(r.frameBudget() == S / 8 && r.maxEntry() == S / 16, "budget and largest entry");
        check(r.alloc(0, 16) == kNone, "nothing to allocate");
        check(r.alloc(S / 16 + 1, 16) == kNone, "too big for the ring");
        uint64_t a = r.alloc(100, 16), b = r.alloc(10, 16);
        check(a == 0 && b == 112, "aligned one after the other");
        check(r.written() == 122, "written up to the last byte");
    }
    {
        // The frame budget: past it the caller copies into the frame's own buffer
        Ring r(S);
        uint64_t used = 0;
        while (r.alloc(S / 32, 16) != kNone) used += S / 32;
        check(used == S / 8, "a frame writes its budget, then no more");
        r.beginFrame();
        check(r.alloc(S / 32, 16) != kNone, "a new frame has a new budget");
    }
    {
        // Wrapping: an entry that doesn't fit before the end starts over at offset 0
        Ring r(S);
        for (int i = 0; i < 15; i++, r.beginFrame()) r.alloc(S / 16, 16); // 15/16 of the ring
        uint64_t p = r.alloc(S / 16 - 1008, 16);                          // 1008 bytes left
        check(p == 15 * (S / 16) && r.written() == S - 1008, "fits up to near the end");
        r.beginFrame();
        uint64_t w = r.alloc(4096, 16);
        check(w == S && r.offset(w) == 0, "starts over at 0 when the tail is short");
        check(r.written() == S + 4096, "the skipped tail counts as written");
    }
    {
        // Usable: at most half the ring behind
        Ring r(S);
        uint64_t p = r.alloc(4096, 16);
        check(r.usable(p), "new data is usable");
        uint64_t written = 4096;
        while (written < S / 2)
        {
            r.beginFrame();
            while (written < S / 2 && r.alloc(4096, 16) != kNone) written += 4096;
        }
        check(r.usable(p), "half a ring behind: still usable");
        r.beginFrame();
        r.alloc(16, 16);
        check(!r.usable(p), "more than half behind: copy it forward");
        check(!r.usable(kNone), "no copy is not usable");
    }
    {
        // History: watched after kStableFrames frames that look the same, not before
        History h;
        observe(h, 42, 10);
        check(!mayWatch(h, 10, 4096), "seen once: not watched");
        observe(h, 42, 10); // again in the same frame: no change
        observe(h, 42, 11);
        check(h.stableFrames == 2 && !mayWatch(h, 11, 4096), "seen twice");
        observe(h, 42, 13); // a frame skipped (alternate passes) still counts as in a row
        check(h.stableFrames == 3 && mayWatch(h, 13, 4096), "three frames alike: watch it");
        check(!mayWatch(h, 13, kMinBytes - 1), "too small to be worth watching");
        observe(h, 43, 14);
        check(h.stableFrames == 1, "different bytes start over");
        observe(h, 43, 15);
        observe(h, 43, 19);
        check(h.stableFrames == 1, "a long gap starts over");
    }
    {
        // Backoff: found written while watched, unwatched for 300 frames, then 600, 1200, 2400 at most
        History h;
        for (uint64_t f = 1; f <= 3; f++) observe(h, 7, f);
        h.pos = 0;
        foundWritten(h, 100);
        check(h.pos == kNone && h.stableFrames == 0 && h.unwatchedUntil == 400, "first write: 300 frames");
        for (uint64_t f = 400; f < 403; f++) observe(h, 7, f);
        check(mayWatch(h, 402, 4096), "watched again after the backoff");
        foundWritten(h, 402);
        check(h.unwatchedUntil == 402 + 600, "second write: 600 frames");
        foundWritten(h, 2000);
        foundWritten(h, 3000);
        foundWritten(h, 9000);
        check(h.unwatchedUntil == 9000 + 2400, "capped at 2400 frames");
    }
    {
        // Pruning drops ranges not seen for kForgetFrames, unless they are backing off
        std::unordered_map<uint64_t, History> m;
        observe(m[1], 1, 10);
        observe(m[2], 2, 100);
        observe(m[3], 3, 10);
        foundWritten(m[3], 10); // unwatched until 310
        prune(m, 100);
        check(m.size() == 2 && m.count(2) && m.count(3), "an old range is dropped; a recent one and one backing off are kept");
        prune(m, 400);
        check(m.empty(), "once its backoff is over it goes too");
    }
    {
        // Fingerprint: a change in a sampled spot shows, the size counts
        std::vector<uint8_t> a(10000, 1);
        uint64_t f0 = fingerprint(a.data(), a.size());
        a[4096 + 10] = 2;
        check(fingerprint(a.data(), a.size()) != f0, "a byte at a page start changes it");
        a[4096 + 10] = 1;
        a[a.size() - 1] = 3;
        check(fingerprint(a.data(), a.size()) != f0, "the last byte changes it");
        a[a.size() - 1] = 1;
        check(fingerprint(a.data(), a.size()) == f0 && fingerprint(a.data(), 9000) != f0, "same bytes, same print; size counts");
    }
    {
        // The simulation: frames recorded with two in flight. Each frame uses ranges as the renderer
        // does: a hit while usable(), otherwise a fresh copy (when the budget allows). Frame F's
        // reads are done once frame F+2 begins (submitAndAdvance waits for F's fence first). Every
        // allocation must not overlap a byte read by a frame that isn't done.
        std::mt19937_64 rng(1);
        Ring r(S);
        struct Use { uint64_t off, bytes; };
        std::vector<std::vector<Use>> reads(2); // the two frames that may still be running
        std::vector<uint64_t> pos(400, kNone), size(400);
        for (auto& s : size) s = 16 + (rng() % (S / 16 - 16));
        bool overlap = false;
        uint64_t hits = 0, copies = 0, refused = 0;
        for (int frame = 0; frame < 3000; frame++)
        {
            // Frame `frame` begins: the one before last is done
            r.beginFrame();
            reads[frame % 2].clear();
            int draws = 20 + int(rng() % 60);
            for (int d = 0; d < draws; d++)
            {
                size_t k = rng() % (frame % 50 < 25 ? 40 : pos.size()); // a hot set, then everything
                if (!r.usable(pos[k]))
                {
                    uint64_t p = r.alloc(size[k], 16);
                    if (p == kNone) { refused++; pos[k] = kNone; continue; } // the frame's buffer instead
                    uint64_t off = r.offset(p);
                    for (const auto& frameReads : reads)
                        for (const Use& u : frameReads)
                            if (off < u.off + u.bytes && u.off < off + size[k]) overlap = true;
                    pos[k] = p;
                    copies++;
                }
                else
                    hits++;
                reads[frame % 2].push_back({ r.offset(pos[k]), size[k] });
            }
        }
        check(!overlap, "no byte a running frame reads is written again");
        check(hits > copies && refused > 0, "the simulation hits, copies and runs out of budget");
        printf("simulation: %llu hits, %llu copies, %llu over budget\n", (unsigned long long)hits, (unsigned long long)copies,
               (unsigned long long)refused);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
