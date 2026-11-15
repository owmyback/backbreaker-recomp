#pragma once
// XAudio render-driver frames -> host sample layouts. Header-only so that tests/audio_convert_test.cpp
// can check it without the rest of the runtime.
//
// A frame, as the game hands it to XAudioSubmitRenderDriverFrame, is 6 channels x 256 samples of
// big-endian float, one channel after another (planar), in the XAudio 5.1 order FL, FR, FC, LFE,
// BL, BR. Sources: Xenia apu/conversion.h (sequential_6_BE_to_interleaved_6_LE) and
// sdl_audio_driver.cc (6 channels -> 256 samples per channel); the channel order is XAudio2's
// default channel mapping, which Xenia's comment cites.
#include <cstdint>
#include <cstring>

namespace apu::frame
{
    constexpr uint32_t kChannels = 6;
    constexpr uint32_t kSamples = 256; // per channel
    constexpr uint32_t kBytes = kChannels * kSamples * 4;
    enum Channel : uint32_t { FL, FR, FC, LFE, BL, BR };

    inline float loadBE(const uint8_t* p)
    {
        uint32_t v;
        std::memcpy(&v, p, 4);
        v = __builtin_bswap32(v);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }

    inline float sample(const uint8_t* frame, uint32_t channel, uint32_t i)
    {
        return loadBE(frame + (channel * kSamples + i) * 4);
    }

    // All six channels, interleaved, host order (SDL's 5.1 layout is the same FL FR FC LFE BL BR).
    inline void toInterleaved6(const uint8_t* frame, float* out)
    {
        for (uint32_t i = 0; i < kSamples; i++)
            for (uint32_t c = 0; c < kChannels; c++)
                out[i * kChannels + c] = sample(frame, c, i);
    }

    // Stereo downmix with the ITU-R BS.775 coefficients: centre and each surround channel at
    // -3 dB into its side, LFE dropped. Front-only content passes through at unity gain (no
    // normalisation), so a game that mixes for stereo is not made quieter. Values above 1.0 are
    // left for the device conversion to clamp.
    inline void toStereo(const uint8_t* frame, float* out)
    {
        constexpr float k = 0.70710678f;
        for (uint32_t i = 0; i < kSamples; i++)
        {
            float c = sample(frame, FC, i) * k;
            out[i * 2 + 0] = sample(frame, FL, i) + c + sample(frame, BL, i) * k;
            out[i * 2 + 1] = sample(frame, FR, i) + c + sample(frame, BR, i) * k;
        }
    }
}
