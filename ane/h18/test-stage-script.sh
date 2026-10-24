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
printf '#!/bin/bash\necho "$@" > "%s/insmod.args"\n[ -n "${FAKE_NOOP:-}" ] && exit 0\necho "[1.0] ane_h18 x: ane_h18 RESULT stage=${FAKE_STAGE:-dt} soc=t8152 verdict=${FAKE_VERDICT:-PASS} reason=fake" >> "%s/dmesg.txt"\n[ -n "${FAKE_NOT6:-}" ] && echo "[1.1] ane_h18 x: ane_h18 psd0 did not return to state 6" >> "%s/dmesg.txt"\nexit 0\n' "$T" "$T" "$T" > "$T/insmod"
printf '#!/bin/bash\ntrue\n' > "$T/rmmod"
printf '#!/bin/bash\ncat "%s/dmesg.txt" 2>/dev/null\n' "$T" > "$T/dmesg"
printf '#!/bin/bash\nif [ -e "%s/loaded" ]; then echo "ane_h18 1 0"; fi\n' "$T" > "$T/lsmod"
chmod +x "$T"/insmod "$T"/rmmod "$T"/dmesg "$T"/lsmod
fails=0
run() {
  env M6_ANE_TEST=1 WORK="$T/work" KO="$T/work/ane_h18.ko" KO_SHA256="$KSHA" WANT_UNAME=fake-kernel UNAME_R=fake-kernel BOOT_ID_FILE="$T/bootid" DT_NODE="$T/dt/soc/ane@419600000" \
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
expect "wrong boot id refused" 2 bash "$S" 1 ffffffff
FAKE_STAGE=dt expect "step 1 passes" 0 bash "$S" 1 0123abcd
[ -e "$T/work/step1-pass-0123abcd" ] && echo "  PASS step 1 marker written" || { echo "  FAIL step 1 marker"; fails=$((fails + 1)); }
grep -q 'stage=dt' "$T/insmod.args" && echo "  PASS step 1 loads stage=dt" || { echo "  FAIL step 1 args"; fails=$((fails + 1)); }
expect "step 3 without GO refused" 2 bash "$S" 3 0123abcd
expect "step 3 after step 1 only refused" 2 bash "$S" 3 0123abcd GO
FAKE_STAGE=psd expect "step 2 passes" 0 bash "$S" 2 0123abcd
grep -q 'stage=psd' "$T/insmod.args" && echo "  PASS step 2 loads stage=psd" || { echo "  FAIL step 2 args"; fails=$((fails + 1)); }
FAKE_STAGE=power expect "step 3 passes with GO" 0 bash "$S" 3 0123abcd GO
grep -q 'stage=power confirm_power=1 psd_state=1 psd_slots=1' "$T/insmod.args" && echo "  PASS step 3 default args: slot 30 only, state 1" || { echo "  FAIL step 3 args"; fails=$((fails + 1)); }
sleep 1
FAKE_STAGE=power expect "a second step 3 after a clean PASS is allowed" 0 bash "$S" 3 0123abcd GO 1 2 1 1
grep -q 'psd_state=1 psd_slots=2 read_mbox=1 read_ctrl=1' "$T/insmod.args" && echo "  PASS step 3 extra arguments: two slots, mailbox, CPU_CONTROL" || { echo "  FAIL step 3 extra args"; fails=$((fails + 1)); }
sleep 1
FAKE_STAGE=power FAKE_VERDICT=FAIL expect "a failed step 3 gives rc 1" 1 bash "$S" 3 0123abcd GO
sleep 1
expect "a step 3 after a failed one is refused until reboot" 2 bash "$S" 3 0123abcd GO
rm -f "$T/work/step3-run-0123abcd"
expect "slots 3 refused" 2 bash "$S" 3 0123abcd GO 1 3
expect "mbox 2 refused" 2 bash "$S" 3 0123abcd GO 1 1 2
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
# step 4 (stage boot)
rm -f "$T/work/step3-run-0123abcd"
sleep 1
expect "step 4 without GO refused" 2 bash "$S" 4 0123abcd
rm -f "$T/work/step3-pass-0123abcd"
expect "step 4 without a step 3 PASS refused" 2 bash "$S" 4 0123abcd GO
sleep 1
FAKE_STAGE=power expect "step 3 passes (precondition for step 4)" 0 bash "$S" 3 0123abcd GO 1 2
sleep 1
expect "step 4 with wait_ms 5 refused" 2 bash "$S" 4 0123abcd GO 1 2 5
expect "step 4 with a non-number wait refused" 2 bash "$S" 4 0123abcd GO 1 2 abc
FAKE_STAGE=boot expect "step 4 passes with GO after a step 3 PASS" 0 bash "$S" 4 0123abcd GO 1 2 500
grep -q 'stage=boot confirm_power=1 confirm_boot=1 psd_state=1 psd_slots=2 boot_wait_ms=500 alias_mbox=1' "$T/insmod.args" && echo "  PASS step 4 args" || { echo "  FAIL step 4 args"; fails=$((fails + 1)); }
sleep 1
FAKE_STAGE=boot expect "a second step 4 on one boot is refused" 2 bash "$S" 4 0123abcd GO
rm -f "$T/work/step4-run-0123abcd"
sleep 1
FAKE_STAGE=boot FAKE_VERDICT=FAIL expect "a failed step 4 gives rc 1" 1 bash "$S" 4 0123abcd GO
grep -q 'reboot' "$T/out" && echo "  PASS a failed step 4 says to reboot" || { echo "  FAIL step 4 reboot message"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
# the dart and hello flags of step 4
expect "step 4 dart=1 with one slot refused" 2 bash "$S" 4 0123abcd GO 1 1 300 1
expect "step 4 dart=2 refused" 2 bash "$S" 4 0123abcd GO 1 2 300 2
expect "step 4 hello=5 refused (early)" 2 bash "$S" 4 0123abcd GO 1 2 300 0 5
[ ! -e "$T/work/step4-run-0123abcd" ] && echo "  PASS a refused step 4 leaves no marker" || { echo "  FAIL marker after refused step 4"; fails=$((fails + 1)); }
sleep 1
FAKE_STAGE=boot expect "step 4 with dart=1 and hello=1 passes" 0 bash "$S" 4 0123abcd GO 1 2 800 1 1
grep -q 'stage=boot confirm_power=1 confirm_boot=1 psd_state=1 psd_slots=2 boot_wait_ms=800 alias_mbox=1 read_dart=1 rtkit_hello=1' "$T/insmod.args" && echo "  PASS step 4 dart and hello args" || { echo "  FAIL step 4 dart and hello args"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
sleep 1
# step 5: step 4 with the firmware map, only for a DTB with iommus and power-domains in the node
expect "step 5 on a node without iommus refused" 2 bash "$S" 5 0123abcd GO 1 2 800 1 1
[ ! -e "$T/work/step4-run-0123abcd" ] && echo "  PASS a refused step 5 leaves no marker" || { echo "  FAIL marker after refused step 5"; fails=$((fails + 1)); }
: > "$T/dt/soc/ane@419600000/iommus"
expect "step 5 with iommus but no power-domains refused" 2 bash "$S" 5 0123abcd GO 1 2 800 1 1
: > "$T/dt/soc/ane@419600000/power-domains"
sleep 1
FAKE_STAGE=boot expect "step 5 on a DTB with iommus and power-domains passes" 0 bash "$S" 5 0123abcd GO 1 2 800 1 1
grep -q 'stage=boot confirm_power=1 confirm_boot=1 psd_state=1 psd_slots=2 boot_wait_ms=800 alias_mbox=1 read_dart=1 map_fw=1 rtkit_hello=1' "$T/insmod.args" && echo "  PASS step 5 args carry map_fw=1" || { echo "  FAIL step 5 args"; fails=$((fails + 1)); }
expect "a second step 5 or 4 on one boot is refused" 2 bash "$S" 4 0123abcd GO
rm -f "$T/work/step4-run-0123abcd" "$T/dt/soc/ane@419600000/iommus" "$T/dt/soc/ane@419600000/power-domains"
sleep 1
FAKE_STAGE=boot expect "step 4 on the old DTB passes and carries no map_fw" 0 bash "$S" 4 0123abcd GO 1 2 800
grep -q 'map_fw' "$T/insmod.args" && { echo "  FAIL step 4 carries map_fw"; fails=$((fails + 1)); } || echo "  PASS step 4 carries no map_fw"
rm -f "$T/work/step4-run-0123abcd"
sleep 1
# hello 2 starts the endpoints, hello 3 also runs the RTKit shutdown, hello 4 is refused
expect "step 4 with hello=5 refused" 2 bash "$S" 4 0123abcd GO 1 2 800 0 5
sleep 1
FAKE_STAGE=boot expect "step 4 with hello=2 passes" 0 bash "$S" 4 0123abcd GO 1 2 800 0 2
grep -q 'alias_mbox=1 rtkit_hello=1 rtkit_start=1' "$T/insmod.args" && ! grep -q 'rtkit_stop' "$T/insmod.args" && echo "  PASS hello=2 args: hello and start, no stop" || { echo "  FAIL hello=2 args"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
sleep 1
FAKE_STAGE=boot expect "step 4 with hello=3 passes" 0 bash "$S" 4 0123abcd GO 1 2 800 1 3
grep -q 'read_dart=1 rtkit_hello=1 rtkit_start=1 rtkit_stop=1' "$T/insmod.args" && echo "  PASS hello=3 args: hello, start and stop=1" || { echo "  FAIL hello=3 args"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
sleep 1
FAKE_STAGE=boot expect "step 4 with hello=4 passes" 0 bash "$S" 4 0123abcd GO 1 2 800 0 4
grep -q 'rtkit_hello=1 rtkit_start=1 rtkit_stop=2' "$T/insmod.args" && echo "  PASS hello=4 args: hello, start and stop=2" || { echo "  FAIL hello=4 args"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
sleep 1
# after a step 4 ran, no ANE step of any number may start until a reboot
touch "$T/work/step4-run-0123abcd"
expect "step 1 after a step 4 is refused" 2 bash "$S" 1 0123abcd
expect "step 2 after a step 4 is refused" 2 bash "$S" 2 0123abcd
expect "step 3 after a step 4 is refused" 2 bash "$S" 3 0123abcd GO
grep -q 'step 4 already ran' "$T/out" && echo "  PASS the refusal names the step 4 marker" || { echo "  FAIL step 4 marker message"; fails=$((fails + 1)); }
rm -f "$T/work/step4-run-0123abcd"
# a step 3 PASS does not survive a later step 3 that fails
touch "$T/work/step2-pass-0123abcd"
sleep 1
FAKE_STAGE=power expect "step 3 passes before the retry" 0 bash "$S" 3 0123abcd GO
sleep 1
FAKE_STAGE=power FAKE_VERDICT=FAIL expect "the retried step 3 fails" 1 bash "$S" 3 0123abcd GO
[ ! -e "$T/work/step3-pass-0123abcd" ] && echo "  PASS a new step 3 removes the old PASS marker" || { echo "  FAIL stale step 3 PASS marker"; fails=$((fails + 1)); }
sleep 1
FAKE_STAGE=boot expect "step 4 after a failed step 3 retry is refused" 2 bash "$S" 4 0123abcd GO
rm -f "$T/work/step3-run-0123abcd"
sleep 1
# stale RESULT line from an earlier step must not count (insmod does nothing: FAKE_NOOP)
rm -f "$T/work/step3-run-0123abcd"
sleep 1
FAKE_STAGE=psd expect "a stale step 2 line does not pass step 3 (insmod loads nothing new)" 1 env FAKE_NOOP=1 bash "$S" 3 0123abcd GO
rm -f "$T/work/step3-run-0123abcd"
sleep 1
touch "$T/work/step1-pass-0123abcd"
FAKE_STAGE=dt expect "a new RESULT line of the wrong stage does not pass step 2" 1 bash "$S" 2 0123abcd
sleep 1
FAKE_STAGE=power FAKE_NOT6=1 expect "a PASS with a slot not returned to state 6 exits 3" 3 env FAKE_NOT6=1 bash "$S" 3 0123abcd GO
rm -f "$T/work/step3-run-0123abcd"
sleep 1
expect "a boot id prefix of 4 characters is refused" 2 bash "$S" 1 0123
# the pin cannot be overridden from the environment unless M6_ANE_TEST=1
env KO="$T/work/ane_h18.ko" KO_SHA256="$KSHA" WORK="$T/work" bash "$S" 1 0123abcd > "$T/out" 2>&1
[ $? = 2 ] && grep -q 'is set in the environment' "$T/out" && echo "  PASS environment overrides refused without M6_ANE_TEST" || { echo "  FAIL environment override"; fails=$((fails + 1)); }
echo "$([ $fails = 0 ] && echo 'STAGE-SCRIPT TEST: all PASS' || echo "STAGE-SCRIPT TEST: $fails FAILED")"
exit $((fails > 0))
