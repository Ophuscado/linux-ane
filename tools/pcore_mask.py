#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Keep an ANE host process on the performance cores.

On Apple Silicon the kernel can place a task that sleeps most of each step on an efficiency core. A decode
thread and its ane-session child, waiting on 38 ANE calls per step, were placed there on the M2. Every call then
cost 0.72 ms more with the exec time unchanged. All 11 steps whose calls all ran there were slower than every
step on the performance cores, by 27.0 ms in the ANE phase on average. No call of a pinned arm ran there.
Pinning the host side to the performance cores removes the slow state. It changes no arithmetic, so outputs are
byte-identical.

The performance cores are the CPUs at the maximum `cpu_capacity`. The list comes from sysfs on every chip, so it
is never hard-coded. A chip with no capacity files, or with all CPUs at one capacity, is left alone. A caller
that already runs on fewer CPUs keeps its mask: the target is the performance cores it may already use, and a
caller that may use only efficiency cores is not moved.

Call `pin_self()` at the very top of a launcher, before numpy or any other import that starts threads. Threads
and child processes inherit the mask, and the ane-session child inherits it when the launcher spawns it. A
thread that exists before the call keeps its old mask. `pin_tasks(pid)` re-applies the mask to every task of a
running process. `OMARCHY_ANE_NO_PIN=1` turns pinning off; any other value leaves it on.

Shell launchers can use the command line: `taskset -c "$(python3 pcore_mask.py)" cmd`. It prints the list in
`taskset -c` form (for example 4-11) and nothing when there is nothing to pin. It prints the chip's full list
and ignores the caller's current mask, unlike `pin_self()`.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

OPT_OUT = "OMARCHY_ANE_NO_PIN"
SYSFS_CPU = Path("/sys/devices/system/cpu")


def p_core_cpus(root: Path = SYSFS_CPU) -> list[int] | None:
    """The CPUs at the maximum capacity, or None when there are no efficiency cores or a capacity is unreadable."""
    capacity: dict[int, int] = {}
    for entry in Path(root).glob("cpu[0-9]*"):
        try:
            capacity[int(entry.name[3:])] = int((entry / "cpu_capacity").read_text())
        except (OSError, ValueError):
            return None
    if not capacity:
        return None
    top = max(capacity.values())
    cpus = sorted(cpu for cpu, value in capacity.items() if value == top)
    return cpus if len(cpus) < len(capacity) else None


def enabled(env=None) -> bool:
    return (os.environ if env is None else env).get(OPT_OUT) != "1"


def _target(allowed: set[int], root: Path, env) -> set[int] | None:
    """The performance cores the caller may use, or None when pinning does not apply."""
    if not enabled(env):
        return None
    cpus = p_core_cpus(root)
    if not cpus:
        return None
    mask = set(cpus) & allowed
    return mask if mask and mask != allowed else None


def pin_self(root: Path = SYSFS_CPU, env=None) -> list[int] | None:
    """Set the calling thread's mask to the performance cores it may use.

    Returns the mask, or None when nothing changed."""
    mask = _target(set(os.sched_getaffinity(0)), root, env)
    if mask is None:
        return None
    os.sched_setaffinity(0, mask)
    return sorted(mask)


def pin_tasks(pid: int, root: Path = SYSFS_CPU, env=None) -> int:
    """Set the mask of every task of `pid`. Returns how many tasks were set.

    A pid that is gone gives 0. A task that exits meanwhile, or one the caller may not change, is skipped.
    The listing is repeated until no new task appears, so a thread started during the call is covered."""
    done = 0
    seen: set[int] = set()
    for _ in range(4):                    # a thread created meanwhile inherits the old mask, so list again
        try:
            tids = {int(name) for name in os.listdir(f"/proc/{pid}/task")} - seen
        except OSError:
            break
        if not tids:
            break
        seen |= tids
        for tid in sorted(tids):
            try:
                mask = _target(set(os.sched_getaffinity(tid)), root, env)
                if mask is None:
                    continue
                os.sched_setaffinity(tid, mask)
                done += 1
            except (ProcessLookupError, FileNotFoundError, PermissionError):
                continue
    return done


def main(root: Path = SYSFS_CPU, env=None) -> int:
    cpus = p_core_cpus(root) if enabled(env) else None
    if cpus:
        runs, start = [], cpus[0]
        for before, after in zip(cpus, cpus[1:] + [None]):
            if after != before + 1:
                runs.append(str(start) if start == before else f"{start}-{before}")
                start = after
        print(",".join(runs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
