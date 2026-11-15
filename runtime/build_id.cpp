// Issue #75: one place works out what source a binary came from, so the log line, the window
// title, capture summaries and --version cannot drift apart. Only this file includes the
// generated header, so a new build date recompiles one translation unit, not the runtime.
#include "build_id.h"

#include <build_version.h>

#include <string>

namespace build_id
{
    const char* revision()
    {
        return BB_GIT_REV BB_GIT_SUFFIX; // adjacent literals: no buffer, no static, no lock
    }

    const char* line()
    {
        static const std::string s =
            std::string(revision()) + " (" + BB_GIT_BRANCH + ", built " + BB_BUILD_DATE + ")";
        return s.c_str();
    }
}
