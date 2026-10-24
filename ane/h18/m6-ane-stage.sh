#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# m6-ane-stage.sh: run ONE ane_h18 stage on the M6 (T8152), as root, and collect the log.
#   m6-ane-stage.sh <1|2|3> <boot-id prefix> [GO [state [slots [mbox [ctrl]]]]]
# Step 1 = stage dt (no MMIO), step 2 = stage psd (read-only PSD slots 30 and 31), step 3 = stage power with
# confirm_power=1 (requests PSD state <state> on the first <slots> slots, default 1 = slot 30 only, then reads CPU_STATUS,
# ASC_STATUS and IORVBAR; mbox=1 also reads the Postoffice status words of three candidate channels, ctrl=1 also reads CPU_CONTROL).
# The state defaults to 1: the AppleT8152PMGR kext decode in the lab receipts reads "on" as request 1 (or 2) for these slots,
# and names 1 as the first experiment. 0 is the value proven for the ATC USB slot 71 and GPU slot 3, 2 is the GPU ASC value.
# Step 3 needs the literal third argument GO (the operator's go-ahead) and a step 2 PASS on this boot. It may run again on
# the same boot only after a step 3 PASS whose slots returned to state 6; a failed or refused-late step 3 needs a reboot.
# Order is enforced per boot: step 2 needs a step 1 PASS marker, step 3 a step 2 PASS marker, in the work directory.
# Refuses on: wrong kernel release, boot id not matching (at least 8 hex characters), missing ANE node, a module whose sha256
# is not the pinned one, a module already loaded, a reused run directory, and any of the variables below set in the
# environment (they are for test-stage-script.sh, which sets M6_ANE_TEST=1). Writes only under $WORK.
# The verdict is read only from RESULT lines that this run added to the kernel log, for the stage of this step.
# A step 3 PASS whose log has a "did not return to state 6" line exits 3: read it before the next step.
# A hang costs one reboot.
set -u

if [ "${M6_ANE_TEST:-}" != 1 ]; then
  for v in WORK KO KO_SHA256 WANT_UNAME UNAME_R BOOT_ID_FILE DT_NODE INSMOD RMMOD DMESG LSMOD; do
    if [ -n "${!v+x}" ]; then echo "m6-ane-stage: REFUSED: $v is set in the environment" >&2; exit 2; fi
  done
fi

WORK=${WORK:-/var/tmp/m6-ane}
KO=${KO:-$WORK/ane_h18.ko}
KO_SHA256=${KO_SHA256:-e75925f3257d87bd823323864a6c74eae4a4c481ab92bedcfdbb5ac08ca8f168}
WANT_UNAME=${WANT_UNAME:-7.1.12-aurora-m6usb-ARCH}
UNAME_R=${UNAME_R:-$(uname -r)}
BOOT_ID_FILE=${BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}
DT_NODE=${DT_NODE:-/proc/device-tree/soc/ane@419600000}
INSMOD=${INSMOD:-insmod}
RMMOD=${RMMOD:-rmmod}
DMESG=${DMESG:-dmesg}
LSMOD=${LSMOD:-lsmod}

step=${1:-}
bid=${2:-}
go=${3:-}
state=${4:-1}
slots=${5:-1}
mbox=${6:-0}
ctrl=${7:-0}
die() { echo "m6-ane-stage: REFUSED: $*" >&2; exit 2; }

case "$step" in 1|2|3) ;; *) die "usage: $0 <1|2|3> <boot-id prefix> [GO [state]]" ;; esac
case "$state" in 0|1|2|3|4|5) ;; *) die "state must be 0 to 5" ;; esac
case "$slots" in 1|2) ;; *) die "slots must be 1 or 2" ;; esac
case "$mbox$ctrl" in 00|01|10|11) ;; *) die "mbox and ctrl must be 0 or 1" ;; esac
[ "${#bid}" -ge 8 ] || die "boot id prefix of at least 8 hex characters required"
[ "$UNAME_R" = "$WANT_UNAME" ] || die "kernel release $UNAME_R, want $WANT_UNAME"
boot=$(tr -d '-' < "$BOOT_ID_FILE" | cut -c1-8)
case "$(tr -d '-' < "$BOOT_ID_FILE")" in "$bid"*) ;; *) die "boot id $boot does not start with $bid" ;; esac
[ -e "$DT_NODE/compatible" ] || die "no $DT_NODE: this DTB has no ANE node"
[ "$(tr -d '\0' < "$DT_NODE/compatible")" = "apple,t8152-ane" ] || die "ANE node compatible is not apple,t8152-ane"
[ -f "$KO" ] || die "no module at $KO"
[ "$(sha256sum "$KO" | cut -c1-64)" = "$KO_SHA256" ] || die "module sha256 is not the pinned $KO_SHA256"
if $LSMOD | grep -q '^ane_h18 '; then die "ane_h18 is already loaded; rmmod it and reboot if step 3 ran"; fi
mkdir -p "$WORK"
if [ "$step" = 2 ] && [ ! -e "$WORK/step1-pass-$boot" ]; then die "step 1 has not passed on boot $boot"; fi
if [ "$step" = 3 ]; then
  [ "$go" = GO ] || die "step 3 needs the third argument GO (the go-ahead on the PSD state)"
  [ -e "$WORK/step2-pass-$boot" ] || die "step 2 has not passed on boot $boot"
  [ ! -e "$WORK/step3-run-$boot" ] || die "step 3 already ran on boot $boot: reboot first, the slot state is unknown"
fi

run="$WORK/run-$(date -u +%Y%m%dT%H%M%SZ)-step$step-boot$boot"
[ ! -e "$run" ] || die "run directory exists"
mkdir "$run"
{
  echo "step=$step boot=$boot uname=$UNAME_R ko_sha256=$KO_SHA256 date=$(date -u +%FT%TZ)"
  uname -v
} > "$run/header.txt"

case "$step" in
  1) args="optin=t8152 stage=dt" ;;
  2) args="optin=t8152 stage=psd" ;;
  3) args="optin=t8152 stage=power confirm_power=1 psd_state=$state psd_slots=$slots"
     [ "$mbox" = 1 ] && args="$args read_mbox=1"
     [ "$ctrl" = 1 ] && args="$args read_ctrl=1"
     : > "$WORK/step3-run-$boot" ;;
esac
case "$step" in 1) want_stage=dt ;; 2) want_stage=psd ;; 3) want_stage=power ;; esac
n0=$($DMESG 2>&1 | grep -c 'ane_h18 RESULT')
echo "insmod $KO $args (RESULT lines already in the kernel log: $n0)" | tee -a "$run/header.txt"
timeout 60 $INSMOD "$KO" $args > "$run/insmod.out" 2>&1
rc=$?
echo "insmod rc=$rc" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18.txt"
cat "$run/dmesg-ane_h18.txt"
timeout 60 $RMMOD ane_h18 > "$run/rmmod.out" 2>&1
echo "rmmod rc=$?" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18-after-rmmod.txt"
grep 'ane_h18 RESULT' "$run/dmesg-ane_h18.txt" | tail -n +$((n0 + 1)) > "$run/new-result-lines.txt"
verdict=$(grep -o "RESULT stage=$want_stage soc=t8152 verdict=[A-Z]* reason=[^ ]*" "$run/new-result-lines.txt" | tail -1)
echo "verdict: ${verdict:-none} (new RESULT lines: $(wc -l < "$run/new-result-lines.txt"), wanted stage $want_stage)" | tee -a "$run/header.txt"
if echo "$verdict" | grep -q 'verdict=PASS'; then
  if [ "$step" = 3 ] && grep -q 'did not return to state 6' "$run/dmesg-ane_h18.txt"; then
    echo "m6-ane-stage: step 3 PASS but a PSD slot did not return to state 6, log in $run" >&2
    exit 3
  fi
  if [ "$step" = 3 ]; then rm -f "$WORK/step3-run-$boot"; else : > "$WORK/step$step-pass-$boot"; fi
  echo "m6-ane-stage: step $step PASS, log in $run"
  exit 0
fi
echo "m6-ane-stage: step $step did not pass (rc=$rc), log in $run" >&2
exit 1
