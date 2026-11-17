// Is the player's image Backbreaker, and the release this project's runtime is built for? (#138)
// The runtime hooks functions at fixed addresses (runtime/*.cpp) and is built for one executable
// layout (runtime/sdk/ppc_config.h), so only a known executable can be installed. Checked in order,
// each failure with its own message and exit code (backbreaker-installer, installer/main.cpp):
//   1. an Xbox 360 disc image at all             (NotXbox360)
//   2. a game disc: it has /default.xex          (NotXbox360)
//   3. every file lies inside the image          (Damaged: a cut-short copy)
//   4. the title ID is Backbreaker's             (OtherGame)
//   5. default.xex's SHA-256 is a known release  (Damaged: a damaged dump, or an untested release)
#pragma once
#include "disc.h"
#include "xex.h"

#include <cstdint>
#include <string>
#include <vector>

namespace installer
{
    constexpr uint32_t kBackbreakerTitleId = 0x464F07DA; // execution info and the SPA's XTHD block agree

    struct KnownRelease
    {
        const char* xexSha256;
        const char* name;
    };
    // default.xex of each release known to work. A hash, not game content.
    extern const KnownRelease kKnownReleases[];
    extern const size_t kKnownReleaseCount;

    enum class CheckStatus { Ok, CannotOpen, NotXbox360, OtherGame, Damaged };

    struct CheckResult
    {
        CheckStatus status = CheckStatus::CannotOpen;
        std::string message;     // for the player; never contains the image's path
        std::string release;     // the known release's name, when Ok
        ExecutionInfo info;      // when default.xex could be read
        std::string xexSha256;   // likewise
        std::vector<DiscFile> files;
    };

    // Reads the file system and default.xex (about 10 MB), not the whole image
    CheckResult checkImage(const DiscImage& image);
    CheckResult checkImage(const std::string& path, DiscImage& image);
}
