#pragma once
// A shader's identity: FNV-1a over the bytes of its microcode (big-endian dwords as in guest
// memory), the name captures, BB_DEBUG_PS, the shader cache and the tools use.
//
// The game loads the same microcode many times a frame, and byte-wise FNV over it was ~12% of the
// command processor thread in a match, computed twice per load (renderer and capture). shaderHash()
// computes it once per distinct microcode: earlier answers are found by length and first/last
// dwords and confirmed by comparing every dword, so the value is always exactly FNV's (#73).
// Command processor thread only.
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gpu
{
    inline uint64_t fnv1aWords(const uint32_t* w, uint32_t n)
    {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t i = 0; i < n; i++)
            for (int b = 0; b < 4; b++) h = (h ^ ((w[i] >> (b * 8)) & 0xFF)) * 1099511628211ull;
        return h;
    }

    inline uint64_t shaderHash(const uint32_t* w, uint32_t n)
    {
        uint64_t key = n * 0x9E3779B97F4A7C15ull;
        for (uint32_t i = 0; i < 4 && i < n; i++) key = (key ^ w[i] ^ (uint64_t(w[n - 1 - i]) << 32)) * 0x100000001B3ull;
        struct Known { std::vector<uint32_t> words; uint64_t fnv; };
        static std::unordered_multimap<uint64_t, Known> known;
        auto [first, last] = known.equal_range(key);
        for (auto it = first; it != last; ++it)
            if (it->second.words.size() == n && !std::memcmp(it->second.words.data(), w, size_t(n) * 4)) return it->second.fnv;
        uint64_t h = fnv1aWords(w, n);
        known.emplace(key, Known{ std::vector<uint32_t>(w, w + n), h });
        return h;
    }
}
