#!/bin/bash
# v7.21: rebuild the SPIR-V headers the decoder embeds (glslangValidator).
set -euo pipefail
cd "$(dirname "$0")"
g() { local src=$1 name=$2; shift 2; glslangValidator -V --target-env vulkan1.1 "$@" --vn "slp_${name}_spv" "$src" -o "${name}_spv.h" >/dev/null; }
g chroma_pack.comp chroma_pack
g chroma_pack8.comp chroma_pack8
g finish8.comp finish8
g to8bit.comp to8bit
g sample_readback.comp sample_readback
g postfx.comp postfx
g postfx_r16.comp postfx_r16
g seam.comp seam
g seam_r16.comp seam_r16
g sharpen.comp sharpen
g sharpen.comp sharpen_r16 -DSH_R16
g wideblur_down.comp wideblur_down
g wideblur_apply.comp wideblur_apply
g wideblur_apply.comp wideblur_apply_r16 -DWB_R16
g pack_rgba10.comp pack_rgba10
g pack_rgba10_fx.comp pack_rgba10_fx
echo "shaders rebuilt"
