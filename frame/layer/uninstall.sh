#!/bin/bash
# Steam Link Pyrowave layer - complete removal (undo for install.sh + all arm scripts).
set -uo pipefail
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/slp_layer.json"
rm -f "$HOME/.local/lib/slp/libslp_layer.so" "$HOME/.local/lib/slp/libslp_pyrodec.so" \
      "$HOME/.local/lib/slp/libpyrowave-shared.so.1" "$HOME/.local/lib/slp/DISABLED" "$HOME/.local/lib/slp/AUTO" "$HOME/.local/lib/slp/MODE" "$HOME/.local/lib/slp/NODITHER" "$HOME/.local/lib/slp/postfx.txt" "$HOME/.local/lib/slp/FULLCOLOUR" "$HOME/.local/lib/slp/RGBA_COMPRESSED" "$HOME/.local/lib/slp/RGBA_LINEAR" "$HOME/.local/lib/slp/NO_MERGE" "$HOME/.local/lib/slp/EXACT_PYRAMID"
rmdir "$HOME/.local/lib/slp" 2>/dev/null || true
rm -f /tmp/slp_arm /tmp/slp_arm_compositor /tmp/slp_decode /tmp/slp_fullcolour
echo "removed manifest, libraries and arm files. Logs (if any) remain in /tmp/slp_layer/ until reboot."
ls -l "$HOME/.local/share/vulkan/implicit_layer.d/slp_layer.json" "$HOME/.local/lib/slp" 2>&1 | sed 's/^/check: /'
