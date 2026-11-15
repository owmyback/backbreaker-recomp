#pragma once
#include <cstdint>
#include <string>

namespace apu
{
    // XMA decoder: context array, kick/lock/clear registers, decoding thread
    void initXma();
    void mmioStore(uint32_t reg, uint32_t value); // value in register (little-endian) order
    uint64_t xmaFramesDecoded();

    // Audio output (audio.cpp). configureAudio before initAudio; both on the main thread before
    // the game starts. device = play on the default SDL playback device; dumpPath = also write
    // the stereo output to this WAV file (empty: no dump).
    void configureAudio(bool device, const std::string& dumpPath);
    void initAudio();
    void printAudioSummary(); // exit report: frame counts, underruns, per-channel levels
}
