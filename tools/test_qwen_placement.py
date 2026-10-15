#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline checks of the placement code in qwen_m2_decode.py (no device, no ane-session).

1. Importing the module does not pin the importer: a harness that imports it keeps its own mask (PINNED is None).
2. Running it as a script pins through pcore_mask before numpy starts: a script run in a child process (with a fake sysfs of two efficiency and two
   performance CPUs, and OMARCHY_ANE_NO_PIN unset) ends with the mask equal to the P CPUs it may use and OPENBLAS_NUM_THREADS=4; with the opt-out set, or
   with OPENBLAS_NUM_THREADS already set by the user, the mask stays and the user's value stays.
3. Decoder.step records the CPU of the decode thread after every call (step_cpus, cpu_hist) without changing the timing tuples.
4. placement() reads the mask of every task of the decode process and of the child from /proc, counts calls off the performance cores, and gives None
   for the off-P count on a chip with no efficiency cores. finish_placement() flags an off-P call on stderr."""
import argparse
import io
import json
import os
import subprocess
import sys
import tempfile
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import numpy as np  # noqa: E402
import pcore_mask  # noqa: E402

# A fake chip where pin_self() WOULD change the mask, in place while the module is imported: the import must still leave the mask alone.
_START = sorted(os.sched_getaffinity(0))
_FAKE = tempfile.TemporaryDirectory()
_FAKE_ROOT = Path(_FAKE.name)
for _cpu in range(max(_START) + 1):
    (_FAKE_ROOT / f"cpu{_cpu}").mkdir()
    (_FAKE_ROOT / f"cpu{_cpu}" / "cpu_capacity").write_text("1024\n" if _cpu in _START[2:4] else "400\n")
_real_defaults = pcore_mask.pin_self.__defaults__
pcore_mask.pin_self.__defaults__ = (_FAKE_ROOT, None)
import qwen_m2_decode as dec  # noqa: E402

pcore_mask.pin_self.__defaults__ = _real_defaults
assert sorted(os.sched_getaffinity(0)) == _START, "importing the module changed the mask"


def fake_sysfs(root, capacities):
    for cpu, cap in enumerate(capacities):
        d = Path(root) / f"cpu{cpu}"
        d.mkdir()
        (d / "cpu_capacity").write_text(f"{cap}\n")


def child_mask(env_extra, sysfs):
    """Run qwen_m2_decode.py's top-of-file hook as __main__ in a child with the fake sysfs; return (mask, OPENBLAS_NUM_THREADS)."""
    code = (
        "import os, runpy, sys\n"
        f"sys.path.insert(0, {str(HERE)!r})\n"
        "import pcore_mask\n"
        f"pcore_mask.SYSFS_CPU = __import__('pathlib').Path({str(sysfs)!r})\n"
        "pcore_mask.p_core_cpus.__defaults__ = (pcore_mask.SYSFS_CPU,)\n"
        "pcore_mask.pin_self.__defaults__ = (pcore_mask.SYSFS_CPU, None)\n"
        "sys.argv = ['qwen_m2_decode.py', '--summary', '--out', os.environ['OUT']]\n"
        "try:\n"
        f"    runpy.run_path({str(HERE / 'qwen_m2_decode.py')!r}, run_name='__main__')\n"
        "except SystemExit:\n"
        "    pass\n"
        "except FileNotFoundError:\n"
        "    pass\n"
        "print(sorted(os.sched_getaffinity(0)), os.environ.get('OPENBLAS_NUM_THREADS'))\n")
    env = {k: v for k, v in os.environ.items() if k not in ("OPENBLAS_NUM_THREADS", pcore_mask.OPT_OUT)}
    env.update(env_extra)
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, env=env, check=True).stdout.strip().splitlines()[-1]
    mask, _, threads = out.rpartition("] ")
    return eval(mask + "]"), (None if threads == "None" else threads)  # noqa: S307  (our own printed list)


class FakeProc:
    def __init__(self, pid):
        self.pid = pid


class FakeSession:
    def __init__(self, pid):
        self.proc = FakeProc(pid)


class FakeRunDecoder:
    """Stands in for Decoder inside run(): `mode` decides how the first step ends ('ok', 'call-stop' raises like a failed CALL, 'kernel-stop' lets a STOP kernel line appear)."""
    mode = "ok"
    child = None

    def __init__(self, args, manifest):
        self.max_len = 8
        self.session = FakeSession(FakeRunDecoder.child.pid)
        self.cpu_hist, self.step_cpus = {}, []

    def configure(self, m):
        self.max_len = m

    def step(self, token, pos, cos, sin, embed, check=None):
        if FakeRunDecoder.mode == "call-stop":
            raise SystemExit("STOP prog_000: exit 1")
        self.step_cpus = [dec.sched_getcpu()]
        self.cpu_hist[self.step_cpus[0]] = self.cpu_hist.get(self.step_cpus[0], 0) + 1
        return np.ones(2, np.float32), [(0.001, 0.5)]

    def close_session(self):
        pass


def run_exit_path(mode):
    """dec.run() on fakes: returns (exit text or None, the record types written to results.jsonl)."""
    import json
    child = subprocess.Popen(["sleep", "30"])
    saved = {k: getattr(dec, k) for k in ("Decoder", "reference_blas", "load_head", "rope_tables", "kernel_marks")}
    calls = {"n": 0}

    def marks():
        calls["n"] += 1
        return [(1.0, "boot")] if mode != "kernel-stop" or calls["n"] % 2 else [(1.0, "boot"), (2.0, "ane EXCH 5 failed")]   # odd call: the mark, even call: the check after the step

    try:
        FakeRunDecoder.mode, FakeRunDecoder.child = mode, child
        dec.Decoder, dec.reference_blas, dec.kernel_marks = FakeRunDecoder, (lambda: False), marks
        dec.load_head = lambda path: (np.ones((4, 2), np.float16), np.ones((4, 2), np.float32), (2, 2, 10000.0))
        dec.rope_tables = lambda seq, dh, rot, base: (np.ones((seq, 2)), np.ones((seq, 2)))
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            for name, text in (("manifest.json", json.dumps({"max_len": 8})), ("ref.json", json.dumps({"prompts": [{"id": "p", "prompt_token_ids": [0, 1], "generated_ids": [0, 0]}]})), ("x", "x")):
                (t / name).write_text(text)
            args = argparse.Namespace(out=str(t / "out"), manifest=str(t / "manifest.json"), ref=str(t / "ref.json"), ref_logits=None, prompts="", new_tokens=2, max_steps=None, m_per_prompt=False,
                                      dump=None, band=None, ane_run=str(t / "x"), session_bin=str(t / "x"), resident=True, resident_lock="call", gguf="x", logits_file=None)
            text = None
            with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                try:
                    dec.run(args)
                except SystemExit as e:
                    text = str(e)
            recs = [json.loads(line) for line in (t / "out" / "results.jsonl").read_text().splitlines()]
        return text, [r["type"] for r in recs], recs
    finally:
        for k, v in saved.items():
            setattr(dec, k, v)
        child.kill()
        child.wait()


def check_exit_paths():
    text, types, recs = run_exit_path("call-stop")
    assert text and text.startswith("STOP prog_000") and types == ["start", "placement"], (text, types)
    assert recs[-1]["decode_masks"] and recs[-1]["child_masks"], "the terminal record carries the masks of both processes"
    text, types, recs = run_exit_path("kernel-stop")
    assert text and text.startswith("STOP kernel") and types[-2:] == ["stop", "placement"] and types.count("placement") == 1, (text, types)
    text, types, recs = run_exit_path("ok")
    assert text is None and types[0] == "start" and types[-1] == "placement" and types.count("placement") == 1 and "prompt" in types, (text, types)


def main():
    assert dec.PINNED is None, "importing the module must not pin"
    allowed = sorted(os.sched_getaffinity(0))
    assert len(allowed) >= 4, "the host test needs 4 CPUs to build a fake 2+2 chip"
    e_cpus, p_cpus = allowed[:2], allowed[2:4]
    with tempfile.TemporaryDirectory() as tmp:
        sysfs = Path(tmp) / "cpu"
        sysfs.mkdir()
        capacity = [0] * (max(allowed) + 1)
        for c in range(len(capacity)):
            capacity[c] = 1024 if c in p_cpus else 400
        fake_sysfs(sysfs, capacity)
        out = Path(tmp) / "run"
        out.mkdir()
        (out / "results.jsonl").write_text("")
        base = {"OUT": str(out)}
        mask, threads = child_mask(base, sysfs)
        assert mask == p_cpus and threads == "4", (mask, threads)
        mask, threads = child_mask({**base, "OPENBLAS_NUM_THREADS": "7"}, sysfs)
        assert mask == p_cpus and threads == "7", "a user's thread count stays"
        mask, threads = child_mask({**base, pcore_mask.OPT_OUT: "1"}, sysfs)
        assert mask == allowed and threads is None, "the opt-out leaves the mask and the thread count alone"

    d = dec.Decoder.__new__(dec.Decoder)
    d.cpu_hist, d.step_cpus = {}, []
    d.max_len, d.session, d.resident_states = 4, None, False
    d.args = type("A", (), {"resident_lock": "call"})()
    d.progs = [{"group_start": True, "group_end": False, "srcs": [], "dsts": [{"lane": "h", "port": "o"}], "states": []},
               {"group_start": False, "group_end": True, "srcs": [], "dsts": [{"lane": "h", "port": "o"}], "states": []}]
    d.states = [{}, {}]
    d.call = lambda i, arrays: ({"o": np.ones((1, 2), np.float16)}, 0.5)
    dec.ctx_vals = lambda pos, max_len, cos, sin: {}
    hidden, timing = d.step(0, 0, None, None, np.ones((1, 1, 2), np.float16))
    assert len(timing) == 2 and all(len(t) == 2 and t[1] == 0.5 and t[0] >= 0 for t in timing), timing
    assert len(d.step_cpus) == 2 and sum(d.cpu_hist.values()) == 2 and all(c >= 0 for c in d.step_cpus), (d.step_cpus, d.cpu_hist)
    d.step(0, 1, None, None, np.ones((1, 1, 2), np.float16))
    assert len(d.step_cpus) == 2 and sum(d.cpu_hist.values()) == 4, "step_cpus restarts every step, cpu_hist accumulates"

    child = subprocess.Popen(["sleep", "30"])
    try:
        d.session = FakeSession(child.pid)
        here = sorted(os.sched_getaffinity(0))
        low, high = here[0], here[1]
        real = pcore_mask.p_core_cpus
        try:
            pcore_mask.p_core_cpus = lambda root=None: [high]
            d.cpu_hist = {low: 3, high: 5}
            rec = dec.placement(d)
            assert rec["calls"] == 8 and rec["calls_off_p_cores"] == 3 and rec["p_cores"] == [high], rec
            assert rec["calls_by_cpu"] == {str(low): 3, str(high): 5}
            rng = f"{here[0]}-{here[-1]}" if here == list(range(here[0], here[-1] + 1)) else None
            assert rec["child_masks"] and len(rec["decode_masks"]) >= 1 and (rng is None or rec["child_masks"] == [rng]), rec
            seen = []
            err, outp = io.StringIO(), io.StringIO()
            with redirect_stderr(err), redirect_stdout(outp):
                dec.finish_placement(d, seen.append)
            assert seen[0]["type"] == "placement" and "WARNING: 3 of 8 calls ran on an efficiency core" in err.getvalue(), err.getvalue()
            assert "3 off the performance cores" in outp.getvalue()
            d.cpu_hist = {high: 5}
            err = io.StringIO()
            with redirect_stderr(err), redirect_stdout(io.StringIO()):
                dec.finish_placement(d, seen.append)
            assert err.getvalue() == "" and seen[1]["calls_off_p_cores"] == 0
            pcore_mask.p_core_cpus = lambda root=None: None
            rec = dec.placement(d)
            assert rec["calls_off_p_cores"] is None and rec["p_cores"] is None, "no efficiency cores: no off-P count"
        finally:
            pcore_mask.p_core_cpus = real
    finally:
        child.kill()
        child.wait()
    assert dec.task_masks(child.pid) == [], "a process that is gone has no masks and does not raise"
    check_exit_paths()
    print("PLACEMENT PASS (import does not pin, script run pins and caps BLAS, opt-out and user value respected, per-call CPU, /proc masks, off-P flag, one placement record on every exit)")


if __name__ == "__main__":
    main()
