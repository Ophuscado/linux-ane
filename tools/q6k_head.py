#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""ctypes wrapper for tools/q6k_head.c: a Q6_K output head that is read as Q6_K, not as fp32.

    head = Q6KHead(blocks, rows=248320, k=2048, threads=8)   # blocks: uint8 array, rows x (k / 256 * 210) bytes
    logits = head.logits(hidden)                             # fp32 [rows], hidden fp32 [k]
    row = head.embed_row(token)                              # fp32 [k], the row dequantized exactly as ggml does

`embed_row(token)` equals the fp32 head row in use today bit for bit, so `embed_row(token).astype(float16)` equals
`head_fp32.astype(float16)[token]`. `logits` differs from a BLAS sgemv on the fp32 head at fp32 rounding (see q6k_head.c).
The library is built on first use with the host C compiler into a cache directory next to this file's user cache.
"""

from __future__ import annotations

import ctypes
import hashlib
import os
import platform
import shutil
import subprocess
from pathlib import Path

import numpy as np

SOURCE = Path(__file__).resolve().parent / "q6k_head.c"
BLOCK_BYTES = 210


def _fp16fml_cpu() -> bool:
    """True on an aarch64 host whose CPU has the FP16FML extension (asimdfhm): FMLAL is available."""
    try:
        return platform.machine() == "aarch64" and " asimdfhm" in Path("/proc/cpuinfo").read_text()
    except OSError:
        return False


def build(out_dir: Path | None = None) -> Path:
    """Compile q6k_head.c into a shared library named after the source hash and return its path."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        raise RuntimeError("q6k_head needs a C compiler")
    out_dir = out_dir or Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "omarchy-ane"
    out_dir.mkdir(parents=True, exist_ok=True)
    digest = hashlib.sha256(SOURCE.read_bytes() + (b"fp16fml" if _fp16fml_cpu() else b"")).hexdigest()[:16]
    lib = out_dir / f"libq6k_head-{digest}.so"
    if not lib.exists():
        tmp = lib.with_suffix(f".{os.getpid()}.tmp")
        cmd = [cc, "-O3", "-pthread", "-shared", "-fPIC", str(SOURCE), "-o", str(tmp), "-lm"]
        if _fp16fml_cpu():
            try:
                subprocess.run(cmd[:5] + ["-march=armv8.2-a+fp16+fp16fml"] + cmd[5:], check=True)
            except subprocess.CalledProcessError:
                subprocess.run(cmd, check=True)   # a compiler without the FMLAL intrinsics: the merged kernel only
        else:
            subprocess.run(cmd, check=True)
        os.replace(tmp, lib)
    return lib


class Q6KHead:
    def __init__(self, blocks: np.ndarray, rows: int, k: int, threads: int = 8, lib: Path | None = None):
        blocks = np.ascontiguousarray(blocks, dtype=np.uint8)
        if k % 256 or blocks.size != rows * (k // 256) * BLOCK_BYTES:
            raise ValueError(f"{blocks.size} bytes is not {rows} rows of {k} Q6_K weights")
        self.blocks, self.rows, self.k, self.threads = blocks, rows, k, threads
        self._lib = ctypes.CDLL(str(lib or build()))
        self._lib.q6k_matvec_f32.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_void_p,
                                             ctypes.c_void_p, ctypes.c_int]
        self._lib.q6k_has_fp16fml.restype = ctypes.c_int
        self._lib.q6k_dequant_row.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        self._lib.q6k_matvec_f32_fp16x.argtypes = self._lib.q6k_matvec_f32.argtypes
        self._fp16x = bool(self._lib.q6k_has_fp16fml()) and os.environ.get("Q6K_FP16X", "1") != "0"
        self._row_bytes = (k // 256) * BLOCK_BYTES

    def logits(self, hidden: np.ndarray) -> np.ndarray:
        x = np.ascontiguousarray(hidden, dtype=np.float32).reshape(-1)
        if x.size != self.k:
            raise ValueError(f"hidden has {x.size} values, the head takes {self.k}")
        y = np.empty(self.rows, dtype=np.float32)
        # The FMLAL kernel rounds x to fp16. When x is already an fp16 value (the engine's output), that changes nothing and the result is bit-identical.
        fn = self._lib.q6k_matvec_f32_fp16x if self._fp16x and np.array_equal(x.astype(np.float16).astype(np.float32), x) else self._lib.q6k_matvec_f32
        fn(self.blocks.ctypes.data, self.rows, self.k, x.ctypes.data, y.ctypes.data, self.threads)
        return y

    def embed_row(self, token: int) -> np.ndarray:
        if not 0 <= token < self.rows:
            raise IndexError(token)
        out = np.empty(self.k, dtype=np.float32)
        self._lib.q6k_dequant_row(self.blocks.ctypes.data + token * self._row_bytes, self.k, out.ctypes.data)
        return out
