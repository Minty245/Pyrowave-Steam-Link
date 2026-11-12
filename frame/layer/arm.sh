#!/bin/bash
# Arm the observe layer for the NEXT process start. Arm files live in /tmp, so a reboot always disarms.
#   ./arm.sh              -> vrlink only (default)
#   ./arm.sh --compositor -> also vrcompositor (Vulkan creation logging only, no V4L2 hooks)
touch /tmp/slp_arm && echo "armed vrlink (/tmp/slp_arm)."
if [ "${1:-}" = "--compositor" ]; then
  touch /tmp/slp_arm_compositor && echo "armed vrcompositor (/tmp/slp_arm_compositor)."
fi
echo "Restart the stream (and SteamVR for the compositor) to pick it up. ./disarm.sh clears both now."
