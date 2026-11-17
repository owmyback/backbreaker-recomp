// Checks runtime/kernel/ntstatus.h (#132): NTSTATUS -> Win32 error codes as RtlNtStatusToDosError
// returns them. No game files. Build:
//   clang++ -std=c++20 -Wall -Wextra -O2 tests/ntstatus_test.cpp -o build/ntstatus_test
#include "../runtime/kernel/ntstatus.h"

#include <cstdio>

static int failures = 0, checks = 0;

static void expect(uint32_t status, uint32_t want, const char* what)
{
    checks++;
    uint32_t got = kernel::ntStatusToDosError(status);
    if (got != want)
    {
        failures++;
        printf("FAIL %s: %08X -> %u, want %u\n", what, status, got, want);
    }
}

int main()
{
    // The two the movie player depends on: end of file, and an overlapped read still pending
    expect(0xC0000011, 38, "STATUS_END_OF_FILE -> ERROR_HANDLE_EOF");
    expect(0x00000103, 997, "STATUS_PENDING -> ERROR_IO_PENDING");
    // What the file system imports return
    expect(0x00000000, 0, "STATUS_SUCCESS");
    expect(0xC0000034, 2, "STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND");
    expect(0xC000003A, 3, "STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND");
    expect(0xC0000022, 5, "STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED");
    expect(0xC0000008, 6, "STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE");
    expect(0x80000006, 18, "STATUS_NO_MORE_FILES -> ERROR_NO_MORE_FILES");
    expect(0xC0000004, 24, "STATUS_INFO_LENGTH_MISMATCH -> ERROR_BAD_LENGTH");
    expect(0xC0000003, 87, "STATUS_INVALID_INFO_CLASS -> ERROR_INVALID_PARAMETER");
    expect(0xC0000023, 122, "STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER");
    expect(0xC0000017, 8, "STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY");
    expect(0xC0000001, 31, "STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE");
    // A Win32 error wrapped as an HRESULT comes back as itself; anything unknown as 317
    expect(0x80070026, 38, "HRESULT_FROM_WIN32(ERROR_HANDLE_EOF)");
    expect(0x80070005, 5, "HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)");
    expect(0xC0DEC0DE, 317, "unmapped -> ERROR_MR_MID_NOT_FOUND");
    expect(0x00000102, 317, "STATUS_TIMEOUT (not mapped) -> ERROR_MR_MID_NOT_FOUND");
    static_assert(kernel::ntStatusToDosError(0xC0000011) == 38, "usable at compile time");

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
