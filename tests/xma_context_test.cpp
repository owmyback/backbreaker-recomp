// Checks runtime/apu/xma_context.h (issue #93): the XMA hardware context's packet and frame walk,
// with synthetic packets and a fake frame decoder. No game files and no FFmpeg. Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/xma_context_test.cpp -o build/xma_context_test
//
// The packets are built the way XMA2 lays them out (Xenia xma_helpers.h and
// XmaContextNew::GetPacketInfo): a 4-byte header (6 bits frames beginning in the packet, 15 bits
// offset of the first one after the header, 3 bits metadata = 1 for XMA2, 8 bits skip count),
// then frames packed bit by bit across packets. A frame is a 15-bit length (in bits, counting
// the length itself), then here a 16-bit id and filler bits derived from the id, and a final
// bit that says whether another frame follows in the same packet.
//
// The fake decoder checks what the context hands it (the padding byte, the length, every filler
// bit - so frames split across packets and buffers must arrive whole) and outputs 512 samples
// equal to the frame id. The test then plays the game's side: kick, read the output ring, move
// the read offset, set the output valid again, and compares the blocks with the expected order.
#include "../runtime/apu/xma_context.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace apu::xma;

static int failures = 0, checks = 0;

static void expect(bool ok, const std::string& what)
{
    checks++;
    if (!ok)
    {
        failures++;
        printf("FAIL %s\n", what.c_str());
    }
}

// ---- synthetic stream ----
constexpr uint32_t kDataBits = kBytesPerPacketData * 8;

static uint32_t fillerBit(uint32_t id, uint32_t k) { return ((id * 2654435761u) >> (k % 29)) & 1; }

struct Stream
{
    std::vector<uint8_t> bytes;          // all packets, back to back
    std::vector<uint32_t> frameOffsets;  // absolute bit offset of each frame (as the context's read offset)
    uint32_t packets = 0;
};

static void putBit(std::vector<uint8_t>& b, uint64_t pos, uint32_t bit)
{
    if (bit) b[pos >> 3] |= uint8_t(0x80 >> (pos & 7));
}

// Frames of the given lengths (bits), ids 0, 1, 2, ...; packed into as many packets as needed.
static Stream buildStream(const std::vector<uint32_t>& lengths, uint32_t minPackets = 0)
{
    uint64_t totalBits = 0;
    for (uint32_t l : lengths) totalBits += l;
    Stream s;
    s.packets = std::max<uint32_t>(minPackets, uint32_t((totalBits + kDataBits - 1) / kDataBits));
    s.bytes.assign(size_t(s.packets) * kBytesPerPacket, 0);
    std::vector<uint32_t> firstFrame(s.packets, 0x7FFF), count(s.packets, 0);
    uint64_t pos = 0; // position in the data stream (headers excluded)
    auto absolute = [](uint64_t p) { return uint32_t(p / kDataBits * kBitsPerPacket + kBitsPerPacketHeader + p % kDataBits); };
    for (uint32_t id = 0; id < lengths.size(); id++)
    {
        const uint32_t len = lengths[id];
        const uint32_t packet = uint32_t(pos / kDataBits);
        if (firstFrame[packet] == 0x7FFF) firstFrame[packet] = uint32_t(pos % kDataBits);
        count[packet]++;
        s.frameOffsets.push_back(absolute(pos));
        const bool last = id + 1 == lengths.size();
        const bool nextInSamePacket = !last && (pos + len) / kDataBits == packet;
        for (uint32_t k = 0; k < len; k++)
        {
            uint32_t bit;
            if (k < 15) bit = (len >> (14 - k)) & 1;
            else if (k < 31) bit = (id >> (30 - k)) & 1;
            else if (k + 1 < len) bit = fillerBit(id, k);
            else bit = nextInSamePacket ? 1 : 0;
            putBit(s.bytes, absolute(pos + k), bit);
        }
        pos += len;
    }
    for (uint32_t p = 0; p < s.packets; p++)
    {
        uint8_t* h = &s.bytes[size_t(p) * kBytesPerPacket];
        const uint32_t off = firstFrame[p];
        h[0] = uint8_t((count[p] << 2) | ((off >> 13) & 3));
        h[1] = uint8_t(off >> 5);
        h[2] = uint8_t(((off & 31) << 3) | 1); // metadata 1 = XMA2
        h[3] = 0;                              // skip count: the next packet is ours
    }
    return s;
}

// ---- fake guest memory and decoder ----
struct Fake
{
    std::map<uint32_t, std::vector<uint8_t>*> regions; // physical base -> bytes
    std::vector<uint32_t> decoded;                      // frame ids in decode order
    std::vector<std::string> errors;
    std::vector<uint32_t> expectedLength;               // by id
    int failId = -1; // the decoder rejects this frame
};

static uint8_t* fakePhysical(void* user, uint32_t address)
{
    auto* f = static_cast<Fake*>(user);
    auto it = f->regions.upper_bound(address);
    --it;
    return it->second->data() + (address - it->first);
}

static bool fakeDecode(void* user, int, int channels, bool, const uint8_t* packet, int size, uint8_t* out)
{
    auto* f = static_cast<Fake*>(user);
    BitStream bits(packet, size_t(size) * 8);
    const uint32_t padStart = uint32_t(bits.read(3)), padEnd = uint32_t(bits.read(3));
    bits.read(2);
    bits.advance(padStart);
    const uint32_t len = uint32_t(bits.read(15)), id = uint32_t(bits.read(16));
    char msg[160];
    if (uint32_t(size) * 8 != 8 + padStart + len + padEnd)
    {
        snprintf(msg, sizeof(msg), "frame %u: packet size %d bytes != 8 + %u + %u + %u bits", id, size, padStart, len, padEnd);
        f->errors.push_back(msg);
    }
    if (id < f->expectedLength.size() && len != f->expectedLength[id])
    {
        snprintf(msg, sizeof(msg), "frame %u: length %u, expected %u", id, len, f->expectedLength[id]);
        f->errors.push_back(msg);
    }
    for (uint32_t k = 31; k + 1 < len; k++)
        if (bits.read(1) != fillerBit(id, k))
        {
            snprintf(msg, sizeof(msg), "frame %u: filler bit %u wrong (frame not reassembled whole)", id, k);
            f->errors.push_back(msg);
            break;
        }
    f->decoded.push_back(id);
    if (int(id) == f->failId)
        return false;
    for (uint32_t i = 0; i < kSamplesPerFrame * uint32_t(channels); i++)
    {
        out[i * 2] = uint8_t(id >> 8);
        out[i * 2 + 1] = uint8_t(id);
    }
    return true;
}

// ---- the game's side ----
struct Run
{
    std::vector<int> blocks; // frame id in each output block read, in order (-1 = silence)
    Stats stats;
    ContextData last{};
};

struct Setup
{
    uint32_t outBlocks = 16, stereo = 0, sdc = 0, padding = 0;
    uint32_t buffer0Packets = 0, buffer1Packets = 0; // 0: the whole stream in buffer 0
    uint32_t loopCount = 0, loopStartFrame = 0, loopEndFrame = 0, loopSubframeEnd = 3, loopSubframeSkip = 0;
    int failId = -1;
};

static Run play(const Stream& s, const std::vector<uint32_t>& lengths, const Setup& setup)
{
    Fake fake;
    fake.expectedLength = lengths;
    fake.failId = setup.failId;
    std::vector<uint8_t> input = s.bytes;
    std::vector<uint8_t> output(setup.outBlocks * kOutputBytesPerBlock, 0xEE);
    fake.regions[0x00100000] = &input;
    fake.regions[0x00800000] = &output;

    ContextData c{};
    const uint32_t b0 = setup.buffer0Packets ? setup.buffer0Packets : s.packets;
    c.input_buffer_0_ptr = 0x00100000;
    c.input_buffer_0_packet_count = b0;
    c.input_buffer_0_valid = 1;
    if (b0 < s.packets)
    {
        c.input_buffer_1_ptr = 0x00100000 + b0 * kBytesPerPacket;
        c.input_buffer_1_packet_count = s.packets - b0;
        c.input_buffer_1_valid = 1;
    }
    c.input_buffer_read_offset = s.frameOffsets[0];
    c.output_buffer_ptr = 0x00800000;
    c.output_buffer_block_count = setup.outBlocks;
    c.output_buffer_valid = 1;
    c.is_stereo = setup.stereo;
    c.sample_rate = 3;
    c.subframe_decode_count = setup.sdc;
    c.output_buffer_padding = setup.padding;
    if (setup.loopCount)
    {
        c.loop_count = setup.loopCount;
        c.loop_start = s.frameOffsets[setup.loopStartFrame];
        c.loop_end = s.frameOffsets[setup.loopEndFrame];
        c.loop_subframe_end = setup.loopSubframeEnd;
        c.loop_subframe_skip = setup.loopSubframeSkip;
    }

    Context context;
    Host host{ &fake, fakePhysical, fakeDecode };
    Run run;
    uint8_t guest[64];
    c.store(guest);
    for (int kick = 0; kick < 1000; kick++)
    {
        const ContextData initial = ContextData::load(guest);
        ContextData data = initial;
        if (context.work(data, host, run.stats))
            mergeDecoderFields(ContextData::load(guest), data, initial).store(guest);
        ContextData now = ContextData::load(guest);
        uint32_t avail;
        if (now.output_buffer_write_offset != now.output_buffer_read_offset)
            avail = (now.output_buffer_write_offset + setup.outBlocks - now.output_buffer_read_offset) % setup.outBlocks;
        else
            avail = now.output_buffer_valid ? 0 : setup.outBlocks; // valid cleared with write == read: full
        if (avail == 0 && !now.anyInputValid())
        {
            run.last = now;
            break;
        }
        for (uint32_t b = 0; b < avail; b++)
        {
            const uint8_t* p = &output[((now.output_buffer_read_offset + b) % setup.outBlocks) * kOutputBytesPerBlock];
            int id = (p[0] << 8) | p[1];
            bool uniform = true;
            for (uint32_t i = 0; i < kOutputBytesPerBlock; i += 2)
                uniform &= ((p[i] << 8) | p[i + 1]) == id;
            expect(uniform, "a block holds samples of one frame");
            run.blocks.push_back(id);
        }
        now.output_buffer_read_offset = (now.output_buffer_read_offset + avail) % setup.outBlocks;
        now.output_buffer_valid = 1;
        now.store(guest);
        run.last = now;
    }
    for (auto& e : fake.errors) expect(false, e);
    return run;
}

static std::vector<int> blocksOf(const std::vector<uint32_t>& ids, uint32_t perFrame)
{
    std::vector<int> v;
    for (uint32_t id : ids)
        for (uint32_t b = 0; b < perFrame; b++) v.push_back(int(id));
    return v;
}

static std::string show(const std::vector<int>& v)
{
    std::string s;
    for (size_t i = 0; i < v.size(); i++)
    {
        if (i == 0 || v[i] != v[i - 1]) s += (i ? " " : "") + std::to_string(v[i]) + "x";
        size_t n = 1;
        while (i + n < v.size() && v[i + n] == v[i]) n++;
        if (i == 0 || v[i] != v[i - 1]) s += std::to_string(n);
    }
    return s;
}

static void same(const char* what, const std::vector<int>& got, const std::vector<int>& want)
{
    expect(got == want, std::string(what) + ": got [" + show(got) + "], expected [" + show(want) + "]");
}

int main()
{
    std::vector<uint32_t> ids10;
    for (uint32_t i = 0; i < 10; i++) ids10.push_back(i);

    // 1. Mono, one buffer: ten frames of 3000-7000 bits, several split across packets.
    {
        std::vector<uint32_t> len = { 3000, 7000, 5000, 6100, 4000, 3500, 6900, 5200, 4400, 3100 };
        Stream s = buildStream(len);
        Run r = play(s, len, Setup{});
        same("mono, one buffer", r.blocks, blocksOf(ids10, 4));
        expect(r.stats.framesDecoded == 10 && r.stats.parseErrors == 0, "mono: 10 decoded, no parse errors");
        expect(!r.last.input_buffer_0_valid, "mono: the input buffer is marked consumed");
    }

    // 2. Stereo, 8 blocks per frame, a ring that is not a multiple of a frame (13 blocks).
    {
        std::vector<uint32_t> len = { 6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000, 6000 };
        Stream s = buildStream(len);
        Setup st;
        st.outBlocks = 13;
        st.stereo = 1;
        Run r = play(s, len, st);
        same("stereo, 13-block ring", r.blocks, blocksOf(ids10, 8));
    }

    // 3. Two input buffers (streaming): the stream is cut between packets 1 and 2, and frame 4
    //    crosses from the last packet of buffer 0 into the first packet of buffer 1.
    {
        std::vector<uint32_t> len = { 5000, 7000, 7000, 7000, 7000, 5000, 6000, 6000, 3000, 3000 };
        Stream s = buildStream(len);
        Setup st;
        st.buffer0Packets = 2;
        Run r = play(s, len, st);
        same("two buffers", r.blocks, blocksOf(ids10, 4));
        expect(!r.last.input_buffer_0_valid && !r.last.input_buffer_1_valid, "two buffers: both consumed");
        expect(r.last.current_buffer == 0, "two buffers: swapped to buffer 1 and past it");
    }

    // 4. A frame whose 15-bit length is split by the packet boundary: frames fill the first packet
    //    up to 10 bits before its end.
    {
        std::vector<uint32_t> len = { 8000, kDataBits - 8000 - 10, 5000, 5000, 5000 };
        Stream s = buildStream(len);
        Run r = play(s, len, Setup{});
        same("split frame header", r.blocks, blocksOf({ 0, 1, 2, 3, 4 }, 4));
        expect(r.stats.parseErrors == 0, "split frame header: no parse errors");
    }

    // 5. Loop: frames 1-3 once more (loop_count 1), with subframe precision: the first pass of the
    //    loop end frame stops after 2 subframes (loop_subframe_end 1), and the loop start frame
    //    skips its first subframe after the jump (loop_subframe_skip 1).
    {
        std::vector<uint32_t> len = { 4000, 4000, 4000, 4000, 4000, 4000 };
        Stream s = buildStream(len);
        Setup st;
        st.loopCount = 1;
        st.loopStartFrame = 1;
        st.loopEndFrame = 3;
        st.loopSubframeEnd = 1;
        st.loopSubframeSkip = 1;
        Run r = play(s, len, st);
        std::vector<int> want = { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5 };
        same("loop with subframe end/skip", r.blocks, want);
        expect(r.last.loop_count == 0, "loop: loop_count counted down to 0");
    }

    // 6. subframe_decode_count 2 and output padding 1 on a 10-block ring: still every frame, in order.
    {
        std::vector<uint32_t> len = { 4000, 4500, 5000, 5500, 6000, 6500, 7000, 3000, 3500, 4000 };
        Stream s = buildStream(len);
        Setup st;
        st.outBlocks = 10;
        st.sdc = 2;
        st.padding = 1;
        Run r = play(s, len, st);
        same("sdc 2, padding 1", r.blocks, blocksOf(ids10, 4));
    }

    // 7. A frame the decoder rejects is played as silence (zeros), the stream goes on.
    {
        std::vector<uint32_t> len = { 4000, 4000, 4000, 4000 };
        Stream s = buildStream(len);
        Setup st;
        st.failId = 2;
        Run r = play(s, len, st);
        same("decode failure", r.blocks, blocksOf({ 0, 1, 0, 3 }, 4));
        expect(r.stats.decodeErrors == 1 && r.stats.framesSilent == 1, "decode failure: counted once");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
