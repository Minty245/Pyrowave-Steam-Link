#!/bin/bash
# Steam Link Pyrowave layer - install (user space only, no sudo).
# Writes only:
#   ~/.local/lib/slp/libslp_layer.so            (Vulkan layer, inert unless armed)
#   ~/.local/lib/slp/libslp_pyrodec.so          (Pyrowave decoder, loaded only when decode-armed)
#   ~/.local/lib/slp/libpyrowave-shared.so.1
#   ~/.local/share/vulkan/implicit_layer.d/slp_layer.json
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIBDIR="$HOME/.local/lib/slp"
MANDIR="$HOME/.local/share/vulkan/implicit_layer.d"
mkdir -p "$LIBDIR" "$MANDIR"
install -m 0644 "$HERE/libslp_layer.so" "$LIBDIR/libslp_layer.so"
for f in libslp_pyrodec.so libpyrowave-shared.so.1; do
  [ -f "$HERE/$f" ] && install -m 0644 "$HERE/$f" "$LIBDIR/$f"
done
sed "s|@LIBPATH@|$LIBDIR/libslp_layer.so|" "$HERE/slp_layer.json.in" > "$MANDIR/slp_layer.json"
chmod 0644 "$MANDIR/slp_layer.json"
echo "installed:"
ls -l "$LIBDIR" "$MANDIR/slp_layer.json"
echo "nothing is armed yet. ./enable_auto.sh = persistent auto-detect (recommended); ./arm_decode.sh = this boot only; ./uninstall.sh removes everything."
