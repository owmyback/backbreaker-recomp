// String helpers and printf-family imports (format strings read from guest memory).
#include "import_support.h"
#include "../log.h"

#include <algorithm>
#include <cstring>
#include <string>

using namespace mem;

namespace
{
    // Varargs come from consecutive GPRs (PPC64 ABI passes doubles in GPRs for varargs too),
    // or from a guest va_list (8-byte slots).
    struct ArgReader
    {
        PPCContext* ctx = nullptr;
        int reg = 0;       // next GPR index when reading registers
        uint32_t list = 0; // guest va_list pointer when nonzero

        uint64_t next()
        {
            if (list)
            {
                uint64_t v = read64(list);
                list += 8;
                return v;
            }
            if (reg > 10)
            {
                warn("printf: more than 8 register varargs, rest read as 0");
                return 0;
            }
            PPCRegister* regs[] = { &ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10 };
            return regs[reg++ - 3]->u64;
        }
    };

    std::string guestString(uint32_t a) { return a ? std::string((const char*)host(a)) : "(null)"; }

    std::string guestWide(uint32_t a)
    {
        if (!a) return "(null)";
        std::string s;
        for (uint16_t c; (c = read16(a)) != 0; a += 2)
            s += c < 0x80 ? char(c) : '?';
        return s;
    }

    std::string format(const char* fmt, ArgReader args)
    {
        std::string out;
        char buf[512];
        for (const char* p = fmt; *p; p++)
        {
            if (*p != '%') { out += *p; continue; }
            if (p[1] == '%') { out += '%'; p++; continue; }

            std::string spec = "%";
            const char* q = p + 1;
            while (*q && strchr("-+ #0", *q)) spec += *q++;
            if (*q == '*') { spec += std::to_string(int32_t(args.next())); q++; }
            while (*q >= '0' && *q <= '9') spec += *q++;
            if (*q == '.')
            {
                spec += *q++;
                if (*q == '*') { spec += std::to_string(int32_t(args.next())); q++; }
                while (*q >= '0' && *q <= '9') spec += *q++;
            }
            int longs = 0;
            bool shortArg = false;
            for (;; q++)
            {
                if (*q == 'l') longs++;
                else if (*q == 'h') shortArg = true;
                else if (q[0] == 'I' && q[1] == '6' && q[2] == '4') { longs = 2; q += 2; }
                else if (*q == 'z' || *q == 't' || *q == 'j') {}
                else break;
            }
            char conv = *q;
            if (!conv) break;
            p = q;

            switch (conv)
            {
            case 'd': case 'i':
            {
                uint64_t v = args.next();
                spec += "lld";
                snprintf(buf, sizeof(buf), spec.c_str(), longs >= 2 ? (long long)v : shortArg ? (long long)int16_t(v) : (long long)int32_t(v));
                out += buf;
                break;
            }
            case 'u': case 'x': case 'X': case 'o':
            {
                uint64_t v = args.next();
                spec += "ll";
                spec += conv;
                snprintf(buf, sizeof(buf), spec.c_str(), longs >= 2 ? (unsigned long long)v : shortArg ? (unsigned long long)uint16_t(v) : (unsigned long long)uint32_t(v));
                out += buf;
                break;
            }
            case 'p':
                snprintf(buf, sizeof(buf), "%08X", uint32_t(args.next()));
                out += buf;
                break;
            case 'c':
                spec += 'c';
                snprintf(buf, sizeof(buf), spec.c_str(), int(uint8_t(args.next())));
                out += buf;
                break;
            case 's': case 'S':
            {
                uint32_t a = uint32_t(args.next());
                bool wide = conv == 'S' ? !shortArg : longs > 0;
                std::string s = wide ? guestWide(a) : guestString(a);
                spec += 's';
                std::string tmp(s.size() + 256, '\0');
                snprintf(tmp.data(), tmp.size(), spec.c_str(), s.c_str());
                out += tmp.c_str();
                break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            {
                uint64_t bits = args.next();
                double d;
                memcpy(&d, &bits, 8);
                spec += conv;
                snprintf(buf, sizeof(buf), spec.c_str(), d);
                out += buf;
                break;
            }
            case 'n':
                args.next();
                break;
            default:
                out += spec;
                out += conv;
                break;
            }
        }
        return out;
    }

    void writeOut(uint32_t dst, uint32_t cap, const std::string& s)
    {
        size_t n = cap ? std::min<size_t>(s.size(), cap - 1) : 0;
        memcpy(host(dst), s.data(), n);
        if (cap) host(dst)[n] = 0;
    }
}

// int sprintf(char* Buffer, const char* Format, ...)
GUEST_IMPORT(sprintf)
{
    uint32_t dst = ctx.r3.u32;
    std::string s = format((const char*)host(ctx.r4.u32), { &ctx, 5, 0 });
    memcpy(host(dst), s.c_str(), s.size() + 1);
    ctx.r3.u64 = s.size();
}

// int _vsnprintf(char* Buffer, size_t Count, const char* Format, va_list Args)
GUEST_IMPORT(_vsnprintf)
{
    uint32_t dst = ctx.r3.u32, cap = ctx.r4.u32;
    std::string s = format((const char*)host(ctx.r5.u32), { &ctx, 0, ctx.r6.u32 });
    writeOut(dst, cap, s);
    ctx.r3.u64 = s.size() < cap ? s.size() : uint64_t(-1);
}

// u32 DbgPrint(const char* Format, ...)
GUEST_IMPORT(DbgPrint)
{
    std::string s = format((const char*)host(ctx.r3.u32), { &ctx, 4, 0 });
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    std::fprintf(stderr, "[game] %s\n", s.c_str());
    ctx.r3.u64 = 0;
}

// ---- ANSI_STRING / UNICODE_STRING: { u16 Length; u16 MaximumLength; u32 Buffer; } ----
GUEST_IMPORT(RtlInitAnsiString)
{
    uint32_t str = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = src ? strlen((const char*)host(src)) : 0;
    write16(str, len);
    write16(str + 2, src ? len + 1 : 0);
    write32(str + 4, src);
}

GUEST_IMPORT(RtlInitUnicodeString)
{
    uint32_t str = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = 0;
    if (src)
        while (read16(src + len * 2)) len++;
    write16(str, len * 2);
    write16(str + 2, src ? (len + 1) * 2 : 0);
    write32(str + 4, src);
}

GUEST_IMPORT(RtlFreeAnsiString) {}

// NTSTATUS RtlMultiByteToUnicodeN(wchar* Dst, u32 MaxBytes, u32* BytesWritten, const char* Src, u32 SrcBytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    uint32_t dst = ctx.r3.u32, maxBytes = ctx.r4.u32, pWritten = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    uint32_t n = std::min(maxBytes / 2, srcBytes);
    for (uint32_t i = 0; i < n; i++)
        write16(dst + i * 2, host(src)[i]);
    if (pWritten) write32(pWritten, n * 2);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlUnicodeToMultiByteN(char* Dst, u32 MaxBytes, u32* BytesWritten, const wchar* Src, u32 SrcBytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    uint32_t dst = ctx.r3.u32, maxBytes = ctx.r4.u32, pWritten = ctx.r5.u32, src = ctx.r6.u32, srcBytes = ctx.r7.u32;
    uint32_t n = std::min(maxBytes, srcBytes / 2);
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = read16(src + i * 2);
        host(dst)[i] = c < 0x100 ? char(c) : '?';
    }
    if (pWritten) write32(pWritten, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlUnicodeStringToAnsiString(ANSI_STRING* Dst, const UNICODE_STRING* Src, BOOL Allocate)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, alloc = ctx.r5.u32;
    uint32_t chars = read16(src) / 2, buf = read32(src + 4);
    if (alloc)
    {
        uint32_t b = virtualAlloc(chars + 1, 16);
        write32(dst + 4, b);
        write16(dst + 2, chars + 1);
    }
    uint32_t cap = read16(dst + 2), out = read32(dst + 4);
    uint32_t n = cap ? std::min(chars, cap - 1u) : 0;
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t c = read16(buf + i * 2);
        host(out)[i] = c < 0x100 ? char(c) : '?';
    }
    if (cap) host(out)[n] = 0;
    write16(dst, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// Memory fill/compare helpers (u32 pattern, big-endian)
GUEST_IMPORT(RtlFillMemoryUlong)
{
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32, pattern = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        write32(dst + i, pattern);
}

GUEST_IMPORT(RtlCompareMemoryUlong)
{
    uint32_t src = ctx.r3.u32, len = ctx.r4.u32, pattern = ctx.r5.u32, i = 0;
    while (i + 4 <= len && read32(src + i) == pattern) i += 4;
    ctx.r3.u64 = i;
}
