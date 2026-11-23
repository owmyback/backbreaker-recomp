// Backbreaker runtime: loads the decrypted image into guest memory and runs the entry point.
#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/kernel.h"
#include "kernel/objects.h"
#include "gpu/gpu.h"
#include "apu/apu.h"
#include "gpu/capture.h"
#include "debug/sampler.h"
#include "debug/thread_times.h"
#include "gpu/renderer.h"
#include "video/vk_context.h"
#include "hid/hid.h"
#include "video/presenter.h"
#include "video/overlay.h"
#include "build_id.h"
#include "log.h"
#include "memory.h"
#include "frame_rate.h"
#include "scene_size.h"
#include "intro_movies.h"
#include "log_file.h"
#include "paths.h"
#include "settings.h"

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fstream>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <timeapi.h>
#else
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#endif

PPC_EXTERN_FUNC(_xstart);

namespace
{
    // Every way the process ends prints this, so a run's last lines say what it was doing and
    // whether the Vulkan validation layer had anything to complain about. Last, the console gets
    // what the log file's copies haven't sent it yet (log_file.h), since _exit follows.
    void printExitSummary()
    {
        kernel::printSummary();
        apu::printAudioSummary();
        vkctx::logValidationSummary();
        log_file::drain();
    }

    // The crash report's guest part: the crashing guest thread's registers, scopes and trace
    void printGuestCrashState()
    {
        kernel::GuestThread* gt = kernel::currentThread();
        if (kernel::Thread* th = kernel::currentThreadObject(); th && th->ctx)
        {
            PPCContext& c = *th->ctx;
            std::fprintf(stderr, "---- guest registers, thread %u ----\n", gt ? gt->id : 0);
            PPCRegister* r[] = { &c.r0,  &c.r2,  &c.r3,  &c.r4,  &c.r5,  &c.r6,  &c.r7,  &c.r8,  &c.r9,  &c.r10,
                                 &c.r11, &c.r12, &c.r14, &c.r15, &c.r16, &c.r17, &c.r18, &c.r19, &c.r20, &c.r21,
                                 &c.r22, &c.r23, &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31 };
            static const int num[] = { 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21,
                                       22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
            for (int i = 0; i < 30; i++)
                std::fprintf(stderr, "  r%-2d %08X%s", num[i], r[i]->u32, i % 5 == 4 ? "\n" : "");
            std::fprintf(stderr, "  r1  %08X  r13 %08X  lr  %08X\n", c.r1.u32, c.r13.u32, uint32_t(c.lr));
        }
        kernel::printScopes(gt);
        if (gt) kernel::printTrace(gt->id, 24);
        kernel::printTrace(0, 12);
    }

    // --timeout, kill and Ctrl+C all end a run with this report, then the process
    void endRun(const char* banner, int code)
    {
        std::fprintf(stderr, "\n==== %s ====\n", banner);
        for (kernel::GuestThread* t : kernel::allThreads())
        {
            std::fprintf(stderr, "\n#### thread %u\n", t->id);
            kernel::printScopes(t);
            kernel::printTrace(t->id, 4);
        }
        printExitSummary();
        std::fflush(nullptr);
        _exit(code);
    }

#ifdef _WIN32
    // Windows (#174). First in line for every exception: a write to a page the renderer watches
    // (mem::watch) is not a crash. ExceptionInformation: [0] 1 for a write, [1] the address.
    LONG CALLBACK watchFaults(EXCEPTION_POINTERS* e)
    {
        const EXCEPTION_RECORD* r = e->ExceptionRecord;
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 &&
            mem::watch::onFault((const void*)r->ExceptionInformation[1]))
            return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // An exception nothing handled: the same report as the Linux signal handler. Host frames are
    // offsets into the program (no symbol names here: llvm-symbolizer or the map file reads them).
    LONG WINAPI crashFilter(EXCEPTION_POINTERS* e)
    {
        static std::atomic<bool> crashing{false};
        if (crashing.exchange(true))
            for (;;) Sleep(INFINITE);
        const EXCEPTION_RECORD* r = e->ExceptionRecord;
        std::fprintf(stderr, "\n==== exception 0x%08lX ====\n", r->ExceptionCode);
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        {
            const uintptr_t fault = r->ExceptionInformation[1];
            const char* kind = r->ExceptionInformation[0] == 1 ? "write" : r->ExceptionInformation[0] == 8 ? "execute" : "read";
            if (mem::isGuest((void*)fault))
                std::fprintf(stderr, "fault address (%s): guest %08X\n", kind, uint32_t(fault - (uintptr_t)mem::base));
            else
                std::fprintf(stderr, "fault address (%s): host %p\n", kind, (void*)fault);
        }
        const uintptr_t exe = (uintptr_t)GetModuleHandleW(nullptr);
        std::fprintf(stderr, "host rip: %p (backbreaker.exe+0x%llx)\n", (void*)e->ContextRecord->Rip,
                     (unsigned long long)(e->ContextRecord->Rip - exe));
        void* frames[48];
        const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
        std::fprintf(stderr, "---- host backtrace ----\n");
        for (USHORT i = 0; i < n; i++)
            std::fprintf(stderr, "  %p (backbreaker.exe+0x%llx)\n", frames[i], (unsigned long long)((uintptr_t)frames[i] - exe));
        printGuestCrashState();
        printExitSummary();
        _exit(3);
    }

    BOOL WINAPI consoleCtrl(DWORD type)
    {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
            endRun(type == CTRL_C_EVENT ? "Ctrl+C received" : type == CTRL_BREAK_EVENT ? "Ctrl+Break received" : "console closed", 130);
        return FALSE;
    }

    void blockEndSignals() {}

    void installHandlers()
    {
        AddVectoredExceptionHandler(1, watchFaults);
        SetUnhandledExceptionFilter(crashFilter);
        SetConsoleCtrlHandler(consoleCtrl, TRUE);
    }

    void startTimeout(int seconds)
    {
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            endRun("timeout reached", 124);
        }).detach();
    }
#else
    void crashHandler(int sig, siginfo_t* si, void* uctx)
    {
        // A write to a page the renderer watches for texture changes (mem::watch): not a crash.
        // Timed with BB_PERF (#146): a fault costs the faulting thread a syscall and a page fault.
        if (sig == SIGSEGV)
        {
            const uint64_t t0 = debug::threadtimes::enabled() ? debug::threadtimes::nowNs() : 0;
            if (mem::watch::onFault(si->si_addr))
            {
                if (t0) debug::threadtimes::addFault(debug::threadtimes::nowNs() - t0);
                return;
            }
        }
        // Only the first crashing thread reports; others wait to be torn down with the process.
        static std::atomic<bool> crashing{false};
        if (crashing.exchange(true))
            for (;;) pause();
        auto* uc = (ucontext_t*)uctx;
        uintptr_t fault = (uintptr_t)si->si_addr;
        uintptr_t rip = uc->uc_mcontext.gregs[REG_RIP];
        std::fprintf(stderr, "\n==== %s ====\n", strsignal(sig));
        if (mem::isGuest((void*)fault))
            std::fprintf(stderr, "fault address: guest %08X\n", uint32_t(fault - (uintptr_t)mem::base));
        else
            std::fprintf(stderr, "fault address: host %p\n", (void*)fault);
        std::fprintf(stderr, "host rip: %p\n", (void*)rip);

        void* frames[48];
        int n = backtrace(frames, 48);
        std::fprintf(stderr, "---- host backtrace (sub_XXXXXXXX = guest function) ----\n");
        backtrace_symbols_fd(frames, n, STDERR_FILENO);

        printGuestCrashState();
        printExitSummary();
        _exit(128 + sig);
    }

    // SIGALRM (--timeout), SIGTERM and SIGINT (kill, Ctrl+C) all end a run with the same report.
    // They're blocked in every thread and taken by one watcher thread with sigwait, so the report
    // runs as ordinary code, not in a signal handler on whatever thread the signal landed on.
    // Threads inherit the mask, so main() blocks them before it starts any thread.
    sigset_t endSignals()
    {
        sigset_t set;
        sigemptyset(&set);
        for (int sig : { SIGALRM, SIGTERM, SIGINT })
            sigaddset(&set, sig);
        return set;
    }

    void blockEndSignals()
    {
        sigset_t set = endSignals();
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }

    void endSignalWatcher()
    {
        sigset_t set = endSignals();
        int sig = 0;
        while (sigwait(&set, &sig) != 0) {}
        // A second kill or Ctrl+C while the report prints ends the process at once: this thread
        // is then the only one that takes the signal, with the default action.
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);

        endRun(sig == SIGALRM ? "timeout reached" : sig == SIGTERM ? "SIGTERM received" : "SIGINT received",
               sig == SIGALRM ? 124 : 128 + sig);
    }

    void installHandlers()
    {
        static std::vector<uint8_t> altStack(1 << 16);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);

        struct sigaction sa{};
        sa.sa_sigaction = crashHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        for (int sig : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT })
            sigaction(sig, &sa, nullptr);
        std::thread(endSignalWatcher).detach();
    }

    void startTimeout(int seconds) { alarm(unsigned(seconds)); }
#endif

    void loadImage(const std::string& path)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f)
            fatal("cannot open %s", path.c_str());
        size_t size = f.tellg();
        if (size != PPC_IMAGE_SIZE)
            fatal("%s is 0x%zX bytes, expected 0x%llX (decrypted default.pe)", path.c_str(), size, PPC_IMAGE_SIZE);
        f.seekg(0);
        f.read((char*)mem::host(PPC_IMAGE_BASE), size);
        info("loaded %s at %08llX (0x%zX bytes)", path.c_str(), PPC_IMAGE_BASE, size);
    }

    // Every slot without a recompiled function points here, so a bad indirect call reports
    // its target (bctr/bctrl jump through ctr) instead of jumping to a null host address.
    PPC_FUNC(missingFunction)
    {
        std::fprintf(stderr, "\n==== indirect call to guest %08X: no recompiled function there (caller lr %08X) ====\n",
                     ctx.ctr.u32, uint32_t(ctx.lr));
        kernel::GuestThread* gt = kernel::currentThread();
        kernel::printScopes(gt);
        if (gt) kernel::printTrace(gt->id, 16);
        printExitSummary();
        _exit(3);
    }

    // Calls fn for each item of a comma-separated option value ("300,1200,2400")
    template <typename F> void forEachItem(const std::string& list, F fn)
    {
        for (size_t p = 0; p < list.size();)
        {
            size_t q = std::min(list.find(',', p), list.size());
            if (q > p) fn(list.substr(p, q - p));
            p = q + 1;
        }
    }

    // Seconds for --screenshot-at / --capture-at; false (and a warning) if the item isn't a number
    bool parseSeconds(const std::string& item, const char* option, double& seconds)
    {
        char* end = nullptr;
        seconds = std::strtod(item.c_str(), &end);
        if (end != item.c_str() && *end == 0 && seconds >= 0) return true;
        warn("%s: \"%s\" is not a number of seconds - ignored", option, item.c_str());
        return false;
    }

    void buildFunctionTable()
    {
        uint8_t* base = mem::base;
        for (uint64_t a = PPC_CODE_BASE; a < PPC_CODE_BASE + PPC_CODE_SIZE; a += 4)
            PPC_LOOKUP_FUNC(base, a) = missingFunction;
        size_t n = 0;
        for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, n++)
            PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        info("indirect call table: %zu functions", n);
    }
}

namespace
{
    PPCContext g_mainCtx{};
    kernel::GuestThread* g_mainThread = nullptr;

    // Runs the game's entry point on the calling host thread. Never returns.
    void runGuestMain()
    {
        // Per-thread state: alternate signal stack (Linux), current guest thread, FPU mode
#ifndef _WIN32
        static thread_local std::vector<uint8_t> altStack(1 << 16);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);
#endif
        kernel::setCurrentThread(g_mainThread);
        kernel::adoptCurrentThread(g_mainThread, &g_mainCtx);
        g_mainCtx.fpscr.loadFromHost();

        info("calling entry point _xstart");
        _xstart(g_mainCtx, mem::base);
        info("_xstart returned (r3=%08X)", g_mainCtx.r3.u32);
        printExitSummary();
        _exit(0);
    }

    void* guestThreadEntry(void*)
    {
        runGuestMain();
        return nullptr;
    }
}

int main(int argc, char** argv)
{
#ifdef _WIN32
    _set_fmode(_O_BINARY); // every file the CRT opens is binary, as on Linux (runtime/platform.h)
    // Sleeps and timed waits last whole timer ticks on Windows, 15.6 ms unless a process asks for
    // finer ones (#174): the guest's Sleep(1), the 1 ms time stamp bundle, the command processor's
    // short polls for the GPU. 1 ms, for as long as the game runs (Windows ends it with the process).
    timeBeginPeriod(1);
#endif
    blockEndSignals(); // first: every thread started from here on inherits the mask
    std::string image; // the positional argument; default <data>/extracted/default.pe once paths are known
    paths::Options pathOptions; // --data-dir, --user-dir (#134)
    bool dumpShaders = false;
    int timeout = 0;
    bool headless = false, fullscreen = false, mute = false, audio = false, listAchievements = false;
    int scaleArg = -1; // --resolution-scale; without it, windowed runs use game/settings.ini's (#115)
    int fpsArg = -1;   // --fps, the same way (#89)
    int anisoArg = -1; // --anisotropy, the same way
    int native720pArg = -1; // --native-720p 0|1, the same way
    int skipIntroArg = -1;  // --skip-intro 0|1, the same way
    int showFpsArg = -1;    // --show-fps 0|1, the same way (#146)
    std::string overlayPreview;
    bool overlayPreviewDetail = false;
    std::string audioDump;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--version")
        {
            // On stdout, unlike the log: it is the answer to a question, not a run's output, and
            // this is what a bug report should quote. It exits before any game file is touched.
            std::printf("backbreaker %s\n", build_id::line());
            return 0;
        }
        else if (a == "--timeout" && i + 1 < argc)
            timeout = std::atoi(argv[++i]);
        else if (a == "--data-dir" && i + 1 < argc)
            pathOptions.dataDir = argv[++i]; // the game's files (disc/, extracted/); player files go to the per-user folders
        else if (a == "--user-dir" && i + 1 < argc)
            pathOptions.userDir = argv[++i]; // all of the player's files (settings, saves, screenshots, ...) in one directory
        else if (a == "--fixed-seed")
            kernel::setFixedSeed(true); // the same random numbers in every test run (issue #71)
        else if (a == "--headless")
            headless = true;
        else if (a == "--mute")
            mute = true; // no audio device; the game's audio still runs (and --audio-dump still works)
        else if (a == "--audio")
            audio = true; // play audio in a headless run too
        else if (a == "--audio-dump" && i + 1 < argc)
            audioDump = argv[++i]; // the stereo output as a 32-bit float WAV
        else if ((a == "--overlay-preview" || a == "--overlay-preview-detail") && i + 1 < argc)
        {
            overlayPreview = argv[++i]; // file.png or file.png@WxH: draw the achievements overlay into a PNG, then exit
            overlayPreviewDetail = a == "--overlay-preview-detail"; // with one achievement open in full
        }
        else if (a == "--achievements")
            listAchievements = true; // print the list with what is unlocked, then exit
        else if (a == "--fullscreen")
            fullscreen = true; // windowed runs only; see game/settings.ini (issue #9)
        else if (a == "--sample-threads" && i + 1 < argc)
            debug::scheduleThreadSampling(std::atof(argv[++i])); // hang diagnosis: see docs/runtime.md
        else if (a == "--no-render")
            gpu::render::disable();
        else if (a == "--resolution-scale" && i + 1 < argc)
            scaleArg = std::atoi(argv[++i]);
        else if (a == "--fps" && i + 1 < argc)
        {
            fpsArg = std::atoi(argv[++i]);
            if (fpsArg != 30 && fpsArg != 60)
            {
                std::fprintf(stderr, "--fps %s: 30 or 60\n", argv[i]);
                return 2;
            }
        }
        else if (a == "--skip-intro" && i + 1 < argc)
        {
            skipIntroArg = std::atoi(argv[++i]);
            if (skipIntroArg != 0 && skipIntroArg != 1)
            {
                std::fprintf(stderr, "--skip-intro %s: 0 or 1\n", argv[i]);
                return 2;
            }
        }
        else if (a == "--show-fps" && i + 1 < argc)
        {
            showFpsArg = std::atoi(argv[++i]);
            if (showFpsArg != 0 && showFpsArg != 1)
            {
                std::fprintf(stderr, "--show-fps %s: 0 or 1\n", argv[i]);
                return 2;
            }
        }
        else if (a == "--native-720p" && i + 1 < argc)
        {
            native720pArg = std::atoi(argv[++i]);
            if (native720pArg != 0 && native720pArg != 1)
            {
                std::fprintf(stderr, "--native-720p %s: 0 or 1\n", argv[i]);
                return 2;
            }
        }
        else if (a == "--anisotropy" && i + 1 < argc)
            anisoArg = std::atoi(argv[++i]);
        else if (a == "--no-shader-cache")
            gpu::render::setShaderCache(false);
        else if (a == "--screenshot" && i + 1 < argc)
            // frame numbers, comma-separated: --screenshot 600,1200 -> screenshots/frame_600.png, ...
            forEachItem(argv[++i], [](const std::string& n) { gpu::render::requestScreenshot(std::strtoull(n.c_str(), nullptr, 10)); });
        else if (a == "--screenshot-at" && i + 1 < argc)
            // seconds since launch (the --press clock): --screenshot-at 200,230 -> screenshots/t_200.png, ...
            forEachItem(argv[++i], [](const std::string& s) {
                double t;
                if (parseSeconds(s, "--screenshot-at", t)) gpu::render::requestScreenshotAt(t, s);
            });
        else if (a == "--dump-shaders")
            dumpShaders = true; // into <captures>/shaders, once paths are known
        else if (a == "--press" && i + 1 < argc)
        {
            if (!hid::setScript(argv[++i]))
                return 2;
            hid::setVirtualPad(true); // user 0 stays connected for the scripted presses
        }
        else if (a == "--capture-frame" && i + 1 < argc)
            // one frame number or a comma-separated list: --capture-frame 300,1500,3000
            forEachItem(argv[++i], [](const std::string& n) { gpu::capture::request(std::strtoull(n.c_str(), nullptr, 10)); });
        else if (a == "--capture-at" && i + 1 < argc)
            // seconds since launch, like --screenshot-at; still lands in captures/frame_N/
            forEachItem(argv[++i], [](const std::string& s) {
                double t;
                if (parseSeconds(s, "--capture-at", t)) gpu::capture::requestAt(t);
            });
        else
            image = a;
    }

    installHandlers();
    paths::init(pathOptions);
    // The installed layout keeps a log file (#146): started before the first line, so it has them all
    const std::string logFile = paths::logs().empty() ? std::string() : log_file::start(paths::logs());
    info("built from %s", build_id::line()); // first line of every log says what made it (#75)
    paths::printLayout();
    if (!logFile.empty())
        info("[log] this run's log: %s (the two runs before it: backbreaker.1.log, backbreaker.2.log)", logFile.c_str());
    if (image.empty())
        image = paths::data() + "/extracted/default.pe";
    const std::string xex = paths::data() + "/extracted/default.xex";
    if (dumpShaders)
        gpu::capture::dumpAllShaders(paths::captures() + "/shaders");

    // Render scale of the 3D scenes and the frame rate. A flag wins for this run and leaves the file
    // alone. Headless runs (test routes, scripted screenshots) don't read the file, so what they
    // produce doesn't depend on the player's settings: scale 1 and the console's 30 fps.
    const settings::Settings file = headless ? settings::Settings{} : settings::load(paths::settingsFile());
    const char* settingsName = paths::settingsFile().c_str();
    uint32_t scale = scaleArg >= 0 ? uint32_t(std::clamp(scaleArg, 1, 4))
                   : headless      ? 1u
                                   : file.resolutionScale;
    gpu::render::setResolutionScale(scale);
    if (scale != 1)
        info("[gpu] resolution scale %u (%s)", scale, scaleArg >= 0 ? "--resolution-scale" : settingsName);
    const uint32_t fps = fpsArg >= 0 ? uint32_t(fpsArg) : headless ? 30u : file.fps;
    frame_rate::set(fps);
    info("%u fps (%s)", fps, fpsArg >= 0 ? "--fps" : headless ? "headless" : settingsName);
    const uint32_t aniso = anisoArg >= 0 ? uint32_t(std::clamp(anisoArg, 1, 16)) : headless ? 1u : file.anisotropy;
    gpu::render::setAnisotropy(aniso);
    if (aniso > 1)
        info("[gpu] anisotropic filtering %ux (%s)", aniso, anisoArg >= 0 ? "--anisotropy" : settingsName);
    const bool native720p = native720pArg >= 0 ? native720pArg == 1 : !headless && file.native720p;
    scene_size::setNative720p(native720p);
    const bool skipIntro = skipIntroArg >= 0 ? skipIntroArg == 1 : headless || file.skipIntro;
    intro_movies::setSkip(skipIntro);
    info("[movie] boot logos %s (%s)", skipIntro ? "skipped" : "shown",
         skipIntroArg >= 0 ? "--skip-intro" : headless ? "headless" : settingsName);
    info("[gpu] matches render at %s (%s)", native720p ? "1280x720" : "the game's 1152x640, stretched to 1280x720",
         native720pArg >= 0 ? "--native-720p" : headless ? "headless" : settingsName);
    // The game's own frame rate and speed on screen (#146); a window's overlay, so never headless
    const bool showFps = !headless && (showFpsArg >= 0 ? showFpsArg == 1 : file.showFps);
    overlay::setShowFps(showFps);
    if (showFps)
        info("[overlay] fps readout on (%s)", showFpsArg >= 0 ? "--show-fps" : settingsName);
    mem::init();
    loadImage(image);
    buildFunctionTable();
    kernel::loadXexHeader(xex);
    kernel::patchImports();
    kernel::startTimeStampThread(); // keeps KeTimeStampBundle current while the game runs
    kernel::initFileSystem(paths::data() + "/disc", paths::user() + "/cache", paths::user() + "/saves");
    if (std::filesystem::is_directory(paths::mods()))
        kernel::setDiscOverlay(paths::mods());
    kernel::initAchievements(paths::user() + "/achievements");
    if (listAchievements)
    {
        kernel::printAchievements();
        return 0;
    }
    if (!overlayPreview.empty())
    {
        uint32_t w = 1920, h = 1080;
        if (size_t at = overlayPreview.rfind('@'); at != std::string::npos)
        {
            std::sscanf(overlayPreview.c_str() + at + 1, "%ux%u", &w, &h);
            overlayPreview.resize(at);
        }
        return overlay::writePreview(overlayPreview.c_str(), w, h, overlayPreviewDetail) ? 0 : 1;
    }

    g_mainThread = kernel::createGuestThread(g_mainCtx, 0x100000);
    gpu::init();
    apu::initXma();
    apu::configureAudio((!headless || audio) && !mute, audioDump); // headless runs are silent unless --audio
    apu::initAudio();

    if (timeout > 0)
        startTimeout(timeout);

    if (headless)
    {
        hid::setVirtualPad(true); // keep user 0 connected for headless test runs
        hid::startInputThread();  // real controllers still work, polled on their own thread
        runGuestMain(); // no window: the game runs on the process main thread, as before
    }

    vkctx::expectWindow(); // the renderer waits for the window's Vulkan device

    // Windowed: the game runs on its own host thread (large stack: recompiled code recurses on
    // the host stack) and the process main thread drives the window, as SDL expects.
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);
    pthread_t guest;
    if (int err = pthread_create(&guest, &attr, guestThreadEntry, nullptr))
        fatal("could not start the guest main thread: %s", strerror(err));
    pthread_attr_destroy(&attr);

    video::run(fullscreen);
    info("window closed - exiting");
    printExitSummary();
    _exit(0);
}
