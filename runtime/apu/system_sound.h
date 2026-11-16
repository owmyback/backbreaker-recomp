#pragma once
// Sounds the runtime itself plays over the game's audio (issue #105). The console's own system
// sounds (the achievement "unlocked" sound) belong to the dashboard, not the game, so these are
// synthesized here. Header-only so tests/system_sound_test.cpp can check them.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace apu::sound
{
    // The unlock sound: a short, bright "pip-pop" (about 0.45 s, mono, peak kChimePeak, starting
    // and ending at exactly 0). A soft 45 ms pickup on B5 (987.8 Hz), then at 70 ms the main tone
    // on E6 (1318.5 Hz), a fourth higher, which glides up into pitch over its first 25 ms (from
    // 0.86 x, the "bloop") with its 2nd harmonic, decays fast (tau 110 ms) and leaves a quiet
    // shimmer an octave and a fifth up (B6 + E7) that fades by the end. Original to this project:
    // the console's own achievement sound is not in the game files and is not reproduced.
    constexpr float kChimePeak = 0.60f; // -4.4 dBFS; with the game ducked under it (below), 7-8 dB over the game in play
    constexpr double kPickupHz = 987.77, kMainHz = 1318.51, kMainStart = 0.070, kLength = 0.45;

    // While the sound plays the game's own mix is turned down (ducked), as consoles do under their
    // system sounds: in play the game runs at about -18 dB RMS (median) and up to -10 dB under a
    // touchdown crowd, louder than the sound itself, which a peak at 0 dBFS could not beat without
    // clipping. kDuckDb under the sound: down over kDuckAttack as it starts, held to its end, back
    // up over kDuckRelease. The ramps are raised-cosine, so there are no clicks.
    constexpr double kDuckDb = -10.0, kDuckAttack = 0.015, kDuckRelease = 0.30;

    // How long the ducking outlasts a sound (in samples)
    inline size_t duckTail(uint32_t rate) { return size_t(kDuckRelease * rate); }

    // The game's gain at sample pos of a sound length samples long (1 before and after)
    inline float duckGain(size_t pos, size_t length, uint32_t rate)
    {
        constexpr double kPi = 3.14159265358979323846;
        const double t = double(pos) / rate, end = double(length) / rate;
        double x; // 0 = the game at full level, 1 = fully ducked
        if (t < kDuckAttack) x = t / kDuckAttack;
        else if (t < end) x = 1;
        else if (t < end + kDuckRelease) x = 1 - (t - end) / kDuckRelease;
        else x = 0;
        x = 0.5 - 0.5 * std::cos(kPi * x);
        return float(1 - (1 - std::pow(10.0, kDuckDb / 20)) * x);
    }

    inline std::vector<float> unlockChime(uint32_t rate = 48000)
    {
        constexpr double kPi = 3.14159265358979323846;
        const size_t n = size_t(kLength * rate);
        std::vector<double> mix(n, 0.0);
        auto envelope = [](double t, double attack, double tau) {
            return t < 0 ? 0.0 : (t < attack ? t / attack : 1.0) * std::exp(-t / tau);
        };
        double mainPhase = 0, shimmerPhase1 = 0, shimmerPhase2 = 0;
        for (size_t i = 0; i < n; i++)
        {
            const double t = double(i) / rate;
            // Pickup: short and soft, a sine with a touch of 2nd harmonic
            const double pe = 0.38 * envelope(t, 0.003, 0.028) * (t < 0.045 ? 1.0 : std::max(0.0, 1.0 - (t - 0.045) / 0.010));
            const double pw = 2 * kPi * kPickupHz * t;
            double v = pe * (std::sin(pw) + 0.2 * std::sin(2 * pw));
            // Main tone: pitch glides 0.86 -> 1.0 over 25 ms (phase accumulated, no clicks)
            const double tm = t - kMainStart;
            if (tm >= 0)
            {
                const double glide = tm < 0.025 ? 0.86 + 0.14 * (tm / 0.025) : 1.0;
                mainPhase += 2 * kPi * kMainHz * glide / rate;
                v += envelope(tm, 0.003, 0.110) * (std::sin(mainPhase) + 0.45 * std::sin(2 * mainPhase) + 0.10 * std::sin(3 * mainPhase));
                // Shimmer: quiet, slower decay, an octave + fifth and two octaves above the main note's root
                shimmerPhase1 += 2 * kPi * (kMainHz * 1.5) / rate;
                shimmerPhase2 += 2 * kPi * (kMainHz * 2.0) / rate;
                v += 0.12 * envelope(tm, 0.010, 0.170) * (std::sin(shimmerPhase1) + 0.6 * std::sin(shimmerPhase2));
            }
            mix[i] = v;
        }
        const double fade = 0.04;
        double peak = 0;
        for (size_t i = 0; i < n; i++)
        {
            const double t = double(i) / rate;
            if (t > kLength - fade) mix[i] *= (kLength - t) / fade; // linear fade to exactly 0
            peak = std::max(peak, std::fabs(mix[i]));
        }
        mix[0] = 0;
        mix[n - 1] = 0;
        std::vector<float> out(n);
        for (size_t i = 0; i < n; i++)
            out[i] = float(mix[i] * kChimePeak / peak);
        return out;
    }
}
