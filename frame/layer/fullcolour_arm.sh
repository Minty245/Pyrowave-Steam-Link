#!/bin/bash
# v7.19 full colour (4:4:4, 10-bit) - arm for THIS BOOT ONLY (a reboot disarms it).
# Writes only /tmp/slp_fullcolour. Both vrlink and vrcompositor read it when they start, so restart SteamVR on the
# Frame afterwards. Undo: ./fullcolour_disarm.sh (then restart SteamVR) or simply reboot.
set -e
[ -f "$HOME/.local/lib/slp/libslp_layer.so" ] || { echo "run ./install.sh first"; exit 1; }
touch /tmp/slp_fullcolour && echo "full colour armed for this boot (/tmp/slp_fullcolour). Restart SteamVR on the Frame, then turn Full colour on in Pyrowave Link."
