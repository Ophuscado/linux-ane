// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ANE clock level (T6001, M1 Max): macOS raises the ANE clock through the PMP and the
 * pmgr DVFS block; Linux never did, so every ANE job ran at the lowest level (level 0).
 * Measured on the 16-inch (T6001) under Linux, receipt receipts/2026-10-24-ane-level-driver:
 *   - level 1 cuts the busy time per job of a bandwidth-bound rung by 40 percent
 *     (1.07 -> 0.65 ms) and a full Qwen decode step by 42 percent (0.2009 -> 0.1158 s,
 *     4.98 -> 8.64 tokens per second); the logits are bit-identical at every level;
 *   - level 2 adds under 3 percent (the program is then memory-bound), levels above
 *     2 were never written, so this file requests level 1 only.
 *
 * What the PMP wants, in the order macOS writes it (a macOS 26.6.2 trace of three ANE power cycles),
 * and in the order the receipt's runs used with the ANE already powered:
 *   up:   DVFS_ON := 1; DVFS_CMD := 0x80000000; PS-REQ bit ANE_SYS set (wait for PS-ACK);
 *         DVFS_CMD := 0x80000001; level word := 0x2000000000000001
 *   down: DVFS_CMD := 0x80000010; level word := 0x2000000000000000; PS-REQ bit cleared;
 *         DVFS_CMD := 0x80000000; DVFS_ON := 0
 * DVFS_ON and DVFS_CMD live in the pmgr block macOS calls RegMap 113 (ADT reg[39], PA 0x285868000,
 * DVFS_ON at +0x2000, DVFS_CMD at +0xa00). The PS-REQ word and the level word live in the PMP PTD
 * window (read entities at +reg*16, update space at +0x10000 + reg*8; PS-REQ reg 0xf8, PS-ACK
 * reg 0x100, PMP-STATUS reg 1, ANE level reg 0x10c).
 *
 * Evidence for every address, offset and word above (kernelcache and trace hashes, run logs): receipts/2026-10-24-ane-level-driver/README.md,
 * section Evidence; that receipt and this file were first committed together in 2bfb216.
 *
 * Safety:
 *  - Optional. The DT must name both windows (reg-names "dvfs" and "ptd"); without them the
 *    driver behaves as before. Level control needs the PMP firmware running (PMP-STATUS reads 1)
 *    and the ANE partition up; otherwise it stays at level 0 and says so in sysfs perf_state.
 *  - The PTD window sits in the PMS SRAM power domain, which the DT lists as an ANE power domain,
 *    so it is powered whenever this runs. All access is ioremap_np (of_iomap takes the bus's
 *    nonposted-mmio into account); a posted mapping took an SError in the first experiments.
 *  - The writes are exactly the words above: DVFS_ON 0/1, DVFS_CMD 0x80000000/1/10, the ANE_SYS
 *    bit of PS-REQ and the level words 0 and 1. The up sequence starts only when the hardware words
 *    are the ones the proving runs saw (PS-REQ 0x60003000, level word 0 or a level word, DVFS_ON 0,
 *    PMP-STATUS 1) and the PS-REQ readback after the ACK is 0x60003400; any other value leaves level 0.
 *    A failure at any step (PS-ACK missing) unwinds in the macOS order and leaves level 0; after three
 *    PS-ACK timeouts the driver stops trying until it is reloaded.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "ane.h"

#define DVFS_ON_OFF		0x2000
#define DVFS_CMD_OFF		0xa00
#define DVFS_CMD_BASE		0x80000000U

#define PTD_UPDATE_OFF		0x10000
#define PTD_REG_PMP_STATUS	0x001
#define PTD_REG_PS_REQ		0x0f8
#define PTD_REG_PS_ACK		0x100
#define PTD_REG_ANE_LEVEL	0x10c
#define PTD_PMP_RUNNING		0x1
#define PS_ANE_SYS		BIT_ULL(10)
/* The PS-REQ words the runs that proved this sequence saw: booted, and with ANE_SYS requested. */
#define PS_REQ_BOOT		0x0000000060003000ULL
#define PS_REQ_UP		(PS_REQ_BOOT | PS_ANE_SYS)
#define ANE_DVFS_MAX_TIMEOUTS	3
#define PTD_LEVEL_WORD		0x2000000000000000ULL

#define ANE_DVFS_LEVEL		1
#define ANE_DVFS_WINDOW_SIZE	0x4000
#define ANE_PTD_WINDOW_SIZE	0x20000
#define ANE_DVFS_STEP_US	1000

static u64 ptd_read(struct ane_device *ane, u32 reg)
{
	return readq(ane->dvfs.ptd + (u64)reg * 16);
}

static void ptd_write(struct ane_device *ane, u32 reg, u64 val)
{
	writeq(val, ane->dvfs.ptd + PTD_UPDATE_OFF + (u64)reg * 8);
}

static void dvfs_step(void)
{
	usleep_range(ANE_DVFS_STEP_US, ANE_DVFS_STEP_US * 2);
}

static void dvfs_cmd(struct ane_device *ane, u32 prev, u32 new)
{
	writel(DVFS_CMD_BASE | prev << 4 | new, ane->dvfs.dvfs + DVFS_CMD_OFF);
}

/* The release half, in the macOS order. Safe to call from any point of the up sequence. */
static void ane_dvfs_release(struct ane_device *ane, bool level_set, bool psreq_set)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (level_set) {
		dvfs_cmd(ane, ANE_DVFS_LEVEL, 0);
		dvfs_step();
		ptd_write(ane, PTD_REG_ANE_LEVEL, PTD_LEVEL_WORD);
		dvfs_step();
	}
	if (psreq_set) {
		ptd_write(ane, PTD_REG_PS_REQ, ptd_read(ane, PTD_REG_PS_REQ) & ~PS_ANE_SYS);
		dvfs_step();
	}
	dvfs_cmd(ane, 0, 0);
	dvfs_step();
	writel(0, d->dvfs + DVFS_ON_OFF);
}

/* Called at the end of runtime resume: the partition is up and the engine is enabled. */
void ane_dvfs_up(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;
	u64 req, ack, lvl;
	int err;

	if (!d->dvfs || !d->ptd || d->active)
		return;
	if (d->ack_timeouts >= ANE_DVFS_MAX_TIMEOUTS) {
		d->state = "disabled-after-ps-ack-timeouts";
		return;
	}

	if ((ptd_read(ane, PTD_REG_PMP_STATUS) & PTD_PMP_RUNNING) != PTD_PMP_RUNNING) {
		d->state = "pmp-not-running";
		return;
	}
	if (readl(d->dvfs + DVFS_ON_OFF)) {
		d->state = "dvfs-busy";
		return;
	}
	req = ptd_read(ane, PTD_REG_PS_REQ);
	if (req != PS_REQ_BOOT) {
		d->state = (req & PS_ANE_SYS) ? "ps-req-already-set" : "ps-req-unexpected";
		return;
	}
	lvl = ptd_read(ane, PTD_REG_ANE_LEVEL);
	if (lvl != 0 && (lvl < PTD_LEVEL_WORD || lvl > (PTD_LEVEL_WORD | 5))) {
		d->state = "level-word-unexpected";
		return;
	}

	writel(1, d->dvfs + DVFS_ON_OFF);
	dvfs_step();
	dvfs_cmd(ane, 0, 0);
	dvfs_step();
	/* Re-read right before the read-modify-write: nothing else may have changed the word meanwhile. */
	req = ptd_read(ane, PTD_REG_PS_REQ);
	if (req != PS_REQ_BOOT) {
		d->state = "ps-req-changed";
		ane_dvfs_release(ane, false, false);
		return;
	}
	ptd_write(ane, PTD_REG_PS_REQ, req | PS_ANE_SYS);
	err = read_poll_timeout(ptd_read, ack, ack & PS_ANE_SYS, 200, 50000, false,
				ane, PTD_REG_PS_ACK);
	if (err) {
		d->ack_timeouts++;
		d->state = "ps-ack-timeout";
		ane_dvfs_release(ane, false, true);
		return;
	}
	if (ptd_read(ane, PTD_REG_PS_REQ) != PS_REQ_UP) {
		d->state = "ps-req-readback-unexpected";
		ane_dvfs_release(ane, false, true);
		return;
	}
	dvfs_cmd(ane, 0, ANE_DVFS_LEVEL);
	dvfs_step();
	ptd_write(ane, PTD_REG_ANE_LEVEL, PTD_LEVEL_WORD | ANE_DVFS_LEVEL);

	d->active = true;
	d->state = "level-1";
	dev_dbg(ane->dev, "ANE level %d requested (DVFS_CMD reads 0x%08x)\n", ANE_DVFS_LEVEL,
		readl(d->dvfs + DVFS_CMD_OFF));
}

/* Called at the start of runtime suspend, after the busy veto and before the partition goes down. */
void ane_dvfs_down(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (!d->active)
		return;
	ane_dvfs_release(ane, true, true);
	d->active = false;
	d->state = "level-0";
}

static ssize_t perf_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ane_device *ane = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", ane->dvfs.state);
}
static DEVICE_ATTR_RO(perf_state);

static void __iomem *ane_dvfs_map(struct ane_device *ane, const char *name, resource_size_t want)
{
	struct device_node *np = ane->dev->of_node;
	struct resource res;
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0 || of_address_to_resource(np, idx, &res) || resource_size(&res) != want)
		return NULL;
	return of_iomap(np, idx);
}

int ane_dvfs_init(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;
	int err;

	d->state = "not-configured";
	d->dvfs = ane_dvfs_map(ane, "dvfs", ANE_DVFS_WINDOW_SIZE);
	d->ptd = ane_dvfs_map(ane, "ptd", ANE_PTD_WINDOW_SIZE);
	if (!d->dvfs || !d->ptd) {
		if (d->dvfs)
			iounmap(d->dvfs);
		if (d->ptd)
			iounmap(d->ptd);
		d->dvfs = d->ptd = NULL;
		return 0;
	}
	d->state = "level-0";
	err = device_create_file(ane->dev, &dev_attr_perf_state);
	if (err) {
		iounmap(d->dvfs);
		iounmap(d->ptd);
		d->dvfs = d->ptd = NULL;
		d->state = "not-configured";
	}
	return err;
}

void ane_dvfs_exit(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (!d->dvfs)
		return;
	device_remove_file(ane->dev, &dev_attr_perf_state);
	ane_dvfs_down(ane);
	iounmap(d->dvfs);
	iounmap(d->ptd);
	d->dvfs = d->ptd = NULL;
}
