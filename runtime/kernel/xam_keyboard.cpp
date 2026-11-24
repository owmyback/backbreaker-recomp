// XAM: the on-screen keyboard (XamShowKeyboardUI). The game asks for it in My Teams, to name a
// custom team, its city and its abbreviation. In a window the overlay shows a text-entry panel
// (runtime/video/overlay.cpp) and the call completes when the player confirms or cancels.
// Headless there is nobody to type, so the call is answered at once: with --keyboard-text, or
// with the text the game offered. Parameters and results follow Xenia's xam_ui.cc.
#include "kernel.h"
#include "import_support.h"
#include "spa.h"
#include "../log.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

using namespace mem;

namespace
{
    constexpr uint32_t ERROR_SUCCESS = 0;
    constexpr uint32_t ERROR_ACCESS_DENIED = 5;
    constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
    constexpr uint32_t ERROR_IO_PENDING = 997;
    constexpr uint32_t ERROR_CANCELLED = 1223;

    std::mutex g_lock;
    std::condition_variable g_finished; // a call without an XOVERLAPPED waits here
    bool g_interactive = false;         // a window is there to ask in
    bool g_haveAnswer = false;          // --keyboard-text
    std::u16string g_answer;

    // One request at a time, as on the console: asked, then taken by the overlay, then finished
    enum class Stage { Idle, Asked, Shown } g_stage = Stage::Idle;
    std::atomic<bool> g_asked{false};
    kernel::TextEntryRequest g_request;
    uint32_t g_buffer = 0, g_bufferLength = 0, g_overlapped = 0, g_result = 0;

    std::u16string guestString(uint32_t p)
    {
        std::u16string s;
        if (!p) return s;
        for (uint32_t i = 0; i < 1024; i++)
        {
            const uint16_t c = read16(p + i * 2);
            if (!c) break;
            s.push_back(char16_t(c));
        }
        return s;
    }

    // For the log: printable ASCII as it is, anything else as '?'
    std::string plain(const std::u16string& s)
    {
        std::string out;
        for (char16_t c : s) out += c >= 0x20 && c < 0x7F ? char(c) : '?';
        return out;
    }

    // The text and its terminator into the game's buffer of `length` characters
    void writeText(uint32_t buffer, uint32_t length, const std::u16string& text)
    {
        uint32_t n = 0;
        for (; n < text.size() && n + 1 < length; n++) write16(buffer + n * 2, uint16_t(text[n]));
        write16(buffer + n * 2, 0);
    }
}

namespace kernel
{
    void setTextEntryInteractive(bool on)
    {
        std::lock_guard lock(g_lock);
        g_interactive = on;
    }

    void setTextEntryAnswer(const std::string& utf8)
    {
        std::lock_guard lock(g_lock);
        g_answer = spa::toUtf16(utf8);
        g_haveAnswer = true;
    }

    bool textEntryAsked() { return g_asked.load(std::memory_order_relaxed); }

    bool takeTextEntry(TextEntryRequest& out)
    {
        std::lock_guard lock(g_lock);
        if (g_stage != Stage::Asked) return false;
        g_stage = Stage::Shown;
        g_asked.store(false, std::memory_order_relaxed);
        out = g_request;
        return true;
    }

    void finishTextEntry(bool accepted, const std::u16string& text)
    {
        std::lock_guard lock(g_lock);
        if (g_stage != Stage::Shown) return;
        if (accepted) writeText(g_buffer, g_bufferLength, text);
        g_result = accepted ? ERROR_SUCCESS : ERROR_CANCELLED;
        if (accepted) info("[xam] keyboard: \"%s\" entered", plain(text).c_str());
        else info("[xam] keyboard: cancelled");
        completeOverlapped(g_overlapped, g_result);
        g_stage = Stage::Idle;
        g_finished.notify_all();
    }
}

// u32 XamShowKeyboardUI(u32 UserIndex, u32 Flags, wchar* DefaultText, wchar* Title, wchar* Description,
//                       wchar* Buffer, u32 BufferLength (characters), XOVERLAPPED* Overlapped)
GUEST_IMPORT(XamShowKeyboardUI)
{
    const uint32_t flags = ctx.r4.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, overlapped = ctx.r10.u32;
    if (!buffer || !length)
    {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }
    kernel::TextEntryRequest request;
    request.text = guestString(ctx.r5.u32);
    request.title = guestString(ctx.r6.u32);
    request.description = guestString(ctx.r7.u32);
    request.maxLength = length - 1;
    if (request.text.size() > request.maxLength) request.text.resize(request.maxLength);

    std::unique_lock lock(g_lock);
    if (!g_interactive || g_haveAnswer)
    {
        const std::u16string& text = g_haveAnswer ? g_answer : request.text;
        writeText(buffer, length, text);
        info("[xam] keyboard \"%s\" (flags %X, %u characters): answered \"%s\", %s", plain(request.title).c_str(), flags,
             uint32_t(request.maxLength), plain(text.substr(0, request.maxLength)).c_str(),
             g_haveAnswer ? "--keyboard-text" : "the text it offered (no window to ask in)");
        ctx.r3.u64 = kernel::completeOverlapped(overlapped, ERROR_SUCCESS);
        return;
    }
    if (g_stage != Stage::Idle)
    {
        ctx.r3.u64 = ERROR_ACCESS_DENIED; // the keyboard is already up
        return;
    }
    info("[xam] keyboard \"%s\" (flags %X, %u characters): asking, \"%s\" to start with", plain(request.title).c_str(), flags,
         uint32_t(request.maxLength), plain(request.text).c_str());
    g_request = std::move(request);
    g_buffer = buffer;
    g_bufferLength = length;
    g_overlapped = overlapped;
    g_stage = Stage::Asked;
    g_asked.store(true, std::memory_order_relaxed);
    if (overlapped)
    {
        write32(overlapped + 0x00, ERROR_IO_PENDING); // not complete yet (XHasOverlappedIoCompleted)
        ctx.r3.u64 = ERROR_IO_PENDING;
        return;
    }
    g_finished.wait(lock, [] { return g_stage == Stage::Idle; });
    ctx.r3.u64 = g_result;
}
