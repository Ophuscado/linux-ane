#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Off-device check of m6-ane-stage.sh: fake uname, boot id, device tree node, insmod, rmmod, dmesg and lsmod, in a temp dir.
# Exits 0 iff every refusal fires and the order rules hold. It loads no module.
set -u
here=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d /dev/shm/h18-stage-test.XXXXXX)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/dt/soc/ane@419600000" "$T/work"
printf 'apple,t8152-ane\0' > "$T/dt/soc/ane@419600000/compatible"
echo "0123abcd-4567-89ab-cdef-0123456789ab" > "$T/bootid"
printf 'module-bytes' > "$T/work/ane_h18.ko"
KSHA=$(sha256sum "$T/work/ane_h18.ko" | cut -c1-64)
printf '#!/bin/bash\necho "$@" > "%s/insmod.args"\necho "[1.0] ane_h18 x: ane_h18 RESULT stage=${FAKE_STAGE:-dt} soc=t8152 verdict=${FAKE_VERDICT:-PASS} reason=fake" > "%s/dmesg.txt"\n' "$T" "$T" > "$T/insmod"
printf '#!/bin/bash\ntrue\n' > "$T/rmmod"
printf '#!/bin/bash\ncat "%s/dmesg.txt" 2>/dev/null\n' "$T" > "$T/dmesg"
printf '#!/bin/bash\nif [ -e "%s/loaded" ]; then echo "ane_h18 1 0"; fi\n' "$T" > "$T/lsmod"
chmod +x "$T"/insmod "$T"/rmmod "$T"/dmesg "$T"/lsmod
fails=0
run() {
  env WORK="$T/work" KO="$T/work/ane_h18.ko" KO_SHA256="$KSHA" WANT_UNAME=fake-kernel UNAME_R=fake-kernel BOOT_ID_FILE="$T/bootid" DT_NODE="$T/dt/soc/ane@419600000" \
    INSMOD="$T/insmod" RMMOD="$T/rmmod" DMESG="$T/dmesg" LSMOD="$T/lsmod" "$@" > "$T/out" 2>&1
}
expect() { # name, want rc, command...
  local name=$1 want=$2 rc
  shift 2
  run "$@"
  rc=$?
  if [ "$rc" = "$want" ]; then echo "  PASS $name"; else echo "  FAIL $name (rc $rc, want $want)"; tail -3 "$T/out"; fails=$((fails + 1)); fi
}
S="$here/m6-ane-stage.sh"
expect "no arguments refused" 2 bash "$S"
expect "step 2 before step 1 refused" 2 bash "$S" 2 0123abcd
expect "step 3 before step 2 refused" 2 bash "$S" 3 0123abcd GO
expect "wrong boot id refused" 2 bash "$S" 1 ffff
FAKE_STAGE=dt expect "step 1 passes" 0 bash "$S" 1 0123abcd
[ -e "$T/work/step1-pass-0123abcd" ] && echo "  PASS step 1 marker written" || { echo "  FAIL step 1 marker"; fails=$((fails + 1)); }
grep -q 'stage=dt' "$T/insmod.args" && echo "  PASS step 1 loads stage=dt" || { echo "  FAIL step 1 args"; fails=$((fails + 1)); }
expect "step 3 without GO refused" 2 bash "$S" 3 0123abcd
expect "step 3 after step 1 only refused" 2 bash "$S" 3 0123abcd GO
FAKE_STAGE=psd expect "step 2 passes" 0 bash "$S" 2 0123abcd
grep -q 'stage=psd' "$T/insmod.args" && echo "  PASS step 2 loads stage=psd" || { echo "  FAIL step 2 args"; fails=$((fails + 1)); }
FAKE_STAGE=power expect "step 3 passes with GO" 0 bash "$S" 3 0123abcd GO
grep -q 'stage=power confirm_power=1 psd_state=1 psd_slots=1' "$T/insmod.args" && echo "  PASS step 3 default args: slot 30 only, state 1" || { echo "  FAIL step 3 args"; fails=$((fails + 1)); }
expect "step 3 twice on one boot refused" 2 bash "$S" 3 0123abcd GO
rm -f "$T/work/step3-run-0123abcd"
sleep 1
FAKE_STAGE=power expect "step 3 with an explicit state 2 passes" 0 bash "$S" 3 0123abcd GO 2
grep -q 'psd_state=2 psd_slots=1' "$T/insmod.args" && echo "  PASS step 3 explicit state 2" || { echo "  FAIL step 3 state 2"; fails=$((fails + 1)); }
rm -f "$T/work/step3-run-0123abcd"
expect "step 3 with state 9 refused" 2 bash "$S" 3 0123abcd GO 9
[ ! -e "$T/work/step3-run-0123abcd" ] && echo "  PASS a refused state leaves no step 3 marker" || { echo "  FAIL marker after refused state"; fails=$((fails + 1)); }
rm -f "$T/work/step3-run-0123abcd"
sleep 1
touch "$T/loaded"
expect "module already loaded refused" 2 bash "$S" 1 0123abcd
rm -f "$T/loaded"
printf 'tampered' > "$T/work/ane_h18.ko"
expect "wrong module sha256 refused" 2 bash "$S" 1 0123abcd
printf 'module-bytes' > "$T/work/ane_h18.ko"
rm -rf "$T/dt/soc/ane@419600000"
expect "missing ANE node refused" 2 bash "$S" 1 0123abcd
mkdir -p "$T/dt/soc/ane@419600000"
printf 'apple,t8112-ane\0' > "$T/dt/soc/ane@419600000/compatible"
expect "wrong compatible refused" 2 bash "$S" 1 0123abcd
printf 'apple,t8152-ane\0' > "$T/dt/soc/ane@419600000/compatible"
rm -f "$T/work/step1-pass-0123abcd"
sleep 1   # run directories are named by the second
FAKE_VERDICT=FAIL FAKE_STAGE=dt expect "a FAIL verdict gives rc 1 and no marker" 1 bash "$S" 1 0123abcd
[ ! -e "$T/work/step1-pass-0123abcd" ] && echo "  PASS no marker after FAIL" || { echo "  FAIL marker after FAIL"; fails=$((fails + 1)); }
env WORK="$T/work" KO="$T/work/ane_h18.ko" WANT_UNAME=other UNAME_R=fake-kernel bash "$S" 1 0123abcd > "$T/out" 2>&1
[ $? = 2 ] && echo "  PASS wrong kernel release refused" || { echo "  FAIL wrong kernel release"; fails=$((fails + 1)); }
echo "$([ $fails = 0 ] && echo 'STAGE-SCRIPT TEST: all PASS' || echo "STAGE-SCRIPT TEST: $fails FAILED")"
exit $((fails > 0))
