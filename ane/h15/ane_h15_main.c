/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15 — OPT-IN EXPERIMENTAL bring-up module for the H15 (M3)
 * ANE family: T8122 (M3), T6030 (M3 Pro), T6031 (M3 Max), T6034
 * (M3 Max die-variant, j514m/j516m). T6034 uses t6031's ADT nodes
 * and is the fourth row in the same module; no t6034 overlay is
 * shipped in this change (assignment scope: t8122/t6030/t6031 only;
 * a t6034 owner copies the t6031 overlay and swaps the compatible).
 *
 * No silicon has run this module. It performs no inference, no
 * CSNE_CMD, and no DRM device; it is a bring-up tool that the
 * first M3 owner drives through the stages below.
 *
 * Stages (selected by the `stage` module parameter):
 *   0 dt     : parse the running DT and print what would be mapped;
 *              no ioremap, no rpm.
 *   1 status : enable ANE power domains, wait for every pmgr ANE
 *              state word to read ACTUAL=0xf, then log every SAFE
 *              word (only the pmgr words qualify for H15).
 *   2 wrapper: refuses on H15 unless confirm_read=1 (CPU_STATUS and
 *              RVBAR are MEASURED-address, INFERENCE-role). With
 *              confirm_read=1 it runs the read-only preflight of the b0
 *              plan: behind the ACTUAL=0xf gate it reads CPU_STATUS,
 *              RVBAR and the two mailbox CTRL words, each address logged
 *              before its read, inside the two ADT-published windows
 *              only. No write, no CPU release.
 *   3 boot   : refuses without fw_path=; with fw_path=, prints one
 *              line per still-unmeasured fact from the ane_h15_facts
 *              table (stub firmware identity H1, mailbox/IRQ roles H2,
 *              segment-ranges/DART geometry H3, RVBAR compose H4,
 *              SCRATCH wake word H5) and refuses. Even with every row
 *              filled this build has no boot path: no engine write,
 *              no CPU release, ever.
 *   4 run    : confirm_run=1 required. The only stage that writes: it starts
 *              the ASC core once (CPU_CONTROL RUN) behind a read-only
 *              preflight (core stopped, RUN clear, RVBAR exactly the value a
 *              stage 2 run measured on this SoC, mailbox words EMPTY), watches
 *              CPU_STATUS and pops at most four mailbox messages (raw words,
 *              no reply), then stops the core again. Refused on any SoC row
 *              without a measured rvbar_expect (today only t6030). See the
 *              comment above ane_h15_stage_run().
 *
 * No MODULE_DEVICE_TABLE: nothing autoloads. probe() returns
 * -EPERM unless `optin=<soc>` matches. Insmod line:
 *   insmod ane_h15.ko optin=t8122 stage=status
 *
 * After stage=boot releases the ASC CPU, remove() refuses and the
 * machine needs a reboot to park the ANE (ane_t6021_boot.c:494-514
 * semantics; corrected in agent/ane-h16-ps-fix and verified by
 * receipts/2026-10-03-ane-h16).
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>
#include <crypto/sha2.h>

#include "ane_h15.h"

static char *optin;
module_param(optin, charp, 0444);
MODULE_PARM_DESC(optin,
		 "Opt-in key: must equal the SoC row name (t8122, t6030, t6031, t6034) or probe returns -EPERM.");

static char *stage = "status";
module_param(stage, charp, 0444);
MODULE_PARM_DESC(stage,
		 "0/1/2/3/4 or name (dt/status/wrapper/boot/run). 1/status (default): power up and log ps ACTUAL. 0/dt: parse the DT only. 2/wrapper: REFUSED on H15 without confirm_read=1. 3/boot: refused without fw_path=. 4/run: refused without confirm_run=1; starts the core once behind a preflight.");

static unsigned int ps_wait_ms = 500;
module_param(ps_wait_ms, uint, 0444);
MODULE_PARM_DESC(ps_wait_ms,
		 "Per-word pmgr ACTUAL=0xf wait, ms. Default 500.");

static bool confirm_boot;
module_param(confirm_boot, bool, 0444);
MODULE_PARM_DESC(confirm_boot,
		 "Required for stage=boot. Off by default; stage=boot refuses without it.");

static bool confirm_read;
module_param(confirm_read, bool, 0444);
MODULE_PARM_DESC(confirm_read,
		 "Stage=wrapper only. Off by default; stage=wrapper refuses without it. With it, stage=wrapper reads CPU_STATUS, RVBAR and the two mailbox CTRL words behind the pmgr ACTUAL=0xf gate. Reads only, nothing is written.");

static bool confirm_run;
module_param(confirm_run, bool, 0444);
MODULE_PARM_DESC(confirm_run,
		 "Required for stage=run. Off by default. Stage=run starts the ASC core once (CPU_CONTROL RUN) behind a read-only preflight, watches CPU_STATUS and the mailbox, and stops it again. Refused on SoCs without a measured RVBAR (rvbar_expect).");

static unsigned int run_wait_ms = 300;
module_param(run_wait_ms, uint, 0444);
MODULE_PARM_DESC(run_wait_ms,
		 "Stage=run: how long the core runs before RUN is cleared again, ms (10 to 2000). Default 300.");

static char *fw_path;
module_param(fw_path, charp, 0444);
MODULE_PARM_DESC(fw_path,
		 "Stage=boot only. This build checks that the value is set; it does not read or hash the file. No pin exists for the H15 stub image. See the runbook.");

/* apple-pmgr-pwrstate word: TARGET bits 3:0, ACTUAL bits 7:4;
 * 0xf is fully on (ane_t6021.h ANE_PS_*; docs/t6021-ane-bringup-
 * findings.md §4 gate). */
#define ANE_H15_PS_ACTUAL	GENMASK(7, 4)
#define ANE_H15_PS_ON		0xfu

static const char * const ane_h15_ps_names[] = {
	"ANE_SYS", "ANE_MPM", "ANE_CPU", "ANE_TD", "ANE_BASE"
};

struct ane_h15 {
	struct device *dev;
	const struct ane_h15_soc *soc;
	void __iomem *engine;
	void __iomem *pmgr;
	struct ane_h15_seg segs[2];
	bool fw_touched;
	bool fw_started;
	/* The ANE node carries several power-domains (sys, mpm, cpu, td,
	 * base). The kernel's automatic attach handles only a single
	 * domain, so for more than one we attach each by id and link it
	 * for runtime PM, as the in-tree ane driver does. */
#define ANE_H15_MAX_PD 8
	struct device *pd_dev[ANE_H15_MAX_PD];
	struct device_link *pd_link[ANE_H15_MAX_PD];
	int pd_count;
};

/* Attach every power-domain the node lists and link it so a single
 * pm_runtime_get on this device powers them all. Returns 0 when the
 * node has 0 or 1 domains (the core's automatic attach covers those).
 */
static int ane_h15_attach_pds(struct ane_h15 *ane)
{
	struct device *dev = ane->dev;
	int count, i;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count <= 1)
		return 0;
	if (count > ANE_H15_MAX_PD) {
		dev_err(dev, "too many power-domains: %d\n", count);
		return -EINVAL;
	}
	for (i = 0; i < count; i++) {
		ane->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(ane->pd_dev[i])) {
			int err = ane->pd_dev[i] ? PTR_ERR(ane->pd_dev[i]) : -ENODEV;

			ane->pd_dev[i] = NULL;
			dev_err(dev, "attach power-domain %d: %d\n", i, err);
			return err;
		}
		/* Count it before adding the link: if device_link_add fails,
		 * ane_h15_detach_pds must still detach this attached domain. */
		ane->pd_count = i + 1;
		ane->pd_link[i] = device_link_add(dev, ane->pd_dev[i],
						  DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME |
						  DL_FLAG_RPM_ACTIVE);
		/* device_link_add() returns NULL on failure, never an
		 * ERR_PTR, so a plain !link test is the right check. */
		if (!ane->pd_link[i]) {
			dev_err(dev, "link power-domain %d\n", i);
			return -EINVAL;
		}
	}
	dev_info(dev, "attached %d ANE power-domains\n", ane->pd_count);
	return 0;
}

static void ane_h15_detach_pds(struct ane_h15 *ane)
{
	int i;

	for (i = ane->pd_count - 1; i >= 0; i--) {
		if (ane->pd_link[i]) {
			device_link_del(ane->pd_link[i]);
			ane->pd_link[i] = NULL;
		}
		if (ane->pd_dev[i]) {
			dev_pm_domain_detach(ane->pd_dev[i], true);
			ane->pd_dev[i] = NULL;
		}
	}
	ane->pd_count = 0;
}

/* ---- RESULT line, emitted at every stage ---- */
static void ane_h15_result(struct ane_h15 *ane, unsigned int stage_idx,
			   const char *verdict, const char *reason)
{
	if (ane && ane->dev)
		dev_crit(ane->dev,
			 "ane_h15 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			 stage_idx, ane->soc->name, verdict, reason);
	else
		pr_crit("ane_h15 RESULT stage=%u soc=%s verdict=%s reason=%s\n",
			stage_idx, "?", verdict, reason);
}

static int ane_h15_stage_idx(const char *name, unsigned int *out)
{
	if (!strcmp(name, "dt") || !strcmp(name, "0")) { *out = 0; return 0; }
	if (!strcmp(name, "status") || !strcmp(name, "1")) { *out = 1; return 0; }
	if (!strcmp(name, "wrapper") || !strcmp(name, "2")) { *out = 2; return 0; }
	if (!strcmp(name, "boot") || !strcmp(name, "3")) { *out = 3; return 0; }
	if (!strcmp(name, "run") || !strcmp(name, "4")) { *out = 4; return 0; }
	return -EINVAL;
}

/* ---- pmgr ACTUAL gate (always runs before any engine access) ---- */

static bool ane_h15_ps_on(struct ane_h15 *ane, unsigned int i, u32 *v)
{
	*v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
	return FIELD_GET(ANE_H15_PS_ACTUAL, *v) == ANE_H15_PS_ON;
}

static int ane_h15_ps_wait(struct ane_h15 *ane)
{

	unsigned int i;
	u32 v;

	for (i = 0; i < 5; i++) {
		unsigned long deadline = jiffies + msecs_to_jiffies(ps_wait_ms);

		while (!ane_h15_ps_on(ane, i, &v)) {
			if (time_after(jiffies, deadline)) {
				dev_err(ane->dev,
					"pmgr %s @+%#x stuck at %#x (ACTUAL=%#lx); refusing to touch the engine window\n",
					ane_h15_ps_names[i], ane->soc->ps_off[i], v,
					FIELD_GET(ANE_H15_PS_ACTUAL, v));
				return -ETIMEDOUT;
			}
			usleep_range(100, 200);
		}
	}
	for (i = 0; i < 5; i++) {
		if (!ane_h15_ps_on(ane, i, &v)) {
			dev_err(ane->dev,
				"pmgr %s @+%#x dropped to %#x after the wait\n",
				ane_h15_ps_names[i], ane->soc->ps_off[i], v);
			return -EIO;
		}
		dev_info(ane->dev, "pmgr %s @+%#x = %#x (ACTUAL 0xf)\n",
			 ane_h15_ps_names[i], ane->soc->ps_off[i], v);
	}
	return 0;
}

/* ---- stage 0: dt-only parse, no MMIO ---- */
static int ane_h15_stage_dt(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned int i;

	dev_info(ane->dev, "stage=dt: soc=%s ane-type=%u", s->name, s->ane_type);
	dev_info(ane->dev, "engine  pa=%#llx size=%#x", s->engine_pa, s->engine_size);
	dev_info(ane->dev, "pmgr    pa=%#llx size=%#x", s->pmgr_pa, s->pmgr_size);
	for (i = 0; i < 5; i++)
		dev_info(ane->dev, "pmgr.%s offset=%#x", ane_h15_ps_names[i],
			 s->ps_off[i]);
	for (i = 0; i < 5; i++) {
		u64 pa = (s->words[i].where == ANE15_WHERE_ENGINE) ? s->engine_pa : s->pmgr_pa;

		dev_info(ane->dev, "word[%u] %-13s where=%s off=%#x size=%u tier=%d",
			 i, s->words[i].name,
			 s->words[i].where == ANE15_WHERE_ENGINE ? "engine" : "pmgr",
			 s->words[i].off, s->words[i].size, s->words[i].tier);
		if (s->words[i].tier == ANE15_TIER_MEASURED_GUARD)
			dev_info(ane->dev, "  pa=%#llx", pa + s->words[i].off);
	}
	if (!s->fw) {
		dev_info(ane->dev, "fw pin: none (stage=boot refuses)");
	} else {
		dev_info(ane->dev, "fw pin: name=%s size=%u manifest=%u (S3 hash is over size)",
			 s->fw->name, s->fw->size, s->fw->manifest_size);
	}
	ane_h15_result(ane, 0, "PASS", "dt-parse-only");
	return 0;
}

/* ---- stage 1: pm up, log the SAFE pmgr words ---- */
static int ane_h15_stage_status(struct ane_h15 *ane)
{

	unsigned int i;
	u32 v;
	int ret;

	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 1, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	/* Re-read every ps word behind the guard; each read logged
	 * BEFORE the access so a hang leaves the last attempted
	 * address in the log. */
	for (i = 0; i < 5; i++) {
		dev_crit(ane->dev, "ane_h15 read pmgr.%s pa=%#llx off=%#x",
			 ane_h15_ps_names[i], ane->soc->pmgr_pa + ane->soc->ps_off[i],
			 ane->soc->ps_off[i]);
		v = readl_relaxed(ane->pmgr + ane->soc->ps_off[i]);
		dev_info(ane->dev,
			 "ane_h15 word=%s pa=%#llx value=%#x actual=%#lx pass=true",
			 ane_h15_ps_names[i], ane->soc->pmgr_pa + ane->soc->ps_off[i],
			 v, FIELD_GET(ANE_H15_PS_ACTUAL, v));
	}
	ane_h15_result(ane, 1, "PASS", "ps-guard+reads");
	return 0;
}

/* ---- stage 2: wrapper preflight ----
 * Without confirm_read=1 the stage refuses before any engine read and
 * lists the ADDR_MEASURED words (their roles are INFERENCE on H15).
 * With confirm_read=1 it runs the read-only preflight of the b0 plan
 * (receipts/2026-10-03-ane-h15, "b1 plan", step 2) once every pmgr word
 * reads ACTUAL=0xf: CPU_STATUS, RVBAR (two 32-bit reads) and the two
 * mailbox CTRL words. Every address is logged BEFORE its read, so a
 * hang leaves the last attempted address in the log. Only offsets
 * inside the two windows the ADT publishes for the ANE node are read:
 * the wrapper (engine+0x1400000, 0x6c000) and the RVBAR block
 * (engine+0x1050000, 0x4000). CPU_CONTROL, the CoreSight line and the
 * T6021 fatal range are refused by ane_h15_read_ok(), whatever the
 * soc row says. Nothing is written.
 */
/* Sources of the numbers below, by exact commit.
 * - Wrapper (CPU_CONTROL +0x44, CPU_STATUS +0x48), RVBAR +0x1050000,
 *   mailbox CTRL words +0x1408110 and +0x1408114, and the fatal range
 *   0x1854000..0x1c04000: ane/t6021/ane_t6021.h at 16cfa8720b80677dbcdf8fd1d04077a8b72768bf
 *   (lines 104-109, 142-143 and 160-163). These are T6021 values. Their
 *   H15 roles are INFERENCE (see the b0 receipt).
 * - The two ADT windows (wrapper 0x6c000 bytes at engine + 0x1400000,
 *   RVBAR block 0x4000 bytes at engine + 0x1050000): the macOS 27.0 ADT
 *   summary adt-27.0.txt, sha256
 *   04fe38b8ee009700f02e2c01e83bf608afab5e843f5878a83f162973a1ffdb84,
 *   as tabulated (A1, A3) in receipts/2026-10-03-ane-h15/README.md at
 *   71ed021859a3f7e55c04756a1c6ab456fa48fc9f.
 * - CoreSight line engine + 0x1010000: docs/t6021-ane-bringup-findings.md
 *   section 7 at 16cfa8720b80677dbcdf8fd1d04077a8b72768bf.
 */
#define ANE_H15_WRAP_OFF	0x1400000u
#define ANE_H15_WRAP_SIZE	0x6c000u
#define ANE_H15_RVBAR_WIN_OFF	0x1050000u
#define ANE_H15_RVBAR_WIN_SIZE	0x4000u
#define ANE_H15_CORESIGHT	0x1010000u
#define ANE_H15_FATAL_LO	0x1854000u
#define ANE_H15_FATAL_HI	0x1c04000u

static bool ane_h15_read_ok(const struct ane_h15_soc *s, u32 off, u32 size)
{
	u64 end = (u64)off + size;
	bool in_wrap = off >= ANE_H15_WRAP_OFF &&
		       end <= (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE;
	bool in_rvbar = off >= ANE_H15_RVBAR_WIN_OFF &&
			end <= (u64)ANE_H15_RVBAR_WIN_OFF + ANE_H15_RVBAR_WIN_SIZE;

	if (!in_wrap && !in_rvbar)
		return false;
	if (end > s->engine_size)
		return false;
	/* CPU_CONTROL is never read (hostile on T6001), neither the
	 * CoreSight line nor the T6021 fatal range. */
	if (off < s->cpu_control + 4 && end > s->cpu_control)
		return false;
	if (off < ANE_H15_CORESIGHT + 4 && end > ANE_H15_CORESIGHT)
		return false;
	if (off < ANE_H15_FATAL_HI && end > ANE_H15_FATAL_LO)
		return false;
	return true;
}

static int ane_h15_read_word(struct ane_h15 *ane, const char *name, u32 off,
			     u32 *v)
{
	if (!ane_h15_read_ok(ane->soc, off, 4)) {
		dev_err(ane->dev, "ane_h15 refusing read %s engine+%#x\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h15 read %s pa=%#llx off=%#x\n", name,
		 ane->soc->engine_pa + off, off);
	*v = readl_relaxed(ane->engine + off);
	dev_info(ane->dev, "ane_h15 word=%s pa=%#llx value=%#x\n", name,
		 ane->soc->engine_pa + off, *v);
	return 0;
}

/* The static offsets hold only for the windows the SoC row describes.
 * Compare a live reg resource, the one of_iomap() mapped, with the row. */
static bool ane_h15_window_ok(struct ane_h15 *ane, int idx, const char *what,
			      u64 pa, u32 size)
{
	struct resource res;
	int ret = of_address_to_resource(ane->dev->of_node, idx, &res);

	if (!ret && res.start == pa && resource_size(&res) == size)
		return true;
	dev_err(ane->dev,
		"ane_h15 %s window mismatch: reg[%d] start=%#llx size=%#llx, SoC row wants start=%#llx size=%#x\n",
		what, idx, ret ? 0ULL : (u64)res.start,
		ret ? 0ULL : (u64)resource_size(&res), pa, size);
	return false;
}

static int ane_h15_stage_wrapper(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 status = 0, rvbar_lo = 0, rvbar_hi = 0, a2i = 0, i2a = 0;
	unsigned int i, refused = 0;
	char why[128];
	int ret;

	if (!confirm_read) {
		for (i = 0; i < 5; i++) {
			const struct ane_h15_word *w = &s->words[i + 5];

			if (w->tier != ANE15_TIER_ADDR_MEASURED)
				continue;
			dev_info(ane->dev,
				 "word %s tier=ADDR_MEASURED not cleared; src=%s",
				 w->name, w->src);
			refused++;
		}
		snprintf(why, sizeof(why),
			 "%u ADDR_MEASURED word(s) lack a macOS capture; role INFERENCE; confirm_read=1 runs the read-only preflight",
			 refused);
		ane_h15_result(ane, 2, "REFUSED", why);
		return -EPERM;
	}

	/* Both windows are checked before any bus access: the pmgr gate
	 * below reads through reg[1], the preflight through reg[0]. */
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, 2, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 2, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &status);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_LO", s->rvbar, &rvbar_lo);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_HI", s->rvbar + 4, &rvbar_hi);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	if (ret) {
		ane_h15_result(ane, 2, "FAIL", "read-refused");
		return ret;
	}
	/* The T6021 capture of a stopped core reads CPU_STATUS 0x2a and
	 * RVBAR 0x1 (ane/t6021/ane_t6021.h). The H15 meaning of the
	 * bits is not measured; the line only compares. */
	dev_info(ane->dev,
		 "ane_h15 preflight cpu_status=%#x rvbar=%#llx a2i_ctrl=%#x i2a_ctrl=%#x t6021_stopped_status=%s\n",
		 status, ((u64)rvbar_hi << 32) | rvbar_lo, a2i, i2a,
		 status == 0x2a ? "equal" : "differs");
	ane_h15_result(ane, 2, "PASS", "preflight-read-only");
	return 0;
}

/* ---- stage 3: see header. The refusal is generated from the
 * family-wide fact table (ane_h15_facts, plan section 1.5): one line
 * per still-unmeasured fact, then the RESULT line. Even when every
 * row is filled, THIS build has no boot path: it refuses with a
 * distinct reason and releases no CPU.
 * ----
 */
static void ane_h15_fact_emit(void *ctx, const struct ane_h15_fact *f)
{
	dev_crit((struct device *)ctx, "ane_h15 hole=%s %s (fill: %s)\n",
		 f->id, f->what, f->fill);
}

static int ane_h15_stage_boot(struct ane_h15 *ane)
{
	int unfilled;

	if (!confirm_boot) {
		ane_h15_result(ane, 3, "REFUSED",
			       "confirm_boot=1 required");
		return -EPERM;
	}
	if (!fw_path) {
		ane_h15_result(ane, 3, "REFUSED",
			       "no firmware pin for this SoC: omarchy-ane-firmware-fetch has no M3 row. Pass fw_path=<file under /lib/firmware> to override; the only known pin is the 27.0 IPSW payload (unmeasured for the stub image). See the runbook.");
		return -ENOENT;
	}
	unfilled = ane_h15_facts_report(ane_h15_facts, ane_h15_n_facts,
					ane_h15_fact_emit, ane->dev);
	if (unfilled) {
		ane_h15_result(ane, 3, "REFUSED", "unfilled-facts");
		return -ENOENT;
	}
	/* Every fact row is filled: the gate would open for a build
	 * that implements the boot path. This one does not; there is
	 * no CPU release and no engine write in THIS stage (stage 4 is the only one that writes).
	 */
	ane_h15_result(ane, 3, "REFUSED", "facts-filled-boot-path-not-built");
	return -ENOSYS;
}

/* ---- stage 4: start the ASC core once, watch, stop it again ----
 *
 * Why this is the next step. The T6030 stage 2 run (PR 175, J516S) read RVBAR 0x0000010000000001 and CPU_STATUS
 * 0x0030002a: iBoot already preloaded the firmware and wrote the entry address, and the core is stopped (m1n1
 * R_CPU_STATUS: STOPPED bit 1 and IDLE bit 5 set, RUNNING bit 0 clear; bits 20 and 21 unknown on H15). The T8152
 * (M6) ANE0 is in the same state (CPU_STATUS 0xa, IORVBAR 0x10000000001). On the M6 the same RUN write took
 * CPU_STATUS from 0xa to 0x8 in under 11 ms: STOPPED clears, RUNNING (bit 0) does not read 1. So "started" is
 * tested as STOPPED cleared here, not as RUNNING set.
 *
 * What it does, in order (every access address is logged BEFORE it):
 *   1. refuses without confirm_run=1, with run_wait_ms outside 10..2000, and on a SoC row without a measured
 *      rvbar_expect (nothing is touched);
 *   2. checks both live reg windows against the SoC row, waits for the five pmgr words to read ACTUAL=0xf;
 *   3. preflight, read only: CPU_STATUS (STOPPED set, RUNNING clear), CPU_CONTROL (RUN clear), RVBAR (exactly
 *      rvbar_expect) and both mailbox CTRL words (EMPTY, bit 17, set: nothing stale). Anything else refuses;
 *   4. CPU_CONTROL |= RUN (bit 4), the macOS AppleASCWrapV8::_runCPU(true) sequence (read, OR 0x10, write);
 *   5. for run_wait_ms every 10 ms: CPU_STATUS and I2A_CTRL; if I2A is not EMPTY it pops the message
 *      (I2A_RECV0 at wrapper+0x8830, I2A_RECV1 at +0x8838, the Linux apple-mailbox v4 layout the other T6030 ASCs
 *      use) and logs the raw words, at most 4 messages. It sends NOTHING and never replies to a HELLO;
 *   6. stops the core the macOS way (_runCPU(false)): clear bit 4, write; re-read, clear bit 5, write; waits for
 *      STOPPED. If the core does not stop the power domains stay on, the windows stay mapped and remove()
 *      refuses: reboot.
 * It writes only CPU_CONTROL, only bits 4 and 5, never RVBAR, never the mailbox, never the DART. The ANE DART
 * nodes are disabled in the overlay and nothing here programs the DART: a core that cannot fetch through it
 * may simply sit started. That is a result, not a failure of this stage.
 */
#define ANE_H15_ST_RUNNING	BIT(0)
#define ANE_H15_ST_STOPPED	BIT(1)
#define ANE_H15_CPU_RUN		BIT(4)
#define ANE_H15_CPU_BIT5	BIT(5)
#define ANE_H15_MBOX_EMPTY	BIT(17)
#define ANE_H15_MBOX_I2A_RECV0	0x830u
#define ANE_H15_MBOX_I2A_RECV1	0x838u
#define ANE_H15_RUN_MAX_MSGS	4u

static int ane_h15_read_ctrl(struct ane_h15 *ane, u32 *v)
{
	const struct ane_h15_soc *s = ane->soc;

	if (s->cpu_control < ANE_H15_WRAP_OFF ||
	    (u64)s->cpu_control + 4 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE ||
	    (u64)s->cpu_control + 4 > s->engine_size)
		return -EPERM;
	dev_crit(ane->dev, "ane_h15 read CPU_CONTROL pa=%#llx off=%#x\n",
		 s->engine_pa + s->cpu_control, s->cpu_control);
	*v = readl_relaxed(ane->engine + s->cpu_control);
	dev_info(ane->dev, "ane_h15 word=CPU_CONTROL pa=%#llx value=%#x\n",
		 s->engine_pa + s->cpu_control, *v);
	return 0;
}

/* The only engine write this module has: CPU_CONTROL, and only bits 4 (RUN) and 5. */
static int ane_h15_write_ctrl(struct ane_h15 *ane, u32 old, u32 val, const char *why)
{
	const struct ane_h15_soc *s = ane->soc;

	if ((old ^ val) & ~(ANE_H15_CPU_RUN | ANE_H15_CPU_BIT5)) {
		dev_err(ane->dev, "ane_h15 refusing CPU_CONTROL write %#x over %#x: bits other than 4 and 5 differ\n", val, old);
		return -EPERM;
	}
	if (s->cpu_control < ANE_H15_WRAP_OFF ||
	    (u64)s->cpu_control + 4 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE ||
	    (u64)s->cpu_control + 4 > s->engine_size)
		return -EPERM;
	dev_crit(ane->dev, "ane_h15 write CPU_CONTROL=%#x pa=%#llx (%s)\n", val,
		 s->engine_pa + s->cpu_control, why);
	writel(val, ane->engine + s->cpu_control);
	return 0;
}

static int ane_h15_stage_run(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 st = 0, ctrl = 0, rv_lo = 0, rv_hi = 0, a2i = 0, i2a = 0, last = 0xffffffffu, m0, m1;
	unsigned int waited, i, msgs = 0, started_seen = 0, posted = 0;
	bool stopped = false;
	int ret;

	if (!confirm_run) {
		ane_h15_result(ane, 4, "REFUSED", "confirm_run=1 required");
		return -EPERM;
	}
	if (run_wait_ms < 10 || run_wait_ms > 2000) {
		ane_h15_result(ane, 4, "REFUSED", "run_wait_ms 10 to 2000");
		return -EINVAL;
	}
	if (!s->rvbar_expect) {
		ane_h15_result(ane, 4, "REFUSED", "no measured RVBAR for this SoC row: run stage 2 and report it");
		return -ENODEV;
	}
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, 4, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, 4, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	/* Preflight, read only. */
	ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
	if (!ret)
		ret = ane_h15_read_ctrl(ane, &ctrl);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_LO", s->rvbar, &rv_lo);
	if (!ret)
		ret = ane_h15_read_word(ane, "RVBAR_HI", s->rvbar + 4, &rv_hi);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
	if (!ret)
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	if (ret) {
		ane_h15_result(ane, 4, "FAIL", "read-refused");
		return ret;
	}
	if ((st & ANE_H15_ST_RUNNING) || !(st & ANE_H15_ST_STOPPED) || (ctrl & ANE_H15_CPU_RUN) ||
	    (((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect ||
	    !(a2i & ANE_H15_MBOX_EMPTY) || !(i2a & ANE_H15_MBOX_EMPTY)) {
		dev_err(ane->dev,
			"ane_h15 preflight: cpu_status=%#x cpu_control=%#x rvbar=%#llx a2i_ctrl=%#x i2a_ctrl=%#x (want stopped, RUN clear, rvbar %#llx, both mailbox words EMPTY)\n",
			st, ctrl, ((u64)rv_hi << 32) | rv_lo, a2i, i2a, s->rvbar_expect);
		ane_h15_result(ane, 4, "FAIL", "preflight-not-as-iboot-left-it");
		return -EBUSY;
	}

	/* Start the core. From here a reboot may be needed. */
	ane->fw_started = true;
	ane->fw_touched = true;
	ret = ane_h15_write_ctrl(ane, ctrl, ctrl | ANE_H15_CPU_RUN, "RUN");
	if (ret) {
		ane->fw_started = false;
		ane->fw_touched = false;
		ane_h15_result(ane, 4, "FAIL", "write-refused");
		return ret;
	}
	for (waited = 0; waited < run_wait_ms; waited += 10) {
		usleep_range(10000, 11000);
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (!ret)
			ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
		if (ret)
			break;
		if (!(st & ANE_H15_ST_STOPPED))
			started_seen = 1;
		if (st != last) {
			dev_info(ane->dev, "ane_h15 t+%ums cpu_status=%#x i2a_ctrl=%#x\n", waited + 10, st, i2a);
			last = st;
		}
		if (!(i2a & ANE_H15_MBOX_EMPTY) && msgs < ANE_H15_RUN_MAX_MSGS) {
			posted = 1;
			m0 = m1 = 0;
			ret = ane_h15_read_word(ane, "MBOX_I2A_RECV0", s->mbox + ANE_H15_MBOX_I2A_RECV0, &m0);
			if (!ret)
				ret = ane_h15_read_word(ane, "MBOX_I2A_RECV1", s->mbox + ANE_H15_MBOX_I2A_RECV1, &m1);
			if (ret)
				break;
			dev_info(ane->dev, "ane_h15 msg%u raw0=%#x raw1=%#x\n", msgs, m0, m1);
			msgs++;
		}
	}
	/* Stop the core the macOS way: clear RUN, then bit 5, each from a fresh read. */
	if (ane_h15_read_ctrl(ane, &ctrl) == 0 &&
	    ane_h15_write_ctrl(ane, ctrl, ctrl & ~ANE_H15_CPU_RUN, "clear RUN") == 0) {
		if (ane_h15_read_ctrl(ane, &ctrl) == 0 && (ctrl & ANE_H15_CPU_BIT5))
			ane_h15_write_ctrl(ane, ctrl, ctrl & ~ANE_H15_CPU_BIT5, "clear bit 5");
	}
	for (i = 0; i < 20; i++) {
		usleep_range(5000, 6000);
		if (ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st))
			break;
		if (st & ANE_H15_ST_STOPPED) {
			stopped = true;
			break;
		}
	}
	dev_info(ane->dev, "ane_h15 run started_seen=%u i2a_posted=%u messages=%u final cpu_status=%#x i2a_ctrl=%#x stopped=%d\n",
		 started_seen, posted, msgs, st, i2a, stopped);
	if (!stopped) {
		dev_emerg(ane->dev,
			  "ane_h15 the core did not report STOPPED after RUN was cleared: power domains stay on, do NOT unload, reboot required\n");
		ane_h15_result(ane, 4, "FAIL", "core-not-stopped");
		/* 0, not an error: a failed probe would release the domains and the windows under a running core */
		return 0;
	}
	ane->fw_started = false;
	ane->fw_touched = false;
	ane_h15_result(ane, 4, "PASS", started_seen ? "core-started-and-stopped" : "core-never-left-stopped");
	return 0;
}

/* ---- driver ---- */
static void ane_h15_unmap_windows(struct ane_h15 *ane)
{
	if (ane->pmgr)
		iounmap(ane->pmgr);
	if (ane->engine)
		iounmap(ane->engine);
}

static int ane_h15_probe(struct platform_device *pdev)
{
	const struct ane_h15_soc *soc = of_device_get_match_data(&pdev->dev);
	struct ane_h15 *ane;
	u32 ane_type;
	unsigned int stage_idx;
	int ret;

	if (!optin || strcmp(optin, soc->name)) {
		dev_err(&pdev->dev,
			"not probing: optin key must be \"%s\" (optin=%s). This module is EXPERIMENTAL; no silicon has run it.\n",
			soc->name, optin ? optin : "(unset)");
		return -EPERM;
	}
	if (of_property_read_u32(pdev->dev.of_node, "apple,ane-type", &ane_type) ||
	    ane_type != soc->ane_type) {
		dev_err(&pdev->dev,
			"device apple,ane-type=%u (want %u); refusing\n",
			ane_type, soc->ane_type);
		return -EINVAL;
	}
	ret = ane_h15_stage_idx(stage, &stage_idx);
	if (ret) {
		dev_err(&pdev->dev, "unknown stage \"%s\"\n", stage);
		return ret;
	}

	ane = devm_kzalloc(&pdev->dev, sizeof(*ane), GFP_KERNEL);
	if (!ane)
		return -ENOMEM;
	ane->dev = &pdev->dev;
	ane->soc = soc;
	platform_set_drvdata(pdev, ane);

	if (stage_idx == 0) {
		ret = ane_h15_stage_dt(ane);
		return ret;
	}

	/* Stages 1, 2, 3 map engine and pmgr via of_iomap (non-posted
	 * when the parent bus advertises nonposted-mmio). */
	ane->engine = of_iomap(pdev->dev.of_node, 0);
	ane->pmgr   = of_iomap(pdev->dev.of_node, 1);
	/* of_iomap returns NULL (not ERR_PTR) when the reg property is
	 * missing or bad; a NULL + offset read would oops. Refuse. */
	if (!ane->engine || !ane->pmgr) {
		dev_err(&pdev->dev,
			"of_iomap failed (engine=%s, pmgr=%s): node reg property missing?\n",
			ane->engine ? "ok" : "NULL", ane->pmgr ? "ok" : "NULL");
		ane_h15_unmap_windows(ane);
		return -ENXIO;
	}

	ret = ane_h15_attach_pds(ane);
	if (ret) {
		ane_h15_detach_pds(ane);
		ane_h15_unmap_windows(ane);
		return ret;
	}

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_get_sync(&pdev->dev);
	if (ret < 0) {
		/*
		 * With several power-domains attached by id, the RPM_ACTIVE
		 * device links already resumed every domain. On T6030 the main
		 * device's get_sync then returned -EACCES (observed at stage 1;
		 * the cause is not established). Only that code, and only with
		 * domains attached, is treated as powered;
		 * the ACTUAL=0xf gate in the stage is the real check. Any other
		 * error fails the probe.
		 */
		if (ane->pd_count > 0 && ret == -EACCES) {
			dev_info(&pdev->dev,
				 "engine powered via %d domain links (main get_sync %d ignored)\n",
				 ane->pd_count, ret);
		} else {
			dev_err(&pdev->dev, "power-domains bring-up failed: %d\n", ret);
			/* a failed get_sync still took a usage count */
			pm_runtime_put_noidle(&pdev->dev);
			pm_runtime_disable(&pdev->dev);
			ane_h15_detach_pds(ane);
			ane_h15_unmap_windows(ane);
			return ret;
		}
	}

	if (stage_idx == 1)       ret = ane_h15_stage_status(ane);
	else if (stage_idx == 2)  ret = ane_h15_stage_wrapper(ane);
	else if (stage_idx == 4)  ret = ane_h15_stage_run(ane);
	else                      ret = ane_h15_stage_boot(ane);

	/* A started core that did not stop keeps its power claim (and its windows, see remove()). */
	if (!ane->fw_started)
		pm_runtime_put_sync(&pdev->dev);
	if (ane->fw_started)
		dev_emerg(&pdev->dev,
			  "firmware started: do NOT unload; reboot to park the ANE\n");
	/* a failed probe never reaches remove(); release everything */
	if (ret) {
		pm_runtime_disable(&pdev->dev);
		ane_h15_detach_pds(ane);
		ane_h15_unmap_windows(ane);
	}
	return ret;
}

static void ane_h15_remove(struct platform_device *pdev)
{
	struct ane_h15 *ane = platform_get_drvdata(pdev);

	if (ane->fw_touched) {
		dev_emerg(&pdev->dev,
			  "remove with the firmware released: reboot required\n");
		return;
	}
	pm_runtime_disable(&pdev->dev);
	ane_h15_detach_pds(ane);
	ane_h15_unmap_windows(ane);
}

static const struct of_device_id ane_h15_of_match[] = {
	{ .compatible = "apple,t8122-ane", .data = &ane_t8122_soc },
	{ .compatible = "apple,t6030-ane", .data = &ane_t6030_soc },
	{ .compatible = "apple,t6031-ane", .data = &ane_t6031_soc },
	{ .compatible = "apple,t6034-ane", .data = &ane_t6034_soc },
	{ }
};

static struct platform_driver ane_h15_driver = {
	.driver = {
		.name = "ane_h15",
		.suppress_bind_attrs = true,
		.of_match_table = ane_h15_of_match,
	},
	.probe = ane_h15_probe,
	.remove = ane_h15_remove,
};
module_platform_driver(ane_h15_driver);

MODULE_AUTHOR("Joshua Warren");
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H15-family ANE bring-up module (dt/status/wrapper/boot/run; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
