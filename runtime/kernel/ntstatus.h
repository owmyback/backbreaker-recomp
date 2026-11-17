// NTSTATUS -> Win32 error code, as RtlNtStatusToDosError does (#132).
//
// The game's own XAPI layer (CreateFile, ReadFile, GetOverlappedResult, ...) is linked into the
// executable and turns every failed kernel call into a Win32 error with this import, and callers
// test those codes: the movie player's reader reads one block past the end of a file and expects
// ERROR_HANDLE_EOF. Every code used to come back as ERROR_MR_MID_NOT_FOUND (317), the answer for
// a status without a mapping, so that read looked like a broken file and every movie stopped
// after its first second or two. The table covers the statuses the runtime returns and the
// common ones around them, with the values Windows maps them to.
#pragma once
#include <cstdint>

namespace kernel
{
    constexpr uint32_t ERROR_MR_MID_NOT_FOUND = 317; // what RtlNtStatusToDosError returns for an unmapped status

    constexpr uint32_t ntStatusToDosError(uint32_t status)
    {
        if (status == 0)
            return 0;
        if ((status & 0xFFFF0000) == 0x80070000) // HRESULT_FROM_WIN32
            return status & 0xFFFF;
        switch (status)
        {
        case 0x00000103: return 997;  // STATUS_PENDING -> ERROR_IO_PENDING
        case 0x80000005: return 234;  // STATUS_BUFFER_OVERFLOW -> ERROR_MORE_DATA
        case 0x80000006: return 18;   // STATUS_NO_MORE_FILES -> ERROR_NO_MORE_FILES
        case 0x8000001A: return 259;  // STATUS_NO_MORE_ENTRIES -> ERROR_NO_MORE_ITEMS
        case 0xC0000001: return 31;   // STATUS_UNSUCCESSFUL -> ERROR_GEN_FAILURE
        case 0xC0000002: return 1;    // STATUS_NOT_IMPLEMENTED -> ERROR_INVALID_FUNCTION
        case 0xC0000003: return 87;   // STATUS_INVALID_INFO_CLASS -> ERROR_INVALID_PARAMETER
        case 0xC0000004: return 24;   // STATUS_INFO_LENGTH_MISMATCH -> ERROR_BAD_LENGTH
        case 0xC0000008: return 6;    // STATUS_INVALID_HANDLE -> ERROR_INVALID_HANDLE
        case 0xC000000D: return 87;   // STATUS_INVALID_PARAMETER -> ERROR_INVALID_PARAMETER
        case 0xC000000F: return 2;    // STATUS_NO_SUCH_FILE -> ERROR_FILE_NOT_FOUND
        case 0xC0000010: return 1;    // STATUS_INVALID_DEVICE_REQUEST -> ERROR_INVALID_FUNCTION
        case 0xC0000011: return 38;   // STATUS_END_OF_FILE -> ERROR_HANDLE_EOF
        case 0xC0000017: return 8;    // STATUS_NO_MEMORY -> ERROR_NOT_ENOUGH_MEMORY
        case 0xC0000022: return 5;    // STATUS_ACCESS_DENIED -> ERROR_ACCESS_DENIED
        case 0xC0000023: return 122;  // STATUS_BUFFER_TOO_SMALL -> ERROR_INSUFFICIENT_BUFFER
        case 0xC0000033: return 123;  // STATUS_OBJECT_NAME_INVALID -> ERROR_INVALID_NAME
        case 0xC0000034: return 2;    // STATUS_OBJECT_NAME_NOT_FOUND -> ERROR_FILE_NOT_FOUND
        case 0xC0000035: return 183;  // STATUS_OBJECT_NAME_COLLISION -> ERROR_ALREADY_EXISTS
        case 0xC000003A: return 3;    // STATUS_OBJECT_PATH_NOT_FOUND -> ERROR_PATH_NOT_FOUND
        case 0xC000003F: return 23;   // STATUS_CRC_ERROR -> ERROR_CRC
        case 0xC0000043: return 32;   // STATUS_SHARING_VIOLATION -> ERROR_SHARING_VIOLATION
        case 0xC000007F: return 112;  // STATUS_DISK_FULL -> ERROR_DISK_FULL
        case 0xC000009A: return 1450; // STATUS_INSUFFICIENT_RESOURCES -> ERROR_NO_SYSTEM_RESOURCES
        case 0xC00000A3: return 21;   // STATUS_DEVICE_NOT_READY -> ERROR_NOT_READY
        case 0xC00000BA: return 5;    // STATUS_FILE_IS_A_DIRECTORY -> ERROR_ACCESS_DENIED
        case 0xC00000BB: return 50;   // STATUS_NOT_SUPPORTED -> ERROR_NOT_SUPPORTED
        case 0xC0000101: return 145;  // STATUS_DIRECTORY_NOT_EMPTY -> ERROR_DIR_NOT_EMPTY
        case 0xC0000102: return 1392; // STATUS_FILE_CORRUPT_ERROR -> ERROR_FILE_CORRUPT
        case 0xC0000103: return 267;  // STATUS_NOT_A_DIRECTORY -> ERROR_DIRECTORY
        case 0xC0000120: return 995;  // STATUS_CANCELLED -> ERROR_OPERATION_ABORTED
        case 0xC0000225: return 1168; // STATUS_NOT_FOUND -> ERROR_NOT_FOUND
        default: return ERROR_MR_MID_NOT_FOUND;
        }
    }
}
