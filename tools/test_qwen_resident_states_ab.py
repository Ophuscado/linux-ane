#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of qwen_resident_states_ab.py (no device).

1. judge(): the frozen gate on synthetic arm records, each negative case on its own, then text mutants of judge() that
   must each be caught (threshold, pair count, token and logits equality, order, exec drift, kernel STOP).
2. One real in-process ABAB run on the three-program fixture through the real ane-session over the fake device
   (see test_qwen_resident_states.py): the four arms must produce identical token ids and logits sha256. The timing ratio
   is not asserted (the fake device has no latency), only that the arms ran in the order A B A B with states registered
   in B and not in A."""
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import qwen_m2_decode as dec  # noqa: E402
import qwen_resident_states_ab as ab  # noqa: E402
import test_qwen_resident_states as base  # noqa: E402


def rec(arm, p50, tokens=(1, 2), sha="aa", execs=(1.0, 2.0), stop=()):
    return {"arm": arm, "tokens": list(tokens), "steps": 16, "logits_sha256": sha, "step_p10_s": p50, "step_p50_s": p50,
            "step_p90_s": p50, "exec_ms_per_program_median": list(execs), "kernel_stop": list(stop)}


def good():
    return [rec("A", 0.21), rec("B", 0.17), rec("A", 0.22), rec("B", 0.18)]


def verdict(mod_judge, records):
    f, _ = mod_judge(records)
    return "ok" if not f else "fail"


def cases(mod_judge):
    out = {"good": verdict(mod_judge, good())}
    r = good(); r[1]["step_p50_s"] = 0.19; out["pair1_ratio_0.905"] = verdict(mod_judge, r)
    r = good(); r[3]["step_p50_s"] = 0.20; out["pair2_ratio_0.909"] = verdict(mod_judge, r)
    r = good(); r[1]["step_p50_s"] = 0.189; out["pair1_ratio_exactly_0.9"] = verdict(mod_judge, r)
    r = good(); r[2]["tokens"] = [1, 3]; out["token_mismatch"] = verdict(mod_judge, r)
    r = good(); r[3]["logits_sha256"] = "bb"; out["logits_mismatch"] = verdict(mod_judge, r)
    r = good(); r[0]["logits_sha256"] = None; out["no_logits"] = verdict(mod_judge, r)
    r = good(); r[1]["exec_ms_per_program_median"] = [1.0, 2.2]; out["exec_drift_10pct"] = verdict(mod_judge, r)
    r = good(); r[1]["kernel_stop"] = ["EXCH x failed"]; out["kernel_stop"] = verdict(mod_judge, r)
    out["order_BABA"] = verdict(mod_judge, [good()[1], good()[0], good()[3], good()[2]])
    out["three_arms"] = verdict(mod_judge, good()[:3])
    return out


EXPECT = {"good": "ok", "pair1_ratio_0.905": "fail", "pair2_ratio_0.909": "fail", "pair1_ratio_exactly_0.9": "ok",
          "token_mismatch": "fail", "logits_mismatch": "fail", "no_logits": "fail", "exec_drift_10pct": "fail",
          "kernel_stop": "fail", "order_BABA": "fail", "three_arms": "fail"}

SRC = (HERE / "qwen_resident_states_ab.py").read_text()
MUTANTS = {
    "threshold_loose": ("THRESHOLD = 0.90", "THRESHOLD = 0.95"),
    "le_to_lt": ("if not ratio <= THRESHOLD:", "if not ratio < THRESHOLD:"),
    "only_first_pair": ("for i in range(0, len(records) - 1, 2)]", "for i in range(0, 1, 2)]"),
    "tokens_not_checked": ("if r[\"tokens\"] != first[\"tokens\"] or not r[\"tokens\"]:", "if False:"),
    "logits_not_checked": ("if r[\"logits_sha256\"] != first[\"logits_sha256\"] or not first[\"logits_sha256\"]:", "if False:"),
    "order_not_checked": ("if [r[\"arm\"] for r in records] != list(order):", "if False:"),
    "exec_drift_ignored": ("if x and y and abs(y - x) > 0.05 * x:", "if False:"),
    "kernel_stop_ignored": ("if r[\"kernel_stop\"]:", "if False:"),
    "ratio_inverted": ("ratio = b[\"step_p50_s\"] / a[\"step_p50_s\"] if a[\"step_p50_s\"] else float(\"inf\")", "ratio = a[\"step_p50_s\"] / b[\"step_p50_s\"]"),
}


def mutant_judge(src):
    ns = {"__name__": "mutant", "__file__": str(HERE / "qwen_resident_states_ab.py")}
    exec(compile(src, "mutant", "exec"), ns)  # noqa: S102
    return ns["judge"]


def integration(failures):
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        serve = base.build_serve(root)
        manifest, ports_dir, anec_dir = base.fixture(root)
        (root / "manifest.json").write_text(json.dumps(manifest))
        (root / "ref.json").write_text(json.dumps({"prompts": [{"id": "p001", "prompt_token_ids": [1, 2, 3], "generated_ids": []}]}))
        embed = np.zeros((4, 2048), np.float16)
        for t in range(4):
            embed[t] = (np.arange(2048) % 251 + t * 7).astype(np.float16)
        head = (np.arange(4 * 2048, dtype=np.float32).reshape(4, 2048) % 13 - 6) / 7
        dec.load_head = lambda path: (embed, head, (4, 4, 10000.0))
        dec.kernel_marks = lambda: []   # dmesg is not readable here; the harness reads it only for STOP lines
        arms = []
        orig = ab.decode_arm

        def spy(decoder, arm, *a, **k):
            r = orig(decoder, arm, *a, **k)
            r["states_registered"] = bool(decoder.session.resident)
            arms.append(r)
            return r
        ab.decode_arm = spy
        rc = ab.main(["--manifest", str(root / "manifest.json"), "--ref", str(root / "ref.json"), "--anec-dir", str(anec_dir),
                      "--ports-dir", str(ports_dir), "--gguf", "unused", "--session-bin", str(serve), "--session-lock",
                      str(root / "t.lock"), "--idle", "0", "--new-tokens", "3", "--out", str(root / "out"), "--timeout", "20"])
        ab.decode_arm = orig
        summary = json.loads((root / "out/summary.json").read_text())
    if [a["arm"] for a in arms] != list("ABAB"):
        failures.append(f"arms ran as {[a['arm'] for a in arms]}")
    if [a["states_registered"] for a in arms] != [False, True, False, True]:
        failures.append(f"states registered per arm {[a['states_registered'] for a in arms]}, want F T F T")
    only_timing = [f for f in summary["failures"] if "B/A step p50" not in f and "exec median" not in f]
    if only_timing:
        failures.append(f"non-timing failures in the fake-device run: {only_timing}")
    if len({a["logits_sha256"] for a in arms}) != 1 or not arms[0]["logits_sha256"]:
        failures.append("logits sha256 differ across arms on the fake device")
    if len({tuple(a["tokens"]) for a in arms}) != 1:
        failures.append("token ids differ across arms on the fake device")
    if rc not in (0, 1):
        failures.append(f"harness exit {rc}")


def main():
    failures = []
    got = cases(ab.judge)
    for k, want in EXPECT.items():
        if got[k] != want:
            failures.append(f"judge case {k}: {got[k]} want {want}")
    for name, (old, new) in MUTANTS.items():
        if SRC.count(old) != 1:
            failures.append(f"mutant {name}: pattern found {SRC.count(old)} times")
            continue
        if cases(mutant_judge(SRC.replace(old, new))) == got:
            failures.append(f"mutant {name} SURVIVED")
    integration(failures)
    for f in failures[:20]:
        print("FAIL", f)
    print("RESIDENT-STATES-AB FAIL" if failures else f"RESIDENT-STATES-AB PASS ({len(EXPECT)} judge cases, {len(MUTANTS)} mutants killed, in-process ABAB on the fake device)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
