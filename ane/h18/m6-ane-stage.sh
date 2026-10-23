#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# m6-ane-stage.sh: run ONE ane_h18 stage on the M6 (T8152), as root, and collect the log.
#   m6-ane-stage.sh <1|2|3> <boot-id prefix> [GO]
# Step 1 = stage dt (no MMIO), step 2 = stage psd (read-only PSD slots 30 and 31), step 3 = stage power with
# confirm_power=1 (requests PSD state 0 on slot 30 only, then reads CPU_STATUS, ASC_STATUS, IORVBAR).
# Step 3 needs the literal third argument GO (the word of Main or w74 on the PSD state) and a step 2 PASS on this boot.
# Order is enforced per boot: step 2 needs a step 1 PASS marker, step 3 a step 2 PASS marker, in the work directory.
# Refuses on: wrong kernel release, boot id not matching, missing ANE node, a module whose sha256 is not the pinned one,
# a module already loaded, a reused run directory. Writes only under $WORK. A hang costs one reboot.
set -u

WORK=${WORK:-/var/tmp/m6-ane}
KO=${KO:-$WORK/ane_h18.ko}
KO_SHA256=${KO_SHA256:-9499a79e003e7b3b2cbc278253d762ea99b284fc3544818b821530c4675b48a6}
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
die() { echo "m6-ane-stage: REFUSED: $*" >&2; exit 2; }

case "$step" in 1|2|3) ;; *) die "usage: $0 <1|2|3> <boot-id prefix> [GO]" ;; esac
[ -n "$bid" ] || die "boot id prefix required"
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
  [ "$go" = GO ] || die "step 3 needs the third argument GO (the word of Main or w74 on the PSD state)"
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
  3) args="optin=t8152 stage=power confirm_power=1 psd_state=0 psd_slots=1"; : > "$WORK/step3-run-$boot" ;;
esac
echo "insmod $KO $args" | tee -a "$run/header.txt"
timeout 60 $INSMOD "$KO" $args > "$run/insmod.out" 2>&1
rc=$?
echo "insmod rc=$rc" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18.txt"
cat "$run/dmesg-ane_h18.txt"
timeout 60 $RMMOD ane_h18 > "$run/rmmod.out" 2>&1
echo "rmmod rc=$?" | tee -a "$run/header.txt"
$DMESG 2>&1 | grep ane_h18 > "$run/dmesg-ane_h18-after-rmmod.txt"
verdict=$(grep -o 'RESULT stage=[a-z]* soc=t8152 verdict=[A-Z]* reason=[^ ]*' "$run/dmesg-ane_h18.txt" | tail -1)
echo "verdict: ${verdict:-none}" | tee -a "$run/header.txt"
if echo "$verdict" | grep -q 'verdict=PASS'; then
  [ "$step" = 3 ] || : > "$WORK/step$step-pass-$boot"
  echo "m6-ane-stage: step $step PASS, log in $run"
  exit 0
fi
echo "m6-ane-stage: step $step did not pass (rc=$rc), log in $run" >&2
exit 1
