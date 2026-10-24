#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Build the staged-Qwen manifest of a folded layout from the control manifest and the compile record of that layout.

A fold joins the DeltaNet state block of one layer to the program after it, so a layout has fewer programs than the
38 of the control. The compile record (ANEForge probe output with one PORTMAP line per program, plus the MIL of
every program) names the tensors of each program. This tool turns that record into manifest entries and checks the
result against the control, on the host, without a device:

  * A program that is not folded must have the MIL of the control program at the same place in the order, and its
    manifest entry is copied from the control manifest unchanged.
  * A folded program must be the control state block (inputs beta, gt, k, q, v, output o) followed by the next
    control program, in that order. Its entry is built from its PORTMAP line and its MIL signature.
  * With the equivalence check on (the default) every folded program must also be the chain of those two control programs, op for op
    (tools/mil_graph_equiv.py, lanes labelled), so a stale or miscompiled fold with the right ports is refused.
  * With --self-check the same builder rebuilds every entry of the control from the control's own record and
    compares it with the control manifest. The builder is trusted only when that comparison holds.

usage: qwen_fold_layout.py CONTROL_MANIFEST RECORD_DIR LAYOUT_ID OUT_MANIFEST [--self-check]
  RECORD_DIR holds <id>/probe.out and <id>/mil/NN-<sha12>.mil for the control id "control" and for LAYOUT_ID.
Exit 0 when every check holds, 1 when one fails, 2 on a usage or parse problem.
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
from pathlib import Path

CTX = ("oh", "inv", "mask", "cosp", "sinp")
STATE_BLOCK_IN = ["beta", "gt", "k", "q", "v"]


def portmaps(path: Path) -> dict[int, dict]:
    maps = {}
    for line in path.read_text().splitlines():
        m = re.match(r"PORTMAP (\d+) (\{.*\})$", line)
        if m:
            maps[int(m.group(1))] = json.loads(m.group(2))
    return maps


def mil_files(directory: Path) -> dict[int, Path]:
    return {int(f.name.split("-")[0]): f for f in sorted(directory.glob("*.mil"))}


def mil_hash(path: Path) -> str:
    return path.name.split("-")[1].split(".")[0]


def signature(path: Path):
    """(inputs [(name, shape)], outputs [(name, shape)]) of the MIL main function, in declaration order."""
    text = path.read_text()
    func = re.search(r"func main<[^>]*>\((.*)\)\s*\{", text)
    outs = re.search(r"\}\s*->\s*\((.*)\);", text)
    if not func or not outs:
        raise ValueError(f"{path}: no main function signature")
    args = [(n, [int(x) for x in sh.replace(" ", "").split(",") if x])
            for sh, n in re.findall(r"tensor<\w+, \[([^\]]*)\]> (\w+)", func.group(1))]
    shapes = {n: [int(x) for x in sh.replace(" ", "").split(",") if x]
              for sh, n in re.findall(r"tensor<\w+, \[([^\]]*)\]> (\w+)\s*=", text)}
    out_names = outs.group(1).replace(" ", "").split(",")
    arg_shapes = dict(args)
    return args, [(n, shapes.get(n) or arg_shapes[n]) for n in out_names]


def entry_from_record(pm: dict, mil: Path) -> dict:
    """The manifest entry of one program from its PORTMAP line and MIL signature."""
    args, outs = signature(mil)
    lane_of = {name: lane for lane, name in pm.get("x", {}).items()}
    lane_of |= {name: lane for lane, name in ((k, pm[k]) for k in CTX if k in pm)}
    state_in = set(pm.get("states", {}))
    out_lane = {name: lane for lane, name in pm.get("h", {}).items()}
    srcs, states_in = [], []
    for name, shape in args:
        if name in state_in:
            states_in.append((name, shape))
            srcs.append({"port": name, "kind": "state_in", "lane": f"state{len(states_in) - 1}", "shape": shape})
        elif name in lane_of:
            lane = lane_of[name]
            srcs.append({"kind": "ctx" if lane in CTX else "lane", "lane": lane, "shape": shape, "port": name})
        else:
            raise ValueError(f"{mil.name}: input {name} is in no PORTMAP lane")
    dsts, state_outs = [], []
    for name, shape in outs:
        if name in out_lane:
            dsts.append({"port": name, "kind": "lane", "lane": out_lane[name], "shape": shape})
        else:
            state_outs.append((name, shape))
    states = []
    taken = set()
    for name, shape in states_in:
        match = next((i for i, (_, s) in enumerate(state_outs) if s == shape and i not in taken), None)
        if match is None:
            raise ValueError(f"{mil.name}: no state output for state input {name} {shape}")
        taken.add(match)
        out_name, out_shape = state_outs[match]
        states.append({"in_port": name, "in_shape": shape, "out_port": out_name, "out_shape": out_shape})
    if len(taken) != len(state_outs):
        raise ValueError(f"{mil.name}: {len(state_outs) - len(taken)} outputs are neither lanes nor states")
    return {"srcs": srcs, "dsts": dsts, "states": states,
            "group_start": bool(pm.get("group_start", False)), "group_end": bool(pm.get("group_end", False))}


def norm(entry: dict) -> str:
    """Order-free form of an entry: ports are named, so the order of srcs, dsts and states carries no meaning."""
    by_port = lambda rows, key: sorted(rows, key=lambda r: r[key])  # noqa: E731
    flat = {"srcs": [{k: v for k, v in r.items() if not (k == "lane" and r["kind"] == "state_in")}
                     for r in by_port(entry["srcs"], "port")],
            "dsts": by_port(entry["dsts"], "port"), "states": by_port(entry["states"], "in_port"),
            "group_start": entry["group_start"], "group_end": entry["group_end"]}
    return json.dumps(flat, sort_keys=True)


def is_state_block(entry: dict) -> bool:
    return (sorted(s["lane"] for s in entry["srcs"] if s["kind"] == "lane") == sorted(STATE_BLOCK_IN)
            and [d["lane"] for d in entry["dsts"]] == ["o"])


def _labels(pm: dict, mil: Path):
    """Lane labels for the MIL arguments and outputs of one program. Lanes and context tables keep their names; state tensors are
    labelled by shape and position (the k-th input of a shape and the k-th output of that shape), the pairing the manifest uses."""
    args, outs = signature(mil)
    shape_key = lambda shape: "x".join(str(d) for d in shape)  # noqa: E731
    inl = {name: lane for lane, name in pm.get("x", {}).items()}
    inl |= {name: key for key, name in ((k, pm[k]) for k in CTX if k in pm)}
    seen: dict[str, int] = {}
    for name, shape in args:
        if name in pm.get("states", {}):
            seen[shape_key(shape)] = seen.get(shape_key(shape), 0) + 1
            inl[name] = f"st:{shape_key(shape)}:{seen[shape_key(shape)]}"
    outl = {name: lane for lane, name in pm.get("h", {}).items()}
    seen = {}
    for name, shape in outs:
        if name not in outl:
            seen[shape_key(shape)] = seen.get(shape_key(shape), 0) + 1
            outl[name] = f"st:{shape_key(shape)}:{seen[shape_key(shape)]}"
    return inl, outl


def chain_equivalent(fused: tuple[dict, Path], first: tuple[dict, Path], second: tuple[dict, Path]):
    """Run tools/mil_graph_equiv.py: is the fused program the first program followed by the second, op for op, with the lanes labelled?
    Returns (equal, text). Each argument is (PORTMAP line, MIL file)."""
    fl, fo = _labels(*fused)
    f1, o1 = _labels(*first)
    f2, o2 = _labels(*second)
    link_in = next((n for n, lane in f2.items() if lane == "o"), None)
    link_out = next((n for n, lane in o1.items() if lane == "o"), None)
    if link_in is None or link_out is None:
        return False, "the pair has no o lane to link"
    join = lambda d: ",".join(f"{k}={v}" for k, v in d.items())  # noqa: E731
    cmd = [sys.executable, str(Path(__file__).resolve().with_name("mil_graph_equiv.py")), str(fused[1]), str(first[1]), str(second[1]),
           "--link", f"{link_in}={link_out}", "--lanes-fused", join(fl), "--lanes-first", join(f1), "--lanes-second", join(f2),
           "--out-fused", join(fo), "--out-first", join(o1), "--out-second", join(o2)]
    run = subprocess.run(cmd, capture_output=True, text=True, check=False)
    return run.returncode == 0, (run.stdout + run.stderr).strip()[-300:]


def build(control: dict, rec_control: Path, rec_layout: Path, equiv: bool = True):
    """Return (manifest, mapping, problems). mapping[i] = the control program indices that layout program i replaces."""
    problems = []
    cmaps, cmils = portmaps(rec_control / "probe.out"), mil_files(rec_control / "mil")
    lmaps, lmils = portmaps(rec_layout / "probe.out"), mil_files(rec_layout / "mil")
    n_control = len(control["programs"])
    if sorted(cmaps) != list(range(n_control)) or sorted(cmils) != list(range(n_control)):
        problems.append("the control record does not cover the control manifest")
        return None, None, problems
    programs, mapping, cursor = [], [], 0
    for i in sorted(lmaps):
        folded = None
        if cursor < n_control and is_state_block(control["programs"][cursor]) and cursor + 1 < n_control:
            if mil_hash(lmils[i]) != mil_hash(cmils[cursor]):
                folded = True
        if folded:
            entry = entry_from_record(lmaps[i], lmils[i])
            first, second = control["programs"][cursor], control["programs"][cursor + 1]
            lane_shapes = {d["lane"]: d["shape"] for d in entry["dsts"]}
            want = {s["lane"]: s["shape"] for s in second["srcs"] if s["kind"] == "lane" and s["lane"] != "o"}
            if "o" in {s["lane"] for s in entry["srcs"]}:
                problems.append(f"layout program {i}: the state block output o crosses the program boundary")
            for lane, shape in {s["lane"]: s["shape"] for s in second["dsts"]}.items():
                if lane_shapes.get(lane) != shape:
                    problems.append(f"layout program {i}: output lane {lane} {lane_shapes.get(lane)} is not control {cursor + 1}'s {shape}")
            for lane in want:
                if lane not in {s["lane"] for s in entry["srcs"]}:
                    problems.append(f"layout program {i}: input lane {lane} of control {cursor + 1} is not an input")
            for lane in (s["lane"] for s in first["srcs"] if s["kind"] == "lane"):
                if lane not in {s["lane"] for s in entry["srcs"]}:
                    problems.append(f"layout program {i}: input lane {lane} of the state block (control {cursor}) is not an input")
            if (len([s for s in entry["states"]]) != len(first["states"]) + len(second["states"])):
                problems.append(f"layout program {i}: {len(entry['states'])} states, control {cursor} and {cursor + 1} hold "
                                f"{len(first['states']) + len(second['states'])}")
            if equiv:
                equal, text = chain_equivalent((lmaps[i], lmils[i]), (cmaps[cursor], cmils[cursor]), (cmaps[cursor + 1], cmils[cursor + 1]))
                if not equal:
                    problems.append(f"layout program {i}: the MIL is not the chain of control {cursor} and {cursor + 1}: {text}")
            programs.append(entry)
            mapping.append([cursor, cursor + 1])
            cursor += 2
        else:
            if cursor >= n_control:
                problems.append(f"layout program {i}: no control program left")
                break
            if mil_hash(lmils[i]) != mil_hash(cmils[cursor]):
                problems.append(f"layout program {i}: MIL {mil_hash(lmils[i])} is not control {cursor}'s {mil_hash(cmils[cursor])}")
            if norm(entry_from_record(lmaps[i], lmils[i])) != norm(control["programs"][cursor]):
                problems.append(f"layout program {i}: PORTMAP entry differs from control manifest entry {cursor}")
            programs.append(control["programs"][cursor])
            mapping.append([cursor])
            cursor += 1
    if cursor != n_control:
        problems.append(f"the layout consumes {cursor} of the {n_control} control programs")
    manifest = json.loads(json.dumps(control))
    manifest["programs"] = programs
    manifest["n_programs"] = len(programs)
    manifest["fold_mapping"] = mapping
    manifest["equiv_checked"] = bool(equiv)    # False when built with --no-equiv: the harness refuses such a manifest
    return manifest, mapping, problems


def self_check(control: dict, rec_control: Path):
    cmaps, cmils = portmaps(rec_control / "probe.out"), mil_files(rec_control / "mil")
    bad = []
    for i, want in enumerate(control["programs"]):
        try:
            got = entry_from_record(cmaps[i], cmils[i])
        except (ValueError, KeyError) as error:
            bad.append(f"control {i}: {error}")
            continue
        if norm(got) != norm(want):
            bad.append(f"control {i}: rebuilt entry differs from the control manifest")
    return bad


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    check = "--self-check" in argv
    equiv = "--no-equiv" not in argv    # only for host tests of the port checks; a run uses the default
    argv = [a for a in argv if a not in ("--self-check", "--no-equiv")]
    if len(argv) != 4:
        print(__doc__)
        return 2
    control_path, record, layout, out = argv
    control = json.loads(Path(control_path).read_text())
    record = Path(record)
    status = 0
    if check:
        bad = self_check(control, record / "control")
        print("self-check: " + ("all %d control entries rebuilt equal" % len(control["programs"]) if not bad else "FAILED"))
        for line in bad:
            print("  " + line)
        status = 1 if bad else 0
    manifest, mapping, problems = build(control, record / "control", record / layout, equiv)
    for line in problems:
        print("PROBLEM: " + line)
    if manifest is None or problems:
        return 1
    Path(out).write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"layout {layout}: {manifest['n_programs']} programs, mapping to control indices:")
    print(json.dumps(mapping))
    return status


if __name__ == "__main__":
    sys.exit(main())
