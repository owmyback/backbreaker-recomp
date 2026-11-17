# Install step of the runtime kit (#138): writes share/backbreaker-runtime/version.txt with the line
# the game itself prints with --version, "<revision> (<branch>, built <date>)" (runtime/build_id.cpp),
# so a launcher can tell whether an installed game was linked against this runtime.
# BB_VERSION_HEADER and BB_KIT_SHARE come from the install(CODE) line before it in CMakeLists.txt.

file(STRINGS "${BB_VERSION_HEADER}" lines REGEX "^#define BB_")
foreach (line IN LISTS lines)
    if (line MATCHES "^#define (BB_[A-Z_]+) +\"([^\"]*)\"")
        set(${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
if (NOT DEFINED BB_GIT_REV OR NOT DEFINED BB_BUILD_DATE)
    message(FATAL_ERROR "${BB_VERSION_HEADER} has no build identity: build the runtime before installing the kit")
endif()
set(out "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${BB_KIT_SHARE}/version.txt")
file(WRITE "${out}" "${BB_GIT_REV}${BB_GIT_SUFFIX} (${BB_GIT_BRANCH}, built ${BB_BUILD_DATE})\n")
message(STATUS "Installing: ${out}")
