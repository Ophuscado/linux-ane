#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
# h15-capture.sh: read-only capture of the facts the H15 boot stages need.
# It reads /proc/device-tree, /proc/interrupts, /proc/iomem, the genpd
# summary and dmesg. It loads no module, maps no register and writes no
# file. Run as a normal user; root only adds /proc/iomem addresses,
# genpd and dmesg.
#
#     bash ane/h15/h15-capture.sh | tee h15-capture.txt
#
# Helps with (see ane_h15_facts in ane_h15_soc.c): H1 (os-fw-version), H2
# (interrupt lines, mailbox nodes) and the reserved asc-firmware regions
# with their iommu addresses, which are part of H3. It does NOT fill
# H3: the ADT segment-ranges, the ASC tunables and the DART sid, vm-base
# and vm-size are not in the Linux device tree. They need the boot ADT.

DT=${DT:-/proc/device-tree}

hexprop() { # file -> hex bytes (first 256) and a printable form
	local f=$1 sz
	sz=$(stat -c %s "$f" 2>/dev/null) || return
	printf '    %s (%s bytes): ' "$(basename "$f")" "$sz"
	if [ "$sz" -le 256 ]; then
		if tr -d '\000' < "$f" | grep -qE '^[[:print:]]+$' 2>/dev/null && [ "$sz" -gt 0 ]; then
			tr '\000' ' ' < "$f"
			echo
		else
			od -An -v -tx1 "$f" | tr -s ' \n' ' '
			echo
		fi
	else
		od -An -v -tx1 -N256 "$f" | tr -s ' \n' ' '
		echo '...(truncated at 256 bytes)'
	fi
}

dumpnode() {
	local n=$1 f
	echo "  node ${n#"$DT"}"
	for f in "$n"/*; do
		[ -f "$f" ] && hexprop "$f"
	done
}

echo "== h15-capture $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "== kernel: $(uname -r)"
echo "== model: $(tr '\000' ' ' < "$DT"/model 2>/dev/null)"
echo "== compatible: $(tr '\000' ' ' < "$DT"/compatible 2>/dev/null)"

echo "== chosen: the os-fw-version property selects the firmware image iBoot preloads (H1)"
for f in "$DT"/chosen/*fw*; do
	[ -f "$f" ] && hexprop "$f"
done

echo "== ANE, every DART and ASC mailbox node (H2, H3; the ANE DART is the one whose reg sits at the ANE engine window + 0x1800000)"
while IFS= read -r c; do
	n=$(dirname "$c")
	if tr '\000' ' ' < "$c" | grep -qE '(apple,t[0-9]+-ane|dart-ane|,ane|-ane |ascwrap|asc-mailbox|-dart)'; then
		dumpnode "$n"
	fi
done < <(find "$DT" -name compatible 2>/dev/null)

echo "== reserved-memory (part of H3: the asc-firmware regions iBoot preloads; segment-ranges and DART geometry are not here)"
for n in "$DT"/reserved-memory/*/; do
	[ -d "$n" ] && dumpnode "${n%/}"
done

echo "== /proc/interrupts lines for mailboxes, DARTs, ANE (H2)"
grep -iE 'ane|dart|mbox|mailbox|asc' /proc/interrupts 2>/dev/null

echo "== /proc/iomem lines for ane and reserved regions"
grep -iE 'ane|asc|reserved' /proc/iomem 2>/dev/null

echo "== genpd: ANE power domains (root only)"
grep -iE 'ane' /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null

echo "== dmesg lines for ane, dart-ane, ascwrap, rtkit (root or dmesg_restrict=0)"
dmesg 2>/dev/null | grep -iE 'ane_h15|dart-ane|ascwrap|apple-rtkit|ane ' | tail -n 80

echo "== end"
