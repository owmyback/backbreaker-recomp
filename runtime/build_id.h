#pragma once
// Which source this binary was built from (issue #75). The log, the window title, a capture's
// summary.txt and --version all say the same thing in the same words, so a screenshot, a capture
// folder and a terminal scrollback all trace back to one checkout.
// Values come from generated/build_version.h, written by cmake/git_version.cmake each build.
namespace build_id
{
    // "30e5912" (or "30e5912-dirty" with uncommitted edits, or "unknown" outside a git checkout)
    const char* revision();

    // "30e5912-dirty (main, built 2026-11-15)" - the form meant for a bug report
    const char* line();
}
