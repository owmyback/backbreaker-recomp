#pragma once
// One-XMA-frame decoder interface (issue #93). xma_frame_decoder.cpp implements it with the
// "xmaframes" decoder of FFmpeg when the build has one (BB_XMA_DECODER, see
// scripts/build_xma_decoder.sh); without it, create() returns nullptr and XMA stays silent.
// Kept apart from the context logic (xma_context.h) so that can be tested without FFmpeg.
#include <cstdint>

namespace apu::xma
{
    struct FrameDecoder;

    bool decoderAvailable();
    FrameDecoder* createFrameDecoder();
    void destroyFrameDecoder(FrameDecoder* d);

    // Decodes one frame. packet = one byte of bit padding information (bits 7-5: padding
    // before the frame, bits 4-2: padding after it, as xmaframes_decode_packet reads it) and
    // then the frame's bits. reset = start a new stream (drop the previous frame's overlap).
    // On success out receives 512 samples per channel of signed 16-bit big-endian PCM, stereo
    // interleaved L R L R, and true is returned. On failure out is left untouched.
    bool decodeFrame(FrameDecoder* d, int sampleRate, int channels, bool reset,
                     const uint8_t* packet, int size, uint8_t* out);
    const char* lastError(FrameDecoder* d);
}
