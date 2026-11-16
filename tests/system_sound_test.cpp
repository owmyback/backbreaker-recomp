// Checks runtime/apu/system_sound.h (issue #105): the synthesized unlock sound. No game files.
// Build: clang++ -std=c++20 -Wall -Wextra -O2 tests/system_sound_test.cpp -o build/system_sound_test
//
// What it must be: 0.45 s at 48 kHz, peak exactly kChimePeak (so it never clips over the game and
// its loudness is fixed), silent at both ends (no click when it starts or stops), no DC; a soft
// B5 pickup first, then the louder E6 main tone that glides up into pitch - checked with a
// Goertzel filter per frequency over short windows.
#include "../runtime/apu/system_sound.h"

#include <cmath>
#include <cstdio>
#include <string>

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

// Power of one frequency in a window (Goertzel)
static double power(const std::vector<float>& x, size_t from, size_t count, double freq, double rate)
{
    const double k = 2 * std::cos(2 * 3.14159265358979323846 * freq / rate);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + count && i < x.size(); i++)
    {
        double s = x[i] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

int main()
{
    const uint32_t rate = 48000;
    const std::vector<float> c = apu::sound::unlockChime(rate);
    expect(c.size() == size_t(apu::sound::kLength * rate), "0.45 s long: " + std::to_string(c.size()) + " samples");

    float peak = 0;
    double sum = 0;
    bool finite = true;
    for (float v : c)
    {
        peak = std::max(peak, std::fabs(v));
        sum += v;
        finite &= std::isfinite(v);
    }
    expect(finite, "all samples finite");
    expect(std::fabs(peak - apu::sound::kChimePeak) < 1e-6f, "peak is kChimePeak: " + std::to_string(peak));
    expect(std::fabs(sum / c.size()) < 1e-3, "no DC offset: mean " + std::to_string(sum / c.size()));
    expect(c.front() == 0.0f && c.back() == 0.0f, "starts and ends at 0");
    expect(std::fabs(c[1]) < 0.01f && std::fabs(c[c.size() - 2]) < 0.01f, "no step at either end");

    // The pickup first: in the first 40 ms, B5 is far stronger than E6
    const size_t w20 = rate * 20 / 1000;
    expect(power(c, rate * 5 / 1000, w20, apu::sound::kPickupHz, rate) > 20 * power(c, rate * 5 / 1000, w20, apu::sound::kMainHz, rate),
           "pickup (B5) first, before the main tone");
    // Then the main tone: from 100 ms on, E6 dominates B5
    expect(power(c, rate * 100 / 1000, w20, apu::sound::kMainHz, rate) > 20 * power(c, rate * 100 / 1000, w20, apu::sound::kPickupHz, rate),
           "main tone (E6) after the pickup");
    // The main tone is the loudest part: its 20 ms after onset carry more energy than the pickup's
    double pickupE = 0, mainE = 0;
    for (size_t i = rate * 5 / 1000; i < rate * 25 / 1000; i++) pickupE += double(c[i]) * c[i];
    for (size_t i = size_t((apu::sound::kMainStart + 0.005) * rate); i < size_t((apu::sound::kMainStart + 0.025) * rate); i++) mainE += double(c[i]) * c[i];
    expect(mainE > 2 * pickupE, "the main tone is louder than the pickup");
    // The glide: right after the main onset the tone sits below E6 (around 0.9 x), later at E6
    const size_t gw = rate * 10 / 1000, g0 = size_t((apu::sound::kMainStart + 0.004) * rate);
    expect(power(c, g0, gw, apu::sound::kMainHz * 0.9, rate) > power(c, g0, gw, apu::sound::kMainHz, rate),
           "the main tone starts below its pitch (glide)");

    // It decays: the last 80 ms are much quieter than the first 150 ms.
    double early = 0, late = 0;
    for (size_t i = 0; i < size_t(0.15 * rate); i++) early += double(c[i]) * c[i];
    for (size_t i = c.size() - size_t(0.08 * rate); i < c.size(); i++) late += double(c[i]) * c[i];
    expect(late / (0.08 * rate) < 0.01 * early / (0.15 * rate), "decays to near silence");

    // Ducking: the game at full level as the sound starts, kDuckDb under it, back to full after
    // the release, with no step anywhere (a step is a click)
    using apu::sound::duckGain;
    const float ducked = float(std::pow(10.0, apu::sound::kDuckDb / 20));
    const size_t len = c.size(), tail = apu::sound::duckTail(rate);
    expect(duckGain(0, len, rate) == 1.0f, "game at full level at the sound's first sample");
    expect(std::fabs(duckGain(len / 2, len, rate) - ducked) < 1e-6f, "game kDuckDb down under the sound: " + std::to_string(duckGain(len / 2, len, rate)));
    expect(std::fabs(duckGain(len - 1, len, rate) - ducked) < 1e-6f, "still down at the sound's end");
    expect(std::fabs(duckGain(len + tail, len, rate) - 1.0f) < 1e-6f, "back to full level after the release");
    float maxStep = 0;
    for (size_t i = 1; i <= len + tail; i++) maxStep = std::max(maxStep, std::fabs(duckGain(i, len, rate) - duckGain(i - 1, len, rate)));
    expect(maxStep < 0.002f, "no step in the ducking (largest " + std::to_string(maxStep) + " per sample)");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
