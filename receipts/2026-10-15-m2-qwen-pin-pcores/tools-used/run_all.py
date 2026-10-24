import json
import statistics as s
import subprocess
from pathlib import Path

A = Path.home() / ".local/share/apple-silicon-lab/artifacts/w73/m2-qwen-pin-ab-20261015"
an = A / "analysis"
for script, arg, out in (("analyze_pin.py", str(A), "analyze_pin.txt"), ("smc_by_arm.py", str(A / "qwen-pin-smc-20261015T164534Z.txt"), "smc_by_arm.txt"),
                         ("irq_by_arm.py", str(A / "ab/raw-arms.json"), "irq_by_arm.txt")):
    res = subprocess.run(["python3", str(an / script), arg], capture_output=True, text=True, check=True)
    (an / out).write_text(res.stdout)
(an / "ARM-TIMES.txt").write_text(
    "logits.f32 birth and mtime read read-only on the M2 at 16:55Z (arm = the 16 generated steps; 11 prefill steps run about 1.9 s before the first write)\n"
    "arm1-U birth=1792082820.226207438 mtime=1792082822.820108390\n"
    "arm2-P birth=1792082888.421672690 mtime=1792082890.964645683\n"
    "arm3-U birth=1792082956.615314655 mtime=1792082959.533311236\n"
    "arm4-P birth=1792083025.132359302 mtime=1792083027.665364476\n")
r = json.loads((A / "ab/raw-arms.json").read_text())
lines = []
for i, name in ((0, "arm1 U"), (2, "arm3 U")):
    g = [x for x in r[i]["rows"] if x["generated"]]
    allE = [x for x in g if sum(c["cpu"] < 4 for c in x["calls"]) == len(x["calls"])]
    noE = [x for x in g if sum(c["cpu"] < 4 for c in x["calls"]) == 0]
    mixed = [x for x in g if x not in allE and x not in noE]
    for label, grp in (("all 38 calls on E", allE), ("no call on E", noE), ("mixed", mixed)):
        if not grp:
            continue
        lines.append(f"{name} {label}: n={len(grp)} wall {s.mean(x['wall_s'] for x in grp) * 1000:.2f} ms, call-wall sum {s.mean(sum(c['wall_s'] for c in x['calls']) for x in grp) * 1000:.2f}, "
                     f"exec sum {s.mean(sum(c['exec_ms'] for c in x['calls']) for x in grp):.2f}, head {s.mean(x['head_s'] for x in grp) * 1000:.2f}, "
                     f"per-call wall {s.mean(c['wall_s'] for x in grp for c in x['calls']) * 1000:.3f} ms, decode on-cpu {s.mean(sum(v[0] for v in x['schedstat_ane']['decode'].values()) for x in grp) / 1e6:.1f} ms, "
                     f"child on-cpu {s.mean(sum(v[0] for v in x['schedstat_ane']['child'].values()) for x in grp) / 1e6:.1f} ms")
P = [x["wall_s"] for i in (1, 3) for x in r[i]["rows"] if x["generated"]]
g3 = [x for x in r[2]["rows"] if x["generated"] and sum(c["cpu"] < 4 for c in x["calls"]) == 38]
lines.append(f"P arms pooled: n={len(P)} mean {s.mean(P) * 1000:.2f} ms; P over the {len(g3)} all-E steps of arm3: {s.mean(P) / s.mean(x['wall_s'] for x in g3):.4f} (post hoc subset, not the gate)")
(an / "split_by_placement.txt").write_text("\n".join(lines) + "\n")
print("\n".join(lines))
