#!/usr/bin/env python3
"""Host checks for tools/pcore_mask.py. No device. Fake sysfs trees stand in for the chips. The pin tests change the
affinity of test threads and restore it. The fake chip makes the lowest CPU the caller may use an efficiency core, so
the tests run on a host whose cpuset does not contain CPU 0."""

import os
import subprocess
import sys
import threading
from pathlib import Path
from unittest import mock

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import pcore_mask
from pcore_mask import OPT_OUT, p_core_cpus, pin_self, pin_tasks

NEEDS_TWO_CPUS = pytest.mark.skipif(len(os.sched_getaffinity(0)) < 2, reason="needs two allowed CPUs")


def fake_sysfs(path, capacities):
    for cpu, capacity in enumerate(capacities):
        (path / f"cpu{cpu}").mkdir(parents=True)
        (path / f"cpu{cpu}" / "cpu_capacity").write_text(f"{capacity}\n")
    (path / "cpufreq").mkdir()            # a non-cpu entry the scan must ignore
    return path


def fake_chip(tmp_path, name="chip"):
    """A chip whose lowest allowed CPU is an efficiency core and whose other CPUs are performance cores."""
    allowed = sorted(os.sched_getaffinity(0))
    capacities = [1024] * (max(allowed) + 1)
    capacities[allowed[0]] = 561
    return fake_sysfs(tmp_path / name, capacities), allowed[0], allowed[1:]


@pytest.mark.parametrize("capacities, expected", [
    ([561] * 4 + [1024] * 8, [4, 5, 6, 7, 8, 9, 10, 11]),            # M2 Max
    ([485] * 2 + [1024] * 8, [2, 3, 4, 5, 6, 7, 8, 9]),              # M1 Max
    ([493] * 4 + [1024] * 4, [4, 5, 6, 7]),                          # M1
    ([1024] * 4 + [561] * 2, [0, 1, 2, 3]),                          # efficiency cores numbered last
    ([1024] * 8, None),                                              # no efficiency cores: leave alone
])
def test_p_cores_come_from_capacity(tmp_path, capacities, expected):
    assert p_core_cpus(fake_sysfs(tmp_path, capacities)) == expected


def test_unreadable_or_missing_capacity_is_not_guessed(tmp_path):
    root = fake_sysfs(tmp_path, [561, 1024, 1024])
    (root / "cpu1" / "cpu_capacity").unlink()
    assert p_core_cpus(root) is None
    assert p_core_cpus(tmp_path / "does-not-exist") is None


def test_command_line_prints_a_taskset_list(tmp_path, capsys):
    assert pcore_mask.main(fake_sysfs(tmp_path / "m2", [561] * 4 + [1024] * 8), {}) == 0
    assert capsys.readouterr().out == "4-11\n"
    assert pcore_mask.main(fake_sysfs(tmp_path / "split", [1024, 561, 1024, 1024]), {}) == 0
    assert capsys.readouterr().out == "0,2-3\n"
    assert pcore_mask.main(tmp_path / "m2", {OPT_OUT: "1"}) == 0
    assert capsys.readouterr().out == ""
    run = subprocess.run([sys.executable, str(ROOT / "tools" / "pcore_mask.py")], capture_output=True, text=True,
                         env={**os.environ, OPT_OUT: "1"}, check=True)
    assert run.stdout == ""


@NEEDS_TWO_CPUS
def test_pin_self_sets_the_calling_thread_and_is_inherited(tmp_path):
    root, _, p_cores = fake_chip(tmp_path)
    before = os.sched_getaffinity(0)
    seen = {}
    try:
        assert pin_self(root, env={}) == p_cores
        assert sorted(os.sched_getaffinity(0)) == p_cores
        worker = threading.Thread(target=lambda: seen.update(mask=sorted(os.sched_getaffinity(0))))
        worker.start()
        worker.join()
        assert seen["mask"] == p_cores                                  # a thread started after the pin inherits it
    finally:
        os.sched_setaffinity(0, before)


@NEEDS_TWO_CPUS
def test_opt_out_and_nothing_to_pin_change_nothing(tmp_path):
    root, _, _ = fake_chip(tmp_path)
    before = os.sched_getaffinity(0)
    assert pin_self(root, env={OPT_OUT: "1"}) is None
    assert pin_self(fake_sysfs(tmp_path / "flat", [1024] * (max(before) + 1)), env={}) is None
    assert os.sched_getaffinity(0) == before
    assert pcore_mask.enabled({}) and not pcore_mask.enabled({OPT_OUT: "1"}) and pcore_mask.enabled({OPT_OUT: "0"})


@NEEDS_TWO_CPUS
def test_an_existing_narrower_mask_is_respected(tmp_path):
    """A caller already limited to performance cores keeps its mask: the target equals the allowed set."""
    root, _, p_cores = fake_chip(tmp_path)
    before = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, set(p_cores))
        assert pin_self(root, env={}) is None
        assert os.sched_getaffinity(0) == set(p_cores)
    finally:
        os.sched_setaffinity(0, before)


@NEEDS_TWO_CPUS
def test_a_caller_that_may_use_only_efficiency_cores_is_not_moved(tmp_path):
    """The target would be empty; setting an empty mask raises EINVAL, so the caller is left as it is."""
    root, e_core, _ = fake_chip(tmp_path)
    before = os.sched_getaffinity(0)
    try:
        os.sched_setaffinity(0, {e_core})
        assert pin_self(root, env={}) is None
        assert os.sched_getaffinity(0) == {e_core}
    finally:
        os.sched_setaffinity(0, before)


@NEEDS_TWO_CPUS
def test_pin_tasks_sets_every_task_of_a_process(tmp_path):
    root, _, p_cores = fake_chip(tmp_path)
    stop = threading.Event()
    threads = [threading.Thread(target=stop.wait) for _ in range(3)]
    for thread in threads:
        thread.start()
    allowed = set(os.sched_getaffinity(0))
    try:
        assert pin_tasks(os.getpid(), root, env={}) >= 4                # the main thread and the three workers
        for name in os.listdir(f"/proc/{os.getpid()}/task"):
            assert set(os.sched_getaffinity(int(name))) == set(p_cores), name
    finally:
        stop.set()
        for thread in threads:
            thread.join()
        for name in os.listdir(f"/proc/{os.getpid()}/task"):
            try:
                os.sched_setaffinity(int(name), allowed)                # a joined thread may linger in /proc
            except ProcessLookupError:
                pass


@NEEDS_TWO_CPUS
def test_pin_tasks_skips_a_task_that_exits_and_a_pid_that_is_gone(tmp_path):
    root, _, _ = fake_chip(tmp_path)
    with mock.patch.object(pcore_mask.os, "sched_setaffinity", side_effect=ProcessLookupError):
        assert pin_tasks(os.getpid(), root, env={}) == 0
    with mock.patch.object(pcore_mask.os, "sched_setaffinity", side_effect=PermissionError):
        assert pin_tasks(os.getpid(), root, env={}) == 0
    child = subprocess.Popen([sys.executable, "-c", "pass"])
    child.wait()
    assert pin_tasks(child.pid, root, env={}) == 0


@NEEDS_TWO_CPUS
def test_pin_tasks_covers_a_thread_that_appears_during_the_call(tmp_path):
    """A helper thread that has not been pinned yet starts a thread right after the first listing. The new thread
    inherits the helper's old mask, so only a second listing finds and pins it."""
    root, _, p_cores = fake_chip(tmp_path)
    stop, spawn, spawned = threading.Event(), threading.Event(), threading.Event()
    late = []

    def helper():
        spawn.wait()
        thread = threading.Thread(target=stop.wait)
        thread.start()
        late.append(thread)
        spawned.set()

    helper_thread = threading.Thread(target=helper)
    helper_thread.start()
    real = os.listdir
    calls = {"n": 0}

    def listing(path):
        calls["n"] += 1
        names = real(path)                                              # the listing as it stood before the new thread
        if calls["n"] == 1:
            spawn.set()
            assert spawned.wait(5)
        return names

    allowed = set(os.sched_getaffinity(0))
    try:
        with mock.patch.object(pcore_mask.os, "listdir", listing):
            pin_tasks(os.getpid(), root, env={})
        assert late
        assert set(os.sched_getaffinity(late[0].native_id)) == set(p_cores)
        assert calls["n"] >= 2
    finally:
        stop.set()
        spawn.set()
        helper_thread.join()
        for thread in late:
            thread.join()
        for name in os.listdir(f"/proc/{os.getpid()}/task"):
            try:
                os.sched_setaffinity(int(name), allowed)
            except ProcessLookupError:
                pass
