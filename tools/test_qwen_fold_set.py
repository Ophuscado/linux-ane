# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host tests for qwen_fold_layout.py and the judge and report functions of qwen_fold_set_ab.py. No device, no session."""
import json
import sys
from pathlib import Path

import pytest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import qwen_fold_layout as layout  # noqa: E402

LANES = ("q", "k", "v")


def ports(rows):
    return [{"port": p, "kind": k, "lane": lane, "shape": s} for p, k, lane, s in rows]


def control_manifest():
    """P0 input block, P1 state block, P2 readout plus next input block, P3 a plain program."""
    p0 = {"srcs": ports([("t1", "lane", "x", [1, 8]), ("t5", "state_in", "state0", [6, 3])]),
          "dsts": ports([("t16", "lane", "q", [4]), ("t20", "lane", "k", [4]), ("t22", "lane", "v", [4]),
                         ("t26", "lane", "beta", [4, 1, 1]), ("t35", "lane", "gt", [4, 1, 1]), ("t37", "lane", "z", [4])]),
          "states": [{"in_port": "t5", "in_shape": [6, 3], "out_port": "t38", "out_shape": [6, 3]}], "group_start": True, "group_end": False}
    p1 = {"srcs": ports([("t14", "lane", "q", [4]), ("t4", "lane", "k", [4]), ("t7", "lane", "v", [4]), ("t0", "lane", "beta", [4, 1, 1]),
                         ("t1", "lane", "gt", [4, 1, 1]), ("t2", "state_in", "state0", [4, 4, 4])]),
          "dsts": ports([("t17", "lane", "o", [4])]),
          "states": [{"in_port": "t2", "in_shape": [4, 4, 4], "out_port": "t13", "out_shape": [4, 4, 4]}], "group_start": False, "group_end": False}
    p2 = {"srcs": ports([("t7", "lane", "x", [1, 8]), ("t2", "lane", "o", [4]), ("t0", "lane", "z", [4]), ("t76", "state_in", "state0", [6, 3])]),
          "dsts": ports([("t71", "lane", "x", [1, 8]), ("t87", "lane", "q", [4]), ("t91", "lane", "k", [4]), ("t93", "lane", "v", [4]),
                         ("t97", "lane", "beta", [4, 1, 1]), ("t106", "lane", "gt", [4, 1, 1]), ("t108", "lane", "z", [4])]),
          "states": [{"in_port": "t76", "in_shape": [6, 3], "out_port": "t109", "out_shape": [6, 3]}], "group_start": False, "group_end": False}
    p3 = {"srcs": ports([("t3", "lane", "x", [1, 8])]), "dsts": ports([("t9", "lane", "x", [1, 8])]), "states": [],
          "group_start": False, "group_end": True}
    return {"max_len": 50, "n_programs": 4, "programs": [p0, p1, p2, p3], "host_head": {}, "aneforge_tip": "x"}


def pm_of(entry):
    pm = {"x": {}, "h": {}, "states": {}}
    for s in entry["srcs"]:
        if s["kind"] == "lane":
            pm["x"][s["lane"]] = s["port"]
        else:
            pm["states"][s["port"]] = s["shape"]
    for d in entry["dsts"]:
        pm["h"][d["lane"]] = d["port"]
    if entry["group_start"]:
        pm["group_start"] = True
    if entry["group_end"]:
        pm["group_end"] = True
    return pm


def mil_of(entry, tag):
    shape = lambda s: ", ".join(str(x) for x in s)  # noqa: E731
    args = ", ".join(f"tensor<fp16, [{shape(s['shape'])}]> {s['port']}" for s in entry["srcs"])
    body = "".join(f"        tensor<fp16, [{shape(d['shape'])}]> {d['port']} = identity(x = {entry['srcs'][0]['port']})[name = string(\"{d['port']}\")];\n"
                   for d in entry["dsts"])
    outs = [d["port"] for d in entry["dsts"]] + [s["out_port"] for s in entry["states"]]
    body += "".join(f"        tensor<fp16, [{shape(s['out_shape'])}]> {s['out_port']} = identity(x = {entry['srcs'][0]['port']})[name = string(\"{s['out_port']}\")];\n"
                    for s in entry["states"])
    return f"program(1.3)\n{{\n    func main<ios18>({args}) {{\n{body}    }} -> ({', '.join(outs)});\n}}\n", tag


def write_record(directory, manifest, mil_tags=None):
    """probe.out with PORTMAP lines and mil/NN-<tag>.mil for each program of a manifest."""
    (directory / "mil").mkdir(parents=True)
    lines = []
    for i, e in enumerate(manifest["programs"]):
        text, tag = mil_of(e, (mil_tags or {}).get(i, f"{i:012x}"))
        (directory / "mil" / f"{i:02d}-{tag}.mil").write_text(text)
        lines.append(f"PORTMAP {i} {json.dumps(pm_of(e))}")
    (directory / "probe.out").write_text("\n".join(lines) + "\n")


def folded_manifest(control):
    """P1 and P2 joined: the state block's output o stays inside, its state and the next program's state are both inputs."""
    p1, p2 = control["programs"][1], control["programs"][2]
    f = {"srcs": ports([("a24", "lane", "x", [1, 8]), ("a0", "lane", "z", [4]), ("a16", "lane", "q", [4]), ("a6", "lane", "k", [4]), ("a9", "lane", "v", [4]),
                        ("a2", "lane", "beta", [4, 1, 1]), ("a3", "lane", "gt", [4, 1, 1]), ("a4", "state_in", "state0", [4, 4, 4]),
                        ("a37", "state_in", "state1", [6, 3])]),
         "dsts": json.loads(json.dumps(p2["dsts"])),
         "states": [{"in_port": "a4", "in_shape": [4, 4, 4], "out_port": "a15", "out_shape": [4, 4, 4]},
                    {"in_port": "a37", "in_shape": [6, 3], "out_port": "a70", "out_shape": [6, 3]}], "group_start": False, "group_end": False}
    assert len(f["states"]) == len(p1["states"]) + len(p2["states"])
    m = json.loads(json.dumps(control))
    m["programs"] = [control["programs"][0], f, control["programs"][3]]
    m["n_programs"] = 3
    return m


@pytest.fixture()
def record(tmp_path):
    control = control_manifest()
    folded = folded_manifest(control)
    write_record(tmp_path / "control", control)
    tags = {0: "000000000000", 2: "000000000003"}
    write_record(tmp_path / "F00", folded, {**tags, 1: "00000000ffff"})
    return tmp_path, control, folded


def test_self_check_rebuilds_every_control_entry(record):
    root, control, _ = record
    assert layout.self_check(control, root / "control") == []


def test_fold_builds_and_maps(record):
    root, control, folded = record
    manifest, mapping, problems = layout.build(control, root / "control", root / "F00", equiv=False)
    assert problems == []
    assert mapping == [[0], [1, 2], [3]]
    assert manifest["n_programs"] == 3 and manifest["fold_mapping"] == mapping
    assert layout.norm(manifest["programs"][1]) == layout.norm(folded["programs"][1])
    assert manifest["programs"][0] == control["programs"][0] and manifest["programs"][2] == control["programs"][3]


def test_build_runs_the_chain_check_on_every_folded_program(record):
    root, control, _ = record
    # the synthetic fold is not a real chain of the synthetic programs, so the default build must refuse it
    _, _, problems = layout.build(control, root / "control", root / "F00")
    assert any("not the chain" in p for p in problems)


def test_unfolded_program_with_other_mil_is_refused(record):
    root, control, _ = record
    (root / "F00" / "mil" / "00-000000000000.mil").rename(root / "F00" / "mil" / "00-0000000000aa.mil")
    _, _, problems = layout.build(control, root / "control", root / "F00", equiv=False)
    assert any("is not control 0's" in p for p in problems)


def test_folded_output_lane_with_wrong_shape_is_refused(record):
    root, control, folded = record
    folded["programs"][1]["dsts"][1]["shape"] = [5]
    write_record(root / "F01", folded, {0: "000000000000", 1: "00000000ffff", 2: "000000000003"})
    _, _, problems = layout.build(control, root / "control", root / "F01", equiv=False)
    assert any("output lane" in p for p in problems)


def test_swapped_state_pairs_are_caught_by_shape(record):
    root, control, folded = record
    folded["programs"][1]["states"][0]["out_port"], folded["programs"][1]["states"][1]["out_port"] = "a70", "a15"
    write_record(root / "F02", folded, {0: "000000000000", 1: "00000000ffff", 2: "000000000003"})
    manifest, _, problems = layout.build(control, root / "control", root / "F02", equiv=False)
    # the MIL declares the outputs in its own order; the builder pairs by shape, so both pairs stay shape-consistent
    assert problems == []
    assert {(s["in_shape"] == s["out_shape"]) for s in manifest["programs"][1]["states"]} == {True}


def test_fold_that_drops_a_state_block_input_is_refused(record):
    root, control, folded = record
    folded["programs"][1]["srcs"] = [s for s in folded["programs"][1]["srcs"] if s["lane"] != "beta"]
    write_record(root / "F03", folded, {0: "000000000000", 1: "00000000ffff", 2: "000000000003"})
    _, _, problems = layout.build(control, root / "control", root / "F03", equiv=False)
    assert any("state block" in p and "beta" in p for p in problems)


def test_the_manifest_records_whether_the_chain_check_ran(record):
    root, control, _ = record
    manifest, _, _ = layout.build(control, root / "control", root / "F00", equiv=False)
    assert manifest["equiv_checked"] is False


def test_a_fold_with_the_wrong_number_of_states_is_refused(record):
    root, control, folded = record
    f = folded["programs"][1]
    f["srcs"] = [s for s in f["srcs"] if s["port"] != "a37"]
    f["states"] = [s for s in f["states"] if s["in_port"] != "a37"]
    write_record(root / "F04", folded, {0: "000000000000", 1: "00000000ffff", 2: "000000000003"})
    _, _, problems = layout.build(control, root / "control", root / "F04", equiv=False)
    assert any("states, control" in p for p in problems)


def test_a_pair_without_an_o_lane_cannot_be_linked():
    first, second, fused = FIX / "state-block.mil", FIX / "readout-mlp-input.mil", FIX / "fused.mil"
    no_o = json.loads(json.dumps(PM_SECOND))
    del no_o["x"]["o"]
    equal, text = layout.chain_equivalent((PM_FUSED, fused), (PM_FIRST, first), (no_o, second))
    assert equal is False and "no o lane" in text


def test_a_layout_that_leaves_control_programs_over_is_refused(record):
    root, control, _ = record
    (root / "F00" / "probe.out").write_text("\n".join(line for line in (root / "F00" / "probe.out").read_text().splitlines() if not line.startswith("PORTMAP 2")) + "\n")
    (root / "F00" / "mil" / "02-000000000003.mil").rename(root / "F00" / "mil" / "xx-000000000003.mil.bak")
    _, _, problems = layout.build(control, root / "control", root / "F00", equiv=False)
    assert any("consumes" in p for p in problems)


FIX = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "hwx-fold"
# The real layer-0 programs: the state block, the readout plus MLP plus next input block, and the fold of the two.
PM_FIRST = {"h": {"o": "t17"}, "states": {"t2": [16, 128, 128]}, "x": {"beta": "t0", "gt": "t1", "k": "t4", "q": "t14", "v": "t7"}}
PM_SECOND = {"h": {"beta": "t41", "gt": "t50", "k": "t35", "q": "t31", "v": "t37", "x": "t15", "z": "t52"}, "states": {"t20": [6144, 3]},
             "x": {"o": "t2", "x": "t7", "z": "t0"}}
PM_FUSED = {"h": {"beta": "t58", "gt": "t67", "k": "t52", "q": "t48", "v": "t54", "x": "t32", "z": "t69"}, "states": {"t37": [6144, 3], "t4": [16, 128, 128]},
            "x": {"beta": "t2", "gt": "t3", "k": "t6", "q": "t16", "v": "t9", "x": "t24", "z": "t0"}}


def test_the_real_fold_is_the_chain_and_a_changed_op_is_not(tmp_path):
    first, second, fused = FIX / "state-block.mil", FIX / "readout-mlp-input.mil", FIX / "fused.mil"
    assert layout.chain_equivalent((PM_FUSED, fused), (PM_FIRST, first), (PM_SECOND, second))[0] is True
    text = fused.read_text()
    assert "silu(" in text
    changed = tmp_path / "changed.mil"
    changed.write_text(text.replace("silu(", "sigmoid(", 1))
    assert layout.chain_equivalent((PM_FUSED, changed), (PM_FIRST, first), (PM_SECOND, second))[0] is False


def test_a_folded_program_that_swaps_two_lanes_is_not_the_chain():
    first, second, fused = FIX / "state-block.mil", FIX / "readout-mlp-input.mil", FIX / "fused.mil"
    swapped = json.loads(json.dumps(PM_FUSED))
    swapped["x"]["q"], swapped["x"]["k"] = swapped["x"]["k"], swapped["x"]["q"]
    assert layout.chain_equivalent((swapped, fused), (PM_FIRST, first), (PM_SECOND, second))[0] is False


np = pytest.importorskip("numpy")
ab = pytest.importorskip("qwen_fold_set_ab")


def arm_record(arm, programs, step_p50, exec_ms, steps=4, tokens=None):
    return {"arm": arm, "tokens": tokens or list(range(steps)), "steps": steps, "logits_sha256": "a" * 64, "logits_bytes": 4 * steps * 3,
            "step_p10_s": step_p50, "step_p50_s": step_p50, "step_p90_s": step_p50, "walls_s": [step_p50] * steps,
            "exec_ms_per_program_median": {f"prog_{i:03d}": exec_ms.get(i, 1.0) for i in range(programs)}, "kernel_stop": [],
            "boot_id_start": "b", "boot_id_end": "b", "bo_total_before": 7, "bo_total_after": 7, "exch_new": 0}


LAYOUTS = {"A": {"n": 4, "mapping": [[0], [1], [2], [3]]}, "F": {"n": 3, "mapping": [[0], [1, 2], [3]]}}
RUN = {"boot_id_pre": "b", "bo_loaded": 7, "kernel_stop": [], "exch_new": 0}


def run_order(order, p50, exec_f=0.8):
    recs = []
    for arm in order:
        n = 4 if arm == "A" else 3
        ex = {1: 1.0, 2: 1.0} if arm == "A" else {1: exec_f}
        recs.append(arm_record(arm, n, p50[arm].pop(0), ex))
    return recs


def test_saving_inside_the_control_spread_is_not_counted():
    recs = run_order(["A", "F", "A", "F"], {"A": [0.170, 0.175], "F": [0.169, 0.172]})
    failures, report = ab.judge_timing(recs, RUN, LAYOUTS, ["A", "F", "A", "F"], 4, anchor=None)
    assert failures == []
    assert report["layouts"]["F"]["outside_control_spread"] is False
    assert report["layouts"]["F"]["exec_saved_per_fold"][0]["saved_ms"] == pytest.approx(1.2)


def test_saving_beyond_the_control_spread_is_counted():
    recs = run_order(["A", "F", "A", "F"], {"A": [0.170, 0.171], "F": [0.150, 0.151]})
    failures, report = ab.judge_timing(recs, RUN, LAYOUTS, ["A", "F", "A", "F"], 4, anchor=None)
    assert failures == [] and report["layouts"]["F"]["outside_control_spread"] is True
    assert report["layouts"]["F"]["median_ratio"] < 0.9


def test_boot_change_bo_growth_and_missing_program_fail():
    order = ["A", "F"]
    recs = run_order(order, {"A": [0.17], "F": [0.17]})
    recs[1]["boot_id_end"] = "other"
    assert any("boot ids" in f for f in ab.judge_timing(recs, RUN, LAYOUTS, order, 4, anchor=None)[0])
    recs = run_order(order, {"A": [0.17], "F": [0.17]})
    recs[1]["bo_total_after"] = 9
    assert any("zero growth" in f for f in ab.judge_timing(recs, RUN, LAYOUTS, order, 4, anchor=None)[0])
    recs = run_order(order, {"A": [0.17], "F": [0.17]})
    del recs[1]["exec_ms_per_program_median"]["prog_002"]
    assert any("exec medians cover" in f for f in ab.judge_timing(recs, RUN, LAYOUTS, order, 4, anchor=None)[0])


def test_identity_needs_every_arm_equal_to_the_control():
    def rec(tokens):
        return {"tokens": tokens, "kernel_stop": [], "exch_new": 0}
    good = [{"prompt": f"p{i}", "A": rec(list(range(3))), "F": rec(list(range(3)))} for i in range(2)]
    bad, summary = ab.judge_identity(good, ["A", "F"], anchor=None, prompts=2, tokens=3)
    assert bad == [] and summary["prompts_with_ids_equal_to_control"] == {"F": 2}
    good[1]["F"] = rec([0, 1, 9])
    bad, _ = ab.judge_identity(good, ["A", "F"], anchor=None, prompts=2, tokens=3)
    assert any("differ" in b for b in bad)


def test_anchor_ids_gate_the_control():
    def rec(tokens):
        return {"tokens": tokens, "kernel_stop": [], "exch_new": 0}
    ident = [{"prompt": "p001", "A": rec([5, 6, 7]), "F": rec([5, 6, 7])}]
    assert ab.judge_identity(ident, ["A", "F"], anchor=[5, 6], prompts=1, tokens=3)[0] == []
    bad, _ = ab.judge_identity(ident, ["A", "F"], anchor=[5, 9], prompts=1, tokens=3)
    assert any("anchor" in b for b in bad)


def test_a_custom_order_must_be_complete_rounds():
    assert ab.check_order(["A", "S", "F", "A", "F", "S"], ["S", "F"]) is None
    assert ab.check_order(["A"], ["F"]) is not None                      # a layout never timed
    assert ab.check_order(["F", "A"], ["F"]) is not None                 # a layout before the first control
    assert ab.check_order(["A", "S", "A", "S", "F"], ["S", "F"]) is not None
    assert ab.check_order(["A", "S", "S", "F"], ["S", "F"]) is not None


def test_default_order_alternates_the_layouts_between_rounds():
    assert ab.default_order(["S", "F"], 3) == ["A", "S", "F", "A", "F", "S", "A", "S", "F"]


def test_build_sets_reads_the_control_files_for_unfolded_programs(tmp_path):
    control = {"programs": [{}, {}, {}, {}]}
    layouts = {"F": {"programs": [{}, {}, {}], "fold_mapping": [[0], [1, 2], [3]]}}
    sets = ab.build_sets(control, layouts, tmp_path / "anec", tmp_path / "ports", tmp_path / "fold")
    files = sets["F"]["files"]
    assert files[0] == sets["A"]["files"][0] and files[2] == sets["A"]["files"][3]
    assert files[1][0] == tmp_path / "fold" / "F" / "prog01" / "program-0.anec" and files[1][2] == "prog_001_fold"


# ---- the window gate and the one-arm mode (sizes are the real program sections of the control and of the 20 program layout)
CTL_SIZES = [34098240, 22464, 117994432, 22464, 117994432, 22464, 223006144, 22464, 117994432, 22464, 117994432, 22464, 223006144, 22464, 117994432, 22464,
             117994432, 22464, 223006144, 22464, 83900608, 34098240, 22464, 117994432, 22464, 223006144, 22464, 117994432, 22464, 117994432, 22464, 223006144,
             22464, 117994432, 22464, 117994432, 22464, 188917120]
S18_MAPPING = [[0], [1, 2], [3, 4], [5, 6], [7, 8], [9, 10], [11, 12], [13, 14], [15, 16], [17, 18], [19, 20], [21], [22, 23], [24, 25], [26, 27], [28, 29],
               [30, 31], [32, 33], [34, 35], [36, 37]]
S18_FOLD = {1: 117996160, 2: 117996160, 3: 223007872, 4: 117996160, 5: 117996160, 6: 223007872, 7: 117996160, 8: 117996160, 9: 223007872, 10: 83902336,
            12: 117996160, 13: 223007872, 14: 117996160, 15: 117996160, 16: 223007872, 17: 117996160, 18: 117996160, 19: 188918848}


class Loaded(Exception):
    pass


def sparse(path, size):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        f.truncate(size)


@pytest.fixture()
def window_world(tmp_path, monkeypatch):
    for i, size in enumerate(CTL_SIZES):
        sparse(tmp_path / "anec" / f"prog_{i:03d}" / "program-0.anec", size)
        (tmp_path / "ports" / f"prog_{i:03d}").mkdir(parents=True)
        (tmp_path / "ports" / f"prog_{i:03d}" / "ports.resolved.json").write_text("{}")
    for i, size in S18_FOLD.items():
        sparse(tmp_path / "fold" / "S18" / f"prog{i:02d}" / "program-0.anec", size)
        (tmp_path / "fold" / "S18" / f"prog{i:02d}" / "ports.resolved.json").write_text("{}")
    (tmp_path / "manifest.json").write_text(json.dumps({"max_len": 50, "programs": [{}] * 38}))
    (tmp_path / "layouts").mkdir()
    (tmp_path / "layouts" / "manifest-S18.json").write_text(json.dumps({"max_len": 50, "programs": [{}] * 20, "fold_mapping": S18_MAPPING,
                                                                        "equiv_checked": True}))
    (tmp_path / "ref.json").write_text(json.dumps({"prompts": [{"id": "p001", "prompt_token_ids": [1], "generated_ids": []}]}))
    (tmp_path / "control-summary.json").write_text(json.dumps({"identity_prompts": [{"prompt": "p001", "A_ids": [1], "A_logits_sha256": "a" * 64}]}))
    monkeypatch.setattr(ab.rs, "environment", lambda: {"reference_blas": False, "blas": [{"path": "x"}]})
    monkeypatch.setattr(ab.rs, "snapshot", lambda: {"boot_id": "b", "bo_total_bytes": 0, "exch_lines": 0})
    monkeypatch.setattr(ab.rs, "read_cap_mb", lambda: 12288)
    monkeypatch.setattr(ab.dec, "kernel_marks", lambda: [])

    def no_decoder(*a, **k):
        raise Loaded()
    monkeypatch.setattr(ab, "FoldDecoder", no_decoder)

    def args(*extra):
        return ["--layout-dir", str(tmp_path / "layouts"), "--fold-root", str(tmp_path / "fold"), "--manifest", str(tmp_path / "manifest.json"),
                "--anec-dir", str(tmp_path / "anec"), "--ports-dir", str(tmp_path / "ports"), "--ref", str(tmp_path / "ref.json"),
                "--out", str(tmp_path / "out"), *extra]
    return tmp_path, args


def test_size_classes_are_powers_of_two():
    assert ab.class_bytes(117994432) == 128 << 20 and ab.class_bytes(223006144) == 256 << 20 and ab.class_bytes(22464) == 32768
    control = sum(ab.class_bytes(s) for s in CTL_SIZES)
    layout = sum(ab.class_bytes(S18_FOLD[i]) if i in S18_FOLD else ab.class_bytes(CTL_SIZES[S18_MAPPING[i][0]]) for i in range(20))
    assert control < ab.WINDOW_LIMIT and layout < ab.WINDOW_LIMIT          # each alone fits
    assert control + sum(ab.class_bytes(s) for s in S18_FOLD.values()) > ab.WINDOW_LIMIT   # together they cannot


def test_control_and_layout_together_are_refused_before_any_load(window_world, capsys):
    _, args = window_world
    assert ab.main(args("--layouts", "S18")) == 2          # a Loaded exception would escape if the gate let it through
    assert "power-of-two classes" in capsys.readouterr().out


def test_the_window_gate_is_the_reason(window_world, monkeypatch, capsys):
    _, args = window_world
    monkeypatch.setattr(ab, "WINDOW_LIMIT", 1 << 40)
    with pytest.raises(Loaded):
        ab.main(args("--layouts", "S18"))                  # with the window limit lifted the same run reaches the decoder


def test_the_control_alone_and_the_layout_alone_pass_the_gate(window_world):
    _, args = window_world
    with pytest.raises(Loaded):
        ab.main(args("--only", "A"))
    with pytest.raises(Loaded):
        ab.main(args("--layouts", "S18", "--only", "S18", "--control-summary", str(window_world[0] / "control-summary.json")))


def test_a_bad_order_is_refused_by_main(window_world, monkeypatch, capsys):
    _, args = window_world
    monkeypatch.setattr(ab, "WINDOW_LIMIT", 1 << 40)          # so the order is the only thing that can refuse
    assert ab.main(args("--layouts", "S18", "--order", "A")) == 2
    assert "followed by every declared layout" in capsys.readouterr().out
    assert ab.main(args("--layouts", "S18", "--order", "S18,A")) == 2


def test_a_manifest_built_without_the_chain_check_is_refused(window_world, capsys):
    root, args = window_world
    path = root / "layouts" / "manifest-S18.json"
    manifest = json.loads(path.read_text())
    manifest["equiv_checked"] = False
    path.write_text(json.dumps(manifest))
    assert ab.main(args("--layouts", "S18", "--only", "S18", "--control-summary", str(root / "control-summary.json"))) == 2
    assert "equiv_checked" in capsys.readouterr().out


def test_a_layout_alone_needs_the_control_summary(window_world, capsys):
    _, args = window_world
    assert ab.main(args("--layouts", "S18", "--only", "S18")) == 2
    assert "control-summary" in capsys.readouterr().out


def test_identity_against_the_control_boot():
    def rec(tokens):
        return {"tokens": tokens, "kernel_stop": [], "exch_new": 0}
    ident = [{"prompt": "p001", "S": rec([1, 2, 3])}]
    bad, summary = ab.judge_identity(ident, ["S"], anchor=None, prompts=1, tokens=3, control_ids={"p001": [1, 2, 3]})
    assert bad == [] and summary["prompts_with_ids_equal_to_control"] == {"S": 1}
    bad, _ = ab.judge_identity(ident, ["S"], anchor=None, prompts=1, tokens=3, control_ids={"p001": [1, 2, 4]})
    assert any("differ" in b for b in bad)
    assert ab.judge_identity(ident, ["S"], anchor=None, prompts=1, tokens=3, control_ids=None)[0]


def test_compare_exec_saved_per_fold_and_boot_comparability():
    import qwen_fold_set_compare as cmp
    mapping = [[0], [1, 2], [3]]
    manifest = {"fold_mapping": mapping}

    def summary(arm, med, rng, p50, spread):
        return {"timing": {"arm": arm, "exec_ms_median": med, "exec_ms_range": rng, "step_p50_s": p50, "spread": spread}, "failures": [], "identity": {}}
    control = summary("A", {"prog_000": 2.0, "prog_001": 3.0, "prog_002": 4.0, "prog_003": 1.0}, {k: [v - 0.01, v + 0.01] for k, v in
                      {"prog_000": 2.0, "prog_001": 3.0, "prog_002": 4.0, "prog_003": 1.0}.items()}, [0.170, 0.171, 0.172], 0.012)
    layout = summary("S", {"prog_000": 2.0, "prog_001": 6.7, "prog_002": 1.0}, {k: [v - 0.01, v + 0.01] for k, v in
                     {"prog_000": 2.0, "prog_001": 6.7, "prog_002": 1.0}.items()}, [0.169, 0.170, 0.171], 0.012)
    report = cmp.compare(control, layout, manifest)
    assert report["exec"]["folds"] == 1 and report["exec"]["per_fold"][0]["saved_ms"] == pytest.approx(0.3)
    assert report["exec"]["outside_noise"] is True and report["exec"]["boots_comparable"] is True
    assert report["step"]["outside_both_spreads"] is False
    layout["timing"]["exec_ms_median"]["prog_002"] = 1.2          # a shared program moved 20 percent between the boots
    assert cmp.compare(control, layout, manifest)["exec"]["boots_comparable"] is False
    layout["timing"]["exec_ms_median"]["prog_002"] = 1.0
    layout["timing"]["exec_ms_median"]["prog_001"] = 7.0          # saved 0.0, inside the noise
    assert cmp.compare(control, layout, manifest)["exec"]["outside_noise"] is False


# ---- w7K's survivors: classes against nominal bytes, one count per path, the load order, the compare checks, --only with --order
def test_the_class_rule_refuses_the_calibration_set_that_nominal_bytes_would_pass(tmp_path):
    def make(name, size):
        p = tmp_path / name
        sparse(p, size)
        return p
    control = [make(f"c{i}", s) for i, s in enumerate(CTL_SIZES)]
    spares = [make("s0", 117996160), make("s1", 117996160)]
    big = make("s2", 223007872)
    assert ab.window_class_bytes(control + spares) < ab.WINDOW_LIMIT          # loaded on the device (3.38 GiB in classes)
    with_big = control + spares + [big]
    assert sum(p.stat().st_size for p in with_big) < ab.WINDOW_LIMIT           # 3.03 GiB nominal: the nominal rule would let it through
    assert ab.window_class_bytes(with_big) > ab.WINDOW_LIMIT                   # 3.63 GiB in classes: refused (the load failed here)


def test_a_file_shared_by_two_programs_is_counted_once(tmp_path):
    big = tmp_path / "anec" / "prog_000" / "program-0.anec"
    sparse(big, 3 << 29)                                                      # 1.5 GiB, a 2 GiB class
    control = {"programs": [{}]}
    layouts = {"L": {"programs": [{}, {}], "fold_mapping": [[0], [0]]}}       # two layout programs, one file
    sets = ab.build_sets(control, layouts, tmp_path / "anec", tmp_path / "ports", tmp_path / "fold")
    paths = ab.loaded_paths(sets, ["L"], [])
    assert len(paths) == 1
    assert ab.window_class_bytes(paths.values()) == 2 << 30 < 3 << 30


def test_sections_load_largest_first(tmp_path, monkeypatch):
    sizes = [10, 300, 20]
    files = []
    for i, size in enumerate(sizes):
        a, t = tmp_path / f"a{i}", tmp_path / f"t{i}.json"
        sparse(a, size)
        t.write_text(json.dumps({"program": f"prog_{i:03d}", "exceptions": []}))
        files.append((a, t, f"prog_{i:03d}"))
    manifest = {"max_len": 50, "programs": [{"srcs": [{"port": "x", "shape": [1]}], "dsts": [], "states": []}] * 3}

    class Session:
        names = ["prog_000"]
        calls = []

        def free(self, name):
            pass

        def load(self, name, anec, table):
            Session.calls.append(name)
            return (1, 0)
    monkeypatch.setattr(ab.dec, "manifest_at", lambda m, max_len: m)
    monkeypatch.setattr(ab.dec, "port_map_from_table", lambda table: {"x": {"direction": "input", "shape": [1]}})
    monkeypatch.setattr(ab.FoldDecoder, "reset", lambda self: None)
    d = object.__new__(ab.FoldDecoder)
    d.arm, d.sets, d.session = "A", {"A": {"manifest": manifest, "files": files, "mapping": [[0], [1], [2]]}}, Session()
    d.configure(50)
    assert Session.calls == ["prog_001", "prog_002", "prog_000"]


def test_compare_refuses_summaries_that_do_not_belong_together(tmp_path):
    import hashlib

    import qwen_fold_set_compare as cmp
    manifest = {"fold_mapping": [[0], [1, 2], [3]]}
    prog = {f"prog_{i:03d}": 1.0 for i in range(4)}
    rng = {k: [0.9, 1.1] for k in prog}
    control = {"frozen": True, "failures": [], "identity": {}, "timing": {"arm": "A", "exec_ms_median": prog, "exec_ms_range": rng, "step_p50_s": [0.17], "spread": None}}
    layout_t = {"arm": "S", "exec_ms_median": {k: 1.0 for k in list(prog)[:3]}, "exec_ms_range": {k: [0.9, 1.1] for k in list(prog)[:3]}, "step_p50_s": [0.17], "spread": None}
    c_path = tmp_path / "control.json"
    c_path.write_text(json.dumps(control))
    sha = hashlib.sha256(c_path.read_bytes()).hexdigest()
    layout = {"frozen": True, "failures": [], "identity": {}, "timing": layout_t, "inputs": {"control_summary_sha256": sha}}
    m_path = tmp_path / "manifest.json"
    m_path.write_text(json.dumps(manifest))

    def run(layout_summary, control_summary=None):
        l_path = tmp_path / "layout.json"
        l_path.write_text(json.dumps(layout_summary))
        if control_summary is not None:
            c_path.write_text(json.dumps(control_summary))
        return cmp.main([str(c_path), str(l_path), str(m_path)])
    assert run(layout) == 0
    assert run({**layout, "inputs": {"control_summary_sha256": "0" * 64}}) == 2           # made against another control summary
    assert run({**layout, "frozen": False}) == 2
    assert run({**layout, "failures": ["identity: x"]}) == 2
    c_path.write_text(json.dumps({**control, "frozen": False}))
    sha2 = hashlib.sha256(c_path.read_bytes()).hexdigest()
    assert run({**layout, "inputs": {"control_summary_sha256": sha2}}) == 2               # the hash matches, the control is not a frozen run


def test_only_with_order_is_refused(window_world, capsys):
    _, args = window_world
    assert ab.main(args("--only", "A", "--order", "A,A")) == 2
    assert "--order does not apply" in capsys.readouterr().out
