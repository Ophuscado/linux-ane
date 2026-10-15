#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Time the Q6_K head matvec on the real tied head of a GGUF. CPU and memory only: no ANE, no GPU, no device node.

usage: q6k_head_bench.py MODEL.gguf [--gguf-py DIR] [--reps 20] [--threads 1,2,4,8] [--tolerance 3e-6]

It pins itself to the performance cores (tools/pcore_mask.py, OMARCHY_ANE_NO_PIN=1 turns that off), reads
token_embd.weight (Q6_K) as bytes, and for each thread count times `logits(hidden)` over --reps runs with a random fp32
hidden vector. It prints one JSON object: per thread count the median, best and worst time in ms, the bytes read, the
GB/s that implies, and the fp32-head baseline the Qwen decode pays today (21.65 ms) only as a reference value.

Each thread count also checks the first 256 logits against a float64 dot of the dequantized rows. After the JSON is
printed the exit code is 0 only when every thread count's largest error is finite and at most --tolerance (default 3e-6,
the gate of the M2 head timing). Otherwise it prints FAIL to stderr and exits 3, so a fast wrong kernel cannot pass.
Exit 2: the tensor is not Q6_K.
"""

import argparse
import json
import math
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pcore_mask

# A script run pins itself before numpy starts its threads; a test that imports this module is left alone.
PINNED = pcore_mask.pin_self() if __name__ == "__main__" else None

import numpy as np
from q6k_head import Q6KHead

BASELINE_FP32_HEAD_MS = 21.65     # the sealed M2 decode head, 2.03 GB read per step (reference value, not measured here)
DEFAULT_TOLERANCE = 3e-6
CHECK_ROWS = 256


def measure(blocks, rows, k, hidden, check, thread_counts, reps, lib=None):
    """Time `logits` per thread count and compare the first CHECK_ROWS logits with `check` (float64, the reference)."""
    out = {}
    for threads in thread_counts:
        head = Q6KHead(blocks, rows, k, threads=threads, lib=lib)
        head.logits(hidden)                                   # warm: page the tensor in
        times = []
        for _ in range(reps):
            start = time.perf_counter()
            logits = head.logits(hidden)
            times.append((time.perf_counter() - start) * 1e3)
        error = float(np.max(np.abs(logits[:CHECK_ROWS].astype(np.float64) - check)))   # NaN stays NaN
        median = statistics.median(times)
        out[str(threads)] = {"median_ms": round(median, 3), "best_ms": round(min(times), 3), "worst_ms": round(max(times), 3),
                             "gb_per_s": round(blocks.nbytes / (median / 1e3) / 1e9, 1), "max_abs_err_first256": error}
    return out


def verdict(per_thread, tolerance):
    """Exit code 0 when every thread count's error is finite and within the tolerance, 3 otherwise, and the reasons."""
    bad = [f"{n} threads: error {r['max_abs_err_first256']!r}" for n, r in per_thread.items()
           if not math.isfinite(r["max_abs_err_first256"]) or r["max_abs_err_first256"] > tolerance]
    return (3 if bad else 0), bad


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("model")
    parser.add_argument("--gguf-py", default="/var/tmp/qwen-decode/gguf-py")
    parser.add_argument("--reps", type=int, default=20)
    parser.add_argument("--threads", default="1,2,4,8")
    parser.add_argument("--tolerance", type=float, default=DEFAULT_TOLERANCE,
                        help="largest accepted |error| of the first 256 logits (default 3e-6)")
    args = parser.parse_args(argv)
    sys.path.insert(0, args.gguf_py)
    from gguf import GGMLQuantizationType, GGUFReader
    from gguf.quants import dequantize

    reader = GGUFReader(args.model, "r")
    tensor = next(t for t in reader.tensors if t.name == "token_embd.weight")
    if tensor.tensor_type != GGMLQuantizationType.Q6_K:
        print(f"token_embd.weight is {tensor.tensor_type.name}, not Q6_K", file=sys.stderr)
        return 2
    k, rows = int(tensor.shape[0]), int(tensor.shape[1])
    blocks = np.asarray(tensor.data).reshape(rows, -1)
    hidden = (np.random.default_rng(1).standard_normal(k) * 2.0).astype(np.float32)
    check = dequantize(np.ascontiguousarray(blocks[:CHECK_ROWS]), GGMLQuantizationType.Q6_K).astype(np.float64) @ hidden.astype(np.float64)
    per_thread = measure(blocks, rows, k, hidden, check, [int(n) for n in args.threads.split(",")], args.reps)
    code, bad = verdict(per_thread, args.tolerance)
    print(json.dumps({"model_bytes": int(blocks.nbytes), "rows": rows, "k": k, "pinned": PINNED, "reps": args.reps,
                      "baseline_fp32_head_ms": BASELINE_FP32_HEAD_MS, "tolerance": args.tolerance, "ok": code == 0,
                      "threads": per_thread}, indent=1))
    if bad:
        print("FAIL: " + "; ".join(bad), file=sys.stderr)
    return code


if __name__ == "__main__":
    sys.exit(main())
