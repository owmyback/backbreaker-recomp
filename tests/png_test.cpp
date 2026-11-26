// Tests runtime/debug/png.cpp (issue #76): every PNG the runtime writes has to read back as PNG.
//
// What is checked, from the file on disk and not from anything the writer kept in memory:
//   - the 8-byte signature
//   - every chunk's length and CRC-32, computed here bit by bit (png.cpp builds a table; using it
//     would only prove the writer agrees with itself)
//   - the IHDR fields, and the chunk order PNG requires (IHDR first, IDAT one or more times, IEND
//     last with no data, nothing after it)
//   - the IDAT zlib header: method, window size, no preset dictionary, correct header check bits
//   - the stream's Adler-32 trailer, computed here over the scanlines
//   - the pixels: inflate IDAT and compare each row against the input, with alpha dropped
//
// The inflating is zlib's (linked in the test only), so the test does not care how IDAT is
// encoded - stored blocks today, real deflate in the faster rewrite on the capture-images branch -
// as long as it is a valid zlib stream. Un-filtering is written here anyway, for all five filter
// types, so a writer that filters its rows still round-trips.
//
// No game files. Build and run: docs/testing.md / .github/workflows/ci.yml
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/png_test.cpp runtime/debug/png.cpp -lz -o build/png_test

#include "../runtime/debug/png.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

static int failures = 0, checks = 0;

static void ok(bool good, const char* what)
{
    checks++;
    if (!good) failures++, printf("FAIL %s\n", what);
}

// ---- CRC-32 and Adler-32, worked out independently of png.cpp -----------------------------
// PNG's CRC is the reflected CRC-32 (polynomial 0xEDB88320, initial 0xFFFFFFFF, final xor).
// Done bit by bit with no table, so it shares no code with the writer's table-driven version.
static uint32_t crc32Ref(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
    {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    return c ^ 0xFFFFFFFFu;
}

// Adler-32, as RFC 1950 defines it: the checksum zlib stores at the end of the stream.
// Reduced every 5552 bytes rather than at the end: a and b are 32-bit, and over a whole image's
// scanlines b would wrap before a final modulo (which is exactly what zlib's own limit does).
static uint32_t adler32Ref(const std::vector<uint8_t>& d)
{
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < d.size(); pos += 5552)
    {
        size_t n = std::min<size_t>(5552, d.size() - pos);
        for (size_t i = 0; i < n; i++) { a += d[pos + i]; b += a; }
        a %= 65521;
        b %= 65521;
    }
    return ((b % 65521) << 16) | (a % 65521);
}

// ---- reading a PNG back ---------------------------------------------------------------------
struct Chunk
{
    std::string type;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> readFile(const std::filesystem::path& p)
{
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return {};
    std::vector<uint8_t> all;
    uint8_t buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f))) all.insert(all.end(), buf, buf + n);
    fclose(f);
    return all;
}

static uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Split a PNG file into chunks, checking the framing and every CRC on the way.
static std::vector<Chunk> readChunks(const std::vector<uint8_t>& file, const char* what)
{
    std::vector<Chunk> out;
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    ok(file.size() >= 8 + 12 + 12 && std::equal(sig, sig + 8, file.begin()), (std::string(what) + ": PNG signature").c_str());
    if (file.size() < 20) return out;
    size_t pos = 8;
    while (pos + 8 <= file.size())
    {
        uint32_t len = be32(&file[pos]);
        std::string type((const char*)&file[pos + 4], 4);
        if (pos + 12 + len > file.size())
        {
            failures++, checks++;
            printf("FAIL %s: chunk %s claims %u bytes, file ends at %zu of %zu\n", what, type.c_str(), len, pos + 12, file.size());
            return out;
        }
        std::vector<uint8_t> data(file.begin() + pos + 8, file.begin() + pos + 8 + len);
        uint32_t want = be32(&file[pos + 8 + len]);
        checks++;
        if (crc32Ref(&file[pos + 4], size_t(4) + len) != want)
            failures++, printf("FAIL %s: chunk %s CRC is %08X, computed %08X\n", what, type.c_str(), want,
                               crc32Ref(&file[pos + 4], size_t(4) + len));
        out.push_back({type, data});
        pos += 12 + len;
    }
    ok(pos == file.size(), (std::string(what) + ": no trailing bytes after the last chunk").c_str());
    return out;
}

// The five PNG row filters, applied to one row (bpp = 3 for the 8-bit RGB this writer makes).
// `above` is the previous row already unfiltered (zeros for the first row), as the format defines.
static void unfilterRow(uint8_t filter, const uint8_t* above, uint8_t* dst, size_t rowBytes)
{
    for (size_t i = 0; i < rowBytes; i++)
    {
        uint8_t left = i >= 3 ? dst[i - 3] : 0;
        uint8_t up = above[i];
        uint8_t upleft = i >= 3 ? above[i - 3] : 0;
        int v;
        switch (filter)
        {
        case 0: v = dst[i]; break;                    // dst starts as the raw byte
        case 1: v = dst[i] + left; break;             // Sub
        case 2: v = dst[i] + up; break;               // Up
        case 3: v = dst[i] + ((left + up) >> 1); break; // Average
        default:                                      // 4: Paeth
            int pp = left + up - upleft, pa = abs(pp - left), pb = abs(pp - up), pc = abs(pp - upleft);
            v = dst[i] + (pa <= pb && pa <= pc ? left : pb <= pc ? up : upleft);
            break;
        }
        dst[i] = uint8_t(v);
    }
}

// Write an image, read it back and check everything. `rgba` is what went in.
static void roundTrip(const std::filesystem::path& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, const char* what)
{
    ok(debug::writePng(path.string(), rgba.data(), w, h), (std::string(what) + ": writePng").c_str());
    std::vector<uint8_t> file = readFile(path);
    ok(!file.empty(), (std::string(what) + ": file readable").c_str());
    if (file.empty()) return;

    std::vector<Chunk> chunks = readChunks(file, what);
    if (chunks.size() < 3)
    {
        failures++, checks++;
        printf("FAIL %s: %zu chunks, expected IHDR + IDAT + IEND\n", what, chunks.size());
        return;
    }
    checks++;
    if (chunks[0].type != "IHDR") { failures++, printf("FAIL %s: first chunk is %s, not IHDR\n", what, chunks[0].type.c_str()); return; }
    ok(chunks[0].data.size() == 13, (std::string(what) + ": IHDR is 13 bytes").c_str());
    if (chunks[0].data.size() != 13) return;
    const uint8_t* ihdr = chunks[0].data.data();
    ok(be32(ihdr) == w, (std::string(what) + ": IHDR width").c_str());
    ok(be32(ihdr + 4) == h, (std::string(what) + ": IHDR height").c_str());
    ok(ihdr[8] == 8, (std::string(what) + ": IHDR bit depth 8").c_str());
    ok(ihdr[9] == 2, (std::string(what) + ": IHDR color type 2 (RGB)").c_str());
    ok(ihdr[10] == 0 && ihdr[11] == 0 && ihdr[12] == 0, (std::string(what) + ": IHDR deflate / no filter / no interlace").c_str());

    // IDAT may be one chunk (as now) or several; IEND must be last and empty; no ancillary chunks.
    std::vector<uint8_t> zstream;
    bool sawIdat = false;
    for (size_t i = 1; i < chunks.size(); i++)
    {
        const std::string& t = chunks[i].type;
        if (t == "IDAT") { sawIdat = true; zstream.insert(zstream.end(), chunks[i].data.begin(), chunks[i].data.end()); }
        else if (i + 1 == chunks.size() && t == "IEND") ok(chunks[i].data.empty(), (std::string(what) + ": IEND has no data").c_str());
        else failures++, printf("FAIL %s: unexpected chunk %s at %zu\n", what, t.c_str(), i);
    }
    ok(sawIdat, (std::string(what) + ": has an IDAT").c_str());
    ok(chunks.back().type == "IEND", (std::string(what) + ": IEND is last").c_str());
    if (!sawIdat) return;

    // zlib header (RFC 1950): deflate method, window no larger than PNG allows (CINFO <= 7),
    // no preset dictionary, and the header's own check bits.
    ok(zstream.size() >= 6, (std::string(what) + ": IDAT holds a zlib stream").c_str());
    if (zstream.size() < 6) return;
    uint16_t zhdr = (uint16_t(zstream[0]) << 8) | zstream[1];
    ok((zstream[0] & 0xF) == 8, (std::string(what) + ": zlib method 8 (deflate)").c_str());
    ok((zstream[0] >> 4) <= 7, (std::string(what) + ": zlib window <= 32 KB").c_str());
    ok(!((zhdr >> 5) & 1), (std::string(what) + ": no preset dictionary").c_str());
    ok(zhdr % 31 == 0, (std::string(what) + ": zlib header check bits").c_str());

    // Inflate. The raw scanlines are exactly (1 filter byte + 3 bytes per pixel) per row.
    size_t rawSize = size_t(h) * (1 + size_t(w) * 3);
    std::vector<uint8_t> raw(rawSize);
    uLongf outLen = uLongf(rawSize);
    int z = uncompress(raw.data(), &outLen, zstream.data(), uLong(zstream.size()));
    checks++;
    if (z != Z_OK) failures++, printf("FAIL %s: uncompress returned %d (%s)\n", what, z, z == Z_DATA_ERROR ? "bad stream" : z == Z_BUF_ERROR ? "buffer too small" : "other");
    if (z != Z_OK) return;
    ok(outLen == rawSize, (std::string(what) + ": IDAT inflates to exactly the scanlines").c_str());
    if (outLen != rawSize) return;

    // The trailer is zlib's Adler-32 of the uncompressed data.
    uint32_t trailer = be32(&zstream[zstream.size() - 4]);
    checks++;
    if (adler32Ref(raw) != trailer)
        failures++, printf("FAIL %s: Adler-32 is %08X, computed %08X\n", what, trailer, adler32Ref(raw));

    // Rows: filter byte then the row. Un-filter, then compare with what went in (alpha dropped).
    std::vector<uint8_t> prev(size_t(w) * 3, 0), row(size_t(w) * 3, 0);
    int filtersSeen = 0;
    for (uint32_t y = 0; y < h; y++)
    {
        const uint8_t* src = raw.data() + size_t(y) * (1 + size_t(w) * 3);
        uint8_t filter = src[0];
        if (filter > 4) { failures++, checks++, printf("FAIL %s: row %u filter type %u\n", what, y, filter); return; }
        filtersSeen |= 1 << filter;
        std::copy(src + 1, src + 1 + size_t(w) * 3, row.begin());
        unfilterRow(filter, prev.data(), row.data(), size_t(w) * 3);
        // Report the first wrong pixel and stop: everything after it in a shifted or corrupted
        // stream is wrong for the same reason, and one line says so.
        bool mismatch = false;
        for (uint32_t x = 0; x < w && !mismatch; x++)
            for (int c = 0; c < 3 && !mismatch; c++)
            {
                checks++;
                if (row[size_t(x) * 3 + c] != rgba[(size_t(y) * w + x) * 4 + c])
                {
                    failures++;
                    printf("FAIL %s: pixel %u,%u channel %d is %u, expected %u\n", what, x, y, c,
                           row[size_t(x) * 3 + c], rgba[(size_t(y) * w + x) * 4 + c]);
                    mismatch = true;
                }
            }
        if (mismatch) return;
        prev = row;
    }
    if (getenv("PNG_TEST_VERBOSE"))
        printf("  %s: %ux%u, %zu byte file, %zu IDAT byte(s), %zu scanline byte(s), filter(s) 0x%X\n",
               what, w, h, file.size(), zstream.size(), rawSize, filtersSeen);
}

// A pattern that varies in every channel and from pixel to pixel, so a row that got copied,
// skipped or shifted shows up as a mismatch.
static std::vector<uint8_t> pattern(uint32_t w, uint32_t h, uint8_t alphaBase)
{
    std::vector<uint8_t> rgba(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
        {
            uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 7 + y * 13);
            p[1] = uint8_t(x * x * 3 + y * 11 + 5);
            p[2] = uint8_t(x * 5 + y * y * 17 + 11);
            p[3] = uint8_t((x + y) * 29 + alphaBase); // dropped by the writer
        }
    return rgba;
}

int main()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / ("backbreaker-png-test-" + std::to_string(getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);

    struct { uint32_t w, h; const char* what; } sizes[] = {
        {1, 1, "1x1"},
        {64, 2, "64x2"},
        {428, 51, "428x51 (scanlines are exactly one 65535 byte stored block)"},
        {429, 51, "429x51 (one byte over the block limit: two stored blocks)"},
        {301, 219, "301x219 (odd size, several stored blocks)"},
    };
    for (auto& s : sizes)
        roundTrip(dir / (std::to_string(s.w) + "x" + std::to_string(s.h) + ".png"), pattern(s.w, s.h, 0), s.w, s.h, s.what);

    // Alpha is dropped: same RGB, different alpha, byte for byte the same file.
    {
        auto a = pattern(9, 7, 0), b = pattern(9, 7, 0x5F);
        for (size_t i = 3; i < a.size(); i += 4) b[i] = uint8_t(~a[i]);
        debug::writePng((dir / "alpha-a.png").string(), a.data(), 9, 7);
        debug::writePng((dir / "alpha-b.png").string(), b.data(), 9, 7);
        ok(readFile(dir / "alpha-a.png") == readFile(dir / "alpha-b.png"), "alpha never reaches the file (two alphas, identical bytes)");
    }

    // Rewriting a path truncates: a shorter image must not leave the old tail behind.
    {
        fs::path p = dir / "rewrite.png";
        auto big = pattern(64, 64, 0), small = pattern(8, 4, 0);
        debug::writePng(p.string(), big.data(), 64, 64);
        size_t bigSize = readFile(p).size();
        debug::writePng(p.string(), small.data(), 8, 4);
        std::vector<uint8_t> now = readFile(p);
        ok(now.size() < bigSize, "rewriting the same path truncates the old image");
        roundTrip(p, small, 8, 4, "written over a longer file");
    }

    // A path that cannot be opened is reported, not crashed on.
    checks++;
    if (debug::writePng((dir / "no-such-dir" / "x.png").string(), pattern(2, 2, 0).data(), 2, 2))
        failures++, printf("FAIL writePng into a missing directory should return false\n");
    checks++;
    if (debug::writePng("", pattern(2, 2, 0).data(), 2, 2))
        failures++, printf("FAIL writePng(\"\") should return false\n");

    fs::remove_all(dir, ec);
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
