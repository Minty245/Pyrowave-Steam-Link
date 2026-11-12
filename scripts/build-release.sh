#!/bin/bash
# Steam Link Pyrowave - build everything and package a release, from Linux (cross-compiles Windows and the Frame).
#
# Needs: cmake, ninja, x86_64-w64-mingw32 (posix threads) and aarch64-linux-gnu toolchains, glslangValidator (only to
# regenerate shaders), and these checkouts / builds:
#   PYROWAVE_SRC   the Steam Link Pyrowave fork of Pyrowave (headers, Granite's Vulkan headers, volk)
#   RMLUI_PREFIX   RmlUi 6.1 + FreeType built for MinGW (install prefix with lib/cmake/RmlUi)
#   OPENVR_DIR     an openvr checkout (headers/, lib/win64/, bin/win64/openvr_api.dll)
# The Pyrowave libraries themselves come from prebuilt/ (built from the fork; see prebuilt/README.md).
#
#   VERSION=1.0.0 scripts/build-release.sh      -> dist/SteamLinkPyrowave-PC-1.0.0.zip, dist/SteamLinkPyrowave-Frame-1.0.0.tar.gz
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${PYROWAVE_SRC:?set PYROWAVE_SRC}" "${RMLUI_PREFIX:?set RMLUI_PREFIX}" "${OPENVR_DIR:?set OPENVR_DIR}"
VERSION="${VERSION:-$(cat "$ROOT/VERSION")}"
B="$ROOT/build"; D="$ROOT/dist"
VK_INC="$PYROWAVE_SRC/Granite/third_party/khronos/vulkan-headers/include"
TC="$ROOT/cmake/x86_64-w64-mingw32.cmake"
mkdir -p "$B" "$D"

echo "== PC: backend"
cmake -S "$ROOT/pc/encoder" -B "$B/backend" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DPYROWAVE_DIR="$PYROWAVE_SRC" -DPYROWAVE_LIB="$ROOT/prebuilt/win64/libpyrowave-shared.dll.a" -DVULKAN_INCLUDE_DIR="$VK_INC" >/dev/null
ninja -C "$B/backend"
echo "== PC: SteamVR driver"
cmake -S "$ROOT/pc/driver/driver_slp" -B "$B/driver" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" >/dev/null
ninja -C "$B/driver"
echo "== PC: Pyrowave Link"
cmake -S "$ROOT/app" -B "$B/app" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$TC" \
  -DCMAKE_FIND_ROOT_PATH="$RMLUI_PREFIX" -DRmlUi_DIR="$RMLUI_PREFIX/lib/cmake/RmlUi" -DOPENVR_DIR="$OPENVR_DIR" >/dev/null
ninja -C "$B/app"
echo "== Frame: decoder + layer"
PYRO_SRC="$PYROWAVE_SRC" PYRO_LIB="$ROOT/prebuilt/aarch64" "$ROOT/frame/decoder/build.sh"
VK_INC="$VK_INC" OUT="$B/layer" "$ROOT/frame/layer/build.sh"

echo "== package: Frame"
F="SteamLinkPyrowave-Frame-$VERSION"; FS="$B/stage/$F"
rm -rf "$B/stage"; mkdir -p "$FS/lib" "$FS/tools" "$FS/selftest"
cp "$B/layer/libslp_layer.so" "$ROOT/frame/layer/slp_layer.json.in" "$ROOT/frame/decoder/out/libslp_pyrodec.so" "$FS/lib/"
cp "$ROOT/prebuilt/aarch64/libpyrowave-shared.so.1" "$FS/lib/"
cp "$ROOT/installer/frame/install.sh" "$ROOT/installer/frame/uninstall.sh" "$FS/"
for s in set_mode power_options set_postfx set_postfx_key fullcolour_arm fullcolour_enable fullcolour_disarm \
         enable_auto disable_auto arm arm_decode disarm; do cp "$ROOT/frame/layer/$s.sh" "$FS/tools/"; done
cp "$ROOT/frame/tools/"*.sh "$FS/tools/"
cp "$ROOT/frame/decoder/out/slp_selftest" "$ROOT/frame/decoder/run_selftest.sh" "$ROOT/prebuilt/aarch64/libpyrowave-shared.so.1" "$FS/selftest/"
cp "$ROOT/installer/frame/README.txt" "$ROOT/LICENSE" "$FS/" 2>/dev/null || true
echo "$VERSION" > "$FS/VERSION"
chmod +x "$FS"/*.sh "$FS"/tools/*.sh "$FS"/selftest/*.sh "$FS/selftest/slp_selftest"
tar -C "$B/stage" --owner=0 --group=0 -czf "$D/$F.tar.gz" "$F"

echo "== package: PC"
P="SteamLinkPyrowave-PC-$VERSION"; PS="$B/stage/$P"
mkdir -p "$PS/driver/slp/bin/win64" "$PS/app/PyrowaveLink/licenses" "$PS/frame"
cp -r "$ROOT/installer/pc/slp/." "$PS/driver/slp/"
cp "$B/driver/driver_slp/bin/win64/driver_slp.dll" "$B/backend/SteamFramePyrowaveBackend.dll" \
   "$ROOT/prebuilt/win64/libpyrowave-shared-1.dll" "$PS/driver/slp/bin/win64/"
cp "$B/app/PyrowaveLink.exe" "$OPENVR_DIR/bin/win64/openvr_api.dll" "$PS/app/PyrowaveLink/"
cp -r "$ROOT/app/assets" "$PS/app/PyrowaveLink/"
cp "$ROOT/app/third_party/rmlui_backends/LICENSE-RmlUi.txt" "$PS/app/PyrowaveLink/licenses/RmlUi-MIT.txt"
cp "$OPENVR_DIR/LICENSE" "$PS/app/PyrowaveLink/licenses/OpenVR-BSD.txt"
cp "$ROOT/app/third_party/licenses/"*.txt "$PS/app/PyrowaveLink/licenses/"
cp "$ROOT/installer/pc/"*.ps1 "$ROOT/installer/pc/"*.cmd "$ROOT/installer/pc/README.txt" "$PS/"
cp "$D/$F.tar.gz" "$PS/frame/"
cp "$ROOT/LICENSE" "$PS/LICENSE.txt"; cp "$ROOT/THIRD_PARTY_NOTICES.md" "$PS/THIRD_PARTY_NOTICES.txt"
echo "$VERSION" > "$PS/VERSION"
x86_64-w64-mingw32-strip "$PS/driver/slp/bin/win64/"*.dll "$PS/app/PyrowaveLink/PyrowaveLink.exe" 2>/dev/null || true
( cd "$B/stage" && rm -f "$D/$P.zip" && zip -qr "$D/$P.zip" "$P" )
ls -la "$D"
