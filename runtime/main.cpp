// Backbreaker runtime: loads the decrypted image into guest memory and runs the entry point.
#include <ppc_config.h>
#include <ppc_context.h>

#include "kernel/kernel.h"
#include "kernel/objects.h"
#include "gpu/gpu.h"
#include "apu/apu.h"
#include "gpu/capture.h"
#include "log.h"
#include "memory.h"

#include <atomic>
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

        kernel::GuestThread* gt = kernel::currentThread();
        if (kernel::Thread* th = kernel::currentThreadObject(); th && th->ctx)
        {
            PPCContext& c = *th->ctx;
            std::fprintf(stderr, "---- guest registers, thread %u ----\n", gt ? gt->id : 0);
            PPCRegister* r[] = { &c.r3, &c.r4, &c.r5, &c.r6, &c.r7, &c.r8, &c.r9, &c.r10, &c.r11, &c.r12 };
            for (int i = 0; i < 10; i++)
                std::fprintf(stderr, "  r%-2d %08X%s", i + 3, r[i]->u32, i % 5 == 4 ? "\n" : "");
            PPCRegister* nv[] = { &c.r24, &c.r25, &c.r26, &c.r27, &c.r28, &c.r29, &c.r30, &c.r31 };
            for (int i = 0; i < 8; i++)
                std::fprintf(stderr, "  r%-2d %08X%s", i + 24, nv[i]->u32, i % 4 == 3 ? "\n" : "");
            std::fprintf(stderr, "  r1  %08X  r13 %08X  lr  %08X\n", c.r1.u32, c.r13.u32, uint32_t(c.lr));
        }
        kernel::printScopes(gt);
        if (gt) kernel::printTrace(gt->id, 24);
        kernel::printTrace(0, 12);
        kernel::printSummary();
        _exit(128 + sig);
    }

    void timeoutHandler(int)
    {
        std::fprintf(stderr, "\n==== timeout reached ====\n");
        for (kernel::GuestThread* t : kernel::allThreads())
        {
            std::fprintf(stderr, "\n#### thread %u\n", t->id);
            kernel::printScopes(t);
            kernel::printTrace(t->id, 4);
        }
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

    // Every slot without a recompiled function points here, so a bad indirect call reports
    // its target (bctr/bctrl jump through ctr) instead of jumping to a null host address.
    PPC_FUNC(missingFunction)
    {
        std::fprintf(stderr, "\n==== indirect call to guest %08X: no recompiled function there (caller lr %08X) ====\n",
                     ctx.ctr.u32, uint32_t(ctx.lr));
        kernel::GuestThread* gt = kernel::currentThread();
        kernel::printScopes(gt);
        if (gt) kernel::printTrace(gt->id, 16);
        kernel::printSummary();
        _exit(3);
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
        else if (a == "--capture-frame" && i + 1 < argc)
        {
            // one frame number or a comma-separated list: --capture-frame 300,1500,3000
            std::string list = argv[++i];
            for (size_t p = 0; p < list.size();)
            {
                size_t q = list.find(',', p);
                gpu::capture::request(std::strtoull(list.substr(p, q - p).c_str(), nullptr, 10));
                p = q == std::string::npos ? list.size() : q + 1;
            }
        }
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
    gpu::init();
    apu::initXma();

    if (timeout > 0)
        alarm(timeout);

    info("calling entry point _xstart");
    _xstart(ctx, mem::base);
    info("_xstart returned (r3=%08X)", ctx.r3.u32);
    kernel::printSummary();
    return 0;
}
