# Building the Windows program on Linux, with MinGW-w64 (on Arch: the package mingw-w64-gcc).
#
#   cmake -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#   ninja -C build-win
#
# SDL3's MinGW development files are found in external/SDL3-<version>/x86_64-w64-mingw32 (from
# SDL3-devel-<version>-mingw.tar.gz), or wherever SDL3_DIR points. Without them the program is
# built headless, which is how the regression checks run it under Wine.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(ALBW_MINGW x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${ALBW_MINGW}-gcc)
set(CMAKE_CXX_COMPILER ${ALBW_MINGW}-g++)
set(CMAKE_RC_COMPILER ${ALBW_MINGW}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${ALBW_MINGW})
# Programs run here (glslc, python); libraries and headers are the target's.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
