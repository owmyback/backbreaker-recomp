#include "png.h"

#include <cstdio>
#include <vector>

namespace debug
{
    namespace
    {
        uint32_t crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu)
        {
            static uint32_t table[256];
            if (!table[1])
                for (uint32_t i = 0; i < 256; i++)
                {
                    uint32_t v = i;
                    for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
                    table[i] = v;
                }
            for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
            return c;
        }

        void put32(std::vector<uint8_t>& v, uint32_t x)
        {
            for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
        }

        void chunk(FILE* f, const char* type, const std::vector<uint8_t>& data)
        {
            std::vector<uint8_t> c;
            put32(c, uint32_t(data.size()));
            c.insert(c.end(), type, type + 4);
            c.insert(c.end(), data.begin(), data.end());
            put32(c, crc(c.data() + 4, c.size() - 4) ^ 0xFFFFFFFFu);
            fwrite(c.data(), 1, c.size(), f);
        }
    }

    bool writePng(const std::string& path, const uint8_t* rgba, uint32_t width, uint32_t height)
    {
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return false;
        static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        fwrite(signature, 1, 8, f);
        std::vector<uint8_t> ihdr;
        put32(ihdr, width);
        put32(ihdr, height);
        ihdr.insert(ihdr.end(), { 8, 2, 0, 0, 0 }); // 8-bit, RGB, deflate, no filter, no interlace
        chunk(f, "IHDR", ihdr);

        // Raw scanlines (filter byte 0 + RGB), stored in uncompressed deflate blocks
        std::vector<uint8_t> raw;
        raw.reserve(size_t(height) * (1 + width * 3));
        for (uint32_t y = 0; y < height; y++)
        {
            raw.push_back(0);
            const uint8_t* row = rgba + size_t(y) * width * 4;
            for (uint32_t x = 0; x < width; x++) raw.insert(raw.end(), { row[x * 4], row[x * 4 + 1], row[x * 4 + 2] });
        }
        std::vector<uint8_t> z = { 0x78, 0x01 };
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
        for (size_t pos = 0; pos < raw.size() || pos == 0;)
        {
            size_t n = std::min<size_t>(65535, raw.size() - pos);
            bool last = pos + n == raw.size();
            z.insert(z.end(), { uint8_t(last ? 1 : 0), uint8_t(n), uint8_t(n >> 8), uint8_t(~n), uint8_t(~n >> 8) });
            z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            pos += n;
            if (last) break;
        }
        put32(z, (b << 16) | a);
        chunk(f, "IDAT", z);
        chunk(f, "IEND", {});
        return fclose(f) == 0;
    }
}
