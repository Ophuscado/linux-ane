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
		 "0/1/2/3 or name (dt/status/wrapper/boot). 1/status (default): power up and log ps ACTUAL. 0/dt: parse the DT only. 2/wrapper: REFUSED on H15 without confirm_read=1. 3/boot: refused without fw_path=.");

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
	 * no CPU release, no engine write, anywhere in this file.
	 */
	ane_h15_result(ane, 3, "REFUSED", "facts-filled-boot-path-not-built");
	return -ENOSYS;
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
	else                      ret = ane_h15_stage_boot(ane);

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
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H15-family ANE bring-up module (dt/status/wrapper/boot; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
