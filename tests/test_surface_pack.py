#!/usr/bin/env python3
"""Host checks for tools/surface_pack.c. The reference is the numpy implementation the lab decoders used: pack = zeros(alloc), assign
np.asarray(dense, float16).reshape(shape) through as_strided, tobytes; unpack = np.array(as_strided view of the raw bytes). The C functions must give exactly the same bytes and arrays,
including the fp32 to fp16 rounding of subnormals, overflow, infinities and NaNs (compared bit for bit, NaN payload aside: both are quiet NaNs with the same sign)."""

import ctypes
import sys
from pathlib import Path

import numpy as np
import pytest
from numpy.lib.stride_tricks import as_strided

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import surface_pack as sp

F16 = np.float16


def surface_view(buf, port):
    n, c, h, w = port["shape"]
    plane, row = port["strides"][1], port["strides"][2]
    return as_strided(buf, shape=(n, c, h, w), strides=(c * plane, plane, row, 2), writeable=buf.flags.writeable)


def ref_pack(dense, port, alloc):
    buf = np.zeros(alloc // 2, dtype=F16)
    surface_view(buf, port)[...] = np.asarray(dense, dtype=F16).reshape(port["shape"])
    return buf.tobytes()


def ref_unpack(raw, port):
    return np.array(surface_view(np.frombuffer(raw, dtype=F16), port))


def make_port(rng, n, c, h, w):
    row = (w * 2 + int(rng.integers(0, 4)) * 32 + 15) // 16 * 16     # padded rows, a multiple of 16 bytes
    plane = h * row + int(rng.integers(0, 3)) * 64
    port = {"shape": [n, c, h, w], "strides": [0, plane, row, 2]}
    alloc = (n * c * plane + 64 + 15) // 16 * 16
    alloc += alloc % 2
    return port, alloc


SHAPES = [(1, 1, 1, 1), (1, 3, 1, 5), (1, 2048, 1, 1), (1, 16, 1, 128), (1, 8, 192, 1), (2, 3, 5, 7), (1, 64, 1, 192), (1, 4, 4, 17)]


@pytest.mark.parametrize("shape", SHAPES)
@pytest.mark.parametrize("dtype", [np.float32, np.float16])
def test_pack_equals_the_numpy_surface_bit_for_bit(shape, dtype):
    rng = np.random.default_rng(sum(shape) * 7 + (dtype == np.float16))
    port, alloc = make_port(rng, *shape)
    dense = (rng.standard_normal(shape) * 30).astype(dtype)
    buf = ctypes.create_string_buffer(alloc)
    sp.pack_into(buf, dense, port)
    assert bytes(buf.raw) == ref_pack(dense, port, alloc)
    ba = bytearray(alloc)
    sp.pack_into(ba, dense, port)
    assert bytes(ba) == ref_pack(dense, port, alloc)


def test_fp32_rounding_matches_numpy_on_edge_values():
    vals = np.array([0.0, -0.0, 1.0, -1.5, 65504.0, 65519.0, 65520.0, 1e6, -1e6, 6.1e-5, 5.96e-8, 2.98e-8, 1e-9, np.inf, -np.inf, np.nan,
                     0.1, 0.3333333, 1.00048828125, 1.00146484375, 2049.0, 2051.0], dtype=np.float32)
    rng = np.random.default_rng(5)
    dense = np.concatenate([vals, (rng.standard_normal(5000) * 10).astype(np.float32), rng.integers(0, 2 ** 32, 5000, dtype=np.uint32).view(np.float32)])
    dense = dense[: dense.size - dense.size % 8].reshape(1, 8, 1, -1)
    port, alloc = make_port(rng, *dense.shape)
    buf = ctypes.create_string_buffer(alloc)
    sp.pack_into(buf, dense, port)
    got = np.frombuffer(bytes(buf.raw), dtype=np.uint16)
    want = np.frombuffer(ref_pack(dense, port, alloc), dtype=np.uint16)
    nan_f32 = np.isnan(dense.reshape(-1))
    gv = np.array(surface_view(got.copy(), port)).reshape(-1)
    wv = np.array(surface_view(want.copy(), port)).reshape(-1)
    assert np.array_equal(gv[~nan_f32], wv[~nan_f32])
    assert np.all(np.isnan(gv[nan_f32].view(F16)))                  # NaN stays NaN


@pytest.mark.parametrize("shape", SHAPES)
def test_unpack_equals_the_numpy_view(shape):
    rng = np.random.default_rng(sum(shape) * 11)
    port, alloc = make_port(rng, *shape)
    raw = rng.integers(0, 256, alloc, dtype=np.uint8).tobytes()
    assert np.array_equal(sp.unpack(raw, port).view(np.uint16), ref_unpack(raw, port).view(np.uint16))
    buf = ctypes.create_string_buffer(raw, alloc)
    assert np.array_equal(sp.unpack(buf, port).view(np.uint16), ref_unpack(raw, port).view(np.uint16))


def test_pack_leaves_the_padding_untouched():
    rng = np.random.default_rng(3)
    port, alloc = make_port(rng, 1, 5, 3, 9)
    buf = ctypes.create_string_buffer(b"\xa5" * alloc, alloc)
    sp.pack_into(buf, rng.standard_normal((1, 5, 3, 9)).astype(np.float32), port)
    covered = np.zeros(alloc // 2, dtype=np.uint16)
    surface_view(covered, port)[...] = 1
    pad = np.frombuffer(bytes(buf.raw), dtype=np.uint16)[covered == 0]
    assert np.all(pad == 0xA5A5)


def test_a_row_beyond_the_buffer_is_refused_and_nothing_is_written_there():
    port = {"shape": [1, 4, 1, 8], "strides": [0, 32, 32, 2]}
    small = ctypes.create_string_buffer(b"\x11" * 64, 64)             # needs 4 * 32 = 128 bytes
    with pytest.raises(ValueError):
        sp.pack_into(small, np.zeros((1, 4, 1, 8), dtype=np.float32), port)
    with pytest.raises(ValueError):
        sp.unpack(bytes(64), port)
    with pytest.raises(ValueError):
        sp.pack_into(ctypes.create_string_buffer(256), np.zeros(31, dtype=np.float32), port)


def test_negative_strides_and_dimensions_are_refused_in_python():
    for port in ({"shape": [1, 2, 1, 4], "strides": [0, -4, 8, 2]}, {"shape": [1, 2, 1, 4], "strides": [0, 8, -8, 2]}, {"shape": [1, 0, 1, 4], "strides": [0, 8, 8, 2]},
                 {"shape": [1, 2, 1, 4], "strides": [0, 2 ** 64 + 8, 8, 2]}, {"shape": [1, 2, 1, 4], "strides": [0, 8, 2 ** 62, 2]}):
        with pytest.raises(ValueError):
            sp.pack_into(ctypes.create_string_buffer(64), np.zeros(8, dtype=np.float32), port)
        with pytest.raises(ValueError):
            sp.unpack(bytes(64), port)


def test_a_stride_that_wraps_the_offset_is_refused_in_c():
    """plane = 2**64 - 4 (what a negative stride becomes as a size_t) must not move the write below the buffer."""
    lib = sp._lib()
    arena = ctypes.create_string_buffer(b"\x5a" * 12288, 12288)
    inner = ctypes.addressof(arena) + 4096
    dense = np.ones(8, dtype=np.float32)
    for plane, row in ((2 ** 64 - 4, 8), (2 ** 63, 2 ** 63), (2 ** 62, 8)):
        rc = lib.surface_pack_f32(inner, 4096, dense.ctypes.data, 1, 2, 1, 4, plane, row)
        assert rc == -1, (plane, row)
    assert lib.surface_pack_f32(inner, 4096, dense.ctypes.data, 1, 1, 2, 4, 8, 2 ** 64 - 8) == -1     # h = 2: the row stride wraps
    raw = bytes(arena.raw)
    assert raw[:4096] == b"\x5a" * 4096 and raw[8192:] == b"\x5a" * 4096          # nothing outside the 4096 byte surface changed
    out = np.empty(8, dtype=np.float16)
    assert lib.surface_unpack(inner, 4096, out.ctypes.data, 1, 2, 1, 4, 2 ** 64 - 4, 8) == -1

