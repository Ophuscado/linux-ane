#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of qwen_pin_ab.py (no device, no real session).

1. set_mask puts a mask on every task of a process and reads it back; a task whose readback differs is refused.
2. judge: the frozen gate over synthetic arms U P U P. Passes at the 0.95 boundary with a difference above the A/A spread; fails above 0.95, fails when the
   pooled difference does not exceed the A/A spread, and fails on a wrong order, a wrong id, a logits hash that differs between arms, a bo_total change, an
   EXCH or STOP line, a boot id change, an affinity readback that differs from the arm's mask, a missing call or CPU, and a short arm."""
import os
import subprocess
import sys
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import qwen_pin_ab as pin  # noqa: E402

STEPS, PROGS = 4, 3
ANCHOR = [1, 2, 3, 4]


def arm(label, wall, sha="a" * 64, tokens=None, calls=PROGS, cpu=5, readback=None, **over):
    mask = pin.MASKS[label]
    rows = [{"pos": 0, "generated": False, "ane_s": 0.1, "calls": [], "decode_cpu": 5, "child_cpu": 5, "freq_khz": [3264000], "threads_cpu": {"1:1": 5}, "busy_ticks_delta": [0] * 12, "loadavg": "0 0 0 1/1 1"}]
    for k in range(STEPS):
        rows.append({"pos": k + 1, "generated": True, "wall_s": wall, "ane_s": wall - 0.02, "head_s": 0.02, "decode_cpu": cpu, "child_cpu": cpu, "freq_khz": [3264000],
                     "threads_cpu": {"1:1": cpu, "2:2": cpu}, "busy_ticks_delta": [0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0], "loadavg": "0 0 0 1/1 1",
                     "calls": [{"wall_s": 0.004, "exec_ms": 3.0, "cpu": cpu} for _ in range(calls)]})
    rb = readback if readback is not None else {"1:1": sorted(mask), "2:2": sorted(mask)}
    rec = {"arm": label, "tokens": tokens or list(ANCHOR), "rows": rows, "affinity": {"set": sorted(mask), "readback": rb, "readback_end": rb}, "logits_sha256": sha,
           "logits_bytes": STEPS * 248_320 * 4, "kernel_stop": [], "boot_id_start": "b", "boot_id_end": "b", "bo_total_before": 7, "bo_total_after": 7, "exch_new": 0}
    rec.update(over)
    return rec


RUN = {"boot_id_pre": "b", "bo_loaded": 7, "exch_new": 0, "kernel_stop": []}


def run(arms, **kw):
    return pin.judge(arms, RUN, steps=STEPS, programs=PROGS, anchor=ANCHOR, **kw)


def good(u1=0.200, p1=0.185, u2=0.198, p2=0.183):
    return [arm("U", u1), arm("P", p1), arm("U", u2), arm("P", p2)]


def expect(arms, text):
    failures, _ = run(arms)
    assert any(text in f for f in failures), (text, failures)


class FakeDecoder:
    """A decoder that answers from a table: three 'programs', no device. Matches the attributes decode_arm uses."""

    def __init__(self, child):
        self.max_len = 100
        self.session = type("S", (), {"proc": child})()
        self.call_cpus = []
        self.configured = 0

    def configure(self, m):
        self.configured += 1

    def step(self, token, pos, cos, sin, embed, check):
        timing = []
        for _ in range(PROGS):
            self.call_cpus.append(pin.cpu_now())
            timing.append((0.001, 0.5))
        return pin.np.full(4, float(token), dtype=pin.np.float32), timing


def decode_arm_on_fake(tmp):
    import numpy as np
    child = subprocess.Popen(["sleep", "30"])
    try:
        pin.MASKS = {"U": sorted(os.sched_getaffinity(0)), "P": sorted(os.sched_getaffinity(0))[:2]}
        pin.dec.kernel_marks = lambda: []
        head = np.eye(4, dtype=np.float32)
        prompt = {"id": "t", "prompt_token_ids": [1, 2]}
        snap = lambda: {"boot_id": "b", "bo_total_bytes": 7, "exch_lines": 0}  # noqa: E731
        rec = pin.decode_arm(FakeDecoder(child), "P", prompt, 3, None, None, None, head, tmp, snap)
        assert len(pin.gen_rows(rec)) == 3 and len(rec["rows"]) == 4 and rec["affinity"]["readback"] and rec["logits_bytes"] == 3 * 16
        r = rec["rows"][-1]
        assert len(r["calls"]) == PROGS and r["threads_cpu"] and len(r["busy_ticks_delta"]) >= 1 and r["loadavg"] and r["decode_cpu"] in pin.MASKS["P"]
        assert all(c["cpu"] in pin.MASKS["P"] for c in r["calls"])
        assert set(r["schedstat_ane"]) == {"decode", "child"} and set(r["schedstat_head"]) == {"decode", "child"} and len(r["busy_ticks_ane"]) == len(r["busy_ticks_delta"])
        assert rec["interrupts"]["before"] is None or "CPU0" in rec["interrupts"]["before"]
        if pin.sched_sum(r, "decode") is not None:
            assert pin.sched_sum(r, "decode")[0] >= 0 and len(pin.sched_cols(r).split("\t")) == 4
        st = pin.stats(rec)
        assert st["steps"] == 3 and isinstance(st["between_calls_mean_ms"], float) and set(st["steps_above_cut"]) == {"185", "190", "195", "200", "205", "210"}
    finally:
        child.kill()
        child.wait()


def main():
    child = subprocess.Popen(["sleep", "30"])
    try:
        one, two = sorted(os.sched_getaffinity(0))[:2]
        got = pin.set_mask([one], [child.pid])
        assert all(v == [one] for ts in got.values() for v in ts.values()), got
        assert pin.cpu_now() >= 0
        try:
            pin.check_mask([one, two], [child.pid])
            raise AssertionError("a differing readback was accepted")
        except pin.dec.Refuse:
            pass
    finally:
        child.kill()
        child.wait()
    failures, summary = run(good())
    assert not failures, failures
    assert 0.92 < summary["pooled_mean_P_over_U"] < 0.94 and summary["logits_equal_sealed_hash"] is False
    failures, _ = run(good(u1=0.2, p1=0.19, u2=0.2, p2=0.19))
    assert not failures, ("ratio exactly 0.95 passes", failures)
    expect(good(u1=0.2, p1=0.191, u2=0.2, p2=0.191), "above 0.95")
    expect(good(u1=0.20, p1=0.185, u2=0.176, p2=0.185), "A/A spread")
    expect([arm("P", 0.18), arm("U", 0.2), arm("U", 0.2), arm("P", 0.18)], "arms are")
    bad = good()
    bad[1] = arm("P", 0.185, tokens=[1, 2, 3, 5])
    expect(bad, "anchor")
    bad = good()
    bad[3] = arm("P", 0.183, sha="b" * 64)
    expect(bad, "logits sha256 differs")
    bad = good()
    bad[2] = arm("U", 0.198, bo_total_after=8)
    expect(bad, "bo_total_bytes")
    bad = good()
    bad[2] = arm("U", 0.198, exch_new=1)
    expect(bad, "EXCH")
    bad = good()
    bad[0] = arm("U", 0.2, kernel_stop=["STOP x"])
    expect(bad, "STOP")
    bad = good()
    bad[1] = arm("P", 0.185, boot_id_end="c")
    expect(bad, "boot id changed")
    bad = good()
    bad[1] = arm("P", 0.185, readback={"1:1": [4, 5, 6, 7, 8, 9, 10, 11], "2:2": list(range(12))})
    expect(bad, "affinity readback")
    bad = good()
    bad[2] = arm("U", 0.198, calls=PROGS - 1)
    expect(bad, "does not hold")
    bad = good()
    bad[0]["rows"][-1]["child_cpu"] = None
    expect(bad, "lacks a decode or child CPU")
    bad = good()
    bad[3]["rows"] = bad[3]["rows"][:-1]
    expect(bad, "generated steps")
    bad = good()
    bad[1]["rows"][-1]["calls"][0]["cpu"] = -1
    expect(bad, "does not hold")
    bad = good()
    bad[2]["rows"][-1]["decode_cpu"] = -1
    expect(bad, "lacks a decode or child CPU")
    a = {"decode": {"1": [1_000_000, 500_000]}, "child": {}}
    b = {"decode": {"1": [4_000_000, 2_500_000], "2": [9, 9]}, "child": {}}
    assert pin.sched_delta(a, b) == {"decode": {"1": [3_000_000, 2_000_000]}, "child": {}}
    assert pin.sched_sum({"schedstat_ane": pin.sched_delta(a, b)}, "decode") == [3.0, 2.0] and pin.sched_sum({"schedstat_ane": pin.sched_delta(a, b)}, "child") is None
    assert pin.allowed(2**22 + 5, 2**22 + 5) is None
    st = pin.stats(arm("U", 0.2, cpu=2))
    assert st["calls_on_E"] == st["calls"] == STEPS * PROGS and st["steps_decode_cpu_on_E"] == STEPS and st["frac_above_190ms"] == 1.0
    assert pin.stats(arm("P", 0.18))["slow_steps_by_core"]["both_P"] == 0
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        decode_arm_on_fake(Path(tmp))
    print("ok: decode_arm on a fake decoder, set_mask, judge boundary, A/A spread, and 12 refusals")


if __name__ == "__main__":
    main()
