#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Compare two H14 ANEC packages section by section, on the host.

An ANEC package is a 0x1000-byte header, the task stream, and the constant section (layout in
`hwx_h14_staged_to_anec.py`). When a program is rebuilt or two programs are folded into one, this shows what changed:
the header counts, the task stream, the constants, and the per-channel tile sizes. It is the check that
`mil_graph_equiv.py` leaves to this tool: that graph check does not compare constant bytes, and this one does.

For each package it prints the task count, the stream and constant sizes with the sha256 of each, the input count and the
channels that carry tiles. It then says whether the header counts, the stream, the constants and the tile table are equal.
`constants contained` says whether the first package's constant section occurs as one contiguous run in the second
(a fold that keeps its weights and adds a few bytes passes it). Nothing is run on a device and nothing is written.

usage: anec_compare.py A.anec B.anec [--json] [--constants-equal]

Exit 0 when the packages are equal in every section (with --constants-equal: when only the constants are required equal),
1 when they differ, 2 when a file is not a readable ANEC package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

HEADER_BYTES = 0x1000
CHANNEL_COUNT = 32
TILE_TABLE_OFFSET = 0x28


class AnecError(Exception):
    pass


def parse_anec(data: bytes) -> dict:
    """Header fields, the stream and the constant section of one package."""
    if len(data) < HEADER_BYTES:
        raise AnecError(f"{len(data)} bytes is shorter than the {HEADER_BYTES}-byte header")
    content, first_task, task_count, stream_size, const_size, input_count, version = struct.unpack_from("<QIIQQII", data, 0)
    tiles = list(struct.unpack_from(f"<{CHANNEL_COUNT}I", data, TILE_TABLE_OFFSET))
    constant_offset = (stream_size + 63) // 64 * 64
    if content != constant_offset + const_size:
        raise AnecError(f"content size {content} differs from align64(stream) + constants = {constant_offset + const_size}")
    if len(data) < HEADER_BYTES + constant_offset + const_size:
        raise AnecError(f"file holds {len(data)} bytes, the header describes {HEADER_BYTES + constant_offset + const_size}")
    stream = data[HEADER_BYTES:HEADER_BYTES + stream_size]
    constants = data[HEADER_BYTES + constant_offset:HEADER_BYTES + constant_offset + const_size]
    return {"version": version, "task_count": task_count, "first_task_bytes": first_task, "input_count": input_count,
            "stream_bytes": stream_size, "constant_bytes": const_size, "tiles": tiles,
            "stream_sha256": hashlib.sha256(stream).hexdigest(), "constants_sha256": hashlib.sha256(constants).hexdigest(),
            "_constants": constants}


def compare(a: dict, b: dict) -> dict:
    counts = ("version", "task_count", "first_task_bytes", "input_count", "stream_bytes", "constant_bytes")
    return {
        "counts_equal": all(a[k] == b[k] for k in counts),
        "stream_equal": a["stream_sha256"] == b["stream_sha256"],
        "constants_equal": a["constants_sha256"] == b["constants_sha256"],
        "constants_contained": a["_constants"] in b["_constants"],
        "tiles_equal": a["tiles"][1:] == b["tiles"][1:],         # tiles[0] is the content size in tiles
        "task_count": [a["task_count"], b["task_count"]],
        "stream_bytes": [a["stream_bytes"], b["stream_bytes"]],
        "constant_bytes": [a["constant_bytes"], b["constant_bytes"]],
    }


def public(info: dict) -> dict:
    return {k: v for k, v in info.items() if not k.startswith("_")} | {
        "channels_with_tiles": [c for c, n in enumerate(info["tiles"]) if c and n]}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").splitlines()[0])
    parser.add_argument("a")
    parser.add_argument("b")
    parser.add_argument("--json", action="store_true", help="print one JSON object")
    parser.add_argument("--constants-equal", action="store_true", help="exit 0 when only the constants must be equal")
    args = parser.parse_args(argv)
    try:
        first, second = parse_anec(Path(args.a).read_bytes()), parse_anec(Path(args.b).read_bytes())
    except (AnecError, OSError, struct.error) as error:
        print(f"anec_compare: {error}", file=sys.stderr)
        return 2
    result = compare(first, second)
    if args.json:
        print(json.dumps({"a": public(first), "b": public(second), "compare": result}, indent=1))
    else:
        for name, info in (("A", first), ("B", second)):
            print(f"{name}: {info['task_count']} tasks, stream {info['stream_bytes']} B ({info['stream_sha256'][:16]}), "
                  f"constants {info['constant_bytes']} B ({info['constants_sha256'][:16]}), {info['input_count']} inputs, "
                  f"channels {public(info)['channels_with_tiles']}")
        for key in ("counts_equal", "stream_equal", "constants_equal", "constants_contained", "tiles_equal"):
            print(f"{key}: {result[key]}")
    if args.constants_equal:
        return 0 if result["constants_equal"] else 1
    return 0 if all(result[k] for k in ("counts_equal", "stream_equal", "constants_equal", "tiles_equal")) else 1


if __name__ == "__main__":
    sys.exit(main())
