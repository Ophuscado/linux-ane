# ANE and GPU side by side on the 13-inch M1 (2026-10-11)

Question: what does a heavy ANE loop cost a GPU job on the same chip, and what does a GPU job cost the ANE loop?

Chip: 13-inch M1 (T8103). Kernel 7.1.12-2-12.6-sep, in-tree `ane` module, omarchy-mlx 0.32.4.dev202610050725+5c15fba (Vulkan GPU through Mesa's Honeykrisp). One boot, one run of six phases.

## Method

Quiet window under the exclusive-window protocol (see Window evidence). Six phases of about 30 s, in this order, so that drift shows up as a difference between the two copies of the same phase:

1. GPU alone. A chain of fp16 2048 x 2048 matmuls (`gpu_bench.py`), one throughput sample per second.
2. ANE alone. One iteration of `h13-filler.sh` (`ONCE=1 ITER_SECONDS=30`): the Parakeet whole encoder, `ane-run --repeat N`, output compared with the macOS gold, driver `busy_ns` read before and after.
3. Both. The ANE iteration starts, and the GPU loop starts 3 s later and runs 25 s.
4. GPU alone again.
5. Both again.
6. ANE alone again.

## Result

In each of the four ANE processes `ane-run` exited 0 and its output buffer matched the macOS gold byte for byte. `ane-run` writes the output once, after the last call of its `--repeat` run, so this checks the final call of each process (218, 218, 212 and 212 calls), not every call.

| Phase | GPU matmul, TFLOP/s (median of per-second samples) | ANE encoder, ms (median of the iteration) | ANE busy, percent |
|---|---:|---:|---:|
| GPU alone, first | 0.5794 | | |
| GPU alone, second | 0.5788 | | |
| ANE alone, first | | 137.485 | 97.1 |
| ANE alone, second | | 137.536 | 97.7 |
| Both, first | 0.5510 | 141.431 | 96.1 |
| Both, second | 0.5508 | 141.509 | 96.5 |

With both running, the GPU keeps 95.1 percent of its solo rate (0.5509 against 0.5791) and the ANE loop runs 2.9 percent slower (141.47 ms against 137.51 ms). Each pair of repeated phases agrees within about 0.1 percent for the GPU and within 0.1 ms for the ANE. Together the two engines delivered 0.951 + 0.972 of their solo throughput, against 1.0 in total if they took turns.

## What this says about the filler

The heavy ANE loop (97 percent busy in this test) costs a compute-bound GPU job about 5 percent. A GPU job costs the ANE loop about 3 percent. Neither engine needs to be stopped for the other to be useful.

## Limits

- One GPU workload, a compute-bound matmul at 0.58 TFLOP/s. A memory-bound GPU job was not measured. `gpu_bench.py` has a bandwidth mode that was never run. An earlier, uncontrolled run with another lane's `llama-bench` beside the ANE showed a slow-down of 15 to 20 percent (see `2026-10-11-m1-parakeet-operating-point`); this test neither reproduces nor refutes that.
- In the "both" phases the ANE iteration overlaps the GPU loop for about 25 of its 30 seconds, so the 2.9 percent is slightly diluted.
- One run: one boot, 30-second phases, two copies of each phase.
- "Compute-bound" is by construction (a matmul chain). The GPU has no busy counter here, so it was not confirmed by one.
- The ANE-alone medians (137.49 and 137.54 ms, about 215 calls each) sit inside the quiet range of the Parakeet operating-point receipt (137.4 to 139.3 ms). That receipt's cell of record, 137.914 ms, is the median of five 20-call processes.
- Exclusivity is only partly evidenced. See the next section.
- The script waited for another lane's GPU lock holder before the window opened. See Window evidence for what is and is not shown.

## Window evidence

The guard's window log records `set exclusive` at 16:09:15Z and `clear` at 16:27:51Z. The first phase began at 16:24:44Z, after the window had waited for another lane's job to finish. The script's own line when the window opened read: `EXCLUSIVE; guard ANE tickets running: 0; GPU lock holders: 1`. I did not resolve what that one holder was, and I recorded no list of processes holding the GPU during the phases. The log lines and the script's own line are in `data/window.txt`. The two copies of the GPU-alone phase agree within 0.1 percent (0.5794 and 0.5788), which argues against a changing neighbour but does not prove there was none.

## Files

- `scripts/cross.sh` opens the exclusive window and runs the phases; `scripts/cross-host.sh` is the phase script; `scripts/h13-filler.sh` is the ANE loop; `scripts/gpu_bench.py` is the GPU loop. The committed scripts are the versions after the run: `gpu_bench.py` gained a bandwidth mode, and `cross.sh` and `cross-host.sh` a `MODE` switch, after run 1. The default (matmul) path that run 1 used is unchanged. The bandwidth mode never ran.
- `data/run-1.txt` is the raw output of the run.
