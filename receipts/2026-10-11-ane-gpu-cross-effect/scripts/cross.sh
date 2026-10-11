#!/bin/bash
# cross.sh HOST_SHORT SSH_ALIAS : exclusive quiet window, stage the bench, run the phases, clear the window, save the output.
set -u
wh=$1; al=$2
D=$HOME/scratch/upstream/cross
QW=$HOME/src/omarchy-mplus-private/tools/idle-guard/quiet-window.sh
trap '"$QW" "$wh" clear' EXIT
timeout 30 ssh -o ConnectTimeout=8 -o BatchMode=yes "$al" "mkdir -p /var/tmp/cross" || exit 1
scp -q -o ConnectTimeout=8 "$D/gpu_bench.py" "$D/cross-host.sh" "$al:/var/tmp/cross/" || exit 1
"$QW" "$wh" set --exclusive || { echo "window failed"; exit 1; }
echo "window set $(date -u +%FT%TZ)"
timeout 900 ssh -o ConnectTimeout=8 -o BatchMode=yes "$al" "MODE=${MODE:-mm} bash /var/tmp/cross/cross-host.sh" | tee "$D/out-$wh-${MODE:-mm}.txt"
