# ANE clock level on the 16-inch (T6001, M1 Max), 2026-10-24

Linux never raised the ANE clock: every job ran at level 0 while macOS reaches
a higher level through the PMP. Four hardware runs on the M1 Max 16-inch
(macOS 26.6.2 trace for the order, Linux 7.1.12 for the runs) found the
sequence and the result; `ane/src/ane_dvfs.c` makes it permanent.

## What the PMP needs (all four parts together)

| Step | Write | Where |
| --- | --- | --- |
| 1 | DVFS_ON := 1 | pmgr DVFS block (macOS RegMap 113 = pmgr ADT reg[39], PA 0x285868000) + 0x2000 |
| 2 | DVFS_CMD := 0x80000000 | same block + 0xa00 |
| 3 | PS-REQ ANE_SYS bit set, wait for PS-ACK | PMP PTD window (0x28e3c0000), update space +0x10000, reg 0xf8 / 0x100 |
| 4 | DVFS_CMD := 0x80000001, level word := 0x2000000000000001 | same block / PTD reg 0x10c |
| release | DVFS_CMD 0x80000010, level word 0x2000000000000000, PS-REQ bit cleared, DVFS_CMD 0x80000000, DVFS_ON := 0 | reverse |

The base of the DVFS block is not ADT reg[113]: AppleT6001PMGR::initRegMaps
(25G83 kernelcache) maps RegMap 113 to ADT index 39. A first attempt at reg[113]
wrote the wrong block and changed nothing; the PMP-only writes (no pmgr words)
changed nothing either.

## Results (rung 20 of the h13 ladder, 200 calls, busy time per job)

| Level | busy per job | vs level 0 |
| --- | --- | --- |
| 0 | 1.0727 ms | 1.00 |
| 1 | 0.6471 ms | 0.603 |
| 2 (A=0) | 0.6331 ms | 0.594 |
| 2 (level word with A=3) | 0.6302 ms | 0.591 |

Qwen decode, one stream, 5 prompts of 64 tokens, step p50: shipped package 0.2009 s (level 0), 0.1158 s
(level 1), 0.1153 s (level 2); int8 package 0.1597 s, 0.0966 s. The logits of all five prompts are
bit-identical at levels 0, 1 and 2. Temperature stayed between 41.0 and 48.1 C over 8 minutes.
The DVFS_CMD word reads back as (level << 4 | level) after a transition, which confirms level 2 was reached
although the program does not get faster (it is memory-bound above level 1). Levels 3 to 5 were never written.

## The driver

`ane_dvfs_up()` runs at the end of runtime resume, `ane_dvfs_down()` at the start of runtime suspend (after
the busy veto). It does nothing unless the DT names the windows (`reg-names` "dvfs" and "ptd"), and it
leaves level 0 (with a reason in sysfs `perf_state`) when the PMP firmware is not running, DVFS_ON is
already set, PS-REQ is already set or PS-ACK does not follow within 50 ms (the sequence is then undone
in the macOS order). The PS-REQ write is a read-modify-write of one bit. All MMIO is ioremap_np.

`ane/test/dvfs_model_test.py` compiles the real `ane_dvfs.c` against fake kernel headers and checks the
exact write sequence up and down, the refusal paths (nothing written), the ACK-timeout unwind and the
read-modify-write.

## Not done here

- Soak: one hour of decode with runtime suspend and resume cycles on the 16-inch.
- T6000 and T6002 (same die-0 layout expected, ADT index 39 unverified): the overlay change is T6001 only.
- The kernel tree's ANE binding allows one `reg` for the T6000 compatible; the extra windows need a binding change
  before the nodes can go into the kernel's device trees.
