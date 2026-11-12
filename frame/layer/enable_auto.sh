#!/bin/bash
# Persistent auto-detect: from now on (also after reboots) vrlink checks the first frame of every
# stream; Pyrowave (SLPW) streams are decoded by Pyrowave, HEVC streams by the hardware as stock.
# Undo: ./disable_auto.sh  (or touch ~/.local/lib/slp/DISABLED, or ./uninstall.sh)
set -e
[ -f "$HOME/.local/lib/slp/libslp_layer.so" ] || { echo "run ./install.sh first"; exit 1; }
touch "$HOME/.local/lib/slp/AUTO" && echo "auto-detect enabled (~/.local/lib/slp/AUTO). Takes effect when vrlink next starts (restart SteamVR on the Frame or reboot)."
