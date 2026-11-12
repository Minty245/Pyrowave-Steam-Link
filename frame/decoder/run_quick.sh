#!/bin/bash
# Steam Link Pyrowave - quick Frame self-test: only the stream you use (832 wide, mode 4). User process only.
cd "$(dirname "$0")"
chmod +x ./slp_selftest 2>/dev/null
OUT="selftest-quick-$(date +%H%M%S).txt"
./slp_selftest --iters 100 --width "${1:-832}" --mode 4 2>&1 | tee "$OUT"
echo "report: $PWD/$OUT"
