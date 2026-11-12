#!/bin/bash
# Steam Link Pyrowave - change ONE post-filter setting, keep the rest (applied live within ~1 s).
#   ./set_postfx_key.sh KEY VALUE
# Keys:
#   deblock_wide   deblock strength on the wide (peripheral) views, 0..4 (scaled by each frame's bits per pixel)
#   deblock_inset  deblock strength on the insets, 0..4
#   deblock_mode   0 = seam (default: only across Pyrowave's block grid, texture kept), 1 = smooth (v7.8 filter)
#   deblock_coarse seam mode: 0 = 16-px grid only, 1 = +32-px seams, 2 = +32 and 64-px seams (default 2)
#   deblock_bpp    1 = strength follows each frame's bits per pixel (default), 0 = always full strength
#   wide_step      smooth mode only: reach 1 = 4 px, 2 = 8 px
#   wide_blur      v7.13: contrast-preserving blur of the wide views after decoding, px (0 = off, ~0.5..4)
#   wide_blur_contrast  v7.13: its contrast restoration, 0..3 (default 1)
#   sharpen        inset sharpening 0..1
#   sharpen_r0     sharpening full inside this fraction of the inset half-size (0.35)
#   sharpen_r1     ... and gone at this one (0.8)
# Show the current settings: ./set_postfx_key.sh show      Undo everything: ./set_postfx.sh off
set -e
F="$HOME/.local/lib/slp/postfx.txt"
[ -d "$(dirname "$F")" ] || { echo "run ./install.sh first"; exit 1; }
if [ "${1:-}" = show ]; then cat "$F" 2>/dev/null || echo "(no postfx.txt: filter off)"; exit 0; fi
[ $# -eq 2 ] || { echo "usage: $0 KEY VALUE | show"; exit 1; }
case "$1" in deblock_wide|deblock_inset|deblock_mode|deblock_coarse|deblock_bpp|wide_step|sharpen|sharpen_r0|sharpen_r1|wide_blur|wide_blur_contrast) ;; *) echo "unknown key $1"; exit 1;; esac
touch "$F"
if grep -q "^$1=" "$F"; then sed -i "s/^$1=.*/$1=$2/" "$F"; else echo "$1=$2" >> "$F"; fi
cat "$F"
