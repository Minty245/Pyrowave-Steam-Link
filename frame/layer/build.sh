#!/bin/bash
# Cross-compile the observe layer for the Steam Frame (aarch64).  Run on the build host.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# VK_INC: Vulkan headers, e.g. $PYRO_SRC/Granite/third_party/khronos/vulkan-headers/include
: "${VK_INC:?set VK_INC to a Vulkan headers include folder}"
OUT="${OUT:-$HERE/out}"
mkdir -p "$OUT"
aarch64-linux-gnu-gcc -std=gnu11 -O2 -g0 -fPIC -shared -fvisibility=hidden \
  -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -I"$VK_INC" \
  -Wl,-z,defs -Wl,--as-needed -Wl,-soname,libslp_layer.so \
  -o "$OUT/libslp_layer.so" "$HERE/slp_layer.c" -ldl -lpthread
aarch64-linux-gnu-strip --strip-unneeded "$OUT/libslp_layer.so"
cp "$HERE/slp_layer.json.in" "$HERE/install.sh" "$HERE/uninstall.sh" "$HERE/arm.sh" "$HERE/arm_decode.sh" "$HERE/disarm.sh" "$HERE/enable_auto.sh" "$HERE/disable_auto.sh" "$HERE/set_mode.sh" "$HERE/set_postfx.sh" "$HERE/set_postfx_key.sh" "$HERE/fullcolour_arm.sh" "$HERE/fullcolour_enable.sh" "$HERE/fullcolour_disarm.sh" "$HERE/power_options.sh" "$OUT/"
echo "built $OUT/libslp_layer.so"
