#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Host test of the --q6k-head hook in qwen_m2_decode.py (no device). Skips when gguf-py or a C compiler is missing.

A tiny GGUF is written with the 64-row real Q6_K fixture of tests/fixtures/q6k as token_embd.weight. Then load_head() runs both ways on it:
  1. default (fp32 head) and q6k=True: embed[token] is equal bit for bit for all 64 tokens, `head @ hidden` agrees to fp32 rounding (max abs 1e-5), and the
     two heads give the same argmax on 20 random hidden vectors;
  2. a token_embd.weight that is not Q6_K is refused with a clear message;
  3. head_threads() is one per performance core when the chip has efficiency cores and 8 (or fewer CPUs) otherwise."""
import os
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, os.environ.get("GGUF_PY", "/var/tmp/qwen-decode/gguf-py"))

import numpy as np  # noqa: E402

try:
    from gguf import GGMLQuantizationType, GGUFWriter  # noqa: E402
except ImportError as e:
    print(f"SKIP q6k hook test: gguf-py is not importable ({e})")
    sys.exit(0)
import q6k_head  # noqa: E402

q6k_head.build()   # a failed C build raises here and FAILS the test: it must not skip

import pcore_mask  # noqa: E402
import qwen_m2_decode as dec  # noqa: E402

FIXTURE = HERE.parent / "tests" / "fixtures" / "q6k" / "token_embd_rows64.bin"
ROWS, K, ROW_BYTES = 64, 2048, 1680


def write_gguf(path, qtype, array):
    w = GGUFWriter(str(path), "qwen35")
    w.add_tensor("token_embd.weight", array, raw_dtype=qtype)
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def main():
    blocks = np.fromfile(FIXTURE, dtype=np.uint8).reshape(ROWS, ROW_BYTES)
    dec.rope_params = lambda reader: (64, 64, 10000.0)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "q6k.gguf"
        write_gguf(path, GGMLQuantizationType.Q6_K, blocks)
        embed32, head32, rope32 = dec.load_head(str(path))
        embedq, headq, ropeq = dec.load_head(str(path), q6k=True)
        assert rope32 == ropeq == (64, 64, 10000.0)
        assert headq.head.threads == dec.head_threads(), "the loaded Q6_K head uses head_threads() threads"
        saved_p = pcore_mask.p_core_cpus
        try:
            for cpus, want in (([4, 5, 6, 7, 8, 9, 10, 11], 8), ([4, 5, 6, 7], 4), ([2, 3, 4, 5, 6, 7, 8, 9], 8)):
                pcore_mask.p_core_cpus = lambda root=None, c=cpus: c
                _, h, _ = dec.load_head(str(path), q6k=True)
                assert h.head.threads == want == len(cpus), (cpus, h.head.threads)
        finally:
            pcore_mask.p_core_cpus = saved_p
        assert head32.dtype == np.float32 and head32.shape == (ROWS, K) and isinstance(headq, dec.Q6KMatrix) and isinstance(embedq, dec.Q6KEmbed)
        for token in range(ROWS):
            a, b = embed32[token], embedq[token]
            assert a.dtype == b.dtype == np.float16 and a.tobytes() == b.tobytes(), f"embedding row {token} differs"
            assert embedq[token][None].shape == (1, K), "embed[token][None] still gives [1, k]"
        rng = np.random.default_rng(7)
        worst = 0.0
        for _ in range(20):
            hidden = (rng.standard_normal(K) * 2.0).astype(np.float32)
            ref, got = head32 @ hidden, headq @ hidden
            assert got.dtype == np.float32 and got.shape == (ROWS,)
            worst = max(worst, float(np.abs(ref - got).max()))
            assert int(np.argmax(ref)) == int(np.argmax(got))
        assert worst < 1e-4, worst
        bad = Path(tmp) / "f32.gguf"
        write_gguf(bad, GGMLQuantizationType.F32, np.zeros((ROWS, K), np.float32))
        try:
            dec.load_head(str(bad), q6k=True)
            raise AssertionError("a non-Q6_K head was accepted")
        except dec.Refuse as e:
            assert "not Q6_K" in str(e), e
    real = pcore_mask.p_core_cpus
    try:
        pcore_mask.p_core_cpus = lambda root=None: [4, 5, 6, 7, 8, 9, 10, 11]
        assert dec.head_threads() == 8
        pcore_mask.p_core_cpus = lambda root=None: [4, 5, 6, 7]
        assert dec.head_threads() == 4
        pcore_mask.p_core_cpus = lambda root=None: None
        assert dec.head_threads() == min(8, os.cpu_count() or 1)
    finally:
        pcore_mask.p_core_cpus = real
    print(f"Q6K-HOOK PASS (embedding rows bit-equal for {ROWS} tokens, logits max abs diff {worst:.2e}, non-Q6_K refused, thread count)")


if __name__ == "__main__":
    main()
