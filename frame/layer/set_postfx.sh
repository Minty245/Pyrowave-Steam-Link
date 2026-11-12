#!/bin/bash
# Steam Link Pyrowave - Frame-side luma post-filter (v7.9: seam deblock by default; to change one value, use set_postfx_key.sh). Applied live (within ~1 s), no reconnect needed.
#   ./set_postfx.sh DEBLOCK_WIDE SHARPEN [R0 R1] [DEBLOCK_INSET] [WIDE_STEP]
#     DEBLOCK_WIDE   deblock on the wide (peripheral) views: 0 = off, 1 = default, up to 4. Scaled by each frame's
#                    bits per pixel: almost nothing on well-coded frames (~1 bit/pixel and up), full on starved ones.
#     SHARPEN        0 = off .. 1 = strongest. Contrast-adaptive sharpening on the gaze insets only.
#     R0 R1          sharpening is full inside R0 and fades to nothing at R1 (fractions of the inset half-size; 0.35 0.8)
#     DEBLOCK_INSET  deblock on the insets (default 0 = off)
#     WIDE_STEP      smooth mode only (deblock_mode=1): reach 1 = 4 px, 2 = 8 px
#   ./set_postfx.sh off      removes the settings file (filter off)
# Needs output MODE 0, 1 or 4 (see set_mode.sh). Undo: ./set_postfx.sh off
set -e
D="$HOME/.local/lib/slp"
[ -d "$D" ] || { echo "run ./install.sh first"; exit 1; }
if [ "${1:-}" = off ]; then rm -f "$D/postfx.txt"; echo "post-filter off."; exit 0; fi
[ $# -ge 2 ] || { echo "usage: $0 DEBLOCK_WIDE SHARPEN [R0 R1] [DEBLOCK_INSET] [WIDE_STEP] | off"; exit 1; }
printf 'deblock_wide=%s\nsharpen=%s\nsharpen_r0=%s\nsharpen_r1=%s\ndeblock_inset=%s\nwide_step=%s\n' \
  "$1" "$2" "${3:-0.35}" "${4:-0.8}" "${5:-0}" "${6:-2}" > "$D/postfx.txt"
echo "post-filter set:"; cat "$D/postfx.txt"
