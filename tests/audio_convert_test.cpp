// Checks runtime/apu/frame_convert.h (issue #90): XAudio render-driver frames (6 channels x 256
// big-endian floats, one channel after another, FL FR FC LFE BL BR) -> interleaved 5.1 and the
// stereo downmix. No game files. Build: clang++ -std=c++20 -Wall -Wextra -O2
// tests/audio_convert_test.cpp -o build/audio_convert_test
//
// Expected values are worked out by hand: each channel c holds sample i = (c + 1) * 0.01 + i / 1024
// (exact in float for the checks below up to rounding, which `near` allows), and the downmix is
// L = FL + 0.70710678 * (FC + BL), R = FR + 0.70710678 * (FC + BR).
#include "../runtime/apu/frame_convert.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace apu::frame;

static int failures = 0, checks = 0;

static void near(const char* what, uint32_t i, float got, float want)
{
    checks++;
    if (std::fabs(got - want) > 1e-6f)
    {
        failures++;
        printf("FAIL %s[%u]: %.9g, expected %.9g\n", what, i, got, want);
    }
}

static float value(uint32_t c, uint32_t i) { return (c + 1) * 0.01f + float(i) / 1024; }

static void storeBE(uint8_t* p, float f)
{
    uint32_t v;
    std::memcpy(&v, &f, 4);
    v = __builtin_bswap32(v);
    std::memcpy(p, &v, 4);
}

int main()
{
    static uint8_t frame[kBytes];
    for (uint32_t c = 0; c < kChannels; c++)
        for (uint32_t i = 0; i < kSamples; i++)
            storeBE(frame + (c * kSamples + i) * 4, value(c, i));

    // The byte order: 1.0f is 3F 80 00 00 in guest memory.
    {
        uint8_t one[4] = { 0x3F, 0x80, 0x00, 0x00 };
        near("loadBE(3F800000)", 0, loadBE(one), 1.0f);
    }

    float six[kSamples * kChannels];
    toInterleaved6(frame, six);
    for (uint32_t i = 0; i < kSamples; i++)
        for (uint32_t c = 0; c < kChannels; c++)
            near(c == FL ? "6ch FL" : c == FR ? "6ch FR" : c == FC ? "6ch FC" : c == LFE ? "6ch LFE" : c == BL ? "6ch BL" : "6ch BR",
                 i, six[i * kChannels + c], value(c, i));

    float stereo[kSamples * 2];
    toStereo(frame, stereo);
    const float k = 0.70710678f;
    for (uint32_t i = 0; i < kSamples; i++)
    {
        near("stereo L", i, stereo[i * 2], value(FL, i) + k * value(FC, i) + k * value(BL, i));
        near("stereo R", i, stereo[i * 2 + 1], value(FR, i) + k * value(FC, i) + k * value(BR, i));
    }

    // Front-only content (the logo movie's sound) passes through unchanged, and LFE never reaches
    // the stereo pair.
    static uint8_t front[kBytes];
    std::memset(front, 0, sizeof(front));
    for (uint32_t i = 0; i < kSamples; i++)
    {
        storeBE(front + (FL * kSamples + i) * 4, 0.5f);
        storeBE(front + (FR * kSamples + i) * 4, -0.25f);
        storeBE(front + (LFE * kSamples + i) * 4, 0.9f);
    }
    toStereo(front, stereo);
    for (uint32_t i = 0; i < kSamples; i++)
    {
        near("front-only L", i, stereo[i * 2], 0.5f);
        near("front-only R", i, stereo[i * 2 + 1], -0.25f);
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
