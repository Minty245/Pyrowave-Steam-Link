#!/bin/bash
# Build the Frame decoder library (libslp_pyrodec.so) and its self-test. ARCH=x86_64 builds a host copy for lavapipe.
#   PYRO_SRC  checkout of the Steam Link Pyrowave fork of Pyrowave (headers, Granite's Vulkan headers and volk)
#   PYRO_LIB  folder with libpyrowave-shared.so.1 (default: the repo's prebuilt/aarch64 for ARCH=aarch64)
# Shaders: ./shaders.sh regenerates the *_spv.h headers (glslangValidator).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
: "${PYRO_SRC:?set PYRO_SRC to the Pyrowave fork checkout}"
ARCH="${ARCH:-aarch64}"
if [ "$ARCH" = aarch64 ]; then
  CXX=aarch64-linux-gnu-g++; CC=aarch64-linux-gnu-gcc; PYRO_LIB="${PYRO_LIB:-$HERE/../../prebuilt/aarch64}"; OUT="$HERE/out"
else
  CXX=g++; CC=gcc; PYRO_LIB="${PYRO_LIB:-$PYRO_SRC/build}"; OUT="$HERE/out-x86"
fi
mkdir -p "$OUT"
VK_INC="$PYRO_SRC/Granite/third_party/khronos/vulkan-headers/include"
VOLK="$PYRO_SRC/Granite/third_party/volk"
INC="-I$HERE -I$PYRO_SRC -I$VK_INC -I$VOLK"
$CC -O2 -fPIC -fvisibility=hidden -c "$VOLK/volk.c" $INC -o "$OUT/volk.o"
$CXX -O2 -fPIC -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -DVK_NO_PROTOTYPES -fvisibility=hidden $INC \
  -c "$HERE/slp_pyro_dec.cpp" -o "$OUT/slp_pyro_dec.o"
$CXX -O2 -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -DVK_NO_PROTOTYPES $INC \
  "$HERE/slp_selftest.cpp" "$OUT/slp_pyro_dec.o" "$OUT/volk.o" -o "$OUT/slp_selftest" \
  "$PYRO_LIB/libpyrowave-shared.so.1" -ldl -lpthread -static-libstdc++ -static-libgcc -Wl,-rpath,'$ORIGIN'
$CXX -O2 -fPIC -std=c++17 -Wall -Wextra -Wno-missing-field-initializers -DVK_NO_PROTOTYPES -fvisibility=hidden $INC \
  -c "$HERE/slp_pyrodec_capi.cpp" -o "$OUT/slp_pyrodec_capi.o"
# Decoder library for the vrlink layer: only slpd_* exported; libstdc++ static and hidden so it never
# interposes on vrlink's own libstdc++.
$CXX -shared -o "$OUT/libslp_pyrodec.so" "$OUT/slp_pyrodec_capi.o" "$OUT/slp_pyro_dec.o" "$OUT/volk.o" \
  "$PYRO_LIB/libpyrowave-shared.so.1" -ldl -lpthread -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL \
  -Wl,-rpath,'$ORIGIN' -Wl,-z,defs -Wl,--version-script="$HERE/pyrodec.map"
cp -L "$PYRO_LIB/libpyrowave-shared.so.1" "$OUT/"
echo "built $OUT/slp_selftest"
