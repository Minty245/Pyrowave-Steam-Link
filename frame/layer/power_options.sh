#!/bin/bash
# Steam Link Pyrowave v7.22 - power options of the headset decoder (read when vrlink next starts).
#   ./power_options.sh status
#   ./power_options.sh compressed on|off   compressed (UBWC) full-colour images (default on since v7.22) or LINEAR
#   ./power_options.sh merge on|off        blur + sharpening folded into the full-colour pack (default on)
#   ./power_options.sh pyramid fast|exact  bilinear blur pyramid (default fast) or the v7.20 6x6 sums
# Each option is one empty file in ~/.local/lib/slp (RGBA_LINEAR, NO_MERGE, EXACT_PYRAMID); nothing else changes.
# Back to the defaults: rm -f ~/.local/lib/slp/RGBA_LINEAR ~/.local/lib/slp/NO_MERGE ~/.local/lib/slp/EXACT_PYRAMID
set -e
D="$HOME/.local/lib/slp"
[ -d "$D" ] || { echo "run ./install.sh first"; exit 1; }
onoff() { [ -e "$D/$1" ] && echo "$2" || echo "$3"; }
case "${1:-status} ${2:-}" in
  "status "*) ;;
  "compressed on") rm -f "$D/RGBA_LINEAR" "$D/RGBA_COMPRESSED" ;;
  "compressed off") touch "$D/RGBA_LINEAR"; rm -f "$D/RGBA_COMPRESSED" ;;
  "merge on") rm -f "$D/NO_MERGE" ;;
  "merge off") touch "$D/NO_MERGE" ;;
  "pyramid fast") rm -f "$D/EXACT_PYRAMID" ;;
  "pyramid exact") touch "$D/EXACT_PYRAMID" ;;
  *) echo "usage: $0 status | compressed on|off | merge on|off | pyramid fast|exact"; exit 1 ;;
esac
rm -f "$D/RGBA_COMPRESSED"   # v7.21's opt-in file; compressed is the default now
echo "full-colour images: $(onoff RGBA_LINEAR LINEAR 'compressed (UBWC) where the driver allows')"
echo "filters in the pack: $(onoff NO_MERGE 'off (separate passes, v7.20)' on)"
echo "blur pyramid:       $(onoff EXACT_PYRAMID 'exact (v7.20)' fast)"
[ "${1:-status}" = status ] || echo "Takes effect when vrlink next starts (end the stream and reconnect)."
