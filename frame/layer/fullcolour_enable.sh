#!/bin/bash
# v7.19 full colour (4:4:4, 10-bit) - persistent (survives reboots). Only after it worked with ./fullcolour_arm.sh.
# Writes only ~/.local/lib/slp/FULLCOLOUR. Undo: ./fullcolour_disarm.sh
set -e
[ -f "$HOME/.local/lib/slp/libslp_layer.so" ] || { echo "run ./install.sh first"; exit 1; }
touch "$HOME/.local/lib/slp/FULLCOLOUR" && echo "full colour enabled persistently (~/.local/lib/slp/FULLCOLOUR). Takes effect when SteamVR next starts."
