#!/bin/bash
# Steam Link Pyrowave - choose how decoded pictures reach vrlink's NV12 surfaces (read when vrlink starts).
#   0  dither pass + 2 copies                                  (v7 behaviour)
#   1  Pyrowave writes 8-bit directly (dither inside Pyrowave) + 2 copies   (default since v7.22)
#   4  like 1, chroma written straight into the surface, luma copied
#   5  (broken on the Frame: corrupt luma; the layer uses 4 instead)
# Modes 2 and 3 are broken on the Frame's driver and are ignored (mode 0 is used).
# Dither: on by default. ./set_mode.sh nodither  turns it off, ./set_mode.sh dither  turns it back on.
# Only pick a mode whose "picture" and "repeat" columns say MATCH in the self-test.
# Undo: rm ~/.local/lib/slp/MODE ~/.local/lib/slp/NODITHER   (back to mode 1 with dither)
set -e
D="$HOME/.local/lib/slp"
[ -d "$D" ] || { echo "run ./install.sh first"; exit 1; }
case "${1:-}" in
  0|1|4) echo "$1" > "$D/MODE"; echo "output mode $1 set." ;;
  nodither) touch "$D/NODITHER"; echo "dither off." ;;
  dither) rm -f "$D/NODITHER"; echo "dither on." ;;
  *) echo "usage: $0 0|1|4|dither|nodither"; exit 1 ;;
esac
echo "Takes effect when vrlink next starts (end the stream and reconnect)."
