"""GPU throughput over SECONDS, one sample per second. MODE mm (default): fp16 2048 matmul chain, TFLOP/s. MODE bw: fp16 add over
256 MiB arrays (read 2, write 1), GB/s. Prints one JSON line: median/min/max and the samples."""
import json
import statistics
import sys
import time

import mlx.core as mx

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
mode = sys.argv[2] if len(sys.argv) > 2 else "mm"
n = 2048
a = mx.random.normal((n, n)).astype(mx.float16)
b = mx.random.normal((n, n)).astype(mx.float16)
mx.eval(a, b)
c = a @ b
mx.eval(c)
samples = []
if mode == "bw":
    x = mx.ones((128 * 1024 * 1024,), dtype=mx.float16)
    y = mx.ones((128 * 1024 * 1024,), dtype=mx.float16)
    mx.eval(x, y)
    mx.eval(x + y)
    t_end = time.time() + secs
    while time.time() < t_end:
        t0 = time.time()
        count = 0
        while time.time() - t0 < 1.0:
            z = x + y
            mx.eval(z)
            count += 1
        samples.append(round(count * 3 * 256 * 1024 * 1024 / (time.time() - t0) / 1e9, 3))
    print(json.dumps({"median_gbps": round(statistics.median(samples), 3), "min": min(samples), "max": max(samples), "n": len(samples), "samples": samples}))
    sys.exit(0)
t_end = time.time() + secs
while time.time() < t_end:
    t0 = time.time()
    count = 0
    while time.time() - t0 < 1.0:
        c = a @ b
        for _ in range(9):
            c = c @ b / 64
        mx.eval(c)
        count += 10
    samples.append(round(count * 2 * n**3 / (time.time() - t0) / 1e12, 4))
print(json.dumps({"median_tflops": round(statistics.median(samples), 4), "min": min(samples), "max": max(samples), "n": len(samples), "samples": samples}))
