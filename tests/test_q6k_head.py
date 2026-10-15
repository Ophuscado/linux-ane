#!/usr/bin/env python3
"""Host checks for tools/q6k_head.c: Q6_K rows dequantized and multiplied by an fp32 vector. No device.

The reference is numpy: the ggml Q6_K dequantization in fp32 ((d * scale) * q) and a float64 matvec of those weights.
The native build is the scalar path; the NEON path is cross-compiled for aarch64 and run under qemu-user when both
tools are installed. Random blocks use finite fp16 scales, scales in the int8 range and any 6-bit values."""

import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tools" / "q6k_head.c"
BLOCK = 210
CROSS = shutil.which("aarch64-linux-gnu-gcc")
QEMU = shutil.which("qemu-aarch64-static")


def make_blocks(rng, rows, k):
    nb = rows * (k // 256)
    raw = rng.integers(0, 256, size=(nb, BLOCK), dtype=np.uint8)
    d = rng.uniform(1e-3, 5e-2, size=nb).astype(np.float16)
    raw[:, 208:210] = d.view(np.uint8).reshape(nb, 2)
    return raw.reshape(rows, (k // 256) * BLOCK)


def dequant_np(rows_blocks, k):
    """ggml dequantize_row_q6_K in fp32: d * scale * q, evaluated as (d * scale) * q."""
    nb = k // 256
    out = np.zeros((rows_blocks.shape[0], k), dtype=np.float32)
    for r, row in enumerate(rows_blocks):
        for b in range(nb):
            blk = row[b * BLOCK:(b + 1) * BLOCK]
            ql, qh, sc = blk[:128], blk[128:192], blk[192:208].view(np.int8).astype(np.float32)
            d = np.float32(blk[208:210].view(np.float16)[0])
            y = out[r, b * 256:(b + 1) * 256]
            for half in range(2):
                qlh, qhh, sch = ql[half * 64:half * 64 + 64], qh[half * 32:half * 32 + 32], sc[half * 8:half * 8 + 8]
                l = np.arange(32)
                isc = l // 16
                q1 = ((qlh[l] & 0xF) | (((qhh[l] >> 0) & 3) << 4)).astype(np.int32) - 32
                q2 = ((qlh[l + 32] & 0xF) | (((qhh[l] >> 2) & 3) << 4)).astype(np.int32) - 32
                q3 = ((qlh[l] >> 4) | (((qhh[l] >> 4) & 3) << 4)).astype(np.int32) - 32
                q4 = ((qlh[l + 32] >> 4) | (((qhh[l] >> 6) & 3) << 4)).astype(np.int32) - 32
                base = half * 128
                y[base + l] = (d * sch[isc + 0]) * q1.astype(np.float32)
                y[base + l + 32] = (d * sch[isc + 2]) * q2.astype(np.float32)
                y[base + l + 64] = (d * sch[isc + 4]) * q3.astype(np.float32)
                y[base + l + 96] = (d * sch[isc + 6]) * q4.astype(np.float32)
    return out


@pytest.fixture(scope="module")
def binaries(tmp_path_factory):
    out = tmp_path_factory.mktemp("q6k")
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc:
        pytest.skip("no C compiler")
    native = out / "q6k_native"
    subprocess.run([cc, "-O2", "-Wall", "-DQ6K_TEST_MAIN", "-DQ6K_NO_NEON", "-pthread", str(SOURCE), "-o", str(native), "-lm"], check=True)
    result = {"scalar": [str(native)]}
    if CROSS and QEMU:
        neon = out / "q6k_neon"
        subprocess.run([CROSS, "-O2", "-Wall", "-static", "-DQ6K_TEST_MAIN", "-pthread", str(SOURCE), "-o", str(neon), "-lm"], check=True)
        result["neon"] = [QEMU, str(neon)]
    return result


def run(binary, tmp_path, blocks, x, row, threads):
    rows, width = blocks.shape[0], x.size
    paths = {name: tmp_path / name for name in ("blocks", "x", "y", "dq")}
    paths["blocks"].write_bytes(blocks.tobytes())
    paths["x"].write_bytes(x.astype(np.float32).tobytes())
    proc = subprocess.run([*binary, str(paths["blocks"]), str(rows), str(width), str(paths["x"]), str(paths["y"]), str(row),
                           str(paths["dq"]), str(threads)], capture_output=True, text=True, check=True)
    return (np.frombuffer(paths["y"].read_bytes(), dtype=np.float32), np.frombuffer(paths["dq"].read_bytes(), dtype=np.float32),
            proc.stdout.strip())


@pytest.mark.parametrize("variant", ["scalar", "neon"])
@pytest.mark.parametrize("rows, k", [(1, 256), (7, 512), (64, 2048)])
def test_dequant_row_is_bit_identical_and_the_matvec_is_close(binaries, tmp_path, variant, rows, k):
    if variant not in binaries:
        pytest.skip("aarch64 cross compiler or qemu-user not installed")
    rng = np.random.default_rng(1000 + rows + k)
    blocks = make_blocks(rng, rows, k)
    x = rng.standard_normal(k).astype(np.float32) * 3.0
    reference = dequant_np(blocks, k)
    y, dq, banner = run(binaries[variant], tmp_path, blocks, x, rows - 1, 3)
    assert banner == ("neon=1" if variant == "neon" else "neon=0")
    assert np.array_equal(dq, reference[rows - 1]), "dequantized row differs from the fp32 reference"
    exact = reference.astype(np.float64) @ x.astype(np.float64)
    scale = np.abs(reference.astype(np.float64)) @ np.abs(x.astype(np.float64))
    err = np.abs(y.astype(np.float64) - exact)
    assert np.all(err <= 1e-5 * scale), (err / scale).max()
    # The comparison point: numpy's own fp32 matvec on the same fp32 weights.
    baseline = np.abs((reference @ x).astype(np.float64) - exact)
    assert err.max() <= 4 * baseline.max() + 1e-6 * scale.max()


@pytest.mark.parametrize("variant", ["scalar", "neon"])
def test_the_thread_count_never_changes_a_bit(binaries, tmp_path, variant):
    if variant not in binaries:
        pytest.skip("aarch64 cross compiler or qemu-user not installed")
    rng = np.random.default_rng(7)
    blocks, x = make_blocks(rng, 37, 1024), rng.standard_normal(1024).astype(np.float32)
    first = run(binaries[variant], tmp_path, blocks, x, 0, 1)[0]
    for threads in (2, 5, 8, 64):
        assert np.array_equal(run(binaries[variant], tmp_path, blocks, x, 0, threads)[0], first), threads


def test_the_neon_path_matches_the_scalar_path_to_fp32_rounding(binaries, tmp_path):
    if "neon" not in binaries:
        pytest.skip("aarch64 cross compiler or qemu-user not installed")
    rng = np.random.default_rng(11)
    blocks, x = make_blocks(rng, 33, 2048), rng.standard_normal(2048).astype(np.float32)
    scalar = run(binaries["scalar"], tmp_path, blocks, x, 0, 2)[0].astype(np.float64)
    neon = run(binaries["neon"], tmp_path, blocks, x, 0, 2)[0].astype(np.float64)
    scale = np.abs(dequant_np(blocks, 2048).astype(np.float64)) @ np.abs(x.astype(np.float64))
    assert np.all(np.abs(neon - scalar) <= 1e-5 * scale)


REAL = Path(__file__).resolve().parent / "fixtures" / "q6k" / "token_embd_rows64.bin"
# sha256 of gguf-py's fp32 dequantization of these 64 rows (the first 63 rows and the last row of the tied head of the
# staged Qwen GGUF), computed on the M2 with gguf-py 0.19.0.
REAL_GGUF_PY_SHA256 = "c7ae550ab80a9640e65b41d0c72f1bc5134919e8a164c7b4f3d54cc201cf1bf2"


def test_real_rows_dequantize_to_what_gguf_py_gives():
    import hashlib

    blocks = np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680)
    assert hashlib.sha256(dequant_np(blocks, 2048).tobytes()).hexdigest() == REAL_GGUF_PY_SHA256


@pytest.mark.parametrize("variant", ["scalar", "neon"])
def test_real_rows_through_the_kernel(binaries, tmp_path, variant):
    if variant not in binaries:
        pytest.skip("aarch64 cross compiler or qemu-user not installed")
    import hashlib

    blocks = np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680)
    reference = dequant_np(blocks, 2048)
    x = np.random.default_rng(5).standard_normal(2048).astype(np.float32) * 2.0
    y, dq, _ = run(binaries[variant], tmp_path, blocks, x, 63, 4)
    assert np.array_equal(dq, reference[63])
    assert hashlib.sha256(dq.tobytes()).hexdigest() != hashlib.sha256(reference[0].tobytes()).hexdigest()
    exact = reference.astype(np.float64) @ x.astype(np.float64)
    scale = np.abs(reference.astype(np.float64)) @ np.abs(x.astype(np.float64))
    assert np.all(np.abs(y.astype(np.float64) - exact) <= 1e-5 * scale)


def test_the_python_wrapper_matches_the_reference(tmp_path):
    sys_path = str(ROOT / "tools")
    import sys

    sys.path.insert(0, sys_path)
    from q6k_head import Q6KHead, build

    blocks = np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680)
    reference = dequant_np(blocks, 2048)
    head = Q6KHead(blocks, 64, 2048, threads=3, lib=build(tmp_path))
    x = np.random.default_rng(9).standard_normal(2048).astype(np.float32)
    exact = reference.astype(np.float64) @ x.astype(np.float64)
    scale = np.abs(reference.astype(np.float64)) @ np.abs(x.astype(np.float64))
    assert np.all(np.abs(head.logits(x).astype(np.float64) - exact) <= 1e-5 * scale)
    for token in (0, 17, 63):
        assert np.array_equal(head.embed_row(token), reference[token])
        assert np.array_equal(head.embed_row(token).astype(np.float16), reference[token].astype(np.float16))
    with pytest.raises(ValueError):
        head.logits(x[:100])
    with pytest.raises(ValueError):
        Q6KHead(blocks[:10], 64, 2048, lib=build(tmp_path))
    with pytest.raises(IndexError):
        head.embed_row(64)


def _bench():
    import sys

    sys.path.insert(0, str(ROOT / "tools"))
    import q6k_head_bench

    return q6k_head_bench


def _measure(tmp_path, blocks, check, threads=(1, 3)):
    from q6k_head import build

    bench = _bench()
    x = np.random.default_rng(21).standard_normal(2048).astype(np.float32)
    return bench.measure(blocks, 64, 2048, x, check, list(threads), 2, lib=build(tmp_path)), x


def test_the_bench_passes_a_correct_kernel_and_fails_a_wrong_one(tmp_path):
    bench = _bench()
    clean = np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680).copy()
    x = np.random.default_rng(21).standard_normal(2048).astype(np.float32)
    check = dequant_np(clean, 2048).astype(np.float64) @ x.astype(np.float64)          # the reference from the clean bytes
    good, _ = _measure(tmp_path, clean, check)
    assert bench.verdict(good, bench.DEFAULT_TOLERANCE) == (0, [])
    damaged = clean.copy()
    damaged[5, 100:140] ^= 0xFF                                                       # a corrupted block in the first 256 rows
    wrong, _ = _measure(tmp_path, damaged, check)
    code, reasons = bench.verdict(wrong, bench.DEFAULT_TOLERANCE)
    assert code == 3 and len(reasons) == 2 and all(r["max_abs_err_first256"] > 1e-3 for r in wrong.values())


def test_the_bench_fails_a_non_finite_error_and_honours_the_tolerance_option(tmp_path):
    bench = _bench()
    clean = np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680).copy()
    x = np.random.default_rng(21).standard_normal(2048).astype(np.float32)
    check = dequant_np(clean, 2048).astype(np.float64) @ x.astype(np.float64)
    nan_block = clean.copy()
    nan_block[7, 208:210] = np.frombuffer(np.float16("nan").tobytes(), dtype=np.uint8)   # d = NaN in the first block of row 7
    result, _ = _measure(tmp_path, nan_block, check, threads=(2,))
    assert not np.isfinite(result["2"]["max_abs_err_first256"])
    assert bench.verdict(result, 1e30)[0] == 3                                         # NaN fails whatever the tolerance is
    mild = clean.copy()
    mild[3, 208:210] = np.frombuffer(np.float16(0.0501).tobytes(), dtype=np.uint8)      # a changed scale: finite error
    finite, _ = _measure(tmp_path, mild, check, threads=(2,))
    err = finite["2"]["max_abs_err_first256"]
    assert np.isfinite(err) and err > bench.DEFAULT_TOLERANCE
    assert bench.verdict(finite, bench.DEFAULT_TOLERANCE)[0] == 3 and bench.verdict(finite, err * 2)[0] == 0


def test_the_bench_command_line_returns_3_on_failure_and_0_on_success(tmp_path, capsys):
    gguf = pytest.importorskip("gguf")
    bench = _bench()
    blocks = np.tile(np.frombuffer(REAL.read_bytes(), dtype=np.uint8).reshape(64, 1680), (10, 1))
    path = tmp_path / "tiny.gguf"
    writer = gguf.GGUFWriter(str(path), "qwen35")
    writer.add_tensor("token_embd.weight", blocks, raw_dtype=gguf.GGMLQuantizationType.Q6_K)
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()
    assert bench.main([str(path), "--reps", "2", "--threads", "1,2"]) == 0
    assert '"ok": true' in capsys.readouterr().out
    assert bench.main([str(path), "--reps", "2", "--threads", "1", "--tolerance", "1e-12"]) == 3
    assert "FAIL" in capsys.readouterr().err
