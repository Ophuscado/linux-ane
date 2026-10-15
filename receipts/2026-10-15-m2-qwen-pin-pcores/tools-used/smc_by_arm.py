import statistics as st
import sys

path = sys.argv[1]
arms = {"1U": (1792082820.226, 1792082822.820), "2P": (1792082888.422, 1792082890.965),
        "3U": (1792082956.615, 1792082959.533), "4P": (1792083025.132, 1792083027.665)}
PRE = 2.0   # the 11 prefill steps run before the first logits write, about 1.9 s
rows = []
for line in open(path):
    if line.startswith("#"):
        continue
    f = line.split()
    d = {kv.split("=")[0]: int(kv.split("=")[1]) for kv in f[1:] if kv.split("=")[1].lstrip("-").isdigit()}
    rows.append((float(f[0]), d))
print(f"samples {len(rows)} from {rows[0][0]:.1f} to {rows[-1][0]:.1f}; median interval {st.median(b[0] - a[0] for a, b in zip(rows, rows[1:])):.3f} s")
keys = ["power1", "power2", "power4", "fan1", "fan2", "temp3", "temp4"]


def show(label, lo, hi):
    sel = [(t, d) for t, d in rows if lo <= t <= hi]
    if not sel:
        print(f"{label}: no samples in [{lo:.1f}, {hi:.1f}]")
        return
    out = {k: st.mean(d[k] for _, d in sel) / (1e6 if k.startswith("power") else 1) for k in keys}
    print(f"{label}: n={len(sel)} " + " ".join(f"{k}={v:.2f}" if k.startswith("power") else f"{k}={v:.0f}" for k, v in out.items()))


names = list(arms)
for i, n in enumerate(names):
    lo, hi = arms[n]
    show(f"arm {n} decode [{lo - PRE:.1f}..{hi:.1f}]", lo - PRE, hi)
    if i + 1 < len(names):
        show(f"idle after {n} [{hi + 6:.1f}..{arms[names[i + 1]][0] - PRE - 4:.1f}]", hi + 6, arms[names[i + 1]][0] - PRE - 4)
show("U arms decode pooled", 0, 0)
for n in ("1U", "3U"):
    lo, hi = arms[n]
    print(n, "samples in decode window:", [(round(t - lo, 1), round(d['power1'] / 1e6, 1), round(d['power4'] / 1e6, 1)) for t, d in rows if lo - PRE <= t <= hi])
for n in ("2P", "4P"):
    lo, hi = arms[n]
    print(n, "samples in decode window:", [(round(t - lo, 1), round(d['power1'] / 1e6, 1), round(d['power4'] / 1e6, 1)) for t, d in rows if lo - PRE <= t <= hi])
