#!/bin/bash
# cross-host.sh : runs ON the host. Phases: GPU alone, ANE alone, both, GPU alone, both, ANE alone (30 s each).
# GPU = fp16 2048 matmul chain through omarchy-mlx; ANE = h13-filler.sh ONCE=1 ITER_SECONDS=30 (Parakeet whole encoder, bit-exact checked).
set -u
D=/var/tmp/cross
PY=/usr/lib/omarchy-mlx/venv/bin/python3
FILLER=/var/tmp/ane-filler/h13-filler.sh
STATS=/sys/class/accel/accel0/device/ane_stats
mkdir -p "$D"
power() { awk '{printf "%.1f", $1/1e6}' /sys/class/hwmon/hwmon2/power1_input 2>/dev/null; }
gpu() { "$PY" "$D/gpu_bench.py" "$1" "${MODE:-mm}" 2>&1 | tail -1; }
ane() { rm -f "$D/ane.log"; ONCE=1 ITER_SECONDS=30 LOG="$D/ane.log" bash "$FILLER" > /dev/null 2>&1; tail -1 "$D/ane.log"; }
phase() { echo "== $1 $(date -u +%FT%TZ) sysW_before=$(power)"; }
phase gpu-alone-1; echo "GPU $(gpu 30)"
phase ane-alone-1; echo "ANE $(ane)"
phase both-1
( ONCE=1 ITER_SECONDS=30 LOG="$D/ane.log" bash "$FILLER" > /dev/null 2>&1 ) &
rm -f "$D/ane.log"; sleep 3
echo "GPU $(gpu 25)"; wait; echo "ANE $(tail -1 "$D/ane.log")"
phase gpu-alone-2; echo "GPU $(gpu 30)"
phase both-2
rm -f "$D/ane.log"
( ONCE=1 ITER_SECONDS=30 LOG="$D/ane.log" bash "$FILLER" > /dev/null 2>&1 ) &
sleep 3
echo "GPU $(gpu 25)"; wait; echo "ANE $(tail -1 "$D/ane.log")"
phase ane-alone-2; echo "ANE $(ane)"
echo "== done $(date -u +%FT%TZ) busy_ns=$(awk '/^busy_ns/{print $2}' "$STATS")"
