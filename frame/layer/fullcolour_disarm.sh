#!/bin/bash
# v7.19 full colour - undo for fullcolour_arm.sh and fullcolour_enable.sh. Takes effect when SteamVR next starts.
rm -f /tmp/slp_fullcolour "$HOME/.local/lib/slp/FULLCOLOUR" && echo "full colour disarmed (vrlink decodes 4:2:0 only, vrcompositor untouched). Restart SteamVR on the Frame; turn Full colour off in Pyrowave Link."
