#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Compare the two boots of a one-arm fold-set run: the control alone (summary A) and a folded layout alone (summary B).

The ANE buffer window cannot hold the control and a 20 program layout together, so each runs in its own boot with
`qwen_fold_set_ab.py --only`. This tool reads both summaries and the layout manifest (its `fold_mapping`) and reports:

  * identity: whether the layout's token ids and logits hashes equal the control's (both gates were already judged in the layout run);
  * exec saved per fold: the exec medians of the control programs a fold replaces, summed, minus the exec median of the folded
    program, with the noise of each term taken from the per-program range over the arms of its own boot. The total is `outside_noise`
    only when it is larger than the sum of those ranges, a conservative linear sum;
  * boot-to-boot drift of the programs both layouts share (the unchanged ones): if they moved by more than 5 percent the two boots are
    not comparable and the exec numbers are labelled so;
  * the step ratio of the two boots' median p50 against BOTH within-boot spreads. Drift between boots is in neither spread, so the step
    ratio is a screen. The exec sum is the measurement.

usage: qwen_fold_set_compare.py CONTROL_SUMMARY LAYOUT_SUMMARY LAYOUT_MANIFEST
Exit 0 when the report was produced, 2 on a usage or input problem, on a summary that is not a frozen run or lists failures, and when the
layout summary was made against a different control summary (its recorded control_summary_sha256 must equal the hash of CONTROL_SUMMARY). The report is JSON on stdout."""

import hashlib
import json
import sys
from pathlib import Path

SHARED_DRIFT = 0.05


def compare(control: dict, layout: dict, manifest: dict) -> dict:
    ct, lt = control.get("timing", {}), layout.get("timing", {})
    for label, t in (("control", ct), ("layout", lt)):
        if "exec_ms_median" not in t:
            raise ValueError(f"the {label} summary is not a one-arm run (no timing.exec_ms_median)")
    mapping = manifest["fold_mapping"]
    c_med, c_rng = ct["exec_ms_median"], ct["exec_ms_range"]
    l_med, l_rng = lt["exec_ms_median"], lt["exec_ms_range"]
    folds, shared = [], []
    total_saved = total_noise = 0.0
    for i, rows in enumerate(mapping):
        name = f"prog_{i:03d}"
        if len(rows) == 1:
            ref = c_med[f"prog_{rows[0]:03d}"]
            shared.append({"layout_program": i, "control_program": rows[0], "control_ms": round(ref, 4), "layout_ms": round(l_med[name], 4),
                           "drift": round((l_med[name] - ref) / ref, 4)})
            continue
        replaced = sum(c_med[f"prog_{j:03d}"] for j in rows)
        noise = sum(c_rng[f"prog_{j:03d}"][1] - c_rng[f"prog_{j:03d}"][0] for j in rows) + (l_rng[name][1] - l_rng[name][0])
        saved = replaced - l_med[name]
        total_saved += saved
        total_noise += noise
        folds.append({"layout_program": i, "control_programs": rows, "replaced_ms": round(replaced, 4), "folded_ms": round(l_med[name], 4),
                      "saved_ms": round(saved, 4), "noise_ms": round(noise, 4)})
    comparable = all(abs(s["drift"]) <= SHARED_DRIFT for s in shared)
    p_c, p_l = sorted(ct["step_p50_s"])[len(ct["step_p50_s"]) // 2], sorted(lt["step_p50_s"])[len(lt["step_p50_s"]) // 2]
    spreads = [x for x in (ct.get("spread"), lt.get("spread")) if x is not None]
    ratio = p_l / p_c
    return {
        "identity": {"layout_identity": layout.get("identity"), "layout_failures": layout.get("failures"), "control_failures": control.get("failures")},
        "exec": {"folds": len(folds), "per_fold": folds, "saved_total_ms": round(total_saved, 4), "noise_total_ms": round(total_noise, 4),
                 "outside_noise": total_saved > total_noise, "shared_programs": shared, "boots_comparable": comparable,
                 "note": "saved = control exec of the replaced programs minus the folded exec, medians over the arms of each boot"},
        "step": {"control_p50_s": p_c, "layout_p50_s": p_l, "ratio": round(ratio, 4), "control_spread": ct.get("spread"), "layout_spread": lt.get("spread"),
                 "outside_both_spreads": bool(spreads) and abs(ratio - 1.0) > max(spreads),
                 "note": "a screen: boot-to-boot drift is in neither spread"},
    }


def validate(control: dict, layout: dict, control_sha256: str) -> list[str]:
    """Reasons the two summaries cannot be compared: not frozen, listed failures, or a layout run made against another control summary."""
    problems = []
    for label, s in (("control", control), ("layout", layout)):
        if s.get("frozen") is not True:
            problems.append(f"the {label} summary is not a frozen run")
        if s.get("failures"):
            problems.append(f"the {label} summary lists failures: {s['failures'][:2]}")
    recorded = layout.get("inputs", {}).get("control_summary_sha256")
    if recorded != control_sha256:
        problems.append(f"the layout run was made against control summary {str(recorded)[:16]}, the one given is {control_sha256[:16]}")
    return problems


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if len(argv) != 3:
        print(__doc__)
        return 2
    try:
        control, layout, manifest = (json.loads(Path(p).read_text()) for p in argv)
        problems = validate(control, layout, hashlib.sha256(Path(argv[0]).read_bytes()).hexdigest())
        if problems:
            print("ERROR: " + "; ".join(problems))
            return 2
        report = compare(control, layout, manifest)
    except (OSError, ValueError, KeyError) as error:
        print(f"ERROR: {error}")
        return 2
    print(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
