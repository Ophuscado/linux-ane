#!/bin/bash
# M2: record-only SMC and battery sensor sampler, once per second, raw. Started by the operator next to the pin run (outside the harness, no hash change).
# Usage: bash m2-smc-sampler.sh <outfile> [max-seconds]   (stop early with: kill $(cat <outfile>.pid))
# One line per second: EPOCHREALTIME then name=value for every *_input of the macsmc_hwmon device (power in microwatts, fans in rpm, temperatures, rails)
# and the macsmc_battery power1_input. Reads only /sys/class/hwmon; touches no accelerator. Pinned by the operator to the E cores (taskset -c 0-3).
set -u
OUT=${1:?outfile}
MAX=${2:-1500}
[ ! -e "$OUT" ] || { echo "REFUSE: $OUT exists"; exit 1; }
smc=""
bat=""
for d in /sys/class/hwmon/hwmon*; do
	case "$(cat "$d/name" 2> /dev/null)" in
		macsmc_hwmon) smc=$d ;;
		macsmc_battery) bat=$d ;;
	esac
done
[ -n "$smc" ] || { echo "REFUSE: no macsmc_hwmon device"; exit 1; }
echo $$ > "$OUT.pid"
{
	echo "# sampler start $EPOCHREALTIME smc=$smc bat=$bat kernel=$(uname -r) boot=$(cut -c1-8 /proc/sys/kernel/random/boot_id)"
	for f in "$smc"/*_label; do echo "# $(basename "${f%_label}") = $(cat "$f" 2> /dev/null)"; done
} > "$OUT"
n=0
while [ "$n" -lt "$MAX" ]; do
	line="$EPOCHREALTIME"
	for f in "$smc"/*_input; do
		line="$line $(basename "${f%_input}")=$(cat "$f" 2> /dev/null)"
	done
	[ -n "$bat" ] && line="$line bat_power1=$(cat "$bat/power1_input" 2> /dev/null)"
	echo "$line" >> "$OUT"
	n=$((n + 1))
	sleep 1
done
