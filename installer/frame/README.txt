Steam Link Pyrowave - Steam Frame package
=========================================

Install / update:   ./install.sh                 (add --full-colour for 4:4:4 10-bit; see below)
Then restart SteamVR on the Frame (or reboot).

Everything goes to your home folder: ~/.local/lib/slp and ~/.local/share/vulkan/implicit_layer.d/slp_layer.json.
Nothing in the system or in SteamVR is changed. The layer decodes Pyrowave streams; HEVC streams stay on the
hardware decoder as before.

Full colour: ./install.sh --full-colour, restart SteamVR on the Frame, then turn "Full colour" on in Pyrowave Link on
the PC. ./install.sh --no-full-colour turns it off again (turn it off in the app first).

Uninstall:          ~/.local/lib/slp/uninstall.sh
Switch off only:    touch ~/.local/lib/slp/DISABLED   (rm it to switch back on)
Self-test:          selftest/run_selftest.sh fc        (GPU decode checks; nothing is installed or changed)
Bug report for a stuck stream:  ~/.local/lib/slp/tools/collect_stall.sh  (run while it is stuck)
Advanced options:   ~/.local/lib/slp/tools/  (set_mode.sh, power_options.sh, ...; each script explains itself)
