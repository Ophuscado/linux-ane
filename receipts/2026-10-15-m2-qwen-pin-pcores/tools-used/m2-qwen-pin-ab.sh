#!/bin/bash
# M2 Max (T6021): pinned wrapper for the Qwen PINNED-against-UNPINNED run: the unchanged 38 programs, arms U P U P in one decoder process, one ane-session, one boot and one load.
# U = CPU mask 0-11, P = mask 4-11 (the P cores) on every task of the decode process and of its ane-session child, set between arms and read back from /proc; OPENBLAS_NUM_THREADS=4 in
# BOTH arms. Harness tools/qwen_pin_ab.py; pre-registered in the notebook entry 20261015T1530Z-m2-qwen-pin-pcores.md. Run ON THE M2 inside an exclusive window, ANE filler off, no GPU job,
# on a boot where the driver holds at most 100 MiB BO bytes after the filler has stopped (the rule of the 2026-10-10 qwen-resident-only-ticket.sh).
# Usage:  [DRY=1] bash m2-qwen-pin-ab.sh <kernel-release> <boot8> [outdir]
# The wrapper pins what runs and sets a runaway guard; the verdict is the harness's own exit code and summary.json. Nothing is rerun and nothing is relaxed here.
# DRY=1 checks every pin and prints the command, touches no ANE.
set -u
KREL=${1:?kernel release}
BOOT=${2:?boot id first 8 chars}
OUT=${3:-/var/tmp/w73-bench/qwen-pin-ab-$(date -u +%Y%m%dT%H%M%SZ)}
DRY=${DRY:-0}
SECONDS=0
CAP=${CAP:-1100}
B=/var/tmp/w73-bench/b822a785
T=$B/tools
PY=${PY:-python3}
MOD=/sys/module/ane_t6021
BT=$MOD/parameters/bo_total_bytes
FRESH_MAX=104857600
PIN_SESSION=918170858b603ede1941a30a7c6ca099da8fff6b5fcd0d93882d8acecfb4e3b3
PIN_MANIFEST=808d676d97aa654cda49e7ba516bd431bb56d1ca2660cb50e17f2698d287972a
PIN_PORTSUMS=0471b422bd3cbe2cb498c85de1559b0469d09a441700e02cb1051472f5bb14dc
PIN_ANECSUMS=3c7d08c243818223a552ca599eb8aa200e5869eb510540c8f6667d17a3cf2152
PIN_REFTOK=0fed31c8d3e6d726e9e6335ac259879a56b77e5461b6f37033faa53c140103d2
PIN_GGUF=4aa0fb13c431514262f259d420ecc95a8714df58ac2a2384514e20b93983f0ff
PIN_GGUF_SIZE=1312164224
PIN_GGUFPY=7ab433abda0ba8e663d8beacdc059561
PIN_HARNESS=9cdc3db42e027a910f9937b79d5fec5f4355d435be12b28d06d8fbda3dd12367
PORTS=/var/tmp/qwen-conform-0e2c3743-r2
ANEC=/var/tmp/qwen-real-anec-h14
DEC=/var/tmp/qwen-decode
export PYTHONPATH=$DEC/gguf-py
export PYTHONDONTWRITEBYTECODE=1
export OPENBLAS_NUM_THREADS=4
say() { echo "[$(date -u +%T)] $*"; }
fail() { say "REFUSE: $*"; exit 1; }
sha_of() { sha256sum "$1" | cut -d' ' -f1; }
st="$(uname -r) $(cut -c1-8 /proc/sys/kernel/random/boot_id) gpujobs=$(pgrep -fc '[g]pu-turn-exec') aneruns=$(pgrep -xc ane-run)"
say "pre-run state: $st load1=$(cut -d' ' -f1 /proc/loadavg) governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2> /dev/null) dry=$DRY"
say "module srcversion=$(cat $MOD/srcversion 2> /dev/null) version=$(cat $MOD/version 2> /dev/null) bo_total_bytes=$(cat $BT 2> /dev/null) bo_total_max_mb=$(cat $MOD/parameters/bo_total_max_mb 2> /dev/null)"
if [ "$DRY" != 1 ]; then
	[ "$st" = "$KREL $BOOT gpujobs=0 aneruns=0" ] || fail "not kernel $KREL boot $BOOT, or a GPU job or ane-run is live"
	held=$(cat $BT 2> /dev/null || echo 999999999999)
	[ "$held" -le "$FRESH_MAX" ] || fail "driver holds $held BO bytes, more than $FRESH_MAX: a fresh boot is required"
fi
[ "$(sha_of $B/tools/ane-session)" = "$PIN_SESSION" ] || fail "ane-session is not the pinned $PIN_SESSION"
[ "$(sha_of $DEC/manifest.json)" = "$PIN_MANIFEST" ] || fail "manifest.json is not the pinned one"
[ "$(sha_of $PORTS/PORTS-RESOLVED-SHA256SUMS)" = "$PIN_PORTSUMS" ] || fail "PORTS-RESOLVED-SHA256SUMS is not the pinned one"
(cd $PORTS && sha256sum -c PORTS-RESOLVED-SHA256SUMS > /dev/null 2>&1) || fail "the conform-r2 ports files do not match their sums"
[ "$(sha_of $ANEC/SHA256SUMS)" = "$PIN_ANECSUMS" ] || fail "the ANEC SHA256SUMS is not the pinned one"
(cd $ANEC && sha256sum -c SHA256SUMS > /dev/null 2>&1) || fail "the ANEC files do not match SHA256SUMS"
[ "$(sha_of $DEC/reference-tokens.json)" = "$PIN_REFTOK" ] || fail "reference-tokens.json is not the pinned one"
if [ "$(stat -c %s $DEC/Qwen3.8-2B-Q4_K_M.gguf)" != "$PIN_GGUF_SIZE" ] || [ "$(sha_of $DEC/Qwen3.8-2B-Q4_K_M.gguf)" != "$PIN_GGUF" ]; then
	fail "the GGUF is not the pinned file"
fi
(cd $B && sha256sum -c PY-PINS > /dev/null 2>&1) || fail "the python files of the head do not match PY-PINS"
[ "$(sha_of $T/qwen_pin_ab.py)" = "$PIN_HARNESS" ] || fail "qwen_pin_ab.py is not the reviewed harness $PIN_HARNESS"
gg=$(cd $DEC/gguf-py && find . -type f -name '*.py' -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-32)
[ "$gg" = "$PIN_GGUFPY" ] || fail "gguf-py (imported by load_head) tree hash $gg is not the pinned $PIN_GGUFPY"
say "pins ok: harness $(echo "$PIN_HARNESS" | cut -c1-16), session $(echo "$PIN_SESSION" | cut -c1-16), manifest $(echo "$PIN_MANIFEST" | cut -c1-8), ports $(echo "$PIN_PORTSUMS" | cut -c1-8), anec sums $(echo "$PIN_ANECSUMS" | cut -c1-8), tokens $(echo "$PIN_REFTOK" | cut -c1-8), gguf $(echo "$PIN_GGUF" | cut -c1-8), python files at the head; OPENBLAS_NUM_THREADS=$OPENBLAS_NUM_THREADS shell mask $(taskset -pc $$ | cut -d: -f2)"
[ ! -e "$OUT" ] || fail "OUT $OUT already exists: a new directory is required"
mkdir -p "$OUT" && mkdir "$OUT/ab" || fail "cannot create $OUT"
cd "$T" || exit 1
CMD=("$PY" qwen_pin_ab.py --session-bin "$B/tools/ane-session" --out "$OUT/ab")
say "command: ${CMD[*]} (runaway guard $CAP s, harness timeout $((CAP - 20)) s)"
if [ "$DRY" = 1 ]; then
	say "DRY: not run"
	exit 0
fi
BO0=$(cat $BT)
say "bo_total_bytes before: $BO0"
timeout -k 15 $((CAP - 20)) "${CMD[@]}" > "$OUT/ab.stdout" 2> "$OUT/ab.stderr"
rc=$?
say "harness rc=$rc after $SECONDS s (0 = every gate holds; 1 = a gate failed; 2 = a refusal before any load; 3 = unfrozen; 124 or 137 = the runaway guard)"
tail -n 14 "$OUT/ab.stdout" | cut -c1-200
tail -n 5 "$OUT/ab.stderr" | cut -c1-200
say "bo_total_bytes after exit: $(cat $BT) (before: $BO0)"
sudo -n dmesg 2> /dev/null | grep -E "EXCH.*failed|DART.*(fault|error)|quarantin|wedge" | tail -n 5 | cut -c1-200
say "ane-session processes left: $(pgrep -xc ane-session)  ane-run: $(pgrep -xc ane-run)"
(cd "$OUT" && find . -type f \( -name '*.txt' -o -name '*.json' -o -name '*.tsv' -o -name '*.stdout' -o -name '*.stderr' -o -name '*.jsonl' -o -name '*.log' -o -name '*.f32' \) -print0 | xargs -0 sha256sum > SHA256SUMS 2> /dev/null; echo "sealed $(wc -l < SHA256SUMS) files")
# every sealed logits file of the four arms against the sha256 the summary holds for it
"$PY" - "$OUT" << 'PYEOF'
import hashlib
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
sj = out / "ab" / "summary.json"
if not sj.exists():
    print("LOGITS_SEAL_NO_SUMMARY: ab/summary.json is missing, nothing to compare")
    sys.exit(5)
summary = json.loads(sj.read_text())
arms = summary.get("arms_meta", [])
checks = []
for i, arm in enumerate(arms):
    checks.append((f"timing arm {i + 1} {arm.get('arm')}", out / "ab" / f"arm{i + 1}-{arm.get('arm')}" / "logits.f32", arm.get("logits_sha256")))
bad = 0
for label, f, want in checks:
    got = hashlib.sha256(f.read_bytes()).hexdigest() if f.exists() else None
    ok = got is not None and got == want
    bad += not ok
    print(f"LOGITS_SEAL {label}: sealed {str(got)[:16]} record {str(want)[:16]} {'MATCH' if ok else 'MISMATCH'}")
print("LOGITS_SEAL_OK" if arms and checks and not bad else "LOGITS_SEAL_MISMATCH")
sys.exit(0 if arms and checks and not bad else 5)
PYEOF
seal_rc=$?
if [ "$rc" = 0 ] && [ "$seal_rc" != 0 ]; then
	say "harness rc 0 but the sealed logits do not match the summary: exit 5"
	rc=5
fi
say "done: $OUT"
exit "$rc"
