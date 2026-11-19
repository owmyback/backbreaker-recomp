// Building the game on the player's machine (#138): XenonRecomp turns the player's executable into
// ppc/, the bundled clang compiles it, and lld links it with the import glue against the runtime
// kit - the same steps as CMakeLists.txt and packaging/installer/link_game.sh, with nothing taken
// from the host but glibc and the kernel. Everything comes from the installer's bundle
// (packaging/installer/build_installer.sh), next to the program:
//   <bundle>/kit/           the runtime kit (CMakeLists.txt, "The runtime kit")
//   <bundle>/xenonrecomp/   XenonRecomp (built with patches/) and its ppc_context.h
//   <bundle>/config/        config/backbreaker.toml and the switch tables
//   <bundle>/toolchain/     clang, lld and their libraries
//   <bundle>/sysroot/       Ubuntu 22.04's glibc, libstdc++ and Vulkan loader for linking (glibc 2.35 floor)
// and the install folder (by default ~/.local/share/backbreaker-recomp/install) gets the checkout's
// layout, which XenonRecomp's config expects: config/, game/disc, game/extracted, ppc/ (headers
// only, once compiled), obj/ and bin/backbreaker.
//
// On Windows (#174) the bundle is the folder `bundle` beside the program (the release zip), its
// toolchain is llvm-mingw (clang, lld and MinGW-w64 with the UCRT, its headers in toolchain/include
// and its libraries in toolchain/x86_64-w64-mingw32/lib: no sysroot of its own), programs end in .exe, and the
// install folder is %LOCALAPPDATA%\backbreaker-recomp\install, with bin/backbreaker.exe.
#pragma once
#include "platform.h"

#include <functional>
#include <string>
#include <vector>

namespace installer
{
    using Progress = std::function<void(double fraction, const std::string& step)>;

    struct Bundle
    {
        std::string root, kit, xenonRecomp, ppcContext, config, cxx, sysroot, toolchainLib;
        std::string xenonRecompSource; // xenonrecomp/SOURCE: the commit and patch it was built from
    };
    // `dir` when given, else <this program's folder>/../lib/backbreaker-installer
    bool findBundle(const std::string& dir, Bundle& bundle, std::string& error);

    // A hash of what the bundle gives ppc/ (the game's code translated and compiled): XenonRecomp's
    // source (xenonrecomp/SOURCE, or the program itself in a bundle without it) and ppc_context.h,
    // the config, the kit's compile arguments and force-included header, and the installer's own
    // flags. Not the XenonRecomp program's bytes: a new build container's clang makes a different
    // file from the same source, and the translation is the same. A new bundle with the same hash
    // only relinks; the release workflow compares it with the last release's (#160), since
    // RecompLauncher reruns `install` only when prepare.version changes.
    std::string bundlePpcInputs(const Bundle& bundle);

    // $XDG_DATA_HOME when absolute (as the specification says), else ~/.local/share; %LOCALAPPDATA% on Windows
    std::string dataHome();
    std::string defaultPrefix(); // <dataHome()>/backbreaker-recomp/install
    std::string gameProgram();   // the game inside the install folder: bin/backbreaker (.exe on Windows)
    // Parallel compiles that fit: the cores, and free memory at 1 GiB a job (the biggest ppc/ file
    // peaks at about 720 MB without debug info). `why` says which limit applied.
    unsigned chooseJobs(std::string& why);

    struct BuildOptions
    {
        std::string prefix;
        unsigned jobs = 0;     // 0: chooseJobs()
        bool recompile = false; // run XenonRecomp and compile ppc/ again even when current
        bool relink = false;    // link again even when current
    };

    enum class BuildResult { UpToDate, Relinked, Rebuilt, Failed };
    // Makes <prefix>/bin/backbreaker current with the bundle, from <prefix>/game/extracted:
    // ppc/ is recompiled only when its inputs changed (bundlePpcInputs and the executable), and the
    // game relinked only when the kit or ppc/ changed.
    // Progress runs from `from` to `to`.
    BuildResult buildGame(const Bundle& bundle, const BuildOptions& options, const Progress& progress, double from,
                          double to, std::string& error);

    // Holds <prefix>/.lock so two installers don't build into one folder at once
    class InstallLock
    {
    public:
        bool acquire(const std::string& prefix, std::string& error);

    private:
        platform::FileLock lock_;
    };
}
