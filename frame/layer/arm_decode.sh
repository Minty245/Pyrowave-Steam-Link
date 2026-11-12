#!/bin/bash
# Arms the Pyrowave decoder stand-in for the NEXT stream: when vrlink opens the hardware decoder,
# the layer answers instead and decodes Pyrowave (SLPW) frames. The PC must be in substitution mode.
# Undo: ./disarm.sh (or reboot), then restart the stream.
touch /tmp/slp_arm /tmp/slp_decode && echo "armed: observe layer + decoder stand-in (/tmp/slp_arm, /tmp/slp_decode)."
echo "vrlink must load the layer: restart the stream (if the layer was not already loaded, restart SteamVR on the Frame)."
