#pragma once
// XMA hardware context, one decoder pass per kick (issue #93). Header-only and free of the rest
// of the runtime, so tests/xma_context_test.cpp can drive it with synthetic packets and a fake
// frame decoder.
//
// Ported from Xenia (xenia-canary, src/xenia/apu/xma_context_new.cc, xma_context.h,
// xma_helpers.h and base/bit_stream.cc; Copyright 2024 Xenia Canary, Copyright 2020-2021 Ben
// Vanik; BSD 3-clause, see docs/licenses/xenia-LICENSE.txt). The logic follows XmaContextNew;
// this file only changes the plumbing (memory access and the decoder are callbacks), turns
// Xenia's asserts into error returns, and decodes a frame it cannot decode as silence instead of
// dropping it, so that a missing decoder still behaves like the silent decoder did before.
//
// XMA in short: input buffers are 2 KB packets with a 4-byte header (frames that begin in the
// packet, offset of the first frame in bits, metadata, packets to skip to the next packet of
// the same stream); frames are bit-packed, start with a 15-bit length and may cross packet
// boundaries; each frame decodes to 512 samples per channel. Output is a ring of 256-byte
// blocks of 16-bit big-endian PCM (stereo interleaved), so a frame is 4 blocks (8 stereo).
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace apu::xma
{
    constexpr uint32_t kBytesPerPacket = 2048;
    constexpr uint32_t kBytesPerPacketHeader = 4;
    constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;
    constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
    constexpr uint32_t kBitsPerPacketHeader = 32;
    constexpr uint32_t kBitsPerFrameHeader = 15;
    constexpr uint32_t kSamplesPerFrame = 512;
    constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * 2;
    constexpr uint32_t kOutputBytesPerBlock = 256;
    constexpr uint32_t kMaxFrameLength = 0x7FFF;
    constexpr uint32_t kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader;
    constexpr int kIdToSampleRate[4] = { 24000, 32000, 44100, 48000 };

    // XMA_CONTEXT_DATA (Xenia xma_context.h): 16 big-endian dwords in guest memory, loaded into
    // host order here so the bitfields line up (clang on x86-64 fills bitfields from bit 0).
    struct ContextData
    {
        // DWORD 0
        uint32_t input_buffer_0_packet_count : 12;
        uint32_t loop_count : 8;
        uint32_t input_buffer_0_valid : 1;
        uint32_t input_buffer_1_valid : 1;
        uint32_t output_buffer_block_count : 5;
        uint32_t output_buffer_write_offset : 5;
        // DWORD 1
        uint32_t input_buffer_1_packet_count : 12;
        uint32_t loop_subframe_end : 2;
        uint32_t unk_dword_1_b : 3;
        uint32_t loop_subframe_skip : 3;
        uint32_t subframe_decode_count : 4;
        uint32_t output_buffer_padding : 3;
        uint32_t sample_rate : 2;
        uint32_t is_stereo : 1;
        uint32_t unk_dword_1_c : 1;
        uint32_t output_buffer_valid : 1;
        // DWORD 2
        uint32_t input_buffer_read_offset : 26;
        uint32_t error_status : 5;
        uint32_t error_set : 1;
        // DWORD 3
        uint32_t loop_start : 26;
        uint32_t parser_error_status : 5;
        uint32_t parser_error_set : 1;
        // DWORD 4
        uint32_t loop_end : 26;
        uint32_t packet_metadata : 5;
        uint32_t current_buffer : 1;
        // DWORD 5-8: physical addresses
        uint32_t input_buffer_0_ptr;
        uint32_t input_buffer_1_ptr;
        uint32_t output_buffer_ptr;
        uint32_t work_buffer_ptr;
        // DWORD 9
        uint32_t output_buffer_read_offset : 5;
        uint32_t : 25;
        uint32_t stop_when_done : 1;
        uint32_t interrupt_when_done : 1;
        // DWORD 10-15
        uint32_t unk_dwords_10_15[6];

        static ContextData load(const uint8_t* guest)
        {
            uint32_t d[16];
            std::memcpy(d, guest, 64);
            for (auto& v : d) v = __builtin_bswap32(v);
            ContextData c;
            std::memcpy(&c, d, 64);
            return c;
        }
        void store(uint8_t* guest) const
        {
            uint32_t d[16];
            std::memcpy(d, this, 64);
            for (auto& v : d) v = __builtin_bswap32(v);
            std::memcpy(guest, d, 64);
        }

        bool inputValid(uint32_t b) const { return b == 0 ? input_buffer_0_valid : input_buffer_1_valid; }
        bool currentInputValid() const { return inputValid(current_buffer); }
        bool anyInputValid() const { return input_buffer_0_valid || input_buffer_1_valid; }
        uint32_t inputAddress(uint32_t b) const { return b == 0 ? input_buffer_0_ptr : input_buffer_1_ptr; }
        uint32_t inputPackets(uint32_t b) const { return b == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count; }
        bool consumeOnly() const { return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0; }
    };
    static_assert(sizeof(ContextData) == 64);

    // ---- packet header (Xenia xma_helpers.h) ----
    inline uint32_t packetFrameCount(const uint8_t* p) { return p[0] >> 2; } // XMA2 only
    inline uint32_t packetMetadata(const uint8_t* p) { return p[2] & 7; }
    inline bool isXma2Packet(const uint8_t* p) { return packetMetadata(p) == 1; }
    inline uint32_t packetSkipCount(const uint8_t* p) { return p[3]; }
    inline uint32_t packetFrameOffset(const uint8_t* p) // bit offset of the first frame in the packet
    {
        return uint32_t(((p[0] & 3) << 13) | (p[1] << 5) | (p[2] >> 3)) + 32;
    }

    // MSB-first bit reader over a byte buffer (Xenia base/bit_stream.cc, reads bit by bit at the
    // edges instead of 8-byte loads, so it never reads outside the buffer).
    class BitStream
    {
    public:
        BitStream(const uint8_t* buffer, size_t sizeBits) : buffer_(buffer), size_(sizeBits) {}
        void setOffset(size_t bits) { offset_ = std::min(bits, size_); }
        size_t offset() const { return offset_; }
        size_t remaining() const { return size_ - offset_; }
        void advance(size_t bits) { setOffset(offset_ + bits); }
        uint64_t peek(size_t bits) const
        {
            uint64_t v = 0;
            for (size_t i = 0; i < bits; i++)
            {
                size_t b = offset_ + i;
                uint32_t bit = b < size_ ? (buffer_[b >> 3] >> (7 - (b & 7))) & 1 : 0;
                v = (v << 1) | bit;
            }
            return v;
        }
        uint64_t read(size_t bits) { uint64_t v = peek(bits); advance(bits); return v; }
        // Copies bits to dest (zeroed by the caller) starting at bit (offset & 7) of dest[0], so
        // the copy keeps the source's alignment within a byte. Returns that bit position.
        uint32_t copy(uint8_t* dest, size_t bits)
        {
            uint32_t shift = uint32_t(offset_ & 7);
            for (size_t i = 0; i < bits; i++)
            {
                size_t s = offset_ + i, d = shift + i;
                if (s >= size_) break;
                if ((buffer_[s >> 3] >> (7 - (s & 7))) & 1)
                    dest[d >> 3] |= uint8_t(0x80 >> (d & 7));
            }
            advance(bits);
            return shift;
        }

    private:
        const uint8_t* buffer_;
        size_t size_, offset_ = 0;
    };

    // Ring of 256-byte blocks in guest memory (Xenia base/ring_buffer.h semantics: read == write
    // means empty, so filling the whole ring makes it look empty - the decoder then clears
    // output_buffer_valid to say "full").
    struct OutputRing
    {
        uint8_t* buffer;
        uint32_t capacity, read, write;
        uint32_t written = 0; // bytes put since this ring view was made
        bool empty() const { return read == write; }
        uint32_t writeCount() const
        {
            if (read == write) return capacity;
            if (write < read) return read - write;
            return capacity - write + read;
        }
        void put(const uint8_t* src, uint32_t count)
        {
            count = std::min(count, capacity);
            uint32_t first = std::min(count, capacity - write);
            std::memcpy(buffer + write, src, first);
            std::memcpy(buffer, src + first, count - first);
            write = (write + count) % capacity;
            written += count;
        }
    };

    // Guest memory and the frame decoder, as callbacks.
    struct Host
    {
        void* user = nullptr;
        uint8_t* (*physical)(void* user, uint32_t address) = nullptr;
        // see xma_frame_decoder.h: one frame in, 512 samples per channel of s16 BE PCM out
        bool (*decode)(void* user, int sampleRate, int channels, bool reset, const uint8_t* packet,
                       int size, uint8_t* out) = nullptr;
    };

    struct Stats
    {
        uint64_t framesDecoded = 0, framesSilent = 0, decodeErrors = 0, parseErrors = 0;
    };

    // Merge a pass's result into the context as it is now in guest memory: only the fields the
    // decoder owns, so the game's own writes during the pass survive (Xenia StoreContextMerged).
    inline ContextData mergeDecoderFields(ContextData fresh, const ContextData& data, const ContextData& initial)
    {
        fresh.loop_count = data.loop_count;
        fresh.output_buffer_write_offset = data.output_buffer_write_offset;
        if (initial.input_buffer_0_valid && !data.input_buffer_0_valid) fresh.input_buffer_0_valid = 0;
        if (initial.input_buffer_1_valid && !data.input_buffer_1_valid) fresh.input_buffer_1_valid = 0;
        if (initial.output_buffer_valid && !data.output_buffer_valid) fresh.output_buffer_valid = 0;
        fresh.input_buffer_read_offset = data.input_buffer_read_offset;
        fresh.error_status = data.error_status;
        fresh.current_buffer = data.current_buffer;
        fresh.output_buffer_read_offset = data.output_buffer_read_offset;
        return fresh;
    }

    class Context
    {
    public:
        // One decoder pass over the context (what a kick asks for). Returns false if nothing was
        // done that needs storing.
        bool work(ContextData& data, const Host& host, Stats& stats)
        {
            if (!data.output_buffer_valid)
                return false;
            if (data.output_buffer_block_count == 0)
            {
                stats.parseErrors++;
                return false;
            }
            const ContextData initial = data;
            OutputRing out{ host.physical(host.user, data.output_buffer_ptr),
                            data.output_buffer_block_count * kOutputBytesPerBlock, 0, 0 };
            out.read = (data.output_buffer_read_offset * kOutputBytesPerBlock) % out.capacity;
            out.write = (data.output_buffer_write_offset * kOutputBytesPerBlock) % out.capacity;
            remainingBlocks_ = int32_t(out.writeCount() / kOutputBytesPerBlock);

            if (data.consumeOnly())
            {
                // Nothing to drain: don't touch the offsets, or the game re-reads stale PCM.
                if (remainingSubframes_ == 0)
                    return false;
                consume(out, data);
                data.output_buffer_write_offset = out.write / kOutputBytesPerBlock;
                if (out.empty())
                    data.output_buffer_valid = 0;
                return true;
            }

            // Subframes consume() writes per iteration, plus the headroom output_buffer_padding asks for.
            const int32_t minimum = int32_t(std::max<uint32_t>(1, data.subframe_decode_count)) +
                                    int32_t(data.output_buffer_padding);
            if (minimum > remainingBlocks_)
                return false; // no room for even one pass: wait for the game to drain the ring

            while (remainingBlocks_ >= minimum)
            {
                const uint32_t preOffset = data.input_buffer_read_offset;
                const uint8_t preRemaining = remainingSubframes_;
                decode(data, host, stats);
                consume(out, data);
                // Keep draining a partly consumed frame: nothing else would deliver its rest.
                if ((!data.anyInputValid() || data.error_status == 4) && remainingSubframes_ == 0)
                    break;
                if (preRemaining == 0 && data.input_buffer_read_offset == preOffset && remainingSubframes_ == 0)
                    break; // no progress possible until the game gives more input
            }

            // Xenia checks only initial.anyInputValid() here, which drops the rest of the stream's
            // last frame when its input was already used up before this pass (caught by
            // tests/xma_context_test.cpp): blocks written in this pass always count.
            if (initial.anyInputValid() || out.written > 0)
                data.output_buffer_write_offset = out.write / kOutputBytesPerBlock;
            else if (data.output_buffer_write_offset != data.output_buffer_read_offset)
            {
                // Starved of input: some games use write == read as a stall detector.
                data.output_buffer_write_offset = data.output_buffer_read_offset;
                data.output_buffer_valid = 0;
            }
            if (remainingBlocks_ == 0 && out.empty())
                data.output_buffer_valid = 0; // the ring is full
            return true;
        }

        void clear(ContextData& data)
        {
            data.input_buffer_0_valid = 0;
            data.input_buffer_1_valid = 0;
            data.output_buffer_valid = 0;
            data.input_buffer_read_offset = kBitsPerPacketHeader;
            data.output_buffer_read_offset = 0;
            data.output_buffer_write_offset = 0;
            remainingSubframes_ = 0;
            loopFrameOutputLimit_ = 0;
            loopStartSkipPending_ = false;
            resetDecoder_ = true; // the decoder keeps the previous frame's overlap otherwise
        }

    private:
        struct PacketInfo
        {
            uint32_t frameCount = 0, currentFrame = 0, currentFrameSize = 0;
            bool lastFrameInPacket() const { return frameCount == 0 || currentFrame == frameCount - 1; }
        };

        static void swapInputBuffer(ContextData& data)
        {
            if (data.current_buffer == 0) data.input_buffer_0_valid = 0;
            else data.input_buffer_1_valid = 0;
            data.current_buffer ^= 1;
            data.input_buffer_read_offset = kBitsPerPacketHeader;
        }

        void consume(OutputRing& out, const ContextData& data)
        {
            if (!remainingSubframes_)
                return;
            const uint8_t total = uint8_t((kBytesPerFrameChannel / kOutputBytesPerBlock) << data.is_stereo);
            if (loopFrameOutputLimit_ > 0)
            {
                const uint8_t consumed = uint8_t(total - remainingSubframes_);
                if (consumed >= loopFrameOutputLimit_)
                {
                    // Loop end reached inside this frame: drop the rest, but charge the headroom
                    // as if the frame had completed.
                    remainingBlocks_ -= data.output_buffer_padding;
                    remainingSubframes_ = 0;
                    loopFrameOutputLimit_ = 0;
                    return;
                }
            }
            int32_t toWrite = std::min<int32_t>(remainingSubframes_, int32_t(std::max<uint32_t>(1, data.subframe_decode_count)));
            if (loopFrameOutputLimit_ > 0)
            {
                const uint8_t consumed = uint8_t(total - remainingSubframes_);
                toWrite = std::min<int32_t>(toWrite, int32_t(loopFrameOutputLimit_) - consumed);
            }
            const int32_t readOffset = total - remainingSubframes_;
            out.put(rawFrame_.data() + kOutputBytesPerBlock * readOffset, uint32_t(toWrite) * kOutputBytesPerBlock);
            const int32_t headroom = (remainingSubframes_ - toWrite == 0) ? int32_t(data.output_buffer_padding) : 0;
            remainingBlocks_ -= toWrite + headroom;
            remainingSubframes_ = uint8_t(remainingSubframes_ - toWrite);
        }

        static int32_t packetNumber(uint32_t size, uint32_t bitOffset)
        {
            if (bitOffset < kBitsPerPacketHeader || bitOffset >= size * 8)
                return -1;
            return int32_t((bitOffset >> 3) / kBytesPerPacket);
        }

        PacketInfo packetInfo(const uint8_t* packet, uint32_t frameOffset) const
        {
            PacketInfo info;
            const uint32_t first = packetFrameOffset(packet);
            BitStream stream(packet, kBitsPerPacket);
            stream.setOffset(first);
            if (frameOffset < first)
            {
                // The tail of a frame split from the previous packet.
                info.currentFrame = 0;
                info.currentFrameSize = first - frameOffset;
            }
            for (;;)
            {
                if (stream.remaining() < kBitsPerFrameHeader)
                {
                    // This frame's length runs into the next packet: count it, with size 0 to
                    // send it to the split-header path.
                    if (stream.remaining() > 0)
                    {
                        if (stream.offset() == frameOffset)
                        {
                            info.currentFrame = info.frameCount;
                            info.currentFrameSize = 0;
                        }
                        info.frameCount++;
                    }
                    break;
                }
                const uint64_t frameSize = stream.peek(kBitsPerFrameHeader);
                if (frameSize == 0 || frameSize == kMaxFrameLength)
                    break;
                if (stream.offset() == frameOffset)
                {
                    info.currentFrame = info.frameCount;
                    info.currentFrameSize = uint32_t(frameSize);
                }
                info.frameCount++;
                if (frameSize > stream.remaining())
                    break; // the last frame, continued in the next packet
                stream.advance(frameSize - 1);
                if (stream.read(1) == 0) // trailing bit: another frame follows
                    break;
            }
            if (isXma2Packet(packet))
            {
                const uint32_t headerCount = packetFrameCount(packet);
                if (headerCount > info.frameCount)
                {
                    // A frame header split across the packet boundary: trust the XMA2 count.
                    if (info.currentFrameSize == 0)
                        info.currentFrame = info.frameCount;
                    info.frameCount = headerCount;
                }
            }
            return info;
        }

        // Packet `index` of the current buffer, or of the other buffer once past its end.
        const uint8_t* nextPacket(const ContextData& data, const Host& host, uint32_t index, uint32_t currentPackets,
                                  uint32_t* bufferOut = nullptr, uint32_t* indexOut = nullptr) const
        {
            uint32_t buffer = data.current_buffer;
            if (index >= currentPackets)
            {
                buffer ^= 1;
                index -= currentPackets;
            }
            if (!data.inputValid(buffer) || !data.inputAddress(buffer) || index >= data.inputPackets(buffer))
                return nullptr;
            if (bufferOut) *bufferOut = buffer;
            if (indexOut) *indexOut = index;
            return host.physical(host.user, data.inputAddress(buffer)) + index * kBytesPerPacket;
        }

        // First packet from `index` on (same buffer) in which a frame begins: its frame's bit
        // offset, or kBitsPerPacketHeader if none.
        static uint32_t nextPacketReadOffset(const uint8_t* buffer, uint32_t index, uint32_t packets)
        {
            while (index < packets)
            {
                const uint8_t* p = buffer + index * kBytesPerPacket;
                const uint32_t offset = packetFrameOffset(p);
                if (offset <= kMaxFrameSizeInBits)
                    return index * kBitsPerPacket + offset;
                const uint32_t skip = packetSkipCount(p);
                if (skip == 0xFF)
                    break;
                index += skip + 1;
            }
            return kBitsPerPacketHeader;
        }

        uint32_t nextPacketReadOffset(const ContextData& data, const Host& host, uint32_t index, uint32_t currentPackets) const
        {
            uint32_t buffer = 0, packet = 0;
            if (!nextPacket(data, host, index, currentPackets, &buffer, &packet))
                return kBitsPerPacketHeader;
            return nextPacketReadOffset(host.physical(host.user, data.inputAddress(buffer)), packet, data.inputPackets(buffer));
        }

        void updateLoopStatus(ContextData& data)
        {
            if (data.loop_count == 0)
                return;
            const uint32_t loopStart = std::max(kBitsPerPacketHeader, uint32_t(data.loop_start));
            const uint32_t loopEnd = std::max(kBitsPerPacketHeader, uint32_t(data.loop_end));
            if (data.input_buffer_read_offset != loopEnd)
                return;
            data.input_buffer_read_offset = loopStart;
            loopStartSkipPending_ = true;
            if (data.loop_count != 255) // 255 = loop forever
                data.loop_count--;
        }

        void decode(ContextData& data, const Host& host, Stats& stats)
        {
            if (!data.anyInputValid() || remainingSubframes_ > 0)
                return;
            if (!data.currentInputValid())
            {
                swapInputBuffer(data);
                if (!data.currentInputValid())
                    return;
            }
            const uint8_t* input = host.physical(host.user, data.inputAddress(data.current_buffer));
            inputBuffer_.fill(0);

            // The loop end frame is decoded first, then the read offset jumps back to loop_start.
            bool loopEndFrame = false;
            if (data.loop_count > 0)
                loopEndFrame = data.input_buffer_read_offset == std::max(kBitsPerPacketHeader, uint32_t(data.loop_end));

            if (data.input_buffer_read_offset < kBitsPerPacketHeader)
                data.input_buffer_read_offset = kBitsPerPacketHeader; // some games kick with offset 0

            const uint32_t inputSize = data.inputPackets(data.current_buffer) * kBytesPerPacket;
            const uint32_t inputPackets = inputSize / kBytesPerPacket;
            const int32_t packetIndex = packetNumber(inputSize, data.input_buffer_read_offset);
            if (packetIndex < 0)
            {
                stats.parseErrors++;
                return;
            }
            const uint8_t* packet = input + packetIndex * kBytesPerPacket;
            const uint32_t firstFrame = packetFrameOffset(packet);
            uint32_t relative = data.input_buffer_read_offset % kBitsPerPacket;
            if (relative < firstFrame)
            {
                // Inside the tail of a frame split from the previous packet, whose beginning we
                // don't have: skip to the first whole frame.
                data.input_buffer_read_offset = packetIndex * kBitsPerPacket + firstFrame;
                relative = firstFrame;
            }
            const uint32_t skipCount = packetSkipCount(packet);
            if (skipCount == 0xFF)
            {
                // No frame begins in this packet: go to the next one.
                const uint32_t next = packetIndex + 1;
                const uint32_t nextOffset = nextPacketReadOffset(data, host, next, inputPackets);
                if (next >= inputPackets || nextOffset == kBitsPerPacketHeader)
                    swapInputBuffer(data);
                data.input_buffer_read_offset = nextOffset;
                return;
            }

            PacketInfo info = packetInfo(packet, relative);
            const uint32_t nextPacketIndex = packetIndex + skipCount + 1;

            if (info.currentFrameSize == 0)
            {
                // The 15-bit frame length itself is split across the packet boundary.
                const uint8_t* next = nextPacket(data, host, nextPacketIndex, inputPackets);
                if (!next)
                {
                    swapInputBuffer(data); // can't resolve it without the next buffer
                    return;
                }
                std::memcpy(inputBuffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
                std::memcpy(inputBuffer_.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
                BitStream combined(inputBuffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
                combined.setOffset(relative - kBitsPerPacketHeader);
                const uint64_t size = combined.peek(kBitsPerFrameHeader);
                if (size == kMaxFrameLength)
                {
                    data.error_status = 4;
                    stats.parseErrors++;
                    return;
                }
                info.currentFrameSize = uint32_t(size);
            }

            BitStream stream(input, size_t(packetIndex + 1) * kBitsPerPacket);
            stream.setOffset(data.input_buffer_read_offset);
            const uint32_t bitsToCopy = std::min<uint32_t>(uint32_t(stream.remaining()), info.currentFrameSize);
            if (bitsToCopy == 0)
            {
                stats.parseErrors++;
                swapInputBuffer(data);
                return;
            }

            if (info.lastFrameInPacket() && stream.remaining() < info.currentFrameSize)
            {
                // The frame continues in the next packet of this stream.
                const uint8_t* next = nextPacket(data, host, nextPacketIndex, inputPackets);
                if (!next)
                {
                    data.error_status = 4;
                    stats.parseErrors++;
                    return;
                }
                std::memcpy(inputBuffer_.data() + kBytesPerPacketData, next + kBytesPerPacketHeader, kBytesPerPacketData);
            }
            std::memcpy(inputBuffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

            BitStream frameStream(inputBuffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
            frameStream.setOffset(relative - kBitsPerPacketHeader);
            xmaFrame_.fill(0);
            const uint32_t paddingStart = frameStream.copy(xmaFrame_.data() + 1, info.currentFrameSize);

            // One byte of padding information, then the frame (xmaframes_decode_packet).
            const uint32_t total = paddingStart + info.currentFrameSize;
            const int size = int(1 + total / 8 + (total % 8 ? 1 : 0));
            const uint32_t paddingEnd = uint32_t(size) * 8 - (8 + total);
            xmaFrame_[0] = uint8_t(((paddingStart & 7) << 5) | ((paddingEnd & 7) << 2));

            const int rate = kIdToSampleRate[std::min<uint32_t>(data.sample_rate, 3)];
            const int channels = data.is_stereo ? 2 : 1;
            const bool reset = resetDecoder_;
            resetDecoder_ = false;
            if (host.decode && host.decode(host.user, rate, channels, reset, xmaFrame_.data(), size, rawFrame_.data()))
                stats.framesDecoded++;
            else
            {
                // No decoder, or this frame failed: play it as silence so timing still works.
                rawFrame_.fill(0);
                if (host.decode) stats.decodeErrors++;
                stats.framesSilent++;
                resetDecoder_ = true;
            }
            remainingSubframes_ = uint8_t(4 << data.is_stereo);

            if (loopEndFrame)
                loopFrameOutputLimit_ = uint8_t((data.loop_subframe_end + 1) << data.is_stereo);
            else
                loopFrameOutputLimit_ = 0;
            if (loopStartSkipPending_)
            {
                const uint8_t skip = uint8_t(data.loop_subframe_skip << data.is_stereo);
                if (skip < remainingSubframes_)
                    remainingSubframes_ = uint8_t(remainingSubframes_ - skip);
                loopStartSkipPending_ = false;
            }

            if (loopEndFrame)
            {
                updateLoopStatus(data);
                return;
            }

            // Where the next frame starts.
            if (!info.lastFrameInPacket())
            {
                const uint32_t nextFrame = (data.input_buffer_read_offset + bitsToCopy) % kBitsPerPacket;
                data.input_buffer_read_offset = packetIndex * kBitsPerPacket + nextFrame;
                return;
            }
            uint32_t nextOffset = nextPacketReadOffset(data, host, nextPacketIndex, inputPackets);
            if (nextPacketIndex >= inputPackets || nextOffset == kBitsPerPacketHeader)
                swapInputBuffer(data);
            if (nextOffset == kBitsPerPacketHeader && data.anyInputValid())
            {
                // At the start of the next buffer: go to its first frame, or on if it has none.
                nextOffset = packetFrameOffset(host.physical(host.user, data.inputAddress(data.current_buffer)));
                if (nextOffset > kMaxFrameSizeInBits)
                {
                    swapInputBuffer(data);
                    return;
                }
            }
            data.input_buffer_read_offset = nextOffset;
        }

        std::array<uint8_t, kBytesPerPacketData * 2> inputBuffer_{};
        // padding byte + the longest frame (0x7FFF bits) + FFmpeg's input padding (64 zero bytes)
        std::array<uint8_t, 1 + 4096 + 64> xmaFrame_{};
        std::array<uint8_t, kBytesPerFrameChannel * 2> rawFrame_{};
        int32_t remainingBlocks_ = 0;
        uint8_t remainingSubframes_ = 0;
        uint8_t loopFrameOutputLimit_ = 0; // subframes to output from the loop end frame (0 = all)
        bool loopStartSkipPending_ = false;
        bool resetDecoder_ = true;
    };
}
