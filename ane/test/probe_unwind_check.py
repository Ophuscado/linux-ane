"""probe_unwind_check.py: structural check of ane_platform_probe's error unwinding (run from the repo root).
Rules: the tail labels run in the order dvfs, boost, then the genpd detach (each undoes only what its init did, and falls into the next);
every goto before ane_dvfs_init goes to detach_boost, as it did before the dvfs patch (never to detach_dvfs: nothing to undo yet);
every goto after ane_dvfs_init goes to detach_dvfs, or to one of disable_pm / put_pm / unregister which fall through into it;
ane_dvfs_init's own failure goes to detach_boost."""
import re
import sys

src = open("ane/src/ane_drv.c").read()
body = src[src.index("static int ane_platform_probe"):src.index("static void ane_platform_remove")]
lines = body.split("\n")
dvfs_line = next(i for i, ln in enumerate(lines) if "ane_dvfs_init(ane)" in ln)
tail = body[body.index("unregister:"):]
order = re.findall(r"^(\w+):", tail, re.M)
errs = []
if order[-3:] != ["disable_pm", "detach_dvfs", "detach_boost"]:
    errs.append("tail label order is %s" % order)
for i, ln in enumerate(lines):
    m = re.search(r"goto (\w+);", ln)
    if not m:
        continue
    tgt = m.group(1)
    if i <= dvfs_line + 2:
        if tgt != "detach_boost":
            errs.append("goto %s at probe line %d is before/at ane_dvfs_init" % (tgt, i))
    elif tgt not in ("detach_dvfs", "disable_pm", "put_pm", "unregister"):
        errs.append("goto %s at probe line %d is after ane_dvfs_init" % (tgt, i))
if "goto detach_boost" not in lines[dvfs_line + 2]:
    errs.append("ane_dvfs_init failure must go to detach_boost")
for e in errs:
    print("FAIL:", e)
print("RESULT: %s" % ("all checks passed" if not errs else "%d FAILURES" % len(errs)))
sys.exit(1 if errs else 0)
