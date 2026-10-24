#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""ctypes wrapper for tools/surface_pack.c: strided fp16 surface pack and unpack for the staged ANE decodes.

    pack_into(buffer, dense, port)      # dense fp32 or fp16 array in (n, c, h, w) order -> the strided surface in `buffer` (a ctypes buffer or bytearray whose padding is zero)
    dense = unpack(raw_or_buffer, port) # fp16 numpy array (n, c, h, w) read from the strided surface

`port` holds "shape" (n, c, h, w) and "strides" with the byte strides of the c axis at index 1 and of the h axis at index 2, as in the lab decoders' port tables.
Both functions give exactly what the numpy versions give (`np.asarray(dense, float16).reshape(shape)` assigned through `as_strided`, and `np.array(as_strided(...))`): fp32 to fp16 rounds to nearest even.
The library is built on first use with the host C compiler.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np

SOURCE = Path(__file__).resolve().parent / "surface_pack.c"


def build(out_dir: Path | None = None) -> Path:
    """Compile surface_pack.c into a shared library named after the source hash and return its path."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise RuntimeError("surface_pack needs a C compiler")
    out_dir = out_dir or Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "omarchy-ane"
    out_dir.mkdir(parents=True, exist_ok=True)
    lib = out_dir / f"libsurface_pack-{hashlib.sha256(SOURCE.read_bytes()).hexdigest()[:16]}.so"
    if not lib.exists():
        tmp = lib.with_suffix(f".{os.getpid()}.tmp")
        subprocess.run([cc, "-O3", "-shared", "-fPIC", str(SOURCE), "-o", str(tmp)], check=True)
        os.replace(tmp, lib)
    return lib


_LIB = None


def _lib() -> ctypes.CDLL:
    global _LIB
    if _LIB is None:
        lib = ctypes.CDLL(str(build()))
        sz = ctypes.c_size_t
        lib.surface_pack_f32.argtypes = [ctypes.c_void_p, sz, ctypes.c_void_p, sz, sz, sz, sz, sz, sz]
        lib.surface_pack_f16.argtypes = lib.surface_pack_f32.argtypes
        lib.surface_unpack.argtypes = lib.surface_pack_f32.argtypes
        _LIB = lib
    return _LIB


def _dims(port):
    n, c, h, w = (int(v) for v in port["shape"])
    plane, row = int(port["strides"][1]), int(port["strides"][2])
    if min(n, c, h, w) < 1 or not 0 <= plane < 2**62 or not 0 <= row < 2**62:
        raise ValueError(f"the port has a non-positive dimension or a stride outside 0..2**62: shape {port['shape']}, strides {port['strides']}")
    return n, c, h, w, plane, row


def pack_into(buffer, dense, port) -> None:
    """Write `dense` into the surface `buffer` (ctypes buffer or bytearray). The padding of `buffer` is not touched."""
    n, c, h, w, plane, row = _dims(port)
    a = np.asarray(dense)
    if a.dtype != np.float32 and a.dtype != np.float16:
        a = a.astype(np.float16)
    a = np.ascontiguousarray(a)
    if a.size != n * c * h * w:
        raise ValueError(f"dense has {a.size} values, the port takes {n * c * h * w}")
    if isinstance(buffer, bytearray):
        dst = (ctypes.c_char * len(buffer)).from_buffer(buffer)
    else:
        dst = buffer
    size = ctypes.sizeof(dst) if not isinstance(buffer, bytearray) else len(buffer)
    fn = _lib().surface_pack_f32 if a.dtype == np.float32 else _lib().surface_pack_f16
    if fn(ctypes.addressof(dst), size, a.ctypes.data, n, c, h, w, plane, row) != 0:
        raise ValueError("the port's rows do not fit the surface buffer")


def unpack(raw, port) -> np.ndarray:
    """Read the surface in `raw` (bytes, bytearray or ctypes buffer) into a new fp16 array of shape (n, c, h, w)."""
    n, c, h, w, plane, row = _dims(port)
    out = np.empty((n, c, h, w), dtype=np.float16)
    if isinstance(raw, (bytes, bytearray, memoryview)):
        view = np.frombuffer(raw, dtype=np.uint8)
        addr, size = view.ctypes.data, view.size
    else:
        addr, size = ctypes.addressof(raw), ctypes.sizeof(raw)
    if _lib().surface_unpack(addr, size, out.ctypes.data, n, c, h, w, plane, row) != 0:
        raise ValueError("the port's rows do not fit the surface buffer")
    return out
