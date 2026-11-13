#!/bin/bash
# Steam Link Pyrowave - why the Steam Frame did not shut down cleanly (read-only).
#
# Run it after a boot that followed a hard power-off:
#     bash shutdown_probe.sh
# It reads the system journal of the previous boot (journalctl asks for your password via sudo; type it yourself)
# and writes ~/slp_shutdown_report.txt. Nothing on the system is changed. Addresses, MAC addresses, Steam IDs,
# serial numbers and host names are redacted from the report.

out="$HOME/slp_shutdown_report.txt"
host=$(hostname 2>/dev/null)

redact() {
	sed -E \
		-e 's/\b([0-9]{1,3}\.){3}[0-9]{1,3}\b/<ipv4>/g' \
		-e 's/\b([0-9a-fA-F]{1,4}:){2,7}[0-9a-fA-F]{1,4}\b/<ipv6>/g' \
		-e 's/\b([0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b/<mac>/g' \
		-e 's/\b7656119[0-9]{10}\b/<steamid>/g' \
		-e 's/(serial|Serial|SERIAL)([ _:=]+)[A-Za-z0-9-]+/\1\2<serial>/g' \
		-e 's/(SessionID|session_id|ConnectionKey|connect_key|AccountID|account_id)([":= ]+)[A-Za-z0-9_-]+/\1\2<id>/g' \
		${host:+-e "s/\\b${host}\\b/<host>/g"}
}

{
	echo "== Steam Link Pyrowave shutdown probe, $(date -u +%Y-%m-%dT%H:%MZ)"
	echo "== Frame install: $(cat ~/.local/lib/slp/VERSION 2>/dev/null || echo 'not found')$( [ -e ~/.local/lib/slp/DISABLED ] && echo ' (DISABLED)')"
	echo "== Full colour armed: $( [ -e ~/.local/lib/slp/FULLCOLOUR ] && echo yes || echo no)"
	echo
	echo "== Boots (most recent last)"
	sudo journalctl --list-boots --no-pager 2>&1 | tail -n 4
	echo
	echo "== Previous boot: shutdown problems"
	sudo journalctl -b -1 --no-pager -o short-monotonic 2>&1 | grep -E -i \
		'timed out|timeout|Killing process|still around|SIGKILL|SIGTERM|watchdog|hung_task|blocked for more than|Failed to (stop|unmount)|A stop job|Stopping|Stopped|Reached target|vrlink|vrcompositor|vrserver|steamvr|slp|pyrowave|msm|adreno|kgsl|gpu hang|fault' |
		tail -n 400
	echo
	echo "== Previous boot: last 250 lines"
	sudo journalctl -b -1 --no-pager -o short-monotonic 2>&1 | tail -n 250
} | redact > "$out"

echo "Wrote $out ($(wc -l < "$out") lines). Have a quick look before sharing it."
