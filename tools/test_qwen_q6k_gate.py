#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of qwen_q6k_gate.numerics() on a tiny synthetic run (64-entry vocabulary, no gguf, no device): a good run passes; an empty prompt set, a run without p001, a run whose
p001 hidden vectors are missing or mismatched, p001 ids that are not the anchors, a wrong id, a logit difference above the margin bar, a NaN and a zero-step prompt each fail."""
import json
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy as np  # noqa: E402
import qwen_q6k_gate as g  # noqa: E402

ROWS = 64
STEPS = 16


class Head:
    def __init__(self, shift=0.0, nan=False):
        self.shift, self.nan = shift, nan

    def logits(self, hidden):
        out = np.zeros(ROWS, np.float32)
        out[int(hidden[0])] = 10.0
        out[63] = 9.0
        out += self.shift
        if self.nan:
            out[5] = np.nan
        return out


def make(tmp, prompts=("p001", "p002"), ids_p001=None, hidden_steps=STEPS):
    summary = {"identity_prompts": []}
    for pid in prompts:
        ids = list(g.ANCHOR_TOKENS[:STEPS]) if pid == "p001" else [3] * STEPS
        if pid == "p001" and ids_p001 is not None:
            ids = ids_p001
        ids = [i % 60 for i in ids]
        summary["identity_prompts"].append({"prompt": pid, "A_ids": ids})
        d = tmp / "ab" / f"identity-{pid}" / "A"
        d.mkdir(parents=True)
        rows = np.zeros((STEPS, ROWS), np.float32)
        hid = np.zeros((hidden_steps, 8), np.float32)
        for s in range(STEPS):
            rows[s, ids[s]] = 10.0
            rows[s, 63] = 9.0
            if s < hidden_steps:
                hid[s, 0] = ids[s]
        rows.tofile(d / "logits.f32")
        (tmp / "hid").mkdir(exist_ok=True)
        np.save(tmp / "hid" / f"hidden_{pid}.npy", hid)
    (tmp / "ab" / "summary.json").write_text(json.dumps(summary))


def run(**kw):
    head = Head(kw.pop("shift", 0.0), kw.pop("nan", False))
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        (tmp / "ab").mkdir()
        make(tmp, **kw)
        return g.numerics(head, tmp / "ab", tmp / "hid", ROWS)


def expect(text, **kw):
    _, bad = run(**kw)
    assert any(text in b for b in bad), (text, bad)


def main():
    g.ANCHOR_TOKENS[:] = [i % 60 for i in g.ANCHOR_TOKENS]
    result, bad = run()
    assert not bad, bad
    assert result["max_abs_diff_all"] == 0.0 and abs(result["min_sealed_margin_all"] - 1.0) < 1e-6
    expect("no p001", prompts=("p002", "p003"))
    expect("no p001", prompts=())
    expect("p001 was not compared", hidden_steps=STEPS - 1)
    expect("anchor", ids_p001=[(i + 1) % 60 for i in g.ANCHOR_TOKENS])
    expect("not below", shift=0.5)
    expect("non-finite", nan=True)
    print("Q6K-GATE PASS (good run; no prompts; no p001; p001 not compared; anchors; margin bar; NaN)")


if __name__ == "__main__":
    main()
