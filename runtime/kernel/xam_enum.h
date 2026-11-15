#pragma once
// An XAM enumerator handle, as XamEnumerate reads it: content lists (xam_content.cpp) and
// achievements (xam_achievements.cpp). Behavior follows Xenia's XEnumerator / xam_enum.cc.
#include "objects.h"

namespace kernel
{
    struct XamEnumerator : Object
    {
        XamEnumerator() : Object(Type::Other) {}
        // Writes the next items (at most the per-call count the enumerator was created with) to
        // buf. Returns ERROR_SUCCESS with count > 0, or ERROR_NO_MORE_FILES once all are out.
        virtual uint32_t write(uint32_t buf, uint32_t length, uint32_t& count) = 0;
    };
}
