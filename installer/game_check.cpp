#include "game_check.h"

#include "crypto.h"

#include <cstdio>

namespace installer
{
    const KnownRelease kKnownReleases[] = {
        // Backbreaker (USA, Europe), disc 1/1, version 2: the image this project is developed with
        {"130f4ca5f0076c2895ce47961d255c5f90c6ec06f02742dfb7fc9e0e1680c347", "Backbreaker (USA, Europe)"},
    };
    const size_t kKnownReleaseCount = sizeof(kKnownReleases) / sizeof(kKnownReleases[0]);

    CheckResult checkImage(const DiscImage& image)
    {
        CheckResult r;
        std::string error;
        if (!image.list(r.files, error)) {
            r.status = CheckStatus::Damaged;
            r.message = error;
            return r;
        }
        const DiscFile* xex = nullptr;
        for (const DiscFile& f : r.files)
            if (f.path == "/default.xex") xex = &f;
        if (!xex) {
            r.status = CheckStatus::NotXbox360;
            r.message = "this Xbox 360 disc has no default.xex, so it isn't a game disc";
            return r;
        }
        for (const DiscFile& f : r.files) {
            if (image.end(f) > image.imageSize()) {
                r.status = CheckStatus::Damaged;
                r.message = "the image is cut short: " + f.path + " runs past its end (an incomplete copy or download)";
                return r;
            }
        }
        std::vector<uint8_t> data;
        Xex x;
        if (!image.read(*xex, data, error) || !x.parse(std::move(data), error)) {
            r.status = CheckStatus::Damaged;
            r.message = error;
            return r;
        }
        Sha256 sha;
        sha.update(x.bytes().data(), x.bytes().size());
        const auto digest = sha.finish();
        r.xexSha256 = toHex(digest.data(), digest.size());
        if (!x.executionInfo(r.info)) {
            r.status = CheckStatus::Damaged;
            r.message = "default.xex has no execution info (title ID): the image is damaged";
            return r;
        }
        if (r.info.titleId != kBackbreakerTitleId) {
            char m[160];
            std::snprintf(m, sizeof(m), "this is another Xbox 360 game (title ID %08X), not Backbreaker (%08X)",
                          r.info.titleId, kBackbreakerTitleId);
            r.status = CheckStatus::OtherGame;
            r.message = m;
            return r;
        }
        for (size_t i = 0; i < kKnownReleaseCount; i++) {
            if (r.xexSha256 == kKnownReleases[i].xexSha256) {
                r.status = CheckStatus::Ok;
                r.release = kKnownReleases[i].name;
                r.message = "this is " + r.release;
                return r;
            }
        }
        r.status = CheckStatus::Damaged;
        r.message = "this is Backbreaker, but its executable isn't the known release (SHA-256 " + r.xexSha256 +
                    "): the image is damaged, or it is a release this project hasn't been made for";
        return r;
    }

    CheckResult checkImage(const std::string& path, DiscImage& image)
    {
        CheckResult r;
        switch (image.open(path, r.message)) {
        case DiscImage::OpenResult::CannotOpen: r.status = CheckStatus::CannotOpen; return r;
        case DiscImage::OpenResult::NotXbox360: r.status = CheckStatus::NotXbox360; return r;
        case DiscImage::OpenResult::Ok: break;
        }
        return checkImage(image);
    }
}
