#!/bin/bash
# Steam Link Pyrowave - install the headset part on the Steam Frame (user space only: nothing outside ~/.local;
# sudo only if you choose to reboot and the reboot needs it).
#
#   ./install.sh                 install / update
#   ./install.sh --full-colour   also enable full colour (4:4:4, 10-bit into the compositor); turn "Full colour" on
#                                in Pyrowave Link on the PC afterwards
#   ./install.sh --no-full-colour   install / update and turn full colour off
#   --reboot / --no-reboot       reboot the Frame afterwards without asking / don't (default: ask)
#
# Writes only:
#   ~/.local/lib/slp/                         libraries, settings files, tools, uninstall.sh
#   ~/.local/share/vulkan/implicit_layer.d/slp_layer.json
# The layer only acts inside vrlink (Pyrowave decoding, chosen per stream: HEVC streams stay on the hardware decoder)
# and, with full colour, vrcompositor. Every other process passes straight through it.
# Undo: ~/.local/lib/slp/uninstall.sh   (or touch ~/.local/lib/slp/DISABLED to switch it off without removing it)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIBDIR="$HOME/.local/lib/slp"
MANDIR="$HOME/.local/share/vulkan/implicit_layer.d"
FC=keep
REBOOT=ask
for a in "$@"; do
  case "$a" in
    --full-colour|--full-color) FC=on ;;
    --no-full-colour|--no-full-color) FC=off ;;
    --reboot) REBOOT=yes ;;
    --no-reboot) REBOOT=no ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (see --help)"; exit 1 ;;
  esac
done
# Offer the reboot that finishes the change. Restarting only SteamVR can leave some of its processes on the previous
# version of the layer, and the Frame may then not shut down cleanly the next time.
offer_reboot() {   # $1 = install | removal
  local ans="$REBOOT"
  if [ "$ans" = ask ]; then
    if [ -t 0 ]; then
      echo "Reboot the Frame now to finish the $1? If you don't, reboot it soon: until then the Frame may not shut down"
      read -r -p "cleanly the first time (hold the power button if it hangs). Reboot now? [Y/n] " ans || ans=n
      case "$ans" in [nN]*) ans=no ;; *) ans=yes ;; esac
    else
      ans=no
    fi
  fi
  if [ "$ans" = yes ]; then
    echo "Rebooting..."
    systemctl reboot 2>/dev/null || sudo systemctl reboot
  else
    echo "Not rebooting. Reboot the Frame before using it, or it may not shut down cleanly the first time."
  fi
}
[ "$(uname -m)" = aarch64 ] || { echo "This is the Steam Frame part (aarch64); this machine is $(uname -m)."; exit 1; }
VERSION="$(cat "$HERE/VERSION" 2>/dev/null || echo dev)"
echo "Steam Link Pyrowave $VERSION - headset install"
mkdir -p "$LIBDIR/tools" "$MANDIR"
for f in libslp_layer.so libslp_pyrodec.so libpyrowave-shared.so.1; do
  install -m 0644 "$HERE/lib/$f" "$LIBDIR/$f"   # new inodes: a running vrlink keeps its old copy until it restarts
done
sed "s|@LIBPATH@|$LIBDIR/libslp_layer.so|" "$HERE/lib/slp_layer.json.in" > "$MANDIR/slp_layer.json"
chmod 0644 "$MANDIR/slp_layer.json"
install -m 0755 "$HERE/uninstall.sh" "$LIBDIR/uninstall.sh"
for f in "$HERE"/tools/*; do install -m 0755 "$f" "$LIBDIR/tools/"; done
echo "$VERSION" > "$LIBDIR/VERSION"
touch "$LIBDIR/AUTO"                     # Pyrowave streams are decoded by Pyrowave, HEVC streams stay stock
rm -f "$LIBDIR/RGBA_COMPRESSED"          # v7.21 opt-in file; compressed images are the default now
case "$FC" in
  on)  touch "$LIBDIR/FULLCOLOUR"; echo "full colour: on (turn \"Full colour\" on in Pyrowave Link on the PC)" ;;
  off) rm -f "$LIBDIR/FULLCOLOUR" /tmp/slp_fullcolour; echo "full colour: off" ;;
  *)   [ -e "$LIBDIR/FULLCOLOUR" ] && echo "full colour: on (unchanged)" || echo "full colour: off (./install.sh --full-colour turns it on)" ;;
esac
if [ -e "$LIBDIR/DISABLED" ]; then echo "note: ~/.local/lib/slp/DISABLED exists, so the layer stays off until you remove it"; fi
echo
echo "Installed to $LIBDIR. Remove it again with: $LIBDIR/uninstall.sh"
offer_reboot install
