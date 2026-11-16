#pragma once
#include <cstdint>
#include <string>

namespace apu
{
    // XMA decoder (xma.cpp): context array, kick/lock/clear registers; a kick decodes on the
    // kicking thread
    void initXma();
    void mmioStore(uint32_t reg, uint32_t value); // value in register (little-endian) order
    uint64_t xmaFramesDecoded();                  // decoded + played as silence
    struct XmaCounters
    {
        uint64_t kicks, decoded, silent, decodeErrors, parseErrors;
        uint64_t passNs, decodeNs; // time in kick passes, and the part of it in the frame decoder
    };
    XmaCounters xmaCounters();

    // Audio output (audio.cpp). configureAudio before initAudio; both on the main thread before
    // the game starts. device = play on the default SDL playback device; dumpPath = also write
    // the stereo output to this WAV file (empty: no dump).
    void configureAudio(bool device, const std::string& dumpPath);
    void initAudio();
    void printAudioSummary(); // exit report: frame counts, underruns, per-channel levels

    // The runtime's unlock chime (#105), mixed over the game's audio from the next frame on; any
    // thread. Restarts it if it is already playing.
    void playUnlockSound();
}
