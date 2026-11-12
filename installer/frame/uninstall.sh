#!/bin/bash
# Steam Link Pyrowave - remove the headset part completely (undo for install.sh). Takes effect when SteamVR next starts
# on the Frame (or after a reboot). Logs in /tmp/slp_layer/ disappear with the next reboot.
set -uo pipefail
LIBDIR="$HOME/.local/lib/slp"
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/slp_layer.json"
rm -f "$LIBDIR"/libslp_layer.so "$LIBDIR"/libslp_pyrodec.so "$LIBDIR"/libpyrowave-shared.so.1 "$LIBDIR"/VERSION \
      "$LIBDIR"/AUTO "$LIBDIR"/DISABLED "$LIBDIR"/MODE "$LIBDIR"/NODITHER "$LIBDIR"/postfx.txt "$LIBDIR"/FULLCOLOUR \
      "$LIBDIR"/RGBA_COMPRESSED "$LIBDIR"/RGBA_LINEAR "$LIBDIR"/NO_MERGE "$LIBDIR"/EXACT_PYRAMID "$LIBDIR"/uninstall.sh
rm -rf "$LIBDIR/tools"
rmdir "$LIBDIR" 2>/dev/null || true
rm -f /tmp/slp_arm /tmp/slp_arm_compositor /tmp/slp_decode /tmp/slp_fullcolour
echo "Steam Link Pyrowave removed from the Frame. Restart SteamVR on the Frame (or reboot)."
[ -d "$LIBDIR" ] && echo "note: $LIBDIR still has files that are not ours: $(ls -A "$LIBDIR" | tr '\n' ' ')"
exit 0
