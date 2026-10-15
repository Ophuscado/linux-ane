#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Pinned against unpinned decode of the staged-Qwen M2 decode (the unchanged 38 programs, host-chained states), arms U P U P in ONE decoder process,
ONE ane-session, ONE boot and ONE load.

Lever under test: the 22 of 54 slow steps of the 10 Oct run cost +28 ms outside the ioctl (+0.74 ms per call) with unchanged exec. The suspected cause is
host placement (an E core, cpu0-3 on this machine) of the decode process or its ane-session child. Arm P sets the CPU mask of the decode process, all its
threads, the ane-session child and all its threads to the P cores 4-11. Arm U sets the original mask 0-11. Nothing else differs: same programs, ports, numerics,
driver, prompt, idle and OPENBLAS_NUM_THREADS (4 in BOTH arms, required by the frozen mode). The mask is switched between arms inside the one process, so no arm
pays a second load, and the readback of Cpus_allowed_list of every task of both processes is checked after every switch and after every arm.

Frozen protocol (nothing here is a default a caller may loosen):
  * four arms in the order U P U P; each decodes p001, 16 generated steps, at least 60 s after the previous arm; the filler must be off;
  * identity: the 16 ids of every arm equal the anchor ids of the unchanged decode; fp32 logits sha256 and size are identical in all four arms (byte equal);
    equality to the sealed 5c359d36 hash is REPORTED (the head runs on 4 BLAS threads here, the sealed runs used the default), never a substitute for arm-to-arm;
  * one boot id; bo_total_bytes read before the load, after it and around every arm: zero growth after the load; no new EXCH failed line and no STOP line;
  * every arm has 16 generated steps with 38 finite positive call walls and exec values each, and the affinity readback matches the arm's mask on every task;
  * SPEED gate (pre-registered; failed stays failed): mean generated-step wall (ane step plus host head) pooled over the two P arms divided by the same over the two
    U arms is at most 0.95, AND the pooled difference is larger than the A/A spread |mean U1 - mean U2|. Prediction recorded before the run (not a gate): ratio 0.93 to
    0.96, and 0.84 to 0.86 against the slower U arm. The p50, p10, p90, the fraction of steps above 190 ms, the outside-ioctl time and the head time are reported.
Every step keeps its raw wall, ane wall, head wall, 38 call walls and exec times, the decode process CPU after every call, and the CPU of the ane-session child
after every step (/proc/PID/stat field 39; a thread's field 39 is the CPU it LAST ran on, so thread placement is a hint and the Cpus_allowed_list readback is the proof). Added after w7C's review, recorded and never gated: per step
the ns on CPU and ns waiting on a run queue of every thread of both processes (/proc/PID/task/TID/schedstat) for the ane window and for the head window, per-CPU busy ticks for both windows
(system wide, USER_HZ units), and /proc/interrupts before and after every arm (where the ANE completion interrupt lands, if there is one). Exit 0 only when all hold and the run used the frozen spec. --unfrozen is for host tests and never exits 0.
--dry-run runs nothing."""
import argparse
import ctypes
import hashlib
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_m2_decode as dec  # noqa: E402
import qwen_resident_ab as old_ab  # noqa: E402
import qwen_resident_states_ab as rs  # noqa: E402

ORDER = "UPUP"
SPEED_BAR = 0.95
SLOW_S = 0.190
FROZEN = {"steps": 16, "programs": 38, "prompt": "p001", "idle": 60, "blas_threads": "4", "anec_total_bytes": 2_754_388_032}
MASKS = {"U": list(range(12)), "P": list(range(4, 12))}
ANCHOR_TOKENS = rs.ANCHOR_TOKENS
SEALED_LOGITS_SHA = "5c359d366ad883ea911e2bfba5f2e600dc6b22fd34eaad5c12059ef5c35ab37a"
good_number = rs.good_number
_LIBC = ctypes.CDLL(None)


def cpu_now():
    """CPU the calling thread runs on (glibc sched_getcpu; os has no such call)."""
    return _LIBC.sched_getcpu()


def tasks(pid):
    return sorted(int(t) for t in os.listdir(f"/proc/{pid}/task"))


def allowed(pid, tid):
    try:
        lines = Path(f"/proc/{pid}/task/{tid}/status").read_text().splitlines()
    except OSError:
        return None
    for line in lines:
        if line.startswith("Cpus_allowed_list:"):
            return parse_list(line.split(":", 1)[1].strip())
    return None


def parse_list(text):
    out = []
    for part in text.split(","):
        a, _, b = part.partition("-")
        out.extend(range(int(a), int(b or a) + 1))
    return out


def set_mask(cpus, pids):
    """The mask on every task of every pid, then the readback of each: returns {pid: {tid: list}}; raises if any task differs."""
    for pid in pids:
        for tid in tasks(pid):
            os.sched_setaffinity(tid, cpus)
    return check_mask(cpus, pids)


def check_mask(cpus, pids):
    seen = {pid: {tid: allowed(pid, tid) for tid in tasks(pid)} for pid in pids}
    bad = {(pid, tid): got for pid, ts in seen.items() for tid, got in ts.items() if got != sorted(cpus)}
    if bad:
        raise dec.Refuse(f"affinity readback differs from {sorted(cpus)}: {bad}")
    return seen


def proc_cpu(pid):
    """Field 39 (processor) of /proc/PID/stat."""
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
        return int(text[text.rindex(")") + 2:].split()[36])
    except (OSError, ValueError, IndexError):
        return None


def freqs():
    out = []
    for i in range(os.cpu_count() or 0):
        try:
            out.append(int(Path(f"/sys/devices/system/cpu/cpu{i}/cpufreq/scaling_cur_freq").read_text()))
        except (OSError, ValueError):
            out.append(None)
    return out


def thread_cpus(pids):
    """{pid:tid: CPU} of every thread of every process now (field 39 of /proc/PID/task/TID/stat)."""
    out = {}
    for pid in pids:
        for tid in tasks(pid):
            try:
                text = Path(f"/proc/{pid}/task/{tid}/stat").read_text()
                out[f"{pid}:{tid}"] = int(text[text.rindex(")") + 2:].split()[36])
            except (OSError, ValueError, IndexError):
                out[f"{pid}:{tid}"] = None
    return out


def busy_ticks():
    """Per-CPU non-idle clock ticks (user+nice+system+irq+softirq+steal) from /proc/stat; the difference over a step is the load on each CPU, ours included."""
    out = []
    for line in Path("/proc/stat").read_text().splitlines():
        if line.startswith("cpu") and line[3:4].isdigit():
            f = [int(x) for x in line.split()[1:9]]
            out.append(f[0] + f[1] + f[2] + f[5] + f[6] + f[7])
    return out


def schedstat(pids):
    """{label: {tid: [ns on CPU, ns waiting on a run queue]}} from /proc/PID/task/TID/schedstat (needs SCHED_INFO; {} for a label whose files are unreadable)."""
    out = {}
    for label, pid in pids.items():
        out[label] = {}
        for tid in tasks(pid):
            try:
                f = Path(f"/proc/{pid}/task/{tid}/schedstat").read_text().split()
                out[label][str(tid)] = [int(f[0]), int(f[1])]
            except (OSError, ValueError, IndexError):
                pass
    return out


def sched_delta(a, b):
    """Per label and thread the ns on CPU and the ns waiting between two schedstat reads (threads present in both)."""
    return {label: {t: [b[label][t][0] - v[0], b[label][t][1] - v[1]] for t, v in ts.items() if t in b.get(label, {})} for label, ts in a.items()}


def interrupts_text():
    """The /proc/interrupts table as it stands (per-CPU columns); kept whole per arm so the ANE completion line can be read after."""
    try:
        return Path("/proc/interrupts").read_text()
    except OSError:
        return None


class PinDecoder(dec.Decoder):
    """The unchanged decoder, recording the CPU of the decode process after every call."""

    def __init__(self, args, manifest):
        self.call_cpus = []
        super().__init__(args, manifest)

    def call(self, i, arrays):
        r = super().call(i, arrays)
        self.call_cpus.append(cpu_now())
        return r


def decode_arm(decoder, arm, prompt, new_tokens, cos, sin, embed, head, out, snap=rs.snapshot):
    """One prompt through one arm. Returns the record with the raw per-step and per-call data; logits go to out/logits.f32."""
    child = decoder.session.proc.pid
    pids = (os.getpid(), child)
    labelled = {"decode": os.getpid(), "child": child}
    masks = {"set": sorted(MASKS[arm])}
    masks["readback"] = {f"{p}:{t}": got for p, ts in set_mask(MASKS[arm], pids).items() for t, got in ts.items()}
    before = snap()
    interrupts_before = interrupts_text()
    decoder.configure(decoder.max_len)
    ids = prompt["prompt_token_ids"]
    token, pos, gen, rows = ids[0], 0, [], []
    logits_file = out / "logits.f32"
    logits_file.unlink(missing_ok=True)
    mark = max((t for t, _ in dec.kernel_marks()), default=0.0)
    while len(gen) < new_tokens and pos < decoder.max_len - 1:
        decoder.call_cpus = []
        ticks0, sched0 = busy_ticks(), schedstat(labelled)
        start = time.monotonic()
        hidden, timing = decoder.step(token, pos, cos, sin, embed, None)
        ane_s = time.monotonic() - start
        ticks1, sched1 = busy_ticks(), schedstat(labelled)
        row = {"pos": pos, "generated": pos >= len(ids) - 1, "ane_s": ane_s, "calls": [{"wall_s": w, "exec_ms": e, "cpu": c} for (w, e), c in zip(timing, decoder.call_cpus)]}
        if row["generated"]:
            head_start = time.monotonic()
            logits = head @ hidden
            with logits_file.open("ab") as f:
                logits.astype(np.float32).tofile(f)
            top1, _, _, _ = dec.top2(logits)
            row["head_s"] = time.monotonic() - head_start
            row["wall_s"] = ane_s + row["head_s"]
            gen.append(int(top1))
            token = top1
        else:
            token = ids[pos + 1]
        ticks2, sched2 = busy_ticks(), schedstat(labelled)
        row.update({"busy_ticks_ane": [b - a for a, b in zip(ticks0, ticks1)], "busy_ticks_delta": [b - a for a, b in zip(ticks0, ticks2)],
                    "schedstat_ane": sched_delta(sched0, sched1), "schedstat_head": sched_delta(sched1, sched2),
                    "decode_cpu": cpu_now(), "child_cpu": proc_cpu(child), "freq_khz": freqs(), "threads_cpu": thread_cpus(pids), "loadavg": Path("/proc/loadavg").read_text().strip()})
        rows.append(row)
        pos += 1
    stop = [line for t, line in dec.kernel_marks() if t > mark and dec.STOP_KERNEL.search(line)]
    after = snap()
    masks["readback_end"] = {f"{p}:{t}": got for p, ts in check_mask(MASKS[arm], pids).items() for t, got in ts.items()}
    interrupts_after = interrupts_text()
    data = logits_file.read_bytes() if logits_file.exists() else b""
    return {"arm": arm, "tokens": gen, "rows": rows, "affinity": masks, "interrupts": {"before": interrupts_before, "after": interrupts_after},
            "logits_sha256": hashlib.sha256(data).hexdigest() if data else None, "logits_bytes": len(data),
            "kernel_stop": stop, "boot_id_start": before["boot_id"], "boot_id_end": after["boot_id"], "bo_total_before": before["bo_total_bytes"],
            "bo_total_after": after["bo_total_bytes"], "exch_new": None if before["exch_lines"] is None or after["exch_lines"] is None else after["exch_lines"] - before["exch_lines"]}


def gen_rows(rec):
    return [r for r in rec["rows"] if r["generated"]]


def sched_sum(row, label, key="schedstat_ane"):
    """[ms on CPU, ms waiting on a run queue] summed over the threads of one process in the step's ane window, None when schedstat gave nothing."""
    ts = row.get(key, {}).get(label, {})
    if not ts:
        return None
    return [sum(v[0] for v in ts.values()) / 1e6, sum(v[1] for v in ts.values()) / 1e6]


def sched_cols(row):
    """Four tab-separated tsv cells: ms on CPU and ms waiting for decode, then for the child (empty when schedstat gave nothing)."""
    cells = []
    for label in ("decode", "child"):
        got = sched_sum(row, label)
        cells += ["", ""] if got is None else [f"{got[0]:.3f}", f"{got[1]:.3f}"]
    return "\t".join(cells)


def sched_mean(rows, label):
    got = [s for s in (sched_sum(r, label) for r in rows) if s is not None]
    return None if len(got) != len(rows) else {"on_cpu_ms": float(np.mean([s[0] for s in got])), "run_queue_wait_ms": float(np.mean([s[1] for s in got]))}


def stats(rec):
    """Per-arm numbers over the generated steps (None where a step lacks data)."""
    g = gen_rows(rec)
    walls = [r["wall_s"] for r in g]
    if not walls:
        return {}
    outside = [(r["ane_s"] - sum(c["exec_ms"] for c in r["calls"]) / 1000.0) * 1000.0 for r in g]
    between = [(r["ane_s"] - sum(c["wall_s"] for c in r["calls"])) * 1000.0 for r in g]
    core = lambda c: None if c is None else ("E" if c < 4 else "P")  # noqa: E731
    return {"arm": rec["arm"], "steps": len(walls), "mean_s": float(np.mean(walls)), "p10_s": rs.pct(walls, 0.1), "p50_s": rs.pct(walls, 0.5), "p90_s": rs.pct(walls, 0.9),
            "max_s": max(walls), "frac_above_190ms": sum(w > SLOW_S for w in walls) / len(walls), "ane_mean_s": float(np.mean([r["ane_s"] for r in g])),
            "steps_above_cut": {str(cut): sum(w > cut / 1000.0 for w in walls) for cut in (185, 190, 195, 200, 205, 210)},
            "between_calls_mean_ms": float(np.mean(between)), "exec_sum_mean_ms": float(np.mean([sum(c["exec_ms"] for c in r["calls"]) for r in g])),
            "call_wall_sum_mean_ms": float(np.mean([sum(c["wall_s"] for c in r["calls"]) * 1000 for r in g])),
            "steps_any_thread_on_E": sum(any(c is not None and c < 4 for c in r["threads_cpu"].values()) for r in g),
            "threads_per_step_min_max": [min(len(r["threads_cpu"]) for r in g), max(len(r["threads_cpu"]) for r in g)],
            "busy_ticks_on_E_mean": float(np.mean([sum(r["busy_ticks_delta"][:4]) for r in g])), "busy_ticks_on_P_mean": float(np.mean([sum(r["busy_ticks_delta"][4:]) for r in g])),
            "schedstat_ane_ms": {label: sched_mean(g, label) for label in ("decode", "child")},
            "head_mean_s": float(np.mean([r["head_s"] for r in g])), "outside_exec_mean_ms": float(np.mean(outside)),
            "call_wall_mean_ms": float(np.mean([c["wall_s"] * 1000 for r in g for c in r["calls"]])),
            "exec_mean_ms": float(np.mean([c["exec_ms"] for r in g for c in r["calls"]])),
            "steps_decode_cpu_on_E": sum(core(r["decode_cpu"]) == "E" for r in g), "steps_child_cpu_on_E": sum(core(r["child_cpu"]) == "E" for r in g),
            "calls_on_E": sum(core(c["cpu"]) == "E" for r in g for c in r["calls"]), "calls": sum(len(r["calls"]) for r in g),
            "slow_steps_by_core": {"decode_E": sum(r["wall_s"] > SLOW_S and core(r["decode_cpu"]) == "E" for r in g), "child_E": sum(r["wall_s"] > SLOW_S and core(r["child_cpu"]) == "E" for r in g),
                                   "both_P": sum(r["wall_s"] > SLOW_S and core(r["decode_cpu"]) == "P" and core(r["child_cpu"]) == "P" for r in g)}}


def check_arm(rec, arm, steps, programs, anchor, mask):
    bad, g = [], gen_rows(rec)
    where = f"arm {arm}"
    if rec["arm"] != arm:
        bad.append(f"{where}: arm label {rec['arm']}")
    if len(g) != steps or len(rec["tokens"]) != steps:
        bad.append(f"{where}: {len(g)} generated steps and {len(rec['tokens'])} ids, need {steps}")
    if anchor is not None and rec["tokens"] != anchor:
        bad.append(f"{where}: ids differ from the anchor")
    for r in g:
        if not (good_number(r.get("wall_s")) and good_number(r.get("ane_s")) and good_number(r.get("head_s"))):
            bad.append(f"{where}: pos {r['pos']} has a missing or non-positive wall")
        if len(r["calls"]) != programs or not all(good_number(c["wall_s"]) and good_number(c["exec_ms"]) and c["cpu"] is not None and c["cpu"] >= 0 for c in r["calls"]):
            bad.append(f"{where}: pos {r['pos']} does not hold {programs} finite positive calls with a CPU each")
        if r["decode_cpu"] is None or r["decode_cpu"] < 0 or r["child_cpu"] is None or r["child_cpu"] < 0:
            bad.append(f"{where}: pos {r['pos']} lacks a decode or child CPU")
        if not r.get("threads_cpu") or not r.get("busy_ticks_delta") or not r.get("loadavg"):
            bad.append(f"{where}: pos {r['pos']} lacks thread placement, busy ticks or loadavg")
    aff = rec["affinity"]
    for key in ("readback", "readback_end"):
        if not aff.get(key) or any(v != sorted(mask) for v in aff[key].values()):
            bad.append(f"{where}: affinity {key} is empty or differs from {sorted(mask)}")
    if rec["logits_bytes"] != steps * 248_320 * 4 or not rec["logits_sha256"]:
        bad.append(f"{where}: logits file is {rec['logits_bytes']} B")
    if rec["boot_id_start"] != rec["boot_id_end"]:
        bad.append(f"{where}: boot id changed")
    if rec["bo_total_before"] is None or rec["bo_total_after"] is None or rec["bo_total_before"] != rec["bo_total_after"]:
        bad.append(f"{where}: bo_total_bytes {rec['bo_total_before']} -> {rec['bo_total_after']}")
    if rec["exch_new"] != 0:
        bad.append(f"{where}: {rec['exch_new']} new EXCH lines")
    if rec["kernel_stop"]:
        bad.append(f"{where}: STOP lines {rec['kernel_stop']}")
    return bad


def judge(records, run, steps=FROZEN["steps"], programs=FROZEN["programs"], anchor=ANCHOR_TOKENS, order=ORDER):
    """The frozen gate. Returns (failures, summary); fails closed on anything missing."""
    bad = []
    if [r["arm"] for r in records] != list(order):
        return [f"arms are {[r['arm'] for r in records]}, need {list(order)}"], {}
    for i, r in enumerate(records):
        bad += check_arm(r, order[i], steps, programs, anchor, MASKS[order[i]])
    if len({r["logits_sha256"] for r in records}) != 1:
        bad.append("logits sha256 differs between arms")
    if len({r["boot_id_start"] for r in records} | {run["boot_id_pre"]}) != 1:
        bad.append("boot id differs")
    if run["bo_loaded"] is None or any(r["bo_total_before"] != run["bo_loaded"] for r in records):
        bad.append("bo_total_bytes after the load differs from an arm read")
    if run["exch_new"] != 0 or run["kernel_stop"]:
        bad.append("run-level kernel check: EXCH or STOP line")
    st = [stats(r) for r in records]
    if bad or not all(st):
        return bad or ["no stats"], {"arms": st}
    u = [s["mean_s"] for s, a in zip(st, order) if a == "U"]
    p = [s["mean_s"] for s, a in zip(st, order) if a == "P"]
    ratio = float(np.mean(p) / np.mean(u))
    spread = abs(u[0] - u[1])
    summary = {"arms": st, "pooled_mean_P_over_U": ratio, "bar": SPEED_BAR, "AA_spread_U_s": spread, "AA_spread_P_s": abs(p[0] - p[1]), "pooled_difference_s": float(np.mean(u) - np.mean(p)),
               "P_over_slower_U": float(np.mean(p) / max(u)), "P_over_faster_U": float(np.mean(p) / min(u)), "prediction": {"pooled_ratio": [0.93, 0.96], "vs_slow_arm": [0.84, 0.86]},
               "logits_equal_sealed_hash": records[0]["logits_sha256"] == SEALED_LOGITS_SHA}
    if not ratio <= SPEED_BAR:
        bad.append(f"pooled mean P/U {ratio:.4f} is above {SPEED_BAR}")
    if not summary["pooled_difference_s"] > spread:
        bad.append(f"pooled difference {summary['pooled_difference_s']:.5f} s does not exceed the A/A spread {spread:.5f} s")
    return bad, summary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prompt", default=FROZEN["prompt"])
    ap.add_argument("--new-tokens", type=int, default=FROZEN["steps"])
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", default="/var/tmp/qwen-conform-0e2c3743-r2")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session")
    ap.add_argument("--session-lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--idle", type=int, default=FROZEN["idle"])
    ap.add_argument("--out", default="/var/tmp/qwen-pin-ab")
    ap.add_argument("--unfrozen", action="store_true", help="host tests only: another size, no preflight; never exits 0")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    if args.dry_run:
        print("DRY: no ANE call. One decoder process, one ane-session, one boot, one load; arms " + " ".join(ORDER) + " (U = mask 0-11, P = mask 4-11 on every task of both processes)")
        print(f"  0. refuse unless: p001, 16 tokens, 38 programs, idle >= 60 s, OPENBLAS_NUM_THREADS={FROZEN['blas_threads']}, OpenBLAS mapped, start mask 0-11, "
              f"bo_total_bytes and cap readable and start + {FROZEN['anec_total_bytes']} < cap")
        print("  1. read boot id and bo_total_bytes; load the 38 programs; read again; take the kernel mark")
        for i, arm in enumerate(ORDER):
            print(f"  {i + 2}. idle {args.idle}s; set mask {MASKS[arm][0]}-{MASKS[arm][-1]} and read it back on every task; arm {arm}: {args.prompt}, {FROZEN['steps']} new tokens; per step wall, per call wall and exec, CPUs")
        print(f"  6. judge: identity, logits sha equal in all arms, one boot, zero bo growth, no EXCH or STOP, affinity readback, pooled mean P/U <= {SPEED_BAR} and larger than the A/A spread")
        return 0
    manifest = json.loads(Path(args.manifest).read_text())
    env = rs.environment()
    start = rs.snapshot() if not args.unfrozen else {"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "bo_total_bytes": old_ab.bo_total_bytes(), "exch_lines": 0}
    cap_mb = rs.read_cap_mb()
    if not args.unfrozen:
        problems = []
        if args.prompt != FROZEN["prompt"] or args.new_tokens != FROZEN["steps"] or len(manifest["programs"]) != FROZEN["programs"]:
            problems.append(f"the frozen gate is prompt {FROZEN['prompt']}, {FROZEN['steps']} new tokens, {FROZEN['programs']} programs")
        if args.idle < FROZEN["idle"]:
            problems.append(f"idle must be at least {FROZEN['idle']} s")
        if os.environ.get("OPENBLAS_NUM_THREADS") != FROZEN["blas_threads"]:
            problems.append(f"OPENBLAS_NUM_THREADS must be {FROZEN['blas_threads']} in both arms")
        if sorted(os.sched_getaffinity(0)) != MASKS["U"]:
            problems.append(f"start mask {sorted(os.sched_getaffinity(0))} is not 0-11")
        if env["reference_blas"] or not env["blas"]:
            problems.append("numpy is not on OpenBLAS (the reference BLAS or no BLAS mapped)")
        if start["bo_total_bytes"] is None or cap_mb is None:
            problems.append("bo_total_bytes or bo_total_max_mb is unreadable")
        elif start["bo_total_bytes"] + FROZEN["anec_total_bytes"] >= cap_mb << 20:
            problems.append(f"bo_total_bytes {start['bo_total_bytes']} plus the 38 sections {FROZEN['anec_total_bytes']} is not below the {cap_mb} MiB cap")
        if problems:
            print("REFUSE: " + "; ".join(problems) + " (use --unfrozen for a host test, which never exits 0)")
            return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ref = json.loads(Path(args.ref).read_text())
    prompt = next(p for p in ref["prompts"] if p["id"] == args.prompt)
    dargs = argparse.Namespace(out=str(out), manifest=args.manifest, anec_dir=args.anec_dir, ports_dir=args.ports_dir, ane_run="unused", timeout=args.timeout, resident=True,
                               session_bin=args.session_bin, resident_lock="call", per_m_dir="/var/tmp/qwen-perM", session_lock=args.session_lock, dump=None, band=None,
                               resident_states=False)
    mark0 = max((t for t, _ in dec.kernel_marks()), default=0.0)
    decoder = PinDecoder(dargs, manifest)
    loaded = rs.snapshot() if not args.unfrozen else {"bo_total_bytes": old_ab.bo_total_bytes()}
    embed, head, (dh, rotary, base) = dec.load_head(args.gguf)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, base)
    exch0 = start["exch_lines"] if not args.unfrozen else 0
    records = []
    for i, arm in enumerate(ORDER):
        time.sleep(args.idle)
        (out / f"arm{i + 1}-{arm}").mkdir(exist_ok=True)
        rec = decode_arm(decoder, arm, prompt, args.new_tokens, cos, sin, embed, head, out / f"arm{i + 1}-{arm}")
        records.append(rec)
        s = stats(rec)
        print(f"arm {i + 1} {arm}: mean {s.get('mean_s')} p50 {s.get('p50_s')} p90 {s.get('p90_s')} s, above 190 ms {s.get('frac_above_190ms')}, logits {str(rec['logits_sha256'])[:16]}", flush=True)
    set_mask(MASKS["U"], (os.getpid(), decoder.session.proc.pid))
    decoder.close_session()
    end = rs.snapshot() if not args.unfrozen else {"exch_lines": 0}
    run = {"boot_id_pre": start["boot_id"], "bo_loaded": loaded["bo_total_bytes"], "kernel_stop": [line for t, line in dec.kernel_marks() if t > mark0 and dec.STOP_KERNEL.search(line)],
           "exch_new": end["exch_lines"] - exch0, "bo_start": start["bo_total_bytes"], "bo_cap_mb": cap_mb}
    failures, summary = judge(records, run, steps=args.new_tokens, anchor=None if args.unfrozen else ANCHOR_TOKENS)
    if args.unfrozen:
        failures = failures + ["unfrozen run: not a gate run"]
    (out / "raw-arms.json").write_text(json.dumps(records, indent=1) + "\n")
    with (out / "steps.tsv").open("w") as f:
        f.write("arm\tpos\tgenerated\twall_s\tane_s\thead_s\tcall_wall_sum_s\texec_sum_ms\tbetween_calls_ms\tdecode_cpu\tchild_cpu\tdecode_on_ms\tdecode_wait_ms\tchild_on_ms\tchild_wait_ms\tthread_cpus\tbusy_ticks_ane\tbusy_ticks_delta\tloadavg\tmin_freq_khz\n")
        for i, r in enumerate(records):
            for row in r["rows"]:
                fr = [x for x in row["freq_khz"] if x is not None]
                f.write(f"{i + 1}{r['arm']}\t{row['pos']}\t{int(row['generated'])}\t{row.get('wall_s', '')}\t{row['ane_s']}\t{row.get('head_s', '')}\t"
                        f"{sum(c['wall_s'] for c in row['calls'])}\t{sum(c['exec_ms'] for c in row['calls'])}\t{(row['ane_s'] - sum(c['wall_s'] for c in row['calls'])) * 1000}\t"
                        f"{row['decode_cpu']}\t{row['child_cpu']}\t{sched_cols(row)}\t{','.join(f'{k}={v}' for k, v in row['threads_cpu'].items())}\t{','.join(map(str, row['busy_ticks_ane']))}\t{','.join(map(str, row['busy_ticks_delta']))}\t"
                        f"{row['loadavg']}\t{min(fr) if fr else ''}\n")
    with (out / "calls.tsv").open("w") as f:
        f.write("arm\tpos\tprog\twall_ms\texec_ms\tcpu\n")
        for i, r in enumerate(records):
            for row in r["rows"]:
                for k, c in enumerate(row["calls"]):
                    f.write(f"{i + 1}{r['arm']}\t{row['pos']}\tprog_{k:03d}\t{c['wall_s'] * 1000}\t{c['exec_ms']}\t{c['cpu']}\n")
    summary.update({"failures": failures, "frozen": not args.unfrozen, "run": run, "environment": env,
                    "raw_walls_s": {f"{i + 1}{r['arm']}": [x["wall_s"] for x in gen_rows(r)] for i, r in enumerate(records)},
                    "arms_meta": [{k: v for k, v in r.items() if k != "rows"} for r in records]})
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    print(json.dumps({k: summary.get(k) for k in ("pooled_mean_P_over_U", "bar", "AA_spread_U_s", "pooled_difference_s", "failures")}, indent=1))
    return 0 if not failures else (3 if args.unfrozen else 1)


if __name__ == "__main__":
    sys.exit(main())
