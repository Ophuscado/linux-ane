#!/usr/bin/env python3
"""Host checks for tools/anec_compare.py. No device. The real package is the H14 state block fixture the converter
reproduces byte for byte; the others are built from its bytes."""

import hashlib
import struct
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from anec_compare import HEADER_BYTES, AnecError, compare, main, parse_anec

FIX = Path(__file__).resolve().parent / "fixtures" / "hwx-fold" / "state-block.anec"


def real() -> bytes:
    return FIX.read_bytes()


def rebuild(base: bytes, stream: bytes | None = None, constants: bytes | None = None, tasks: int | None = None) -> bytes:
    """The same package with a different stream, constant section or task count; the content size follows."""
    info = parse_anec(base)
    old_stream = base[HEADER_BYTES:HEADER_BYTES + info["stream_bytes"]]
    offset = (info["stream_bytes"] + 63) // 64 * 64
    old_constants = base[HEADER_BYTES + offset:HEADER_BYTES + offset + info["constant_bytes"]]
    stream = old_stream if stream is None else stream
    constants = old_constants if constants is None else constants
    new_offset = (len(stream) + 63) // 64 * 64
    header = bytearray(base[:HEADER_BYTES])
    struct.pack_into("<Q", header, 0, new_offset + len(constants))
    struct.pack_into("<I", header, 12, info["task_count"] if tasks is None else tasks)
    struct.pack_into("<QQ", header, 16, len(stream), len(constants))
    return bytes(header) + stream + bytes(new_offset - len(stream)) + constants


def test_the_real_package_parses_to_the_known_numbers():
    info = parse_anec(real())
    assert (info["task_count"], info["stream_bytes"], info["constant_bytes"], info["input_count"]) == (10, 1924, 16384, 6)
    assert info["version"] == 1
    assert hashlib.sha256(real()).hexdigest().startswith("a35bb0279d3e15fd")
    assert len(real()) == HEADER_BYTES + (1924 + 63) // 64 * 64 + 16384


def test_a_package_equals_itself_and_a_rebuild_with_the_same_parts():
    a = parse_anec(real())
    result = compare(a, parse_anec(rebuild(real())))
    assert result["counts_equal"] and result["stream_equal"] and result["constants_equal"] and result["tiles_equal"]


def test_a_changed_constant_byte_changes_only_the_constants():
    info = parse_anec(real())
    constants = bytearray(info["_constants"])
    constants[100] ^= 1
    result = compare(info, parse_anec(rebuild(real(), constants=bytes(constants))))
    assert result["stream_equal"] and result["counts_equal"] and not result["constants_equal"] and not result["constants_contained"]


def test_a_changed_stream_byte_changes_only_the_stream():
    stream = bytearray(real()[HEADER_BYTES:HEADER_BYTES + 1924])
    stream[200] ^= 1
    result = compare(parse_anec(real()), parse_anec(rebuild(real(), stream=bytes(stream))))
    assert not result["stream_equal"] and result["constants_equal"] and result["counts_equal"]


def test_a_fold_that_keeps_the_weights_and_adds_bytes_passes_contained_not_equal():
    info = parse_anec(real())
    bigger = rebuild(real(), constants=info["_constants"] + bytes(64), tasks=11)
    result = compare(info, parse_anec(bigger))
    assert result["constants_contained"] and not result["constants_equal"] and not result["counts_equal"]
    assert result["task_count"] == [10, 11]


def test_unreadable_packages_are_named():
    with pytest.raises(AnecError):
        parse_anec(real()[:100])
    with pytest.raises(AnecError):
        parse_anec(real()[:-8])                                         # shorter than the header describes
    damaged = bytearray(real())
    struct.pack_into("<Q", damaged, 0, 12345)
    with pytest.raises(AnecError):
        parse_anec(bytes(damaged))


def test_command_line_exit_codes(tmp_path, capsys):
    a, same, other, bad = (tmp_path / n for n in ("a.anec", "same.anec", "other.anec", "bad.anec"))
    a.write_bytes(real())
    same.write_bytes(rebuild(real()))
    constants = bytearray(parse_anec(real())["_constants"])
    constants[0] ^= 0xFF
    other.write_bytes(rebuild(real(), constants=bytes(constants)))
    bad.write_bytes(b"not an anec")
    assert main([str(a), str(same)]) == 0
    assert "constants_equal: True" in capsys.readouterr().out
    assert main([str(a), str(other)]) == 1
    assert main([str(a), str(other), "--constants-equal"]) == 1
    assert main([str(a), str(a), "--constants-equal"]) == 0
    assert main([str(a), str(bad)]) == 2
    assert main([str(a), str(tmp_path / "missing.anec")]) == 2
    assert main([str(a), str(same), "--json"]) == 0
    assert '"constants_equal": true' in capsys.readouterr().out


def patched(base: bytes, offset: int, fmt: str, value: int) -> bytes:
    data = bytearray(base)
    struct.pack_into(fmt, data, offset, value)
    return bytes(data)


def test_constants_equal_ignores_a_different_stream(tmp_path):
    """The fold case: the stream differs, the constants are the same bytes."""
    stream = bytearray(real()[HEADER_BYTES:HEADER_BYTES + 1924])
    stream[300] ^= 1
    a, b = tmp_path / "a.anec", tmp_path / "b.anec"
    a.write_bytes(real())
    b.write_bytes(rebuild(real(), stream=bytes(stream)))
    assert main([str(a), str(b), "--constants-equal"]) == 0
    assert main([str(a), str(b)]) == 1


def test_the_tile_table_is_compared_except_the_content_size_entry():
    info = parse_anec(real())
    assert info["tiles"][5] > 0
    changed = parse_anec(patched(real(), 0x28 + 5 * 4, "<I", info["tiles"][5] + 1))
    assert not compare(info, changed)["tiles_equal"]
    content_only = parse_anec(patched(real(), 0x28, "<I", info["tiles"][0] + 1))      # tiles[0] is the content size
    assert compare(info, content_only)["tiles_equal"]


def test_the_stream_length_and_the_first_task_size_are_part_of_the_counts():
    info = parse_anec(real())
    longer = parse_anec(rebuild(real(), stream=real()[HEADER_BYTES:HEADER_BYTES + 1924] + bytes(64)))
    result = compare(info, longer)
    assert not result["counts_equal"] and result["stream_bytes"] == [1924, 1988]
    first_task = parse_anec(patched(real(), 8, "<I", info["first_task_bytes"] + 16))
    assert not compare(info, first_task)["counts_equal"]
    assert compare(info, first_task)["stream_equal"]
