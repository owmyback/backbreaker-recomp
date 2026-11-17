// The build targets x86-64-v3 (AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE): the recompiled game's vector
// code leans on it. Every x86 desktop CPU since 2013-2015 and the Steam Deck's have it. On one
// without, the first such instruction would end the process with SIGILL and no explanation, so this
// file - built for baseline x86-64 (CMakeLists.txt) - checks before any other constructor runs and
// says what is missing instead, also in a message box, since a launch from a menu or from Steam's
// Gaming Mode shows no terminal (#134).
#include <SDL3/SDL_messagebox.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace
{
    __attribute__((constructor(101))) void checkCpu()
    {
        __builtin_cpu_init();
        std::string missing;
        auto need = [&](bool has, const char* name) {
            if (!has) missing += missing.empty() ? name : std::string(", ") + name;
        };
        need(__builtin_cpu_supports("avx"), "AVX");
        need(__builtin_cpu_supports("avx2"), "AVX2");
        need(__builtin_cpu_supports("fma"), "FMA");
        need(__builtin_cpu_supports("bmi"), "BMI1");
        need(__builtin_cpu_supports("bmi2"), "BMI2");
        need(__builtin_cpu_supports("f16c"), "F16C");
        need(__builtin_cpu_supports("movbe"), "MOVBE");
        need(__builtin_cpu_supports("popcnt"), "POPCNT");
        if (missing.empty())
            return;
        std::string text = "This build needs a CPU with x86-64-v3 instructions (Intel Haswell / AMD Excavator, "
                           "2013-2015, or newer). This CPU lacks: " + missing + ".";
        std::fprintf(stderr, "backbreaker: %s\n", text.c_str());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Backbreaker", text.c_str(), nullptr);
        _exit(1);
    }
}
