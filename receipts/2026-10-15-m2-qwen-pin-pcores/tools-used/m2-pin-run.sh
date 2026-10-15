#!/bin/bash
# Operator wrapper: SMC sampler (CPU 11, nice 19) beside the cleared pin wrapper, then stop the sampler. Touches nothing else.
set -u
KREL=$1
BOOT=$2
TS=$(date -u +%Y%m%dT%H%M%SZ)
SMC=/var/tmp/w73-bench/qwen-pin-smc-$TS.txt
OUT=/var/tmp/w73-bench/qwen-pin-ab-$TS
setsid taskset -c 11 nice -n 19 bash /var/tmp/w73-bench/m2-smc-sampler.sh "$SMC" 1500 > /dev/null 2>&1 < /dev/null &
sleep 3
echo "sampler: $SMC pid $(cat "$SMC.pid" 2> /dev/null)"
DRY=0 bash /var/tmp/w73-bench/m2-qwen-pin-ab.sh "$KREL" "$BOOT" "$OUT"
rc=$?
kill "$(cat "$SMC.pid")" 2> /dev/null
echo "pin wrapper rc=$rc out=$OUT smc=$SMC sampler lines $(wc -l < "$SMC")"
exit "$rc"
