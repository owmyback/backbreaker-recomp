#pragma once
// Glue between recompiled code and the runtime's kernel implementations.
#include <ppc_config.h>
#include <ppc_context.h>

#include "../memory.h"

struct ImportFunction { uint32_t thunk; const char* name; };
struct ImportVariable { uint32_t slot; const char* name; };
struct ImportFunctionPointer { uint32_t slot; uint32_t thunk; const char* name; };

extern const ImportFunction g_importFunctions[];
extern const ImportVariable g_importVariables[];
extern const ImportFunctionPointer g_importFunctionPointers[];

// Called by every import that has no real implementation yet: logs, returns 0.
void UnimplementedImport(const char* name, PPCContext& ctx);
// Records an implemented import call in the trace ring (for crash reports).
void TraceImport(const char* name, PPCContext& ctx);

// Weak default for every import; a GUEST_IMPORT definition elsewhere replaces it.
#define PPC_WEAK_IMPORT(name) \
    __attribute__((weak)) PPC_FUNC(__imp__##name) { UnimplementedImport(#name, ctx); }

// Define a real implementation: GUEST_IMPORT(KeTlsAlloc) { ... ctx.r3.u64 = ...; }
#define GUEST_IMPORT(name) \
    static void impl_##name(PPCContext& ctx, uint8_t* base); \
    PPC_FUNC(__imp__##name) { TraceImport(#name, ctx); impl_##name(ctx, base); } \
    static void impl_##name(PPCContext& ctx, [[maybe_unused]] uint8_t* base)

// Xbox NTSTATUS values
constexpr uint32_t STATUS_SUCCESS = 0x00000000;
constexpr uint32_t STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr uint32_t STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr uint32_t STATUS_NO_MEMORY = 0xC0000017;
constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
