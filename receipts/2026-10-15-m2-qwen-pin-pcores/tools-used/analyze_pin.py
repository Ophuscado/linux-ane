import json
import statistics as st
import sys
from pathlib import Path

D = Path(sys.argv[1])
raw = json.load(open(D / 'ab/raw-arms.json'))
summ = json.load(open(D / 'ab/summary.json'))
print("== per-arm (order U P U P)")
for i, (r, a) in enumerate(zip(raw, summ['arms'])):
    g = [x for x in r['rows'] if x['generated']]
    calls = [c for x in g for c in x['calls']]
    cpus = [c['cpu'] for c in calls]
    onE = sum(c < 4 for c in cpus)
    byc = {}
    for c in cpus:
        byc[c] = byc.get(c, 0) + 1
    ee = [x['wall_s'] for x in g]
    print(f"arm{i+1} {r['arm']}: mean {a['mean_s']*1000:.2f} ms p50 {a['p50_s']*1000:.2f} p90 {a['p90_s']*1000:.2f} max {a['max_s']*1000:.2f}; ane {a['ane_mean_s']*1000:.2f} head {a['head_mean_s']*1000:.2f} between {a['between_calls_mean_ms']:.2f} callwallsum {a['call_wall_sum_mean_ms']:.2f} execsum {a['exec_sum_mean_ms']:.2f} outside_exec {a['outside_exec_mean_ms']:.2f}")
    print(f"   calls on E {onE}/{len(cpus)}; calls by cpu {dict(sorted(byc.items()))}; steps decode_cpu on E {a['steps_decode_cpu_on_E']}, child_cpu on E {a['steps_child_cpu_on_E']}; any-thread-on-E steps {a['steps_any_thread_on_E']}; E ticks {a['busy_ticks_on_E_mean']:.2f} P ticks {a['busy_ticks_on_P_mean']:.2f}")
    print(f"   schedstat ane window: {a['schedstat_ane_ms']}")
    print("   raw step walls ms:", ' '.join(f"{w*1000:.1f}" for w in ee))
    hs = [x['head_s'] for x in g]
    print(f"   head ms mean {st.mean(hs)*1000:.2f} min {min(hs)*1000:.2f} max {max(hs)*1000:.2f}")
print()
print("== per-step detail U arms")
for i in (0, 2):
    r = raw[i]
    print(f"arm{i+1} {r['arm']}")
    for x in [y for y in r['rows'] if y['generated']]:
        ce = sum(c['cpu'] < 4 for c in x['calls'])
        sd = x['schedstat_ane']
        dec_on = sum(v[0] for v in sd['decode'].values()) / 1e6
        ch_on = sum(v[0] for v in sd['child'].values()) / 1e6
        ch_w = sum(v[1] for v in sd['child'].values()) / 1e6
        dec_w = sum(v[1] for v in sd['decode'].values()) / 1e6
        print(f"  pos {x['pos']:3d} wall {x['wall_s']*1000:6.1f} ane {x['ane_s']*1000:6.1f} callsum {sum(c['wall_s'] for c in x['calls'])*1000:6.1f} exec {sum(c['exec_ms'] for c in x['calls']):6.1f} head {x['head_s']*1000:5.1f} calls_on_E {ce:2d} dec_cpu {x['decode_cpu']:2d} child_cpu {x['child_cpu']:2d} dec_on {dec_on:5.1f} dec_wait {dec_w:.3f} ch_on {ch_on:5.1f} ch_wait {ch_w:.3f}")
