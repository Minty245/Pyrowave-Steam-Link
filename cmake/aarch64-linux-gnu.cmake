# CMake toolchain file for cross-compiling to aarch64-linux-gnu
# (Debian/Ubuntu: sudo apt install gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu)
#
# Usage:
#   cmake -B build -DCMAKE_TOOLCHAIN_FILE=../../cmake/aarch64-linux-gnu.cmake ...

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_AR           aarch64-linux-gnu-ar)
set(CMAKE_STRIP        aarch64-linux-gnu-strip)

# Sysroot — override with -DCMAKE_SYSROOT=/path if your cross-sysroot differs.
# set(CMAKE_SYSROOT /usr/aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
