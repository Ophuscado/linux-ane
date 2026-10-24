#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Identity and timing of folded staged-Qwen layouts against the unchanged 38 programs, in ONE decoder process, ONE
ane-session and ONE boot, with host-chained states in every arm.

A layout joins the DeltaNet state block of some layers to the program after it (tools/qwen_fold_layout.py builds its
manifest and its mapping to the control programs). Arm A is the control. Every other arm is a layout, for example F00
(one fold, 37 programs) and S18 (all 18 DeltaNet layers folded, 20 programs). Every program of a layout that is not folded is
the control's own ANEC and port table, byte for byte, so the session's key cache hits and no device work is added for it.
Every folded program of every layout is loaded once, before the BO reading that follows the load, under a spare name,
so a later arm switch allocates nothing.

Phases, in this order:
  1. IDENTITY: the ten frozen prompts, 32 new tokens each, arm A then every other arm, no idle between them. Gate: every
     arm's 32 token ids equal A's on every prompt, and A's 16 ids on p001 equal the 16 anchor ids of the 2026-10-10 receipt.
     The fp32 logits of every prompt are compared with A's: byte equality and the maximum absolute difference are REPORTED.
  2. TIMING: the arms in --order (default A, S18, F00 repeated for --rounds rounds), p001, --new-tokens new tokens, at
     least --idle seconds of idle before each. REPORTED per layout: step p50 against the control arms of the same round, the
     control's own spread (largest over smallest control p50), the per-program exec medians, and the exec saved per fold
     (the sum of the replaced control programs' exec medians minus the folded program's). A speed difference counts only when it
     is larger than the control's own spread; the summary says which side of that line it falls.
Integrity gates (exit 1 when one fails): one boot id; bo_total_bytes unchanged after the load; no new EXCH failed line and no STOP
line over the whole span; every arm has exactly its program count of finite positive exec medians; every arm's first 16 ids equal
the anchor ids. There is no speed gate: a fold saves a fraction of one call per layer, so the result is a measurement.
The ANE buffers live in a 4 GiB window with power-of-two size classes, and program sections are held until the next reboot. The control
and a 20 program layout each take about 3.1 GiB of it, so they cannot share a boot. --only ARM runs one arm in one boot (A on one boot,
a layout on another): identity compares the token ids and the logits hash with the control summary of the other boot
(--control-summary), and tools/qwen_fold_set_compare.py reads the two summaries. The refusal counts power-of-two classes against
3.5 GiB, not the software cap.
The harness pins itself to the performance cores (tools/pcore_mask.py) before numpy starts its BLAS threads and records the
placement. --unfrozen lets a host test use another size and never exits 0. --dry-run runs nothing."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pcore_mask  # noqa: E402

# Before numpy starts its BLAS threads, which inherit the mask. OMARCHY_ANE_NO_PIN=1 turns the pin off.
PINNED = pcore_mask.pin_self() if __name__ == "__main__" else None

import argparse  # noqa: E402
import hashlib  # noqa: E402
import json  # noqa: E402
import time  # noqa: E402

import numpy as np  # noqa: E402
import qwen_m2_decode as dec  # noqa: E402
import qwen_resident_ab as old_ab  # noqa: E402
import qwen_resident_states_ab as rs  # noqa: E402

CONTROL = "A"
STEP_NEW_TOKENS = 32
FROZEN = {"prompt": "p001", "idle": 60, "identity_prompts": 10, "identity_tokens": 32, "control_programs": 38}
ANCHOR_TOKENS = rs.ANCHOR_TOKENS
good_number = rs.good_number


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build_sets(control_manifest, layout_manifests, anec_dir, ports_dir, fold_root):
    """Per arm: the manifest and, per program, (anec file, port table, the table's own program name).
    A layout manifest carries "fold_mapping": per layout program, the control indices it replaces (one index: copied
    from the control; two: a folded program, read from <fold_root>/<id>/prog<NN>/)."""
    control_files = [(Path(anec_dir) / f"prog_{i:03d}" / "program-0.anec", Path(ports_dir) / f"prog_{i:03d}" / "ports.resolved.json", f"prog_{i:03d}")
                     for i in range(len(control_manifest["programs"]))]
    sets = {CONTROL: {"manifest": control_manifest, "files": control_files, "mapping": [[i] for i in range(len(control_files))]}}
    for layout, manifest in layout_manifests.items():
        mapping = manifest["fold_mapping"]
        if len(mapping) != len(manifest["programs"]):
            raise dec.Refuse(f"{layout}: {len(mapping)} mapping rows for {len(manifest['programs'])} programs")
        files = []
        for i, rows in enumerate(mapping):
            if len(rows) == 1:
                files.append(control_files[rows[0]])
            else:
                d = Path(fold_root) / layout / f"prog{i:02d}"
                files.append((d / "program-0.anec", d / "ports.resolved.json", f"prog_{i:03d}_fold"))
        sets[layout] = {"manifest": manifest, "files": files, "mapping": mapping}
    return sets


WINDOW_LIMIT = int(3.5 * 2 ** 30)   # in classes: the control alone (3.13 GiB) and the control with two 128 MiB spares (3.38 GiB) loaded; a 256 MiB spare on top (3.63 GiB) failed


def class_bytes(size):
    """The power-of-two size class of one program section."""
    c = 1
    while c < size:
        c <<= 1
    return c


def window_class_bytes(paths):
    """Bytes the sections take in the window: each file rounded up to its power-of-two class."""
    return sum(class_bytes(Path(p).stat().st_size) for p in paths)


def loaded_paths(sets, run_arms, spares):
    """The ANEC files loaded in this boot, one entry per path: a control program shared by two arms or two layout programs is held once."""
    return {str(f[0]): f[0] for arm in run_arms for f in sets[arm]["files"]} | {str(a): a for _, a, _ in spares}


def spare_loads(sets):
    """The distinct folded programs of every layout: (key, anec, table) for loading under spare names."""
    seen, out = {}, []
    for layout, s in sets.items():
        if layout == CONTROL:
            continue
        for rows, (anec, table, _) in zip(s["mapping"], s["files"]):
            if len(rows) > 1:
                key = (sha256_file(anec), sha256_file(table))
                if key not in seen:
                    seen[key] = f"fold_{key[0][:8]}_{key[1][:6]}"
                    out.append((seen[key], anec, table))
    return out


class FoldDecoder(dec.Decoder):
    """The decoder with a per-arm program set: self.arm picks the manifest and the files of configure()."""

    def __init__(self, args, sets, arm=CONTROL):
        self.sets = sets
        self.arm = arm
        super().__init__(args, sets[arm]["manifest"])

    def configure(self, m):
        s = self.sets[self.arm]
        self.manifest = s["manifest"]
        if m != int(s["manifest"]["max_len"]):
            raise dec.Refuse("the fold harness runs the build's max_len only")
        self.max_len, self.progs, self.tables = m, dec.manifest_at(s["manifest"], m)["programs"], []
        if len(self.progs) != len(s["files"]):
            raise dec.Refuse(f"arm {self.arm}: {len(self.progs)} programs in the manifest, {len(s['files'])} files")
        for i, pr in enumerate(self.progs):
            anec, path, orig = s["files"][i]
            if not (path.is_file() and anec.is_file()):
                raise dec.Refuse(f"arm {self.arm} program {i}: {path} or {anec} is missing")
            table = json.loads(path.read_text())
            if table.get("program") != orig or table.get("exceptions"):
                raise dec.Refuse(f"{path}: wrong program (need {orig}) or unresolved exceptions")
            ports = dec.port_map_from_table(table)
            dims = lambda shape: [d for d in shape if d != 1]  # noqa: E731
            want = {x["port"]: ("input", dims(x["shape"])) for x in pr["srcs"]}
            want.update({d["port"]: ("output", dims(d["shape"])) for d in pr["dsts"]})
            want.update({x["out_port"]: ("output", dims(x["out_shape"])) for x in pr["states"]})
            got = {n: (p["direction"], dims(p["shape"])) for n, p in ports.items()}
            if got != want:
                raise dec.Refuse(f"{path}: ports {sorted(got.items())} do not match the manifest {sorted(want.items())}")
            self.tables.append((anec, path, ports))
        if self.session is not None:
            for name in [n for n in list(self.session.names) if n.startswith("prog_")]:
                self.session.free(name)
            by_size = sorted(range(len(self.tables)), key=lambda k: -Path(self.tables[k][0]).stat().st_size)   # large sections first (fragmentation)
            for i in by_size:
                anec, path, ports = self.tables[i]
                n_in = len([p for p in ports.values() if p["direction"] == "input"])
                n_out = len([p for p in ports.values() if p["direction"] == "output"])
                got = self.session.load(f"prog_{i:03d}", anec, path)
                if got != (n_in, n_out):
                    raise dec.Refuse(f"prog_{i:03d}: session reports {got}, table holds ({n_in}, {n_out})")
        self.reset()


def decode_run(decoder, arm, prompt, new_tokens, cos, sin, embed, head, out, snap=rs.snapshot):
    """One prompt through one arm: the record, with the fp32 logits of every generated step in out/logits.f32."""
    decoder.arm = arm
    before = snap()
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
            gen.append(int(top1))
            token = top1
        else:
            token = ids[pos + 1]
        pos += 1
    stop = [line for t, line in dec.kernel_marks() if t > mark and dec.STOP_KERNEL.search(line)]
    after = snap()
    names = [f"prog_{i:03d}" for i in range(len(decoder.progs))]
    per_prog = {}
    for i, name in enumerate(names):
        col = [e[i] for e in execs if len(e) > i]
        per_prog[name] = float(np.median(col)) if col and len(col) == len(execs) and all(x is not None for x in col) else None
    data = logits_file.read_bytes() if logits_file.exists() else b""
    return {"arm": arm, "prompt": prompt["id"], "tokens": gen, "steps": len(walls),
            "logits_sha256": hashlib.sha256(data).hexdigest() if data else None, "logits_bytes": len(data),
            "step_p10_s": rs.pct(walls, 0.1) if walls else None, "step_p50_s": rs.pct(walls, 0.5) if walls else None,
            "step_p90_s": rs.pct(walls, 0.9) if walls else None, "walls_s": walls,
            "exec_ms_per_program_median": per_prog, "kernel_stop": stop,
            "boot_id_start": before["boot_id"], "boot_id_end": after["boot_id"],
            "bo_total_before": before["bo_total_bytes"], "bo_total_after": after["bo_total_bytes"],
            "exch_new": None if before["exch_lines"] is None or after["exch_lines"] is None else after["exch_lines"] - before["exch_lines"]}


def logits_compare(path_a, path_b):
    """Byte equality and the maximum absolute difference of two fp32 logits files (REPORTED, never gated)."""
    a, b = Path(path_a).read_bytes(), Path(path_b).read_bytes()
    if a == b:
        return {"bytes_equal": True, "max_abs_diff": 0.0, "n": len(a) // 4}
    if len(a) != len(b) or len(a) % 4:
        return {"bytes_equal": False, "max_abs_diff": None, "n": None, "note": "sizes differ"}
    x, y = np.frombuffer(a, np.float32), np.frombuffer(b, np.float32)
    return {"bytes_equal": False, "max_abs_diff": float(np.max(np.abs(x.astype(np.float64) - y.astype(np.float64)))), "n": int(x.size)}


def check_arm(r, steps, programs, where, anchor):
    """Structural and numeric requirements on one arm record of a given program count."""
    need = ("arm", "tokens", "steps", "logits_sha256", "logits_bytes", "step_p10_s", "step_p50_s", "step_p90_s", "walls_s", "exec_ms_per_program_median",
            "kernel_stop", "boot_id_start", "boot_id_end", "bo_total_before", "bo_total_after", "exch_new")
    missing = [k for k in need if k not in r]
    if missing:
        return [f"{where}: record lacks {missing}"]
    bad = []
    if r["steps"] != steps:
        bad.append(f"{where}: {r['steps']} generated steps, need {steps}")
    if not isinstance(r["tokens"], list) or len(r["tokens"]) != steps or not all(isinstance(t, int) and not isinstance(t, bool) for t in r["tokens"]):
        bad.append(f"{where}: token ids are not {steps} integers")
    elif anchor is not None and r["tokens"][:len(anchor)] != anchor:
        bad.append(f"{where}: the first {len(anchor)} token ids differ from the receipt's anchor ids")
    if not isinstance(r["walls_s"], list) or len(r["walls_s"]) != steps or not all(good_number(w) for w in r["walls_s"]):
        bad.append(f"{where}: step walls are not {steps} finite positive numbers")
    for k in ("step_p10_s", "step_p50_s", "step_p90_s"):
        if not good_number(r[k]):
            bad.append(f"{where}: {k} is not finite and positive")
    execs = r["exec_ms_per_program_median"]
    names = [f"prog_{i:03d}" for i in range(programs)]
    if not isinstance(execs, dict) or set(execs) != set(names):
        bad.append(f"{where}: exec medians cover {len(execs) if isinstance(execs, dict) else '?'} programs, need exactly the {programs} named {names[0]} .. {names[-1]}")
    else:
        for name in names:
            if not good_number(execs[name]):
                bad.append(f"{where}: {name} exec median {execs[name]!r} is not finite and positive")
    if not isinstance(r["logits_sha256"], str) or len(r["logits_sha256"]) != 64:
        bad.append(f"{where}: no logits sha256")
    if not isinstance(r["logits_bytes"], int) or r["logits_bytes"] <= 0 or r["logits_bytes"] % (4 * steps):
        bad.append(f"{where}: logits file of {r['logits_bytes']!r} bytes is not {steps} equal fp32 vectors")
    if r["kernel_stop"]:
        bad.append(f"{where}: kernel STOP line {r['kernel_stop'][0]}")
    if r["exch_new"] != 0:
        bad.append(f"{where}: EXCH failed lines since the arm started: {r['exch_new']!r}")
    for k in ("boot_id_start", "boot_id_end"):
        if not isinstance(r[k], str) or not r[k]:
            bad.append(f"{where}: no {k}")
    for k in ("bo_total_before", "bo_total_after"):
        if not isinstance(r[k], int) or isinstance(r[k], bool) or r[k] < 0:
            bad.append(f"{where}: {k} unreadable ({r[k]!r})")
    return bad


def judge_identity(ident, arms, anchor=ANCHOR_TOKENS, prompts=FROZEN["identity_prompts"], tokens=FROZEN["identity_tokens"], control_ids=None):
    """Gate 1: every arm's ids equal the control's on every prompt; the control's p001 ids start with the anchor ids.
    control_ids ({prompt: ids}, from the control summary of another boot) stands in for the control record when the control is not in this run."""
    bad = []
    if not isinstance(ident, list) or len(ident) != prompts:
        return [f"identity: {len(ident) if hasattr(ident, '__len__') else '?'} prompts, need {prompts}"], {}
    here = CONTROL in arms
    if not here and not isinstance(control_ids, dict):
        return ["identity: the control is not in this run and no control ids were given"], {}
    equal = {arm: 0 for arm in arms if arm != CONTROL}
    for e in ident:
        pid = e.get("prompt")
        base = e.get(CONTROL) if here else {"tokens": control_ids.get(pid)}
        if not isinstance(base, dict) or (not here and not isinstance(base["tokens"], list)):
            bad.append(f"identity {pid}: no control record")
            continue
        for arm in arms:
            r = e.get(arm)
            if not isinstance(r, dict):
                bad.append(f"identity {pid}: record lacks arm {arm}")
                continue
            if not isinstance(r.get("tokens"), list) or len(r["tokens"]) != tokens or not all(isinstance(t, int) and not isinstance(t, bool) for t in r["tokens"]):
                bad.append(f"identity {pid} arm {arm}: ids are not {tokens} integers")
            if r.get("kernel_stop") or r.get("exch_new") != 0:
                bad.append(f"identity {pid} arm {arm}: kernel STOP or EXCH lines ({r.get('kernel_stop')!r}, {r.get('exch_new')!r})")
            if arm != CONTROL:
                if r.get("tokens") == base.get("tokens") and isinstance(r.get("tokens"), list) and len(r["tokens"]) == tokens:
                    equal[arm] += 1
                else:
                    bad.append(f"identity {pid}: arm {arm} ids differ from the control's")
    first = ident[0].get(arms[0], {}).get("tokens") if ident else None
    if anchor is not None and (not isinstance(first, list) or first[:len(anchor)] != anchor):
        bad.append(f"identity: arm {arms[0]}'s p001 ids do not start with the receipt's anchor ids")
    return bad, {"prompts": prompts, "tokens_each": tokens, "prompts_with_ids_equal_to_control": equal}


def judge_timing(records, run, layouts, order, steps, anchor=ANCHOR_TOKENS):
    """Integrity gates over the timing arms, and the speed report (never a gate)."""
    failures = rs.check_run(run)
    programs = {CONTROL: layouts[CONTROL]["n"], **{k: v["n"] for k, v in layouts.items() if k != CONTROL}}
    if not isinstance(records, list) or len(records) != len(order):
        return failures + [f"{len(records) if hasattr(records, '__len__') else '?'} timing arm records, need exactly {len(order)}"], {}
    if [r.get("arm") for r in records] != list(order):
        failures.append(f"arm order {[r.get('arm') for r in records]} is not {list(order)}")
    for i, r in enumerate(records):
        failures += check_arm(r, steps, programs.get(r.get("arm"), 0), f"arm {i + 1} ({r.get('arm')})", anchor)
    if failures:
        return failures, {}
    boots = {r[k] for r in records for k in ("boot_id_start", "boot_id_end")} | {run["boot_id_pre"]}
    if len(boots) != 1:
        failures.append(f"{len(boots)} different boot ids across the load and the arms")
    for i, r in enumerate(records):
        for k in ("bo_total_before", "bo_total_after"):
            if r[k] != run["bo_loaded"]:
                failures.append(f"arm {i + 1}: {k} {r[k]} differs from the {run['bo_loaded']} read after the load (limit: zero growth)")
    if len(set(order)) == 1:
        return failures, single_arm_report(records)
    return failures, speed_report(records, layouts, order)


def single_arm_report(records):
    """One arm timed alone in this boot: its step p50 per round, its own spread, and the per-program exec medians over its rounds."""
    p50 = [r["step_p50_s"] for r in records]
    per_prog = {}
    for r in records:
        for name, v in r["exec_ms_per_program_median"].items():
            per_prog.setdefault(name, []).append(v)
    return {"arm": records[0]["arm"], "step_p50_s": p50, "spread": max(p50) / min(p50) - 1.0 if len(p50) > 1 else None, "arms": len(p50),
            "exec_ms_median": {k: float(np.median(v)) for k, v in per_prog.items()},
            "exec_ms_range": {k: [min(v), max(v)] for k, v in per_prog.items()},
            "note": "one arm in this boot: compare it with the other boot's summary with tools/qwen_fold_set_compare.py"}


def speed_report(records, layouts, order):
    """Per layout: step p50 against the control arm of the same round, the control spread, and the exec saved per fold."""
    rounds = len(order) // len(set(order))
    controls = [r for r in records if r["arm"] == CONTROL]
    ctl_p50 = [r["step_p50_s"] for r in controls]
    spread = max(ctl_p50) / min(ctl_p50) - 1.0 if len(ctl_p50) > 1 else None
    report = {"rounds": rounds, "control_step_p50_s": ctl_p50, "control_spread": spread, "control_arms": len(ctl_p50),
              "spread_note": "the spread of the control arms is a screen over that many arms, not a significance test; "
                             "a layout with one fold saves about 0.3 ms and cannot clear it, so read it as a pipeline check",
              "layouts": {}}
    ctl_exec = {}
    for r in controls:
        for name, v in r["exec_ms_per_program_median"].items():
            ctl_exec.setdefault(name, []).append(v)
    ctl_med = {k: float(np.median(v)) for k, v in ctl_exec.items()}
    ctl_exec_spread = {k: (max(v) - min(v)) for k, v in ctl_exec.items()}
    ctl_exec_spread_total = sum(ctl_exec_spread.values())
    for layout in [a for a in dict.fromkeys(order) if a != CONTROL]:
        ratios, steps = [], []
        for i, r in enumerate(records):
            if r["arm"] != layout:
                continue
            round_control = max((j for j in range(i) if records[j]["arm"] == CONTROL), default=None)
            if round_control is not None:
                ratios.append(r["step_p50_s"] / records[round_control]["step_p50_s"])
            steps.append(r["step_p50_s"])
        mapping = layouts[layout]["mapping"]
        exec_med = {}
        for r in records:
            if r["arm"] == layout:
                for name, v in r["exec_ms_per_program_median"].items():
                    exec_med.setdefault(name, []).append(v)
        exec_med = {k: float(np.median(v)) for k, v in exec_med.items()}
        saved = []
        for i, rows in enumerate(mapping):
            if len(rows) > 1:
                replaced = sum(ctl_med[f"prog_{j:03d}"] for j in rows)
                saved.append({"layout_program": i, "control_programs": rows, "replaced_exec_ms": round(replaced, 4),
                              "folded_exec_ms": round(exec_med[f"prog_{i:03d}"], 4), "saved_ms": round(replaced - exec_med[f"prog_{i:03d}"], 4)})
        total_saved = round(sum(s["saved_ms"] for s in saved), 4)
        median_ratio = float(np.median(ratios)) if ratios else None
        beyond = None if spread is None or median_ratio is None else abs(median_ratio - 1.0) > spread
        report["layouts"][layout] = {
            "programs": layouts[layout]["n"], "step_p50_s": steps, "step_ratio_to_round_control": [round(x, 4) for x in ratios],
            "median_ratio": None if median_ratio is None else round(median_ratio, 4),
            "step_difference_ms": None if median_ratio is None else round((median_ratio - 1.0) * float(np.median(ctl_p50)) * 1e3, 2),
            "outside_control_spread": beyond, "folds": len(saved), "exec_saved_per_fold": saved, "exec_saved_total_ms": total_saved,
            "exec_saved_total_vs_control_exec_spread_ms": round(ctl_exec_spread_total, 4)}
    return report


def default_order(layout_ids, rounds):
    """The control first in every round, the layouts in alternating order (forward, reversed, forward, ...), so no layout always
    follows the same arm or always sits at the same place after the idle and the reload."""
    order = []
    for r in range(rounds):
        order += [CONTROL] + (layout_ids if r % 2 == 0 else layout_ids[::-1])
    return order


def check_order(order, layout_ids):
    """None when --order is complete rounds (one control followed by every declared layout once), else the reason it is not."""
    if not order or order[0] != CONTROL:
        return "the order must start with the control"
    rounds, current = [], []
    for arm in order:
        if arm == CONTROL and current:
            rounds.append(current)
            current = []
        current.append(arm)
    rounds.append(current)
    for r in rounds:
        if r[0] != CONTROL or sorted(r[1:]) != sorted(layout_ids):
            return f"round {r} is not one {CONTROL} followed by every declared layout once ({sorted(layout_ids)})"
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--layouts", default="", help="comma list of layout ids, each with <layout-dir>/manifest-<id>.json and <fold-root>/<id>/progNN/")
    ap.add_argument("--layout-dir", default=".")
    ap.add_argument("--fold-root", default=".")
    ap.add_argument("--only", help="run ONE arm in this boot: the control A or one layout id (the 4 GiB window cannot hold the control and a layout)")
    ap.add_argument("--control-summary", help="with --only LAYOUT: summary.json of the control's boot, for the token ids and logits hashes to compare with")
    ap.add_argument("--order", help="comma list of arms for the timing phase (default: control then the layouts in --layouts order, for --rounds rounds)")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--prompt", default=FROZEN["prompt"])
    ap.add_argument("--new-tokens", type=int, default=STEP_NEW_TOKENS)
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", default="/var/tmp/qwen-conform-0e2c3743-r2")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session")
    ap.add_argument("--session-lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--timeout", type=int, default=120)
    ap.add_argument("--idle", type=int, default=FROZEN["idle"])
    ap.add_argument("--identity-prompts", type=int, default=FROZEN["identity_prompts"])
    ap.add_argument("--identity-tokens", type=int, default=FROZEN["identity_tokens"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--unfrozen", action="store_true", help="host tests only: another size, no preflight; never exits 0")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)
    only = args.only
    layout_ids = [x for x in args.layouts.split(",") if x]
    if only and only != CONTROL and only not in layout_ids:
        layout_ids = [only]
    if only and args.order:
        print("REFUSE: --only runs one arm for --rounds rounds; --order does not apply to it")
        return 2
    if only:
        order = [only] * args.rounds
        problem = None if only == CONTROL or args.control_summary else "--only LAYOUT needs --control-summary (the token ids and logits hashes of the control's boot)"
        if only == CONTROL:
            layout_ids = []
    else:
        order = [x for x in args.order.split(",") if x] if args.order else default_order(layout_ids, args.rounds)
        problem = check_order(order, layout_ids)
    if problem:
        print(f"REFUSE: {problem}")
        return 2
    if args.dry_run:
        print("DRY: no ANE call. One decoder process, one ane-session, one boot; host-chained states in every arm")
        print(f"  arms in this boot: {[only] if only else [CONTROL] + layout_ids}; layouts from {args.layout_dir}")
        print("  0. refuse unless: p001, idle >= 60 s, OpenBLAS mapped, every manifest, ANEC and port table is present, bo_total_bytes and bo_total_max_mb readable "
              "and the power-of-two classes of the loaded sections are below 3.5 GiB of the 4 GiB window")
        print("  1. read boot id and bo_total_bytes; load the programs of the arms; read bo_total_bytes again; take the kernel mark")
        print(f"  2. IDENTITY: {args.identity_prompts} prompts x {args.identity_tokens} tokens per arm"
              + ("; gate: ids equal the control summary's" if only and only != CONTROL else "; gate: ids equal A's on every prompt"))
        for i, arm in enumerate(order):
            print(f"  {i + 3}. TIMING idle {args.idle}s; arm {arm}: {args.prompt}, {args.new_tokens} new tokens")
        print("  last. judge the integrity gates; report")
        return 0
    manifest = json.loads(Path(args.manifest).read_text())
    layout_manifests = {i: json.loads((Path(args.layout_dir) / f"manifest-{i}.json").read_text()) for i in layout_ids}
    run_arms = [only] if only else [CONTROL] + layout_ids
    control_ids, control_logits = None, {}
    if only and only != CONTROL:
        saved = json.loads(Path(args.control_summary).read_text())
        control_ids = {e["prompt"]: e.get(f"{CONTROL}_ids") for e in saved.get("identity_prompts", [])}
        control_logits = {e["prompt"]: e.get(f"{CONTROL}_logits_sha256") for e in saved.get("identity_prompts", [])}
    env = rs.environment()
    start = rs.snapshot() if not args.unfrozen else {"boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(), "bo_total_bytes": old_ab.bo_total_bytes(), "exch_lines": 0}
    cap_mb = rs.read_cap_mb()
    sets = build_sets(manifest, layout_manifests, args.anec_dir, args.ports_dir, args.fold_root)
    spares = spare_loads({k: v for k, v in sets.items() if k in run_arms}) if len(run_arms) > 1 else []
    present = [p for p in loaded_paths(sets, run_arms, spares).values() if Path(p).is_file()]
    window_bytes = window_class_bytes(present)
    if not args.unfrozen:
        problems = []
        if args.prompt != FROZEN["prompt"] or args.identity_prompts != FROZEN["identity_prompts"] or args.identity_tokens != FROZEN["identity_tokens"]:
            problems.append("the frozen run is p001 in the timing phase and 10 prompts of 32 tokens in the identity phase")
        if len(manifest["programs"]) != FROZEN["control_programs"]:
            problems.append("the control manifest does not hold 38 programs")
        for layout in layout_ids:
            if layout in run_arms and layout_manifests[layout].get("equiv_checked") is not True:
                problems.append(f"{layout}: the manifest was built without the MIL chain check (equiv_checked is not true); rebuild it with qwen_fold_layout.py")
        for layout in run_arms:
            for anec, table, _ in sets[layout]["files"]:
                if not (Path(anec).is_file() and Path(table).is_file()):
                    problems.append(f"{layout}: {anec} or {table} is missing")
                    break
        if args.idle < FROZEN["idle"]:
            problems.append(f"idle must be at least {FROZEN['idle']} s")
        if env["reference_blas"] or not env["blas"]:
            problems.append("numpy is not on OpenBLAS (the reference BLAS or no BLAS mapped)")
        if start["bo_total_bytes"] is None or cap_mb is None:
            problems.append("bo_total_bytes or bo_total_max_mb is unreadable")
        elif start["bo_total_bytes"] + window_bytes >= cap_mb << 20:
            problems.append(f"bo_total_bytes {start['bo_total_bytes']} plus the sections {window_bytes} is not below the {cap_mb} MiB cap")
        if window_bytes > WINDOW_LIMIT:
            problems.append(f"the sections take {window_bytes / 2 ** 30:.2f} GiB in power-of-two classes, above {WINDOW_LIMIT / 2 ** 30:.1f} GiB (the 4 GiB window is not usable to its bound) "
                            "(the control alone is 3.13 GiB; the window cannot hold the control and a layout, use --only in two boots)")
        if only and only != CONTROL and (not control_ids or any(not isinstance(v, list) for v in control_ids.values())):
            problems.append("the control summary holds no token ids per prompt")
        if problems:
            print("REFUSE: " + "; ".join(problems) + " (use --unfrozen for a host test, which never exits 0)")
            return 2
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    ref = json.loads(Path(args.ref).read_text())
    prompts = ref["prompts"][:args.identity_prompts]
    timing_prompt = next(p for p in ref["prompts"] if p["id"] == args.prompt)
    dargs = argparse.Namespace(out=str(out), manifest=args.manifest, anec_dir=args.anec_dir, ports_dir=args.ports_dir, ane_run="unused", timeout=args.timeout,
                               resident=True, session_bin=args.session_bin, resident_lock="call", per_m_dir="/var/tmp/qwen-perM", session_lock=args.session_lock,
                               dump=None, band=None, resident_states=False)
    mark0 = max((t for t, _ in dec.kernel_marks()), default=0.0)   # before the load: the span covers the load too
    decoder = FoldDecoder(dargs, sets, run_arms[0])
    for name, anec, table in spares:
        got = decoder.session.load(name, anec, table)
        ports = dec.port_map_from_table(json.loads(Path(table).read_text())).values()
        want = (len([p for p in ports if p["direction"] == "input"]), len([p for p in ports if p["direction"] == "output"]))
        if got != want:
            raise dec.Refuse(f"{name}: session reports {got}, table holds {want}")
    loaded = rs.snapshot() if not args.unfrozen else {"bo_total_bytes": old_ab.bo_total_bytes()}
    embed, head, (dh, rotary, base) = dec.load_head(args.gguf)
    cos, sin = dec.rope_tables(decoder.max_len, dh, rotary, base)
    exch0 = start["exch_lines"] if not args.unfrozen else 0
    ident = []
    for prompt in prompts:
        d = out / f"identity-{prompt['id']}"
        entry = {"prompt": prompt["id"]}
        for arm in run_arms:
            (d / arm).mkdir(parents=True, exist_ok=True)
            entry[arm] = decode_run(decoder, arm, prompt, args.identity_tokens, cos, sin, embed, head, d / arm)
        if CONTROL in run_arms:
            entry["logits"] = {arm: logits_compare(d / CONTROL / "logits.f32", d / arm / "logits.f32") for arm in run_arms if arm != CONTROL}
            entry["control_matches_reference_ids"] = entry[CONTROL]["tokens"] == prompt["generated_ids"][:len(entry[CONTROL]["tokens"])]
            print(f"identity {prompt['id']}: ids equal {[(a, entry[a]['tokens'] == entry[CONTROL]['tokens']) for a in run_arms if a != CONTROL]}, "
                  f"logits bytes equal {[(a, entry['logits'][a]['bytes_equal']) for a in run_arms if a != CONTROL]}", flush=True)
        else:
            arm = run_arms[0]
            entry["logits_sha256_equals_control_boot"] = entry[arm]["logits_sha256"] == control_logits.get(prompt["id"])
            print(f"identity {prompt['id']}: ids equal control boot {entry[arm]['tokens'] == control_ids.get(prompt['id'])}, "
                  f"logits sha equal control boot {entry['logits_sha256_equals_control_boot']}", flush=True)
        ident.append(entry)
    records = []
    for i, arm in enumerate(order):
        time.sleep(args.idle)
        (out / f"arm{i + 1}-{arm}").mkdir(exist_ok=True)
        rec = decode_run(decoder, arm, timing_prompt, args.new_tokens, cos, sin, embed, head, out / f"arm{i + 1}-{arm}")
        records.append(rec)
        print(f"arm {i + 1} {arm}: step p50 {rec['step_p50_s']} s over {rec['steps']} steps, logits {str(rec['logits_sha256'])[:16]}", flush=True)
    decoder.close_session()
    end = rs.snapshot() if not args.unfrozen else {"exch_lines": 0}
    run = {"boot_id_pre": start["boot_id"], "bo_loaded": loaded["bo_total_bytes"],
           "kernel_stop": [line for t, line in dec.kernel_marks() if t > mark0 and dec.STOP_KERNEL.search(line)],
           "exch_new": end["exch_lines"] - exch0, "bo_start": start["bo_total_bytes"], "bo_cap_mb": cap_mb, "window_class_bytes": window_bytes}
    anchor = ANCHOR_TOKENS if not args.unfrozen else None
    f_ident, s_ident = judge_identity(ident, run_arms, anchor, args.identity_prompts, args.identity_tokens, control_ids)
    layouts = {CONTROL: {"n": len(manifest["programs"]), "mapping": sets[CONTROL]["mapping"]},
               **{k: {"n": len(layout_manifests[k]["programs"]), "mapping": sets[k]["mapping"]} for k in layout_ids}}
    f_time, s_time = judge_timing(records, run, layouts, order, args.new_tokens, anchor)
    failures = f_ident + f_time + (["unfrozen run: not a gate run"] if args.unfrozen else [])
    inputs = {"manifest": sha256_file(args.manifest), **{f"manifest-{i}": sha256_file(Path(args.layout_dir) / f"manifest-{i}.json") for i in layout_ids},
              "folded": {name: {"anec_sha256": sha256_file(a), "table_sha256": sha256_file(t)} for name, a, t in spares}}
    if only and only != CONTROL:
        inputs["control_summary_sha256"] = sha256_file(args.control_summary)
    summary = {"identity": s_ident, "timing": s_time, "timing_logits_equal_in_all_arms": len({r["logits_sha256"] for r in records}) == 1, "failures": failures,
               "frozen": not args.unfrozen, "run": run, "environment": env, "placement": {"pinned": PINNED}, "inputs": inputs, "order": order, "only": only,
               "identity_prompts": [{k: v for k, v in e.items() if k not in run_arms} | {f"{a}_logits_sha256": e[a]["logits_sha256"] for a in run_arms}
                                    | {f"{a}_ids": e[a]["tokens"] for a in run_arms} for e in ident],
               "arms": [{k: v for k, v in r.items() if k != "walls_s"} for r in records]}
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    print(json.dumps({"identity": s_ident, "speed": s_time, "failures": failures}, indent=1))
    return 0 if not failures else (3 if args.unfrozen else 1)


if __name__ == "__main__":
    sys.exit(main())
