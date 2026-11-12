Steam Link Pyrowave - PC package
================================

1. Close SteamVR.
2. Double-click Install.cmd. It installs to %LOCALAPPDATA%\Programs\SteamLinkPyrowave (no administrator rights),
   registers the SteamVR driver, adds "Pyrowave Link" to the Start menu and an entry to Settings > Apps.
3. Install the headset part on the Steam Frame, one of:
   - On the Frame (Desktop Mode): extract frame\SteamLinkPyrowave-Frame-*.tar.gz (also on the release page),
     open Konsole in that folder, run ./install.sh
   - From this PC over SSH: double-click Install-Frame.cmd and enter the Frame's address (needs SSH enabled on the
     Frame: in Konsole run "passwd" once, then "sudo systemctl enable --now sshd").
   Then restart SteamVR on the Frame.
4. Start SteamVR and connect with Steam Link. Pyrowave Link (Start menu, or the SteamVR dashboard) has the settings.

Full colour (optional): install the Frame part with --full-colour (Install-Frame.cmd asks), restart SteamVR
on the Frame, then switch "Full colour" on in Pyrowave Link. Without it enabled on the Frame the headset shows no picture.

Uninstall: Settings > Apps > Steam Link Pyrowave (keeps your settings; Uninstall.ps1 -RemoveSettings removes them).
On the Frame: ~/.local/lib/slp/uninstall.sh

More help: the project README and docs/TROUBLESHOOTING.md on the release page.
