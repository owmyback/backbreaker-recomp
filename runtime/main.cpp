// Backbreaker runtime: loads the decrypted image into guest memory and runs the entry point.
#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/kernel.h"
#include "kernel/objects.h"
#include "log.h"
#include "memory.h"

#include <csignal>
#include <cstring>
#include <execinfo.h>
#include <fstream>
#include <string>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

PPC_EXTERN_FUNC(_xstart);

namespace
{
    void crashHandler(int sig, siginfo_t* si, void* uctx)
    {
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
        kernel::printTrace();
        kernel::printSummary();
        _exit(128 + sig);
    }

    void timeoutHandler(int)
    {
        std::fprintf(stderr, "\n==== timeout reached ====\n");
        kernel::printTrace();
        kernel::printSummary();
        _exit(124);
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
        signal(SIGALRM, timeoutHandler);
    }

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

    void buildFunctionTable()
    {
        uint8_t* base = mem::base;
        size_t n = 0;
        for (PPCFuncMapping* m = PPCFuncMappings; m->host; m++, n++)
            PPC_LOOKUP_FUNC(base, m->guest) = m->host;
        info("indirect call table: %zu functions", n);
    }
}

int main(int argc, char** argv)
{
    std::string image = "game/extracted/default.pe";
    std::string xex = "game/extracted/default.xex";
    int timeout = 0;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--timeout" && i + 1 < argc)
            timeout = std::atoi(argv[++i]);
        else
            image = a;
    }

    installHandlers();
    mem::init();
    loadImage(image);
    buildFunctionTable();
    kernel::loadXexHeader(xex);
    kernel::patchImports();
    kernel::initFileSystem("game/disc", "game/cache");

    static PPCContext ctx{};
    ctx.fpscr.loadFromHost();
    kernel::GuestThread* mainThread = kernel::createGuestThread(ctx, 0x100000);
    kernel::setCurrentThread(mainThread);
    kernel::adoptCurrentThread(mainThread, &ctx);

    if (timeout > 0)
        alarm(timeout);

    info("calling entry point _xstart");
    _xstart(ctx, mem::base);
    info("_xstart returned (r3=%08X)", ctx.r3.u32);
    kernel::printSummary();
    return 0;
}
