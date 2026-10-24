"""dvfs_model_test.py: userspace model test of the REAL ane/src/ane_dvfs.c with fake kernel headers (no hardware).
usage: python3 ane/test/dvfs_model_test.py   (from the repo root; needs gcc)
Checks: the exact write sequence up and down; the refusal paths write nothing; the PS-ACK timeout unwinds in the macOS order."""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
T = os.path.join(ROOT, "ane", "test", "_build")
os.makedirs(T + "/inc/linux", exist_ok=True)

COMMON = r"""
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned long long resource_size_t;
#define __iomem
#define BIT_ULL(n) (1ULL << (n))
#define ETIMEDOUT 110
struct device_node { int x; };
struct device { int x; struct device_node *of_node; };
struct device_attribute { int x; };
struct resource { u64 start, end; };
#define resource_size(r) ((r)->end - (r)->start + 1)
#define DEVICE_ATTR_RO(n) struct device_attribute dev_attr_##n
#define device_create_file(d, a) 0
#define device_remove_file(d, a) ((void)0)
#define dev_get_drvdata(d) ((void *)0)
#define dev_dbg(d, ...) ((void)0)
#define sysfs_emit(b, ...) 0
#define ssize_t long
extern int log_n;
extern char log_buf[8192];
void fake_usleep(void);
#define usleep_range(a, b) fake_usleep()
u32 fake_readl(void *p);
u64 fake_readq(void *p);
void fake_writel(u32 v, void *p);
void fake_writeq(u64 v, void *p);
#define readl fake_readl
#define readq fake_readq
#define writel fake_writel
#define writeq fake_writeq
#define iounmap(p) ((void)0)
int fake_match(struct device_node *np, const char *prop, const char *name);
int fake_addr(struct device_node *np, int idx, struct resource *res);
void *fake_iomap(struct device_node *np, int idx);
#define of_property_match_string fake_match
#define of_address_to_resource fake_addr
#define of_iomap fake_iomap
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before_read, args...) \
	({ int __i, __r = -ETIMEDOUT; for (__i = 0; __i < 5; __i++) { (val) = op(args); if (cond) { __r = 0; break; } } __r; })
"""
for n in ("delay", "device", "io", "iopoll", "of", "of_address"):
    with open(T + "/inc/linux/%s.h" % n, "w") as f:
        f.write('#include "../common.h"\n')
with open(T + "/inc/common.h", "w") as f:
    f.write(COMMON)
shutil.copy(os.path.join(ROOT, "ane", "src", "ane_dvfs.c"), T + "/ane_dvfs.c")
with open(T + "/ane.h", "w") as f:
    f.write(r"""
#include "inc/common.h"
struct ane_device {
	struct device *dev;
	struct ane_dvfs { void __iomem *dvfs; void __iomem *ptd; bool active; const char *state; } dvfs;
};
int ane_dvfs_init(struct ane_device *ane);
void ane_dvfs_exit(struct ane_device *ane);
void ane_dvfs_up(struct ane_device *ane);
void ane_dvfs_down(struct ane_device *ane);
""")

TEST = r"""
#include "ane_dvfs.c"

int log_n;
char log_buf[8192];
static u64 mem_pa[64], mem_val[64];
static int mem_n;
static int ack_mode = 1;	/* 1: PS-ACK follows PS-REQ, 0: never */
static int no_regs;
static struct device dev0;
static struct device_node np0;
#define DVFS_BASE 0x285868000ULL
#define PTD_BASE 0x28e3c0000ULL

static u64 get(u64 pa) { int i; for (i = 0; i < mem_n; i++) if (mem_pa[i] == pa) return mem_val[i]; return 0; }
static void put(u64 pa, u64 v) { int i; for (i = 0; i < mem_n; i++) if (mem_pa[i] == pa) { mem_val[i] = v; return; } mem_pa[mem_n] = pa; mem_val[mem_n++] = v; }
void fake_usleep(void) { }
u32 fake_readl(void *p) { return (u32)get((u64)(uintptr_t)p); }
u64 fake_readq(void *p) { return get((u64)(uintptr_t)p); }
void fake_writel(u32 v, void *p) { log_n += sprintf(log_buf + log_n, "W32 %llx %08x\n", (u64)(uintptr_t)p, v); put((u64)(uintptr_t)p, v); }
void fake_writeq(u64 v, void *p)
{
	u64 pa = (u64)(uintptr_t)p;

	log_n += sprintf(log_buf + log_n, "W64 %llx %016llx\n", pa, v);
	/* update space reg*8 shows in the read entity reg*16; PS-ACK follows PS-REQ */
	put(PTD_BASE + (pa - PTD_BASE - 0x10000) / 8 * 16, v);
	if (pa == PTD_BASE + 0x10000 + 0xf8 * 8 && ack_mode)
		put(PTD_BASE + 0x100 * 16, v);
}
int fake_match(struct device_node *np, const char *prop, const char *name) { if (no_regs) return -22; return !strcmp(name, "dvfs") ? 1 : 2; }
int fake_addr(struct device_node *np, int idx, struct resource *res)
{
	if (idx == 1) { res->start = DVFS_BASE; res->end = DVFS_BASE + 0x3fff; }
	else { res->start = PTD_BASE; res->end = PTD_BASE + 0x1ffff; }
	return 0;
}
void *fake_iomap(struct device_node *np, int idx) { return (void *)(uintptr_t)(idx == 1 ? DVFS_BASE : PTD_BASE); }

static struct ane_device ane;
static int fails;
#define EXPECT(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static void fresh(void)
{
	mem_n = 0; log_n = 0; log_buf[0] = 0; ack_mode = 1; no_regs = 0;
	memset(&ane, 0, sizeof(ane));
	ane.dev = &dev0;
	dev0.x = 0;
	((struct device *)&dev0)->x = 0;
	put(PTD_BASE + 1 * 16, 1);			/* PMP-STATUS running */
	put(PTD_BASE + 0xf8 * 16, 0x60003000ULL);	/* PS-REQ as booted */
	put(PTD_BASE + 0x100 * 16, 0x60003000ULL);	/* PS-ACK */
	ane_dvfs_init(&ane);
}

int main(void)
{
	static const char *const up = "W32 28586a000 00000001\nW32 285868a00 80000000\nW64 28e3d07c0 0000000060003400\nW32 285868a00 80000001\nW64 28e3d0860 2000000000000001\n";
	static const char *const dn = "W32 285868a00 80000010\nW64 28e3d0860 2000000000000000\nW64 28e3d07c0 0000000060003000\nW32 285868a00 80000000\nW32 28586a000 00000000\n";
	char want[1024];

	np0.x = 0;
	dev0.x = 0;
	/* 1: the full cycle, twice */
	fresh();
	EXPECT(ane.dvfs.dvfs && ane.dvfs.ptd, "both windows mapped");
	ane_dvfs_up(&ane);
	EXPECT(!strcmp(log_buf, up), "up sequence is exactly the D2c one");
	EXPECT(ane.dvfs.active && !strcmp(ane.dvfs.state, "level-1"), "active, level-1");
	ane_dvfs_up(&ane);
	EXPECT(!strcmp(log_buf, up), "a second up writes nothing");
	ane_dvfs_down(&ane);
	snprintf(want, sizeof(want), "%s%s", up, dn);
	EXPECT(!strcmp(log_buf, want), "down sequence is exactly the D2c release");
	EXPECT(!ane.dvfs.active && !strcmp(ane.dvfs.state, "level-0"), "inactive, level-0");
	ane_dvfs_down(&ane);
	EXPECT(!strcmp(log_buf, want), "a second down writes nothing");
	log_n = 0; log_buf[0] = 0;
	ane_dvfs_up(&ane);
	EXPECT(!strcmp(log_buf, up), "the cycle repeats");
	/* 2: PMP not running */
	fresh(); put(PTD_BASE + 16, 0);
	ane_dvfs_up(&ane);
	EXPECT(log_n == 0 && !strcmp(ane.dvfs.state, "pmp-not-running") && !ane.dvfs.active, "PMP off: nothing written");
	/* 3: DVFS_ON already set */
	fresh(); put(DVFS_BASE + 0x2000, 1);
	ane_dvfs_up(&ane);
	EXPECT(log_n == 0 && !strcmp(ane.dvfs.state, "dvfs-busy"), "DVFS_ON set: nothing written");
	/* 4: PS-REQ bit already set by somebody else */
	fresh(); put(PTD_BASE + 0xf8 * 16, 0x60003400ULL);
	ane_dvfs_up(&ane);
	EXPECT(log_n == 0 && !strcmp(ane.dvfs.state, "ps-req-already-set"), "PS-REQ bit set: nothing written");
	/* 5: no PS-ACK: unwind in the macOS order, level 0 */
	fresh(); ack_mode = 0;
	ane_dvfs_up(&ane);
	snprintf(want, sizeof(want), "W32 28586a000 00000001\nW32 285868a00 80000000\nW64 28e3d07c0 0000000060003400\nW64 28e3d07c0 0000000060003000\nW32 285868a00 80000000\nW32 28586a000 00000000\n");
	EXPECT(!strcmp(log_buf, want), "ACK timeout unwinds: psreq clear, cmd 0, DVFS_ON 0");
	EXPECT(!ane.dvfs.active && !strcmp(ane.dvfs.state, "ps-ack-timeout"), "state ps-ack-timeout");
	/* 6: DT without the windows */
	fresh(); memset(&ane, 0, sizeof(ane)); ane.dev = &dev0; no_regs = 1; ane_dvfs_init(&ane);
	log_n = 0; ane_dvfs_up(&ane); ane_dvfs_down(&ane);
	EXPECT(!ane.dvfs.dvfs && log_n == 0 && !strcmp(ane.dvfs.state, "not-configured"), "no DT windows: driver does nothing");
	/* 7: other PS-REQ bits survive (read-modify-write) */
	fresh(); put(PTD_BASE + 0xf8 * 16, 0x60003001ULL);
	ane_dvfs_up(&ane); ane_dvfs_down(&ane);
	EXPECT(strstr(log_buf, "W64 28e3d07c0 0000000060003401") && strstr(log_buf, "W64 28e3d07c0 0000000060003001"), "other PS-REQ bits kept");
	/* 8: nothing but the allowed words and addresses, ever */
	{
		char *p = log_buf;

		while ((p = strstr(p, "W32 ")) != 0) {
			unsigned long long a; unsigned v;

			sscanf(p, "W32 %llx %x", &a, &v);
			EXPECT((a == DVFS_BASE + 0x2000 && v <= 1) || (a == DVFS_BASE + 0xa00 && (v == 0x80000000U || v == 0x80000001U || v == 0x80000010U)), "W32 only allowed words");
			p++;
		}
	}
	printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: all checks passed\n", fails);
	return fails != 0;
}
"""
with open(T + "/test.c", "w") as f:
    f.write(TEST)
r = subprocess.run(["gcc", "-Wall", "-I" + T + "/inc", "-I" + T, "-o", T + "/t", T + "/test.c"], capture_output=True, text=True)
errs = [line for line in r.stderr.splitlines() if "error" in line]
if errs:
    print("\n".join(errs))
    sys.exit(2)
sys.exit(subprocess.run([T + "/t"]).returncode)
