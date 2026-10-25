#!/bin/sh
# test_level_dt_install.sh: the device tree check and guard of packaging/arch/omarchy-ane-level-dt (the single copy in its .install),
# against two small device trees built here with dtc: one the overlay was written for, one with a changed pms_sram phandle.
# Checks: the check passes and fails accordingly; post_install keeps the overlay file on the good tree and removes it on the bad one
# (libalpm does not abort an installation when a scriptlet fails, so removal after extraction is the enforcement); --guard does the same.
# usage: sh tools/test_level_dt_install.sh   (from the repository root; needs dtc and fdtget)
set -u
cd "$(dirname "$0")/.." || exit 2
command -v dtc > /dev/null 2>&1 && command -v fdtget > /dev/null 2>&1 || { echo "test_level_dt_install: needs dtc and fdtget; skipped"; exit 0; }
INSTALL=packaging/arch/omarchy-ane-level-dt/omarchy-ane-level-dt.install
T=$(mktemp -d)
fails=0
mk() { # mk <pms_sram phandle> <out>
	cat > "$T/t.dts" << EOF
/dts-v1/;
/ {
	soc {
		ane@285c04000 {
			compatible = "apple,t6000-ane";
			reg = <0x2 0x85c04000 0x0 0x24000>;
			reg-names = "engine";
			power-domains = <0x15 0x19 0x1a 0x1b 0x1c>;
			status = "okay";
		};
		power-management@28e080000 {
			power-controller@2c8 { phandle = <0x15>; label = "ane_sys_cpu"; };
			power-controller@c010 { phandle = <0x19>; label = "ane_set1"; };
			power-controller@c018 { phandle = <0x1a>; label = "ane_set2"; };
			power-controller@c020 { phandle = <0x1b>; label = "ane_set3"; };
			power-controller@c028 { phandle = <0x1c>; label = "ane_set4"; };
			power-controller@2e0 { phandle = <$1>; label = "pms_sram"; };
		};
	};
};
EOF
	dtc -q -I dts -O dtb -o "$2" "$T/t.dts"
}
mk 0x20 "$T/good.dtb"
mk 0x21 "$T/bad.dtb"
export OMARCHY_ANE_LEVEL_OVERLAY="$T/overlay.dtbo"
# the scriptlet functions only (the install file has no top-level commands)
. "./$INSTALL"
check() { # check <name> <condition result 0/1>
	if [ "$2" = 0 ]; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}
OMARCHY_ANE_LEVEL_DTBS="$T/good.dtb" _level_check > /dev/null 2>&1
check "check passes on the tree the overlay was written for" $?
OMARCHY_ANE_LEVEL_DTBS="$T/bad.dtb" _level_check > /dev/null 2>&1
[ $? != 0 ]
check "check fails on a tree with another pms_sram phandle" $?
echo overlay > "$OMARCHY_ANE_LEVEL_OVERLAY"
OMARCHY_ANE_LEVEL_DTBS="$T/good.dtb" post_install > /dev/null 2>&1
[ -e "$OMARCHY_ANE_LEVEL_OVERLAY" ]
check "post_install keeps the overlay on a matching tree" $?
OMARCHY_ANE_LEVEL_DTBS="$T/bad.dtb" post_install > /dev/null 2>&1
[ ! -e "$OMARCHY_ANE_LEVEL_OVERLAY" ]
check "post_install removes the overlay on a mismatching tree" $?
echo overlay > "$OMARCHY_ANE_LEVEL_OVERLAY"
OMARCHY_ANE_LEVEL_DTBS="$T/bad.dtb" post_upgrade > /dev/null 2>&1
[ ! -e "$OMARCHY_ANE_LEVEL_OVERLAY" ]
check "post_upgrade removes the overlay on a mismatching tree" $?
echo overlay > "$OMARCHY_ANE_LEVEL_OVERLAY"
OMARCHY_ANE_LEVEL_DTBS="$T/bad.dtb" _level_guard > /dev/null 2>&1
[ ! -e "$OMARCHY_ANE_LEVEL_OVERLAY" ]
check "the guard removes the overlay on a mismatching tree" $?
echo overlay > "$OMARCHY_ANE_LEVEL_OVERLAY"
OMARCHY_ANE_LEVEL_DTBS="$T/good.dtb" _level_guard > /dev/null 2>&1
[ -e "$OMARCHY_ANE_LEVEL_OVERLAY" ]
check "the guard keeps the overlay on a matching tree" $?
pre_install > /dev/null 2>&1
check "pre_install never returns failure (libalpm ignores it; the enforcement is after extraction)" $?
[ "$fails" = 0 ] && echo "test_level_dt_install: ok" || { echo "test_level_dt_install: $fails FAILURES"; exit 1; }
