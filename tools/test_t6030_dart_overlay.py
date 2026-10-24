#!/usr/bin/env python3
"""ane/h15/t6030-ane-dart-experimental.dts against the same-boot ADT facts of a J516S (T6030).

The ADT /arm-io/dart-ane has three translation instances (DARTLLT, DARTBRD, DARTBWR at 0x309800000, 0x309810000, 0x309820000, 0x4000 each),
the DAPF aperture window at 0x309804000, and one interrupt, 577. The overlay must enable the three instances and not the aperture, give every
node the one interrupt, and make the ane node a client of all three with stream 0 (the one stream the ADT names for the ANE). Offline; compiles
the overlay with dtc when dtc is on the PATH. Four mutations of the text each fail the check.
"""
import pathlib
import re
import shutil
import subprocess
import sys

DTS = pathlib.Path(__file__).resolve().parent.parent / "ane/h15/t6030-ane-dart-experimental.dts"
BANKS = {"309800000": "okay", "309810000": "okay", "309820000": "okay", "309804000": "disabled"}
IRQ = "577"


def block(text, head):
    m = re.search(re.escape(head) + r"\s*\{", text)
    if not m:
        return ""
    i, depth = m.end(), 1
    while depth:
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    return text[m.end():i - 1]


def check(text):
    errs = []
    names = {}
    for unit, want in BANKS.items():
        m = re.search(r"(ane_dart\d): iommu@" + unit, text)
        if not m:
            errs.append(f"no DART node at {unit}")
            continue
        names[unit] = m.group(1)
        body = block(text, m.group(0))
        st = re.search(r'status = "(\w+)"', body)
        if not st or st.group(1) != want:
            errs.append(f"{unit}: status {st.group(1) if st else None}, want {want}")
        irq = re.search(r"interrupts = <0 (\d+) 4>", body)
        if not irq or irq.group(1) != IRQ:
            errs.append(f"{unit}: interrupt {irq.group(1) if irq else None}, want {IRQ}")
        reg = re.search(r"reg = <0x3 0x(\w+) 0x0 0x4000>", body)
        if not reg or int(reg.group(1), 16) + 0x300000000 != int(unit, 16):
            errs.append(f"{unit}: reg does not give {unit} size 0x4000")
    ane = block(text, "ane: ane@308000000")
    got = re.search(r"iommus = (<[^;]*>);", ane)
    want = ", ".join(f"<&{names[u]} 0>" for u in ("309800000", "309810000", "309820000") if u in names)
    if not got or re.sub(r"\s+", " ", got.group(1)) != want:
        errs.append(f"ane iommus {got.group(1) if got else None}, want {want}")
    return errs


def main():
    text = DTS.read_text()
    errs = check(text)
    mutants = [
        text.replace('status = "okay";', 'status = "disabled";', 2),            # banks 0 and 1 off
        re.sub(r"(ane_dart2: iommu@309820000.*?)status = \"okay\"", r'\1status = "disabled"', text, count=1, flags=re.DOTALL),
        text.replace("<&ane_dart1 0>, ", "", 1),                                 # a bank missing from iommus
        re.sub(r"(ane_dart3: iommu@309804000.*?)status = \"disabled\"", r'\1status = "okay"', text, count=1, flags=re.DOTALL),
    ]
    for i, m in enumerate(mutants):
        assert m != text and check(m), f"mutant {i} passed"
    if shutil.which("dtc"):
        r = subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o", "/dev/null", str(DTS)], capture_output=True, text=True, check=False)
        if r.returncode:
            errs.append("dtc: " + r.stderr.strip().splitlines()[-1])
    for e in errs:
        print("FAIL", e)
    print(f"{len(errs)} failure(s)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
