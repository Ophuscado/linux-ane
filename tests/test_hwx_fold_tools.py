#!/usr/bin/env python3
"""Host checks, no device: the HWX to ANEC converter and the one-program port table on a real Apple H14 program,
and the fused-versus-chain MIL check on the staged Qwen fold pair.

The fixtures in tests/fixtures/hwx-fold are the DeltaNet state block (HWX, the ANEC the converter emitted for it,
its MIL) and the MIL of the readout/MLP/input-block program that follows it and of the single program that replaces
the pair. The compiler command line inside the HWX had a build path; it is replaced by a same-length placeholder
(the converter does not read it, and the test below proves the ANEC is unchanged)."""

import hashlib
import json
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from hwx_h14_staged_to_anec import convert
from hwx_ports import derive_program, main as ports_main
from mil_graph_equiv import MilError, compare, main as equiv_main, parse_mil

FIX = Path(__file__).resolve().parent / "fixtures" / "hwx-fold"
STAGED_HWX = Path("/var/tmp/qwen-real-hwx-h14")
STAGED_ANEC = Path("/var/tmp/qwen-real-anec-h14")

LANES = {
    "fused": dict(t16="q", t6="k", t9="v", t2="beta", t3="gt", t24="x", t0="z", t4="S", t37="conv"),
    "first": dict(t14="q", t4="k", t7="v", t0="beta", t1="gt", t2="S"),
    "second": dict(t7="x", t0="z", t20="conv"),
}
OUTS = {
    "fused": dict(t32="x", t48="q1", t52="k1", t54="v1", t58="beta1", t67="gt1", t69="z1", t15="S1", t70="conv1"),
    "first": dict(t13="S1", t17="o"),
    "second": dict(t15="x", t31="q1", t35="k1", t37="v1", t41="beta1", t50="gt1", t52="z1", t53="conv1"),
}
LINK = ("t2", "t17")


def programs(fused_text=None):
    return (parse_mil(fused_text or (FIX / "fused.mil").read_text()), parse_mil((FIX / "state-block.mil").read_text()),
            parse_mil((FIX / "readout-mlp-input.mil").read_text()))


def test_fixtures_match_their_sums():
    for line in (FIX / "SHA256SUMS").read_text().splitlines():
        digest, name = line.split()
        assert hashlib.sha256((FIX / name).read_bytes()).hexdigest() == digest, name


def test_converter_reproduces_the_known_anec_byte_for_byte():
    anec, info = convert((FIX / "state-block.hwx").read_bytes())
    assert anec == (FIX / "state-block.anec").read_bytes()
    assert hashlib.sha256(anec).hexdigest().startswith("a35bb0279d3e15fd")
    assert info


@pytest.mark.parametrize("prog", ["prog_001", "prog_002"])
def test_converter_reproduces_the_staged_programs(prog):
    """The converter on the staged Apple HWX gives exactly the ANEC the M2 runs (needs the staged inputs)."""
    hwx, anec = STAGED_HWX / prog / "model.hwx", STAGED_ANEC / prog / "program-0.anec"
    if not (hwx.is_file() and anec.is_file()):
        pytest.skip("staged Qwen HWX/ANEC are not on this host")
    assert convert(hwx.read_bytes())[0] == anec.read_bytes()


def test_one_program_table_has_the_ports_and_no_exceptions(tmp_path):
    table = derive_program("state_block", FIX / "state-block.hwx", FIX / "state-block.anec", FIX / "state-block.mil")
    assert table["schema_version"] == 2 and table["exceptions"] == []
    inputs = {p["name"]: p["shape"] for p in table["ports"] if p["direction"] == "input"}
    outputs = {p["name"]: p["shape"] for p in table["ports"] if p["direction"] == "output"}
    assert inputs == {"t0": [1, 16, 1, 1], "t1": [1, 16, 1, 1], "t14": [1, 1, 16, 128], "t2": [1, 16, 128, 128],
                      "t4": [1, 1, 16, 128], "t7": [1, 1, 16, 128]}
    assert outputs == {"t13": [1, 16, 128, 128], "t17": [1, 1, 16, 128]}
    assert sorted(len(a["ports"]) for a in table["ambiguities"]) == [2, 3]   # beta/gt and q/k/v input groups
    out = tmp_path / "ports.json"
    assert ports_main(["--program", "state_block", "--hwx", str(FIX / "state-block.hwx"),
                       "--anec", str(FIX / "state-block.anec"), "--mil", str(FIX / "state-block.mil"), "--out", str(out)]) == 0
    assert json.loads(out.read_text()) == table


def test_one_program_table_passes_the_runner_loader():
    pytest.importorskip("numpy")
    from qwen_prog_run import port_map_from_table
    table = derive_program("state_block", FIX / "state-block.hwx", FIX / "state-block.anec", FIX / "state-block.mil")
    assert len(port_map_from_table(table)) == 8


def test_fused_program_is_the_chain_of_the_pair():
    fused, first, second = programs()
    report = compare(fused, first, second, LINK, LANES, OUTS)
    assert report["equal"], report
    assert report["fused_nodes"] == report["chain_nodes"] == 136
    assert set(report["outputs"]) == {"x", "q1", "k1", "v1", "beta1", "gt1", "z1", "S1", "conv1"}


def test_swapped_input_lanes_are_caught():
    fused, first, second = programs()
    swapped = {**LANES, "fused": {**LANES["fused"], "t16": "k", "t6": "q"}}
    assert not compare(fused, first, second, LINK, swapped, OUTS)["equal"]


def test_a_changed_op_is_caught():
    text = (FIX / "fused.mil").read_text()
    assert "silu(" in text
    fused, first, second = programs(text.replace("silu(", "sigmoid(", 1))
    assert not compare(fused, first, second, LINK, LANES, OUTS)["equal"]


def _equal_after(replace_text):
    text = (FIX / "fused.mil").read_text()
    mutated = replace_text(text)
    assert mutated != text
    fused, first, second = programs(mutated)
    return compare(fused, first, second, LINK, LANES, OUTS)["equal"]


def test_inline_const_values_are_compared():
    """Slice begins, reshape shapes and scalar consts sit in the op attribute bracket; a fused program that slices the wrong head must not pass."""
    import re
    assert not _equal_after(lambda s: s.replace("([0, 2048])", "([0, 1024])", 1))
    assert not _equal_after(lambda s: s.replace("([16, 1, 128])", "([16, 128, 1])", 1))
    assert not _equal_after(lambda s: re.sub(r"(val = tensor<int32, \[\d+\]>\()\[[^\]]*\](\))", r"\g<1>[7]\g<2>", s))
    assert not _equal_after(lambda s: re.sub(r"(val = (?:fp16|int32|bool)\()[^)]*(\))", r"\g<1>5\g<2>", s, count=3))


def test_a_blob_offset_is_masked_but_the_rest_of_the_const_is_not():
    import re
    assert _equal_after(lambda s: re.sub(r"offset = uint64\(64\)", "offset = uint64(65)", s, count=1))


def test_a_wrong_link_is_refused_or_unequal():
    fused, first, second = programs()
    with pytest.raises(MilError):
        compare(fused, first, second, ("t2", "nope"), LANES, OUTS)
    assert not compare(fused, first, second, ("t0", "t17"), LANES, OUTS)["equal"]


def test_command_line_exit_codes(tmp_path, capsys):
    base = [str(FIX / "fused.mil"), str(FIX / "state-block.mil"), str(FIX / "readout-mlp-input.mil"), "--link", "t2=t17"]
    lanes = ["--lanes-fused", ",".join(f"{k}={v}" for k, v in LANES["fused"].items()),
             "--lanes-first", ",".join(f"{k}={v}" for k, v in LANES["first"].items()),
             "--lanes-second", ",".join(f"{k}={v}" for k, v in LANES["second"].items())]
    outs = ["--out-fused", ",".join(f"{k}={v}" for k, v in OUTS["fused"].items()),
            "--out-first", ",".join(f"{k}={v}" for k, v in OUTS["first"].items()),
            "--out-second", ",".join(f"{k}={v}" for k, v in OUTS["second"].items())]
    assert equiv_main(base + lanes + outs) == 0
    assert "EQUAL" in capsys.readouterr().out
    bad = tmp_path / "bad.mil"
    bad.write_text("not mil")
    assert equiv_main([str(bad)] + base[1:]) == 2
