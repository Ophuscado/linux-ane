/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h18: OPT-IN EXPERIMENTAL bring-up module for the H18 ANE0 of the T8152 (Mac mini M6, J873g).
 *
 * Stages dt, psd and power ran on a Mac mini M6 (see README-bringup.md). Stage boot has not run on any machine.
 * The firmware is preloaded by iBoot (the ADT /arm-io/ane segment-ranges name where) and iBoot programs IORVBAR. This module never writes
 * IORVBAR or a mailbox. Stage boot sets the core's RUN bit once, watches the status and the Postoffice outboxes for a short time, then clears RUN.
 *
 * Stages (`stage` parameter):
 *   dt    : check the device tree windows against the SoC row and log them. No mapping, no MMIO.
 *   psd   : read-only. Map the two PSD slot windows (30 and 31) and read VALID and, when set, STATUS.
 *           The proven PSD probe does exactly these two reads. No write.
 *   power : needs confirm_power=1. Request PSD state `psd_state` (default 0, run) on the first `psd_slots`
 *           slots (default 1: slot 30 only), wait for each acknowledge as pmgr-psd.c does, then read
 *           CPU_STATUS, ASC_STATUS and IORVBAR (CPU_CONTROL with read_ctrl=1, the mailbox status words with read_mbox=1), then put the
 *           slots back to state 6 unless keep_on=1. Every write and read address is logged BEFORE the access.
 *
 *   boot  : needs confirm_power=1 and confirm_boot=1. Like power, then: refuse unless the core is stopped, CPU_CONTROL.RUN is clear and
 *           IORVBAR is exactly the value iBoot leaves (ane_h18_soc.c). Then write CPU_CONTROL |= RUN, watch CPU_STATUS and the status words
 *           of the three candidate Postoffice channels for boot_wait_ms, read at most 8 outbox messages (raw words are logged), write
 *           CPU_CONTROL &= ~RUN and wait for STOPPED. If the core does not stop, the PSD slots stay on and a reboot is required.
 *
 * Nothing autoloads: there is no MODULE_DEVICE_TABLE, and probe returns -EPERM unless optin=t8152.
 *   insmod ane_h18.ko optin=t8152 stage=psd
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h18.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin, "Opt-in key: must be t8152 or probe returns -EPERM.");

static char *stage = "dt";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage, "dt (default, no MMIO), psd (read the PSD slots) or power (request PSD state, needs confirm_power=1).");

static bool confirm_power;
module_param(confirm_power, bool, 0444);
MODULE_PARM_DESC(confirm_power, "Required for stage=power. Off by default.");

static unsigned int psd_state;
module_param(psd_state, uint, 0444);
MODULE_PARM_DESC(psd_state, "Stage=power: PSD state to request, 0 (run) to 5. Default 0.");

static unsigned int psd_slots = 1;
module_param(psd_slots, uint, 0444);
MODULE_PARM_DESC(psd_slots, "Stage=power: how many PSD slots to request, in order (30 then 31). Default 1.");

static bool keep_on;
module_param(keep_on, bool, 0444);
MODULE_PARM_DESC(keep_on, "Stage=power: leave the slots running after the reads. Default off: request state 6 again.");

static bool read_mbox;
module_param(read_mbox, bool, 0444);
MODULE_PARM_DESC(read_mbox, "Stage=power: also read the Postoffice status word of this ANE's channel.");

static bool confirm_boot;
module_param(confirm_boot, bool, 0444);
MODULE_PARM_DESC(confirm_boot, "Required for stage=boot, together with confirm_power=1. Off by default.");

static unsigned int boot_wait_ms = 300;
module_param(boot_wait_ms, uint, 0444);
MODULE_PARM_DESC(boot_wait_ms, "Stage=boot: how long the core runs before RUN is cleared again, ms (10 to 2000). Default 300.");

static bool read_ctrl;
module_param(read_ctrl, bool, 0444);
MODULE_PARM_DESC(read_ctrl, "Stage=power: also read CPU_CONTROL (ASC + 0x44). Read only, never written. Default off.");

static unsigned int psd_timeout_ms = 1000;
module_param(psd_timeout_ms, uint, 0444);
MODULE_PARM_DESC(psd_timeout_ms, "Stage=power: per-slot acknowledge wait, ms. Default 1000 (the ADT psd-timeout-ms).");

enum { ANE_H18_STAGE_DT, ANE_H18_STAGE_PSD, ANE_H18_STAGE_POWER, ANE_H18_STAGE_BOOT };

struct ane_h18 {
	struct device *dev;
	const struct ane_h18_soc *soc;
	void __iomem *psd[ANE_H18_NPSD];
	void __iomem *asc;
	void __iomem *rvbar;
	void __iomem *mbox;
	bool psd_on[ANE_H18_NPSD];
	bool fw_started;
};

static void ane_h18_result(struct ane_h18 *ane, const char *stg, const char *verdict, const char *reason)
{
	dev_crit(ane->dev, "ane_h18 RESULT stage=%s soc=%s verdict=%s reason=%s\n", stg, ane->soc->name, verdict, reason);
}

static int ane_h18_stage_idx(const char *name, unsigned int *out)
{
	if (!strcmp(name, "dt")) { *out = ANE_H18_STAGE_DT; return 0; }
	if (!strcmp(name, "psd")) { *out = ANE_H18_STAGE_PSD; return 0; }
	if (!strcmp(name, "power")) { *out = ANE_H18_STAGE_POWER; return 0; }
	if (!strcmp(name, "boot")) { *out = ANE_H18_STAGE_BOOT; return 0; }
	return -EINVAL;
}

/* Find a named reg entry and require that it is exactly the window the SoC row describes. A stale or shifted
 * device tree must not make the static offsets or the PSD request writes land on another block.
 */
static int ane_h18_window(struct ane_h18 *ane, const char *name, u64 pa, u64 size, int *idx)
{
	struct device_node *np = ane->dev->of_node;
	struct resource res;
	int i = of_property_match_string(np, "reg-names", name);

	if (i < 0 || of_address_to_resource(np, i, &res)) {
		dev_err(ane->dev, "ane_h18 reg-names \"%s\" missing\n", name);
		return -ENODEV;
	}
	if (res.start != pa || resource_size(&res) != size) {
		dev_err(ane->dev, "ane_h18 window %s mismatch: reg start=%#llx size=%#llx, SoC row wants start=%#llx size=%#llx\n",
			name, (u64)res.start, (u64)resource_size(&res), pa, size);
		return -EINVAL;
	}
	*idx = i;
	return 0;
}

static u64 ane_h18_psd_pa(const struct ane_h18_soc *s, unsigned int i)
{
	return ANE_H18_PSD_BASE + ((u64)s->psd_slot[i] << 8);
}

/* Check the numeric properties the SoC row depends on. The live ADT says ane-type 784 and mailbox number 12. */
static int ane_h18_check_props(struct ane_h18 *ane)
{
	struct device_node *np = ane->dev->of_node;
	u32 type = 0, num = 0;

	if (of_property_read_u32(np, "apple,ane-type", &type) || type != ane->soc->ane_type) {
		dev_err(ane->dev, "ane_h18 apple,ane-type=%u, SoC row wants %u\n", type, ane->soc->ane_type);
		return -EINVAL;
	}
	if (of_property_read_u32(np, "msgbox-mailbox-num", &num) || num != ane->soc->mbox_num) {
		dev_err(ane->dev, "ane_h18 msgbox-mailbox-num=%u, SoC row wants %u\n", num, ane->soc->mbox_num);
		return -EINVAL;
	}
	return 0;
}

/* Resolve and check every window; indexes in idx[]: 0 asc, 1 rvbar, 2 mbox, 3.. psd. */
static int ane_h18_check_windows(struct ane_h18 *ane, int *idx)
{
	const struct ane_h18_soc *s = ane->soc;
	char nm[8];
	unsigned int i;
	int ret;

	ret = ane_h18_check_props(ane);
	if (!ret)
		ret = ane_h18_window(ane, "asc", s->asc_pa, s->asc_size, &idx[0]);
	if (!ret)
		ret = ane_h18_window(ane, "rvbar", s->rvbar_pa, s->rvbar_size, &idx[1]);
	if (!ret)
		ret = ane_h18_window(ane, "mbox", s->mbox_pa, s->mbox_size, &idx[2]);
	for (i = 0; !ret && i < ANE_H18_NPSD; i++) {
		snprintf(nm, sizeof(nm), "psd%u", i);
		ret = ane_h18_window(ane, nm, ane_h18_psd_pa(s, i), ANE_H18_PSD_WIN_SIZE, &idx[3 + i]);
	}
	return ret;
}

static void ane_h18_unmap(struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++)
		if (ane->psd[i]) {
			iounmap(ane->psd[i]);
			ane->psd[i] = NULL;
		}
	if (ane->asc) { iounmap(ane->asc); ane->asc = NULL; }
	if (ane->rvbar) { iounmap(ane->rvbar); ane->rvbar = NULL; }
	if (ane->mbox) { iounmap(ane->mbox); ane->mbox = NULL; }
}

static int ane_h18_map_psd(struct ane_h18 *ane, const int *idx)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++) {
		ane->psd[i] = of_iomap(ane->dev->of_node, idx[3 + i]);
		if (!ane->psd[i]) {
			dev_err(ane->dev, "ane_h18 of_iomap psd%u failed\n", i);
			return -ENXIO;
		}
	}
	return 0;
}

/* ---- stage dt ---- */
static int ane_h18_stage_dt(struct ane_h18 *ane, const int *idx)
{
	const struct ane_h18_soc *s = ane->soc;
	unsigned int i;

	(void)idx;
	dev_info(ane->dev, "ane_h18 stage=dt soc=%s ane-type=%u mailbox-num=%u\n", s->name, s->ane_type, s->mbox_num);
	dev_info(ane->dev, "ane_h18 asc pa=%#llx size=%#x rvbar pa=%#llx size=%#x mbox pa=%#llx size=%#x\n",
		 s->asc_pa, s->asc_size, s->rvbar_pa, s->rvbar_size, s->mbox_pa, s->mbox_size);
	for (i = 0; i < ANE_H18_NPSD; i++)
		dev_info(ane->dev, "ane_h18 psd%u slot=%u pa=%#llx\n", i, s->psd_slot[i], ane_h18_psd_pa(s, i));
	ane_h18_result(ane, "dt", "PASS", "dt-windows-match");
	return 0;
}

/* ---- stage psd: read only ---- */
static int ane_h18_stage_psd(struct ane_h18 *ane)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_NPSD; i++) {
		u32 valid, status = 0;

		dev_crit(ane->dev, "ane_h18 read psd%u slot=%u VALID pa=%#llx\n", i, ane->soc->psd_slot[i],
			 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_VALID);
		valid = readl(ane->psd[i] + ANE_H18_PSD_VALID);
		if (valid & 1) {
			/* a STATUS read clears the acknowledge, so only when VALID is set (as pmgr-psd.c) */
			dev_crit(ane->dev, "ane_h18 read psd%u slot=%u STATUS pa=%#llx\n", i, ane->soc->psd_slot[i],
				 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_STATUS);
			status = readl(ane->psd[i] + ANE_H18_PSD_STATUS);
		}
		dev_info(ane->dev, "ane_h18 psd%u slot=%u valid=%#x status=%#x state=%s\n", i, ane->soc->psd_slot[i], valid,
			 status, (valid & 1) ? ((status & ANE_H18_PSD_STATE_MASK) == 0 ? "P0-run" : "other") : "not-valid");
	}
	ane_h18_result(ane, "psd", "PASS", "psd-read-only");
	return 0;
}

/* ---- stage power ---- */
static int ane_h18_psd_request(struct ane_h18 *ane, unsigned int i, u32 state)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(psd_timeout_ms);
	u32 valid = 0, status = 0;

	dev_crit(ane->dev, "ane_h18 write psd%u slot=%u REQUEST=%u pa=%#llx\n", i, ane->soc->psd_slot[i], state,
		 ane_h18_psd_pa(ane->soc, i) + ANE_H18_PSD_REQUEST);
	writel(state, ane->psd[i] + ANE_H18_PSD_REQUEST);
	for (;;) {
		valid = readl(ane->psd[i] + ANE_H18_PSD_VALID);
		if (valid & 1) {
			status = readl(ane->psd[i] + ANE_H18_PSD_STATUS);
			if ((status & ANE_H18_PSD_STATE_MASK) == state) {
				dev_info(ane->dev, "ane_h18 psd%u slot=%u state %u acknowledged (valid=%#x status=%#x)\n", i,
					 ane->soc->psd_slot[i], state, valid, status);
				return 0;
			}
		}
		if (time_after(jiffies, timeout))
			break;
		usleep_range(50, 200);
	}
	dev_err(ane->dev, "ane_h18 psd%u slot=%u request %u NOT acknowledged: valid=%#x status=%#x diag=%#x\n", i,
		ane->soc->psd_slot[i], state, valid, status, readl(ane->psd[i] + ANE_H18_PSD_DIAG));
	return -ETIMEDOUT;
}

/* Request state 6 on every slot this module turned on, in reverse order. Returns how many slots did not acknowledge it. */
static int ane_h18_psd_off(struct ane_h18 *ane)
{
	int i, failed = 0;

	for (i = ANE_H18_NPSD - 1; i >= 0; i--) {
		if (!ane->psd_on[i])
			continue;
		if (ane_h18_psd_request(ane, i, ANE_H18_PSD_P6_RAIL)) {
			dev_warn(ane->dev, "ane_h18 psd%d did not return to state 6\n", i);
			failed++;
		}
		ane->psd_on[i] = false;
	}
	return failed;
}

static int ane_h18_read(struct ane_h18 *ane, const char *name, void __iomem *base, u64 pa, u32 off, u32 size_win, u32 *v)
{
	if ((u64)off + 4 > size_win) {
		dev_err(ane->dev, "ane_h18 refusing read %s +%#x outside its window\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h18 read %s pa=%#llx\n", name, pa + off);
	*v = readl(base + off);
	dev_info(ane->dev, "ane_h18 word=%s pa=%#llx value=%#x\n", name, pa + off, *v);
	return 0;
}

/* Candidate Postoffice channels for this ANE: mailbox number, twice the number, twice the number plus one. */
static u32 ane_h18_po_chan(const struct ane_h18_soc *s, unsigned int cand)
{
	return cand == 0 ? s->mbox_num : s->mbox_num * 2 + (cand - 1);
}

static int ane_h18_stage_power(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 cpu_status = 0, asc_status = 0, cpu_ctrl = 0, rv_lo = 0, rv_hi = 0, po[ANE_H18_PO_NCAND] = { 0 };
	unsigned int i;
	int ret = 0;

	if (!confirm_power) {
		ane_h18_result(ane, "power", "REFUSED", "confirm_power=1 required");
		return -EPERM;
	}
	if (psd_state >= ANE_H18_PSD_P6_RAIL || psd_slots < 1 || psd_slots > ANE_H18_NPSD) {
		ane_h18_result(ane, "power", "REFUSED", "psd_state must be 0 to 5 and psd_slots 1 or 2");
		return -EINVAL;
	}
	for (i = 0; i < psd_slots; i++) {
		ret = ane_h18_psd_request(ane, i, psd_state);
		if (ret) {
			ane_h18_psd_off(ane);
			ane_h18_result(ane, "power", "FAIL", "psd-not-acknowledged");
			return ret;
		}
		ane->psd_on[i] = true;
	}
	/* Everything below is read-only and happens only after every requested slot acknowledged. */
	ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &cpu_status);
	if (!ret)
		ret = ane_h18_read(ane, "ASC_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_STATUS, s->asc_size, &asc_status);
	if (!ret && read_ctrl)
		ret = ane_h18_read(ane, "CPU_CONTROL", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_CONTROL, s->asc_size, &cpu_ctrl);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_LO", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF, s->rvbar_size, &rv_lo);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_HI", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF + 4, s->rvbar_size, &rv_hi);
	/* The channel of this ANE in the Postoffice block is not verified: the lab notebook's T8152 driver notes give
	 * "msgbox_using_postoffice + (msgbox_mailbox_num << 1)" (channel 25), a review reads the same expression as the
	 * packed pair that makes it channel 12, and the SMC node does not fit either. Read the status word of all
	 * three candidate channels (12, 24, 25); they are in one block, the same window, and the log decides. */
	for (i = 0; !ret && read_mbox && i < ANE_H18_PO_NCAND; i++) {
		char nm[16];

		snprintf(nm, sizeof(nm), "PO_STATUS_CH%u", ane_h18_po_chan(s, i));
		ret = ane_h18_read(ane, nm, ane->mbox, s->mbox_pa, ane_h18_po_chan(s, i) * ANE_H18_PO_STRIDE + ANE_H18_PO_STATUS,
				   s->mbox_size, &po[i]);
	}
	if (ret) {
		if (!keep_on)
			ane_h18_psd_off(ane);
		ane_h18_result(ane, "power", "FAIL", "read-refused");
		return ret;
	}
	dev_info(ane->dev, "ane_h18 power cpu_status=%#x asc_status=%#x iorvbar=%#llx psd_state=%u slots=%u keep_on=%d\n",
		 cpu_status, asc_status, ((u64)rv_hi << 32) | rv_lo, psd_state, psd_slots, keep_on);
	if (read_ctrl)
		dev_info(ane->dev, "ane_h18 cpu_control=%#x\n", cpu_ctrl);
	if (read_mbox)
		dev_info(ane->dev, "ane_h18 po_status ch%u=%#x ch%u=%#x ch%u=%#x\n", ane_h18_po_chan(s, 0), po[0], ane_h18_po_chan(s, 1),
			 po[1], ane_h18_po_chan(s, 2), po[2]);
	if (!keep_on && ane_h18_psd_off(ane)) {
		ane_h18_result(ane, "power", "FAIL", "psd-not-returned-to-state-6");
		return -EIO;
	}
	ane_h18_result(ane, "power", "PASS", "psd-acked+asc-read");
	return 0;
}

/* Read the outbox of one Postoffice channel if its status says it holds a message. Logs the raw 16 bytes. Returns 1 if a message was read. */
static int ane_h18_po_pop(struct ane_h18 *ane, unsigned int chan, u32 status)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 off = chan * ANE_H18_PO_STRIDE + ANE_H18_PO_OUTBOX, w[4] = { 0 };
	unsigned int i;

	if ((status & 0xf) == 0)
		return 0;
	if ((u64)off + 16 > s->mbox_size)
		return 0;
	dev_crit(ane->dev, "ane_h18 read outbox ch%u pa=%#llx (status %#x)\n", chan, s->mbox_pa + off, status);
	for (i = 0; i < 4; i++)
		w[i] = readl(ane->mbox + off + 4 * i);
	dev_info(ane->dev, "ane_h18 msg ch%u msg0=%#llx msg1=%#llx\n", chan, ((u64)w[1] << 32) | w[0], ((u64)w[3] << 32) | w[2]);
	return 1;
}

static int ane_h18_stage_boot(struct ane_h18 *ane)
{
	const struct ane_h18_soc *s = ane->soc;
	u32 st = 0, ctrl = 0, rv_lo = 0, rv_hi = 0, last = 0xffffffffu, po;
	unsigned int i, c, waited = 0, msgs = 0, running_seen = 0;
	bool stopped = false;
	int ret = 0;

	if (!confirm_power || !confirm_boot) {
		ane_h18_result(ane, "boot", "REFUSED", "confirm_power=1 and confirm_boot=1 required");
		return -EPERM;
	}
	if (psd_state >= ANE_H18_PSD_P6_RAIL || psd_slots < 1 || psd_slots > ANE_H18_NPSD || boot_wait_ms < 10 || boot_wait_ms > 2000) {
		ane_h18_result(ane, "boot", "REFUSED", "psd_state 0 to 5, psd_slots 1 or 2, boot_wait_ms 10 to 2000");
		return -EINVAL;
	}
	for (i = 0; i < psd_slots; i++) {
		ret = ane_h18_psd_request(ane, i, psd_state);
		if (ret) {
			ane_h18_psd_off(ane);
			ane_h18_result(ane, "boot", "FAIL", "psd-not-acknowledged");
			return ret;
		}
		ane->psd_on[i] = true;
	}
	/* Preflight, all read only: the core must be stopped with RUN clear and IORVBAR exactly as iBoot leaves it. */
	ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &st);
	if (!ret)
		ret = ane_h18_read(ane, "CPU_CONTROL", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_CONTROL, s->asc_size, &ctrl);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_LO", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF, s->rvbar_size, &rv_lo);
	if (!ret)
		ret = ane_h18_read(ane, "IORVBAR_HI", ane->rvbar, s->rvbar_pa, ANE_H18_RVBAR_OFF + 4, s->rvbar_size, &rv_hi);
	if (ret) {
		ane_h18_psd_off(ane);
		ane_h18_result(ane, "boot", "FAIL", "read-refused");
		return ret;
	}
	if ((st & ANE_H18_ST_RUNNING) || !(st & ANE_H18_ST_STOPPED) || (ctrl & ANE_H18_CPU_RUN) || (((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect) {
		dev_err(ane->dev, "ane_h18 preflight: cpu_status=%#x cpu_control=%#x iorvbar=%#llx (want stopped, RUN clear, iorvbar %#llx)\n", st, ctrl,
			((u64)rv_hi << 32) | rv_lo, s->rvbar_expect);
		ane_h18_psd_off(ane);
		ane_h18_result(ane, "boot", "FAIL", "preflight-not-as-iboot-left-it");
		return -EBUSY;
	}
	/* Start the core. From here a reboot may be needed. */
	dev_crit(ane->dev, "ane_h18 write CPU_CONTROL=%#x pa=%#llx (RUN)\n", ctrl | ANE_H18_CPU_RUN, s->asc_pa + ANE_H18_ASC_CPU_CONTROL);
	ane->fw_started = true;
	writel(ctrl | ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);
	for (waited = 0; waited < boot_wait_ms; waited += 10) {
		usleep_range(10000, 11000);
		ret = ane_h18_read(ane, "CPU_STATUS", ane->asc, s->asc_pa, ANE_H18_ASC_CPU_STATUS, s->asc_size, &st);
		if (ret)
			break;
		if (st & ANE_H18_ST_RUNNING)
			running_seen = 1;
		if (st != last) {
			dev_info(ane->dev, "ane_h18 t+%ums cpu_status=%#x\n", waited + 10, st);
			last = st;
		}
		for (c = 0; c < ANE_H18_PO_NCAND && msgs < ANE_H18_PO_MAXMSG; c++) {
			u32 off = ane_h18_po_chan(s, c) * ANE_H18_PO_STRIDE + ANE_H18_PO_STATUS;

			if ((u64)off + 4 > s->mbox_size)
				continue;
			po = readl(ane->mbox + off);
			msgs += ane_h18_po_pop(ane, ane_h18_po_chan(s, c), po);
		}
	}
	/* Stop the core again. */
	ctrl = readl(ane->asc + ANE_H18_ASC_CPU_CONTROL);
	dev_crit(ane->dev, "ane_h18 write CPU_CONTROL=%#x pa=%#llx (clear RUN)\n", ctrl & ~ANE_H18_CPU_RUN, s->asc_pa + ANE_H18_ASC_CPU_CONTROL);
	writel(ctrl & ~ANE_H18_CPU_RUN, ane->asc + ANE_H18_ASC_CPU_CONTROL);
	for (i = 0; i < 20; i++) {
		usleep_range(5000, 6000);
		st = readl(ane->asc + ANE_H18_ASC_CPU_STATUS);
		if (st & ANE_H18_ST_STOPPED) {
			stopped = true;
			break;
		}
	}
	dev_info(ane->dev, "ane_h18 boot running_seen=%u messages=%u final cpu_status=%#x stopped=%d\n", running_seen, msgs, st, stopped);
	if (!stopped) {
		dev_emerg(ane->dev, "ane_h18 the core did not stop after RUN was cleared: the PSD slots stay on, reboot required\n");
		ane_h18_result(ane, "boot", "FAIL", "core-not-stopped");
		return ret ? ret : -EBUSY;
	}
	ane->fw_started = false;
	if (!keep_on)
		ane_h18_psd_off(ane);
	if (ret) {
		ane_h18_result(ane, "boot", "FAIL", "read-refused");
		return ret;
	}
	ane_h18_result(ane, "boot", "PASS", running_seen ? "core-ran-and-stopped" : "core-never-reported-running");
	return 0;
}

static int ane_h18_probe(struct platform_device *pdev)
{
	struct ane_h18 *ane;
	unsigned int stage_idx;
	int idx[3 + ANE_H18_NPSD];
	int ret;

	if (!optin || strcmp(optin, ane_t8152_soc.name)) {
		dev_err(&pdev->dev, "not probing: load with optin=%s. EXPERIMENTAL: no hardware has run this module.\n", ane_t8152_soc.name);
		return -EPERM;
	}
	ret = ane_h18_stage_idx(stage, &stage_idx);
	if (ret) {
		dev_err(&pdev->dev, "unknown stage \"%s\" (dt, psd, power or boot)\n", stage);
		return ret;
	}
	ane = devm_kzalloc(&pdev->dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = &pdev->dev;
	ane->soc = &ane_t8152_soc;
	platform_set_drvdata(pdev, ane);

	ret = ane_h18_check_windows(ane, idx);
	if (ret) {
		ane_h18_result(ane, stage, "FAIL", "dt-window-mismatch");
		return ret;
	}
	if (stage_idx == ANE_H18_STAGE_DT)
		return ane_h18_stage_dt(ane, idx);

	ret = ane_h18_map_psd(ane, idx);
	if (ret)
		goto out;
	if (stage_idx == ANE_H18_STAGE_PSD) {
		ret = ane_h18_stage_psd(ane);
		goto out;
	}
	/* stage=power: the ASC windows are mapped here, but nothing reads them before the PSD acknowledge. */
	ane->asc = of_iomap(pdev->dev.of_node, idx[0]);
	ane->rvbar = of_iomap(pdev->dev.of_node, idx[1]);
	if (read_mbox || stage_idx == ANE_H18_STAGE_BOOT)
		ane->mbox = of_iomap(pdev->dev.of_node, idx[2]);
	if (!ane->asc || !ane->rvbar || ((read_mbox || stage_idx == ANE_H18_STAGE_BOOT) && !ane->mbox)) {
		dev_err(&pdev->dev, "ane_h18 of_iomap of an ASC window failed\n");
		ret = -ENXIO;
		goto out;
	}
	ret = stage_idx == ANE_H18_STAGE_BOOT ? ane_h18_stage_boot(ane) : ane_h18_stage_power(ane);
out:
	/* a failed probe never reaches remove(); a stage that passed keeps its mappings until remove.
	 * If the core may still be running, keep the mappings (the module cannot be removed after a failed bind anyway). */
	if (ret && !ane->fw_started)
		ane_h18_unmap(ane);
	return ret;
}

static void ane_h18_remove(struct platform_device *pdev)
{
	struct ane_h18 *ane = platform_get_drvdata(pdev);

	if (ane) {
		if (ane->fw_started) {
			dev_emerg(&pdev->dev, "ane_h18 the core may still run: reboot required\n");
			return;
		}
		if (keep_on)
			dev_warn(&pdev->dev, "ane_h18 keep_on=1: PSD slots left running\n");
		ane_h18_unmap(ane);
	}
}

static const struct of_device_id ane_h18_of_match[] = {
	{ .compatible = "apple,t8152-ane" },
	{ }
};

static struct platform_driver ane_h18_driver = {
	.driver = {
		.name = "ane_h18",
		.suppress_bind_attrs = true,
		.of_match_table = ane_h18_of_match,
	},
	.probe = ane_h18_probe,
	.remove = ane_h18_remove,
};
module_platform_driver(ane_h18_driver);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H18 ANE0 bring-up for the T8152 (dt, psd, power; no firmware path)");
MODULE_LICENSE("Dual MIT/GPL");
