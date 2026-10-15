import json
import sys

raw = json.load(open(sys.argv[1]))


def parse(text):
    out = {}
    for line in text.splitlines()[1:]:
        f = line.split()
        if not f or not f[0].endswith(":"):
            continue
        n = 12
        counts = []
        for x in f[1:1 + n]:
            if not x.isdigit():
                break
            counts.append(int(x))
        out[f[0].rstrip(":") + " " + " ".join(f[1 + len(counts):])[:60]] = counts
    return out


for i, r in enumerate(raw):
    b, a = parse(r["interrupts"]["before"]), parse(r["interrupts"]["after"])
    moved = []
    for k, v in a.items():
        if k in b and len(v) == len(b[k]):
            d = [x - y for x, y in zip(v, b[k])]
            if sum(d) > 0:
                moved.append((sum(d), k, d))
    moved.sort(reverse=True)
    print(f"arm{i + 1} {r['arm']}: {len(moved)} lines moved; top 6")
    for s, k, d in moved[:6]:
        print(f"   {s:6d} {k[:50]:50s} per-CPU {d}")
    print("   lines containing 'ane':", [k for k in a if "ane" in k.lower()])
