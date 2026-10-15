# Qwen staged decode on the M2 Max: pinned to the performance cores against unpinned

Date: 2026-10-15. One Apple M2 Max, one fresh boot (id `b010a7cd`), kernel 7.1.12-2-20261013.18-sep-ARCH, governor performance, the driver holding 0 bytes before the run. One session: the 38 Qwen3.8-2B programs loaded once, four arms in the fixed order U P U P, 16 generated steps each on prompt p001, at least 60 s idle before each arm.

The registered speed gate FAILED. The cause of the slow steps is measured. This receipt claims no speed result.

## What each arm did

- Arm U: the unchanged mask, CPUs 0-11.
- Arm P: CPUs 4-11 on every task of the decode process and of the `ane-session` child, set between arms and read back from `/proc/PID/task/TID/status` at the start and the end of each arm.
- `OPENBLAS_NUM_THREADS=4` in both arms. Same programs, port tables, prompt, driver and numerics in all four.

CPUs 0-3 are the efficiency cores (`cpu_capacity` 561, 2.4 GHz). CPUs 4-11 are the performance cores (`cpu_capacity` 1024, 3.26 GHz).

## Registered gates

| Gate | Result |
|---|---|
| The 16 token ids of every arm equal the anchor ids; the fp32 logits hash is the same in all four arms | PASS. All four hash to `5c359d366ad883ea...`, the hash of the earlier runs on default BLAS threads. |
| One boot; `bo_total_bytes` constant after the load (2,805,055,488); no EXCH or STOP line; the affinity readback equals the arm mask on every task | PASS |
| Pooled mean step P over U at most 0.95 | MET: 0.9266 (P arms 168.41 and 167.82 ms, U arms 171.86 and 190.99 ms) |
| The pooled difference is larger than the A/A spread of the two U arms | FAILED: 13.31 ms against 19.14 ms |

A failed gate stays failed. The second clause assumed that the spread of the unpinned arms is noise. In this run the spread was the effect.

## What happened in the U arms

Raw step walls in ms (`data/summary.json`, `raw_walls_s`):

```
1U 171.2 170.6 170.8 171.7 172.1 171.8 171.7 172.5 172.3 171.7 171.7 172.2 172.7 172.4 172.3 171.9
2P 167.7 166.8 168.5 167.4 168.0 166.8 167.2 169.1 170.8 168.7 169.0 170.2 168.9 167.0 167.7 170.7
3U 171.2 172.3 171.9 172.4 172.1 195.6 199.6 200.4 201.2 202.0 201.6 198.7 197.7 199.7 199.5 200.0
4P 168.4 167.2 167.5 168.9 167.9 167.7 167.9 168.4 167.2 168.4 168.4 167.6 167.6 167.2 167.1 167.9
```

- Arm 1 U stayed on performance cores 4-7 for all 16 steps.
- Arm 3 U ran its first 5 generated steps on performance cores (171.99 ms). At the sixth step the scheduler moved the decode thread and the session child to the efficiency cores. 28 of 38 calls ran there in that step (195.6 ms). In the next 10 steps all 38 calls ran there.
- Those 10 steps: 200.04 ms per step. The sum of the call walls was 173.39 ms against 146.19 ms (+27.2 ms). The sum of the exec times did not change (130.54 against 130.73 ms). One call took 4.563 ms against 3.844 ms (+0.72 ms). The decode thread used 28.4 ms of CPU against 9.3 ms and the child 30.8 against 16.8 ms. The run-queue wait stayed near 0, so the threads were not waiting for a core. They ran slower on the core they had.
- 5 of 5 generated steps with no call on an efficiency core were fast. 10 of 10 generated steps with every call on one were slow. The prefill step 0 of arm 3 U also ran all 38 calls on efficiency cores: its ANE phase took 164.7 ms against about 145 ms for its other prefill steps. Counting it, 11 steps ran every call there and all 11 were slower in the ANE phase than every step that ran none (the ANE phase is the sum of the 38 call walls plus the host work between them).
- Both P arms: no step above 169 ms, and 0 of 608 calls on an efficiency core. Efficiency-core busy ticks were 0.25 and 0.31 per step against 14.6 in arm 3 U.

Why the scheduler picks an efficiency core is not tested here. A task that sleeps about 76 percent of each step in the ioctl poll is a candidate for energy-aware wake placement. That is an inference.

## The host head

The head matmul took 21.65 and 21.66 ms in the P arms against 25.55 ms in the fast U arm. In that arm part of the BLAS threads ran on efficiency cores. So the pin gains about 3.9 ms per step even when the ANE phase stays on performance cores. The run cannot separate this from the placement effect of the calls.

## Accounting of one step (arm 1 U)

171.86 ms = 130.73 exec + 15.46 inside the call walls but outside the exec (host pack, pipe, session) + 0.12 between calls + 25.55 head. The call walls sum to 146.19 ms.

## Other readings

- Power, one sample per second from the SMC hardware monitor, 4 to 5 samples per arm: total system power in the decode windows 15.6 to 18.4 W, idle 8.8 to 11.0 W, fans 2315 to 2320 and 2499 to 2507 rpm in every window. No second power state showed. The slow state here is placement.
- `/proc/interrupts` has no line for the ANE in any snapshot. The call waits by a 50 to 100 microsecond poll in the calling thread.

## Limits

One boot and one session. The run does not show that the move repeats on another boot, in a longer decode, or at the same rate. The gate is failed, not passed, and no speed credit follows from it.

## Files

- `data/summary.json`, `data/steps.tsv`: harness output (per-step walls, per-call CPU summary, thread placement, schedstat sums, busy ticks).
- `data/split_by_placement.txt`, `data/analyze_pin.txt`, `data/irq_by_arm.txt`, `data/smc_by_arm.txt`, `data/ARM-TIMES.txt`: the analysis outputs. `tools-used/` holds the harness, its host test, the wrapper, the sampler and the analysis scripts.
- The raw per-call data (`raw-arms.json`, `calls.tsv`, the four 15,892,480-byte logits files and the 1 Hz sensor file) are kept with the lab artifacts. `data/SHA256SUMS-lab-artifacts.txt` lists their hashes.
- `SHA256SUMS`: the hashes of the files in this directory.
