#!/bin/bash
# Read-only: collects what is needed to diagnose a "connected, no video" stream. Run it on the Frame WHILE the stream
# is stuck (before restarting anything). Writes one archive next to this script; nothing else is changed.
# Copies: the slp layer logs of the last 20 minutes, the vrlink / vrcompositor journal lines of this boot, the
# process list for vrlink/vrcompositor (start times show restarts), the kernel's video decoder / GPU messages, and
# (v7.31) where each vrlink / vrcompositor thread is waiting right now (a hung stream shows which one is stuck and in
# what: the video driver, a lock, ...). A copy also goes to stall-latest.tar.gz (a fixed name, easy to fetch over SSH). IPv4 addresses, Steam IDs and session IDs are
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
{ journalctl -k -b --no-pager -o short-monotonic 2>/dev/null |
    grep -iE 'iris|venus|video|vidc|v4l2|msm|adreno|kgsl|gpu|drm|fault|hang|timeout|watchdog|smmu' | tail -400; } | redact > "$WORK/kernel.txt"
{ for p in $(pgrep -x vrlink; pgrep -x vrcompositor); do
    echo "== $(cat /proc/$p/comm 2>/dev/null) (pid $p)"
    for t in /proc/$p/task/*; do
      printf '%s %-16s state=%s wchan=%s\n' "${t##*/}" "$(cat "$t/comm" 2>/dev/null)" \
        "$(awk '{print $3}' "$t/stat" 2>/dev/null)" "$(cat "$t/wchan" 2>/dev/null)"
    done
  done; } 2>&1 | redact > "$WORK/threads.txt"
tar -czf "$HERE/stall-$STAMP.tar.gz" -C "$WORK" . && cp -f "$HERE/stall-$STAMP.tar.gz" "$HERE/stall-latest.tar.gz" &&
  echo "wrote $HERE/stall-$STAMP.tar.gz"
rm -rf "$WORK"
