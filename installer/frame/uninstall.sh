#!/bin/bash
# Steam Link Pyrowave - remove the headset part completely (undo for install.sh). Takes effect after a reboot of the Frame
# (offered at the end). Logs in /tmp/slp_layer/ disappear with the next reboot.
#   --reboot / --no-reboot   reboot the Frame afterwards without asking / don't (default: ask)
set -uo pipefail
LIBDIR="$HOME/.local/lib/slp"
REBOOT=ask
for a in "$@"; do
  case "$a" in --reboot) REBOOT=yes ;; --no-reboot) REBOOT=no ;; esac
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
rm -f "$HOME/.local/share/vulkan/implicit_layer.d/slp_layer.json"
rm -f "$LIBDIR"/libslp_layer.so "$LIBDIR"/libslp_pyrodec.so "$LIBDIR"/libpyrowave-shared.so.1 "$LIBDIR"/VERSION \
      "$LIBDIR"/AUTO "$LIBDIR"/DISABLED "$LIBDIR"/MODE "$LIBDIR"/NODITHER "$LIBDIR"/postfx.txt "$LIBDIR"/FULLCOLOUR \
      "$LIBDIR"/RGBA_COMPRESSED "$LIBDIR"/RGBA_LINEAR "$LIBDIR"/NO_MERGE "$LIBDIR"/EXACT_PYRAMID "$LIBDIR"/uninstall.sh
rm -rf "$LIBDIR/tools"
rmdir "$LIBDIR" 2>/dev/null || true
rm -f /tmp/slp_arm /tmp/slp_arm_compositor /tmp/slp_decode /tmp/slp_fullcolour
echo "Steam Link Pyrowave removed from the Frame."
[ -d "$LIBDIR" ] && echo "note: $LIBDIR still has files that are not ours: $(ls -A "$LIBDIR" | tr '\n' ' ')"
offer_reboot removal
exit 0
