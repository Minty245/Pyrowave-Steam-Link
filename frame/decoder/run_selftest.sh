#!/bin/bash
# Steam Link Pyrowave - Frame GPU decode self-test. User process only: no root, nothing installed,
# vrlink / SteamVR untouched. Writes its report next to this script.
# ./run_selftest.sh      everything
# ./run_selftest.sh fc   only the 768 full-colour run (v7.21 power checks and per-pass GPU times; a minute or two)
cd "$(dirname "$0")"
chmod +x ./slp_selftest 2>/dev/null   # Windows copies drop the execute bit
OUT="selftest-$(date +%H%M%S).txt"
{ echo "== os"; grep -E '^(VERSION_ID|BUILD_ID)=' /etc/os-release; ldd --version 2>&1 | head -1
  if [ "${1:-}" != fc ]; then
    echo "== run"; ./slp_selftest --iters 200; echo "exit=$?"
    echo "== run 960"; ./slp_selftest --iters 100 --width 960 --mode 1; echo "exit=$?"
    echo "== run 832 mode 4"; ./slp_selftest --iters 100 --width 832 --mode 4; echo "exit=$?"
  fi
  echo "== run 768 full colour (v7.19, v7.21 power)"; ./slp_selftest --iters 200 --width 768 --mode 6 --budget 173000; echo "exit=$?"; } 2>&1 | tee "$OUT"
echo "report: $PWD/$OUT"
