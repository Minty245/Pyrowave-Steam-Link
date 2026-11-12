#!/bin/bash
# Read-only: collects what is needed to diagnose a "connected, no video" stream. Run it on the Frame WHILE the stream
# is stuck (before restarting anything). Writes one archive next to this script; nothing else is changed.
# Copies: the slp layer logs of the last 20 minutes, the vrlink / vrcompositor journal lines of this boot, and the
# process list for vrlink/vrcompositor (start times show restarts). IPv4 addresses, Steam IDs and session IDs are
# replaced before anything is written.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
STAMP="$(date +%H%M%S)"
WORK="$(mktemp -d)"
redact() { sed -E 's/\b([0-9]{1,3}\.){3}[0-9]{1,3}\b/<ip>/g; s/7656119[0-9]{10}/<steamid>/g; s/(session[ _-]?(id)?[:= ]+)[0-9a-fA-F]{6,}/\1<id>/Ig; s/\b[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\b/<uuid>/g'; }
mkdir -p "$WORK/slp_layer"
if [ -d /tmp/slp_layer ]; then
  find /tmp/slp_layer -maxdepth 1 -name '*.log' -mmin -20 | while read -r f; do redact < "$f" > "$WORK/slp_layer/$(basename "$f")"; done
fi
{ echo "== processes (start time = last restart)"; ps -eo pid,lstart,etime,comm | grep -E 'vrlink|vrcompositor|vrserver' ;
  echo "== layer files"; ls -la "$HOME/.local/lib/slp" 2>/dev/null; ls -la /tmp/slp_* 2>/dev/null; } 2>&1 | redact > "$WORK/state.txt"
{ journalctl -b --no-pager -o short-monotonic 2>/dev/null | grep -iE 'vrlink|vrcompositor|segfault|core dump|coredump|killed' | tail -400; } | redact > "$WORK/journal.txt"
{ coredumpctl list --no-pager 2>/dev/null | grep -iE 'vrlink|vrcompositor' | tail -20; } | redact > "$WORK/coredumps.txt"
tar -czf "$HERE/stall-$STAMP.tar.gz" -C "$WORK" . && echo "wrote $HERE/stall-$STAMP.tar.gz"
rm -rf "$WORK"
