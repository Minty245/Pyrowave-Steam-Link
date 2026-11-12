# CMake toolchain file for cross-compiling Windows x64 DLL on Linux.
# (Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64)
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/x86_64-w64-mingw32.cmake ...

set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_AR           x86_64-w64-mingw32-ar)
set(CMAKE_STRIP        x86_64-w64-mingw32-strip)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
