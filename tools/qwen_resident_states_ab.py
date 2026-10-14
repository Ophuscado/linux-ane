#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""A/B/A/B of the staged-Qwen M2 decode: host-chained states (arm A, the unchanged --resident path) against
session-resident states (arm B, --resident-states), in ONE decoder process, ONE ane-session and ONE boot.

The 38 programs LOAD once; every arm calls Decoder.configure, which FREEs and re-LOADs the same keys (no device work),
resets the states and, for arm B only, registers the state pairs. Arms alternate A B A B so a drift of the machine
cannot favour one side, and none of them pays a second program load (a second process per arm would grow
bo_total_bytes by about 450 MB per arm). Each arm decodes the same prompt for the same number of new tokens, 60 s
after the previous arm by default.

Per generated step the wall is the decoder's own ane_wall_s plus host_head_s (the quantity qwen_resident_ab.py
reports). Gate, fixed before any run: the token ids and the fp32 logits file sha256 are identical in all four arms;
every arm's token ids equal the first arm's; B/A of the step p50 is at most 0.90 in BOTH pairs (A1,B1) and (A2,B2).
The program-by-program exec time (timings.tsv, the session's exec_us) is reported per arm and must not move by more
than 5 percent between the arms of a pair. Exit 0 only if the gate holds. --dry-run prints the plan and runs nothing."""
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_m2_decode as dec  # noqa: E402

ORDER = "ABAB"
THRESHOLD = 0.90


def pct(values, q):
    values = sorted(values)
    return values[min(int(q * len(values)), len(values) - 1)]


def decode_arm(decoder, arm, prompt, new_tokens, cos, sin, embed, head, out):
    """One prompt through the decoder: returns the arm record. The logits of every generated step go to out/logits.f32."""
    decoder.resident_states = arm == "B"
    decoder.configure(decoder.max_len)
    ids = prompt["prompt_token_ids"]
    token, pos, gen, walls, execs = ids[0], 0, [], [], []
    logits_file = out / "logits.f32"
    logits_file.unlink(missing_ok=True)
    mark = max((t for t, _ in dec.kernel_marks()), default=0.0)
    while len(gen) < new_tokens and pos < decoder.max_len - 1:
        start = time.monotonic()
        hidden, timing = decoder.step(token, pos, cos, sin, embed, None)
        ane_s = time.monotonic() - start
        if pos >= len(ids) - 1:
            head_start = time.monotonic()
            logits = head @ hidden
            with logits_file.open("ab") as f:
                logits.astype(np.float32).tofile(f)
            top1, _, _, _ = dec.top2(logits)
            walls.append(ane_s + (time.monotonic() - head_start))
            execs.append([e for _, e in timing])
            gen.append(top1)
            token = top1
        else:
            token = ids[pos + 1]
        pos += 1
    stop = [line for t, line in dec.kernel_marks() if t > mark and dec.STOP_KERNEL.search(line)]
    per_prog = [float(np.median([e[i] for e in execs if e[i] is not None])) if execs else None for i in range(len(execs[0]))] \
        if execs else []
    return {"arm": arm, "tokens": gen, "steps": len(walls),
            "logits_sha256": hashlib.sha256(logits_file.read_bytes()).hexdigest() if logits_file.exists() else None,
            "step_p10_s": pct(walls, 0.1), "step_p50_s": pct(walls, 0.5), "step_p90_s": pct(walls, 0.9),
            "walls_s": [round(w, 4) for w in walls], "exec_ms_per_program_median": per_prog, "kernel_stop": stop}


def judge(records, order=ORDER):
    """The frozen gate over the arm records, in order. Returns (failures, summary)."""
    failures = []
    if [r["arm"] for r in records] != list(order):
        failures.append(f"arm order {[r['arm'] for r in records]} is not {list(order)}")
    first = records[0]
    for r in records:
        if r["kernel_stop"]:
            failures.append(f"arm {r['arm']}: kernel STOP line {r['kernel_stop'][0]}")
        if r["tokens"] != first["tokens"] or not r["tokens"]:
            failures.append(f"arm {r['arm']}: token ids differ from the first arm")
        if r["logits_sha256"] != first["logits_sha256"] or not first["logits_sha256"]:
            failures.append(f"arm {r['arm']}: logits sha256 differs from the first arm")
    pairs = [(records[i], records[i + 1]) for i in range(0, len(records) - 1, 2)]
    ratios = []
    for a, b in pairs:
        ratio = b["step_p50_s"] / a["step_p50_s"] if a["step_p50_s"] else float("inf")
        ratios.append(ratio)
        if not ratio <= THRESHOLD:
            failures.append(f"pair {a['arm']}{b['arm']}: B/A step p50 {ratio:.4f} is above {THRESHOLD}")
        ea, eb = a["exec_ms_per_program_median"], b["exec_ms_per_program_median"]
        for i, (x, y) in enumerate(zip(ea, eb)):
            if x and y and abs(y - x) > 0.05 * x:
                failures.append(f"pair {a['arm']}{b['arm']}: program {i} exec median {x:.3f} -> {y:.3f} ms moved more than 5 percent")
                break
    return failures, {"ratios_B_over_A": [round(x, 4) for x in ratios], "threshold": THRESHOLD, "pairs": len(pairs)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prompt", default="p001")
    ap.add_argument("--new-tokens", type=int, default=16)
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", default="/var/tmp/qwen-conform-0e2c3743-r2")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session")
    ap.add_argument("--session-lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--idle", type=int, default=60)
    ap.add_argument("--out", default="/var/tmp/qres-states-ab")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    if args.dry_run:
        print("DRY: no ANE call. One decoder process, one ane-session, arms in the order " + " ".join(ORDER))
        for i, arm in enumerate(ORDER):
            print(f"  {i + 1}. idle {args.idle}s; arm {arm}: {args.prompt}, {args.new_tokens} new tokens, "
                  f"--resident{' --resident-states' if arm == 'B' else ''}")
        print(f"  gate: tokens and logits sha256 equal in all arms; B/A step p50 <= {THRESHOLD} in both pairs; "
              "per-program exec within 5 percent per pair; exit 0 only if it holds")
        return 0
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(Path(args.manifest).read_text())
    ref = json.loads(Path(args.ref).read_text())
    prompt = next(p for p in ref["prompts"] if p["id"] == args.prompt)
    dargs = argparse.Namespace(out=str(out), manifest=args.manifest, anec_dir=args.anec_dir, ports_dir=args.ports_dir,
                               ane_run="unused", timeout=args.timeout, resident=True, session_bin=args.session_bin,
                               resident_lock="call", per_m_dir="/var/tmp/qwen-perM", session_lock=args.session_lock,
                               dump=None, band=None, resident_states=False)
    decoder = dec.Decoder(dargs, manifest)
    embed, head, (dh, rotary, base) = dec.load_head(args.gguf)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, base)
    records = []
    for i, arm in enumerate(ORDER):
        time.sleep(args.idle)
        (out / f"arm{i + 1}-{arm}").mkdir(exist_ok=True)
        rec = decode_arm(decoder, arm, prompt, args.new_tokens, cos, sin, embed, head, out / f"arm{i + 1}-{arm}")
        rec["boot_id"] = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        records.append(rec)
        print(f"arm {i + 1} {arm}: step p50 {rec['step_p50_s']:.4f} s over {rec['steps']} steps, logits {str(rec['logits_sha256'])[:16]}", flush=True)
    decoder.close_session()
    failures, summary = judge(records)
    summary.update({"failures": failures, "arms": [{k: v for k, v in r.items() if k != "walls_s"} for r in records]})
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    print(json.dumps({k: summary[k] for k in ("ratios_B_over_A", "threshold", "failures")}, indent=1))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
