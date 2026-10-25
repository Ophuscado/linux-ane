#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Offline check of the program load order of qwen_m2_decode.py (no device, no ane-session).

load_order() puts the largest program file first and keeps index order among equal sizes, and Decoder.configure loads through it. The reason is a
measurement on an M2 (the driver's 32-bit IOVA window, power of two aligned sections): index order refused a large section after earlier sessions, largest
first loaded all 38 programs in eight sessions in a row."""
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import qwen_m2_decode as dec

fails = 0


def check(ok, name):
    global fails
    print(f"  {'PASS' if ok else 'FAIL'} {name}")
    fails += 0 if ok else 1


with tempfile.TemporaryDirectory() as td:
    tables = []
    for i, n in enumerate([100, 300, 200, 300, 50]):
        f = Path(td) / f"p{i}.anec"
        f.write_bytes(b"\0" * n)
        tables.append((str(f), None, None))
    check(dec.load_order(tables) == [1, 3, 2, 0, 4], "largest first, equal sizes in index order")
    check(dec.load_order([]) == [], "no programs")

body = Path(dec.__file__).read_text().split("def configure")[1].split("def reset")[0]
check("for i in load_order(self.tables):" in body and "enumerate(self.tables)" not in body, "configure loads through load_order, not in index order")
sys.exit(1 if fails else 0)
