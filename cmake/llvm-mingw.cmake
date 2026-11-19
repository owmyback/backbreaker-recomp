# Cross-compiling the runtime and the game for Windows (x86_64, UCRT) with llvm-mingw (#174):
#   scripts/windows_deps.sh        # llvm-mingw into third_party/llvm-mingw, the libraries into third_party/win64
#   cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/llvm-mingw.cmake -DBB_PORTABLE=ON \
#         -DFFMPEG_XMA=$PWD/third_party/win64/ffmpeg-xma
#   ninja -C build-win backbreaker   # build-win/backbreaker.exe
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
get_filename_component(_bb_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LLVM_MINGW "${_bb_root}/third_party/llvm-mingw" CACHE PATH "llvm-mingw (scripts/windows_deps.sh)")
set(BB_WIN64 "${_bb_root}/third_party/win64" CACHE PATH "Windows builds of SDL3, shaderc and the Vulkan headers")
set(CMAKE_C_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-clang++")
set(CMAKE_RC_COMPILER "${LLVM_MINGW}/bin/x86_64-w64-mingw32-windres")
set(CMAKE_AR "${LLVM_MINGW}/bin/llvm-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB "${LLVM_MINGW}/bin/llvm-ranlib" CACHE FILEPATH "")
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW}/x86_64-w64-mingw32" "${BB_WIN64}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
