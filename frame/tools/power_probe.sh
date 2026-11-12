#!/bin/bash
# Read-only power / clock sampler (v2) for the Steam Frame. Changes nothing (no clocks, governors or files outside this
# folder). Run it while a stream is going: ./power_probe.sh <label> [seconds]   e.g. ./power_probe.sh hevc 30
# Writes power-<label>-<time>.txt next to this script: battery power, GPU clock + busy %, CPU clocks and load,
# and CPU time of vrlink / vrcompositor. No addresses or IDs are collected.
set -u
LABEL="${1:-run}"; SECS="${2:-30}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/power-$LABEL-$(date +%H%M%S).txt"
rd() { [ -r "$1" ] && cat "$1" 2>/dev/null; }
# find sources once
BAT=$(ls -d /sys/class/power_supply/* 2>/dev/null | while read -r d; do [ "$(rd $d/type)" = "Battery" ] && echo "$d"; done | head -1)
GPU=$(ls -d /sys/class/devfreq/* 2>/dev/null | grep -iE 'gpu|kgsl|adreno|3d00000' | head -1)
[ -z "$GPU" ] && GPU=$(ls -d /sys/class/devfreq/* 2>/dev/null | head -1)
KGSL=/sys/class/kgsl/kgsl-3d0
# by executable name: vrlink renames its main thread (top shows "vrlinkr..."), so pgrep -x vrlink finds nothing
pid_of() { for p in /proc/[0-9]*; do [ "$(basename "$(readlink "$p/exe" 2>/dev/null)")" = "$1" ] && { echo "${p#/proc/}"; return; }; done; }
cpu_ticks() { local p="$1"; [ -n "$p" ] && [ -r /proc/$p/stat ] && awk '{print $14+$15}' /proc/$p/stat || echo 0; }
{
echo "== power_probe $LABEL, ${SECS}s"
echo "battery: ${BAT:-none}   gpu devfreq: ${GPU:-none}   kgsl: $([ -d $KGSL ] && echo yes || echo no)"
[ -n "$GPU" ] && echo "gpu governor: $(rd $GPU/governor)  available: $(rd $GPU/available_frequencies)"
for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$(basename $p): $(rd $p/scaling_governor) $(rd $p/related_cpus | tr '\n' ' ')"; done
echo
echo "battery status now: $( [ -n "$BAT" ] && rd $BAT/status )   (power_W keeps its sign: compare runs with the same status)"
[ -n "$GPU" ] && echo "gpu polling_interval: $(rd $GPU/polling_interval) ms  min/max: $(rd $GPU/min_freq) / $(rd $GPU/max_freq)"
echo "t  power_W  gpu_MHz(min-avg-max)  gpu_busy%  cpu_load%  cpu_MHz(per policy)  battery"
VL=$(pid_of vrlink); VC=$(pid_of vrcompositor)
vl0=$(cpu_ticks "$VL"); vc0=$(cpu_ticks "$VC")
read -r _ a b c d e f g _ < /proc/stat; idle0=$((d+e)); tot0=$((a+b+c+d+e+f+g))
sumP=0; n=0
ts_snap() { [ -n "$GPU" ] && [ -r $GPU/trans_stat ] && awk '/^[ *]+[0-9]+:/{gsub(/\*/,""); print $1, $NF}' $GPU/trans_stat; }
TS0="$(ts_snap)"
for t in $(seq 1 "$SECS"); do
  P=""
  if [ -n "$BAT" ]; then
    pn=$(rd $BAT/power_now)
    # v2: sign kept (the meaning of + / - depends on the gauge; the status column says charging or not)
    if [ -n "$pn" ]; then P=$(awk -v x="$pn" 'BEGIN{printf "%.2f", x/1e6}')
    else i=$(rd $BAT/current_now); v=$(rd $BAT/voltage_now); [ -n "$i" ] && [ -n "$v" ] && P=$(awk -v i="$i" -v v="$v" 'BEGIN{printf "%.2f", i*v/1e12}'); fi
  fi
  # v2: GPU clock sampled 10x in that second (min-avg-max)
  F=""; if [ -n "$GPU" ]; then F=$(for k in 1 2 3 4 5 6 7 8 9 10; do cat $GPU/cur_freq 2>/dev/null; sleep 0.09; done | awk '{v=$1/1e6; s+=v; if(n==0||v<mn)mn=v; if(v>mx)mx=v; n++} END{if(n) printf "%.0f-%.0f-%.0f", mn, s/n, mx}'); else sleep 1; fi
  B=""; [ -r $KGSL/gpu_busy_percentage ] && B=$(rd $KGSL/gpu_busy_percentage | tr -d ' %')
  [ -z "$B" ] && [ -r $GPU/load ] && B=$(rd $GPU/load | cut -d@ -f1)
  read -r _ a b c d e f g _ < /proc/stat; idle=$((d+e)); tot=$((a+b+c+d+e+f+g))
  L=$(awk -v di=$((idle-idle0)) -v dt=$((tot-tot0)) 'BEGIN{ if (dt>0) printf "%.0f", 100*(1-di/dt); }'); idle0=$idle; tot0=$tot
  C=$(for p in /sys/devices/system/cpu/cpufreq/policy*; do awk '{printf "%.0f ", $1/1000}' $p/scaling_cur_freq 2>/dev/null; done)
  ST=$( [ -n "$BAT" ] && rd $BAT/status )
  echo "$t  ${P:-?}  ${F:-?}  ${B:-?}  ${L:-?}  $C  ${ST:-}"
  [ -n "$P" ] && sumP=$(awk -v s=$sumP -v p=$P 'BEGIN{print s+p}') && n=$((n+1))
done
vl1=$(cpu_ticks "$VL"); vc1=$(cpu_ticks "$VC"); hz=$(getconf CLK_TCK)
echo
[ $n -gt 0 ] && echo "average battery power: $(awk -v s=$sumP -v n=$n 'BEGIN{printf "%.2f", s/n}') W over $n s"
echo "vrlink CPU: $(awk -v d=$((vl1-vl0)) -v h=$hz -v s=$SECS 'BEGIN{printf "%.0f", 100*d/h/s}')% of one core;  vrcompositor CPU: $(awk -v d=$((vc1-vc0)) -v h=$hz -v s=$SECS 'BEGIN{printf "%.0f", 100*d/h/s}')% of one core"
TS1="$(ts_snap)"
if [ -n "$TS0" ] && [ -n "$TS1" ]; then
  echo; echo "== GPU time at each clock during this run"
  join <(echo "$TS0" | sort) <(echo "$TS1" | sort) | awk '{d=$3-$2; t+=d; f[$1]=d} END{for (k in f) if (f[k]>0) printf "%5.0f MHz  %6.1f s  %5.1f%%\n", k/1e6, f[k]/1000, 100*f[k]/t}' | sort -n
fi
} 2>&1 | tee "$OUT"
echo "wrote $OUT"
