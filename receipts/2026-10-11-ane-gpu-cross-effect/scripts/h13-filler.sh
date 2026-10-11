#!/bin/bash
# h13-filler.sh : ANE filler for H13 hosts (13-inch M1 T8103, M1 Max T6001). Loops the Parakeet whole encoder
# (458,018,816 B program, 20 to 1,300 calls per ane-run process), checks the output bit-exact against the macOS gold
# (cmp, no python) and appends ONE line per iteration to the log. Needs only the m1slot staging (ane-run, pk-in, program).
# Takes /var/tmp/ane-run.lock for each ane-run process (the same lock ane-run users take), never kills anything but its own child.
# Exits 0 on SIGTERM/SIGINT or when $HOME/.local/state/idle-guard/ane-stop exists (checked every second, also during a call).
# Exits 3 after 5 failed iterations in a row. ITER_SECONDS (default 180) sets the length of an iteration.
set -u
ROOT=${ROOT:-/var/tmp/m1slot}
TOOLS=$ROOT/tree/tools
IN=$ROOT/pk-in
ANEC=${ANEC:-/usr/lib/omarchy-mlx/venv/lib/python3.14/site-packages/mlx/share/mlx-omarchy/parakeet-1/bundles/parakeet-encoder-whole/program-0.anec}
[ -f "$ANEC" ] || ANEC=$ROOT/parakeet/program-0.anec
ITER_SECONDS=${ITER_SECONDS:-180}
ONCE=${ONCE:-0}   # 1: one iteration per invocation (for a caller that loops), exit 0 only if bit-exact
STOP=${STOP:-$HOME/.local/state/idle-guard/ane-stop}
LOG=${LOG:-$ROOT/ane-filler-h13.log}
OUT=$ROOT/filler-out
STATS=/sys/class/accel/accel0/device/ane_stats
LOCK=/var/tmp/ane-run.lock
WANT_BYTES=458018816
mkdir -p "$OUT"
chip=$(tr -d '\0' < /proc/device-tree/compatible 2>/dev/null | grep -o 't[0-9]\{4\}' | head -1)
case "$chip" in t6001) ms=440 ;; *) ms=140 ;; esac
MSF=/var/tmp/ane-filler-h13.ms
[ -s "$MSF" ] && ms=$(head -1 "$MSF")
CHILD=0
on_term() { echo "$(date -u +%FT%TZ) class=h13 chip=${chip:-?} event=terminated" >> "$LOG"; [ "$CHILD" != 0 ] && kill -TERM "$CHILD" 2>/dev/null; exec 9>&-; exit 0; }
trap on_term TERM INT
for f in "$TOOLS/ane-run" "$IN/mask.f16" "$IN/feat.f16" "$IN/gold-hidden.f16" "$ANEC"; do
	[ -f "$f" ] || { echo "$(date -u +%FT%TZ) class=h13 event=missing file=$f" >> "$LOG"; exit 2; }
done
[ "$(stat -c %s "$ANEC")" = "$WANT_BYTES" ] || { echo "$(date -u +%FT%TZ) class=h13 event=program-size-mismatch" >> "$LOG"; exit 2; }
fails=0
while [ ! -e "$STOP" ]; do
	n=$(( ITER_SECONDS * 1000 / ms )); [ "$n" -lt 5 ] && n=5; [ "$n" -gt 2000 ] && n=2000
	exec 9<>"$LOCK"
	tl0=$(date +%s.%N)
	flock -w 600 9 || { echo "$(date -u +%FT%TZ) class=h13 event=lock-timeout" >> "$LOG"; exec 9>&-; sleep 10; continue; }
	[ -e "$STOP" ] && { exec 9>&-; break; }
	t0=$(date +%s.%N)
	b0=$(awk '/^busy_ns/{print $2}' "$STATS"); j0=$(awk '/^jobs/{print $2}' "$STATS")
	timeout $(( ITER_SECONDS * 3 + 300 )) "$TOOLS/ane-run" --anec "$ANEC" --in "0=$IN/mask.f16" --in "1=$IN/feat.f16" \
		--out "0=$OUT/hidden.f16" --out "1=$OUT/mask_out.f16" --tile-shift 9 --time --repeat "$n" > "$OUT/exec.txt" 2>&1 &
	CHILD=$!
	while kill -0 "$CHILD" 2>/dev/null; do
		[ -e "$STOP" ] && kill -TERM "$CHILD" 2>/dev/null
		sleep 1
	done
	wait "$CHILD"; rc=$?
	CHILD=0
	b1=$(awk '/^busy_ns/{print $2}' "$STATS"); j1=$(awk '/^jobs/{print $2}' "$STATS")
	exec 9>&-
	[ -e "$STOP" ] && { echo "$(date -u +%FT%TZ) class=h13 chip=${chip:-?} event=stop-file-interrupted-iteration" >> "$LOG"; break; }
	[ "$rc" -ge 128 ] && { echo "$(date -u +%FT%TZ) class=h13 event=killed-by-signal rc=$rc" >> "$LOG"; [ "$ONCE" = 1 ] && exit 0; continue; }
	exact=no
	[ "$rc" -eq 0 ] && cmp -s -n 480000 "$OUT/hidden.f16" "$IN/gold-hidden.f16" && exact=yes
	t1=$(date +%s.%N)
	med=$(sed -n 's/.* median \([0-9.]*\) .*/\1/p' "$OUT/exec.txt" | head -1)
	p90=$(sed -n 's/.* p90 \([0-9.]*\) .*/\1/p' "$OUT/exec.txt" | head -1)
	line=$(awk -v t0="$t0" -v t1="$t1" -v tl0="$tl0" -v b0="$b0" -v b1="$b1" -v j0="$j0" -v j1="$j1" -v n="$n" -v med="${med:-0}" -v p90="${p90:-0}" -v rc="$rc" -v ex="$exact" -v chip="${chip:-?}" \
		'BEGIN{w=t1-t0; printf "class=h13 chip=%s n=%d calls=%d median_ms=%s p90_ms=%s busy_ms_job=%.2f wall_s=%.2f lockwait_s=%.1f busy_pct=%.1f bitexact=%s rc=%d", chip, n, j1-j0, med, p90, (j1>j0)?(b1-b0)/(j1-j0)/1e6:0, w, t0-tl0, (w>0)?(b1-b0)/(w*1e9)*100:0, ex, rc}')
	echo "$(date -u +%FT%TZ) $line" >> "$LOG"
	if [ "$rc" -eq 0 ] && [ "$exact" = yes ]; then
		fails=0
		[ -n "$med" ] && ms=$(awk -v m="$med" 'BEGIN{printf "%d", (m<1)?1:m}'); echo "$ms" > "$MSF"
	else
		[ "$ONCE" = 1 ] && exit 1
		fails=$(( fails + 1 ))
		[ -e "$STOP" ] && break
		[ "$fails" -ge 5 ] && { echo "$(date -u +%FT%TZ) class=h13 event=giving-up fails=$fails" >> "$LOG"; exit 3; }
		sleep 30
	fi
	[ "$ONCE" = 1 ] && exit 0
done
echo "$(date -u +%FT%TZ) class=h13 chip=${chip:-?} event=stop-file-exit" >> "$LOG"
exit 0
