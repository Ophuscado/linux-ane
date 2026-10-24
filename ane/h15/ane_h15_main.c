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
 *   4 dart   : confirm_read=1 required. Read only: the four ANE DART windows (engine+0x1800000..),
 *              version, lock state, and per stream TCR, TTBR and the enable bits. Says whether iBoot left a
 *              stream enabled and mapped. See the comment above ane_h15_stage_dart().
 *   5 run    : confirm_run=1 required. The only stage that writes: it starts
 *              the ASC core once (CPU_CONTROL RUN) behind a read-only
 *              preflight (core stopped, RUN clear, RVBAR exactly the value a
 *              stage 2 run measured on this SoC, mailbox words EMPTY), watches
 *              CPU_STATUS and pops at most four mailbox messages (raw words,
 *              no reply), then stops the core again. Refused on any SoC row
 *              without a measured rvbar_expect (today only t6030), and without a live DART
 *              stream from the same scan stage 4 does (run_unmapped=1 overrides). See the
 *              comment above ane_h15_stage_run().
 *
 *   6 rtkit  : confirm_run=1 and confirm_rtkit=1 required. Stage 5 plus an RTKit management session on the ASC
 *              mailbox: the first stage that writes the mailbox send registers. See ane_h15_rtk_* below.
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
		 "0/1/2/3/4/5/6 or name (dt/status/wrapper/boot/dart/run/rtkit). 1/status (default): power up and log ps ACTUAL. 0/dt: parse the DT only. 2/wrapper: REFUSED on H15 without confirm_read=1. 3/boot: refused without fw_path=. 4/dart: read-only DART liveness, needs confirm_read=1. 5/run: refused without confirm_run=1 and without a live DART stream; starts the core once behind a preflight. 6/rtkit: stage 5 plus an RTKit session over the ASC mailbox, needs confirm_rtkit=1 as well.");

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

static bool run_unmapped;
module_param(run_unmapped, bool, 0444);
MODULE_PARM_DESC(run_unmapped,
		 "stage=run only: start the core even though stage=dart found no enabled DART stream (the core cannot fetch its firmware then; expect silence)");

static bool map_fw;
module_param(map_fw, bool, 0444);
MODULE_PARM_DESC(map_fw,
		 "stage=run only: before RUN attach the ANE to its DART through the iommu API and map the two preloaded firmware segments named by fw_seg. Needs iommus in the node. Default off.");

static char *fw_seg;
module_param(fw_seg, charp, 0444);
MODULE_PARM_DESC(fw_seg,
		 "stage=run map_fw=1: TEXT and DATA of the preloaded firmware as hex physical:size pairs, \"TEXTPA:SIZE,DATAPA:SIZE\", from the live ADT /arm-io/ane segment-ranges of this boot. 16 KiB aligned, outside system RAM. TEXT is mapped read only at the RVBAR entry address, DATA read+write right after it.");

static bool confirm_rtkit;
module_param(confirm_rtkit, bool, 0444);
MODULE_PARM_DESC(confirm_rtkit,
		 "stage=rtkit only: required. Stage 6 is stage 5 plus an RTKit management session on the ASC mailbox: it WRITES the mailbox send registers (HELLO reply, endpoint map, STARTEP, power states) and then runs the shutdown handshake. Nobody has run it on an H15 part.");

static unsigned int rtkit_wait_ms = 2000;
module_param(rtkit_wait_ms, uint, 0444);
MODULE_PARM_DESC(rtkit_wait_ms,
		 "stage=rtkit: how long to wait for the session to reach AP power ON and the application endpoints (100 to 10000 ms, default 2000)");

static unsigned int rtkit_stop = 1;
module_param(rtkit_stop, uint, 0444);
MODULE_PARM_DESC(rtkit_stop,
		 "stage=rtkit: shutdown before RUN is cleared. 1 (default): AP QUIESCED then IOP SLEEP, restartable (apple_rtkit_shutdown). 2: OFF then OFF, the core cannot be restarted until a reboot (apple_rtkit_poweroff).");

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
	struct iommu_domain *domain;	/* map_fw: the DART domain, NULL when none */
	u64 map_pa[2], map_iova[2], map_size[2];
	unsigned int map_n;	/* segments mapped so far */
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
	if (!strcmp(name, "dart") || !strcmp(name, "4")) { *out = 4; return 0; }
	if (!strcmp(name, "run") || !strcmp(name, "5")) { *out = 5; return 0; }
	if (!strcmp(name, "rtkit") || !strcmp(name, "6")) { *out = 6; return 0; }
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

/* The v4 mailbox receive registers are 64-bit and pop on read (ane_t6021.h:138-147): one readq per register,
 * never two 32-bit reads, which would take the message apart (and eat it). */
static int ane_h15_read_word64(struct ane_h15 *ane, const char *name, u32 off, u64 *v)
{
	if ((off & 7) || !ane_h15_read_ok(ane->soc, off, 8)) {
		dev_err(ane->dev, "ane_h15 refusing 64-bit read %s engine+%#x\n", name, off);
		return -EPERM;
	}
	dev_crit(ane->dev, "ane_h15 read64 %s pa=%#llx off=%#x\n", name,
		 ane->soc->engine_pa + off, off);
	*v = readq_relaxed(ane->engine + off);
	dev_info(ane->dev, "ane_h15 word64=%s pa=%#llx value=%#llx\n", name,
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
	 * no CPU release and no engine write in THIS stage (stage 5 is the only one that writes).
	 */
	ane_h15_result(ane, 3, "REFUSED", "facts-filled-boot-path-not-built");
	return -ENOSYS;
}

/* ---- stage 4: is any ANE DART stream enabled? read only ----
 *
 * Sources: ane/h15/run-stage-sources.md (apple-dart.c t8110 at omarchy-linux 94fb2334, data/ane-soc/t6030.json).
 *
 * The M6 (T8152) lesson (an earlier run on a Mac mini M6, boot 235c8ff4, and an offline decode of the Gen3 DART, 2026-10-23): RUN alone gave
 * a core that left STOPPED and posted nothing; a later board read of the ANE DART (boot 5f6aa530, PR 183 and 185) found every
 * ENABLE_STREAMS word 0, so iBoot left no stream enabled on the M6. macOS maps TEXT and DATA through the ANE DART before it
 * releases the core. This stage reads, and only reads, what the T6030 DART says, so a tester's first boot answers
 * it instead of a core start.
 *
 * T6030 differs from the M6 here: its ANE DART is `dart,t8110` (ADT S1, data/ane-soc/t6030.json), the register
 * file drivers/iommu/apple-dart.c already drives (PARAMS3 +0x08, PARAMS4 +0x0c, PROTECT +0x200, PROTECT_LOCK
 * +0x208, ENABLE_STREAMS +0xc00, TCR +0x1000+4*sid, TTBR +0x1400+4*sid, one TTBR per stream). The ADT lists four
 * 0x4000 windows; which of them is the main block is not stated (the M6 has a DAPF block among its five). So
 * only a window whose PARAMS3 version equals window 0's is scanned; the others are logged and left alone.
 *
 * Reads: PARAMS3 of each window, then for each scanned window PARAMS4, PROTECT, PROTECT_LOCK, ENABLE_STREAMS
 * word 0 and 1, TCR for streams 0..63 and TTBR where TCR is non-zero. All inside engine+0x1800000..+0x1824000, below the
 * T6021 fatal range, behind the same ACTUAL=0xf gate, each group logged before it is read. A stream is "live"
 * when TCR translate is set, TTBR is valid and its ENABLE bit is set. The stage writes nothing.
 */
#define ANE_H15_DART_WIN	0x4000u
#define ANE_H15_DART_PARAMS3	0x08u
#define ANE_H15_DART_PARAMS4	0x0cu
#define ANE_H15_DART_PROTECT	0x200u
#define ANE_H15_DART_LOCK	0x208u
#define ANE_H15_DART_ENABLE	0xc00u
#define ANE_H15_DART_TCR	0x1000u
#define ANE_H15_DART_TTBR	0x1400u
#define ANE_H15_DART_SIDS	64u

static const u32 ane_h15_dart_rel[4] = { 0, 0x10000, 0x20000, 0x4000 };

static bool ane_h15_dart_off_ok(u32 off)
{
	return off == ANE_H15_DART_PARAMS3 || off == ANE_H15_DART_PARAMS4 ||
	       off == ANE_H15_DART_PROTECT || off == ANE_H15_DART_LOCK ||
	       off == ANE_H15_DART_ENABLE || off == ANE_H15_DART_ENABLE + 4 ||
	       (off >= ANE_H15_DART_TCR && off < ANE_H15_DART_TCR + 4 * ANE_H15_DART_SIDS && !(off & 3)) ||
	       (off >= ANE_H15_DART_TTBR && off < ANE_H15_DART_TTBR + 4 * ANE_H15_DART_SIDS && !(off & 3));
}

static int ane_h15_dart_rd(struct ane_h15 *ane, unsigned int inst, u32 off, u32 *v)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 woff;

	if (!s->dart_off || inst > 3 || !ane_h15_dart_off_ok(off))
		return -EPERM;
	woff = s->dart_off + ane_h15_dart_rel[inst] + off;
	if ((u64)woff + 4 > s->engine_size || woff + 4 > ANE_H15_FATAL_LO)
		return -EPERM;
	*v = readl_relaxed(ane->engine + woff);
	return 0;
}

static void ane_h15_dart_note(struct ane_h15 *ane, unsigned int inst, const char *what)
{
	const struct ane_h15_soc *s = ane->soc;

	dev_crit(ane->dev, "ane_h15 read DART inst=%u %s pa=%#llx\n", inst, what,
		 s->engine_pa + s->dart_off + ane_h15_dart_rel[inst]);
}

/* *live = number of live streams over the scanned windows; *locked = windows reporting PROTECT_LOCK bit 0. */
static int ane_h15_dart_scan(struct ane_h15 *ane, unsigned int *live, unsigned int *locked)
{
	u32 p3[4] = { 0 }, p4, prot, lock, en[2], tcr, ttbr;
	unsigned int inst, sid, nsid;
	int ret;

	*live = *locked = 0;
	for (inst = 0; inst < 4; inst++) {
		ane_h15_dart_note(ane, inst, "PARAMS3");
		ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PARAMS3, &p3[inst]);
		if (ret)
			return ret;
		dev_info(ane->dev, "ane_h15 dart inst=%u params3=%#x\n", inst, p3[inst]);
		if (inst == 0 && (!p3[0] || p3[0] == 0xffffffffu || !(p3[0] & 0xff00))) {
			dev_err(ane->dev, "ane_h15 dart inst=0 PARAMS3 %#x is not a DART version word\n", p3[0]);
			return -ENODEV;
		}
		if ((p3[inst] & 0xffff) != (p3[0] & 0xffff)) {
			dev_info(ane->dev, "ane_h15 dart inst=%u version differs from inst 0: not scanned\n", inst);
			continue;
		}
		ane_h15_dart_note(ane, inst, "PARAMS4 PROTECT PROTECT_LOCK ENABLE_STREAMS[0..1]");
		ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PARAMS4, &p4);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_PROTECT, &prot);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_LOCK, &lock);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE, &en[0]);
		if (!ret)
			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_ENABLE + 4, &en[1]);
		if (ret)
			return ret;
		nsid = FIELD_GET(GENMASK(8, 0), p4);
		if (nsid > ANE_H15_DART_SIDS)
			nsid = ANE_H15_DART_SIDS;
		dev_info(ane->dev, "ane_h15 dart inst=%u params4=%#x streams=%u protect=%#x lock=%#x enable0=%#x enable1=%#x\n",
			 inst, p4, nsid, prot, lock, en[0], en[1]);
		if (lock & 1)
			(*locked)++;
		ane_h15_dart_note(ane, inst, "TCR[0..] and TTBR[] where TCR is set");
		for (sid = 0; sid < nsid; sid++) {
			bool on;

			ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_TCR + 4 * sid, &tcr);
			if (ret)
				return ret;
			ttbr = 0;
			if (tcr) {
				ret = ane_h15_dart_rd(ane, inst, ANE_H15_DART_TTBR + 4 * sid, &ttbr);
				if (ret)
					return ret;
			}
			on = (en[sid / 32] >> (sid % 32)) & 1;
			if (tcr || ttbr || on)
				dev_info(ane->dev, "ane_h15 dart inst=%u sid=%u tcr=%#x ttbr=%#x enabled=%u\n",
					 inst, sid, tcr, ttbr, on);
			if ((tcr & 1) && (ttbr & 1) && on)
				(*live)++;
		}
	}
	return 0;
}

static int ane_h15_stage_dart(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned int live = 0, locked = 0;
	int ret;

	if (!confirm_read) {
		ane_h15_result(ane, 4, "REFUSED", "confirm_read=1 required");
		return -EPERM;
	}
	if (!s->dart_off) {
		ane_h15_result(ane, 4, "REFUSED", "no ADT DART window for this SoC row");
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
	ret = ane_h15_dart_scan(ane, &live, &locked);
	if (ret) {
		ane_h15_result(ane, 4, "FAIL", "dart-read-refused-or-not-a-dart");
		return ret;
	}
	dev_info(ane->dev, "ane_h15 dart summary live_streams=%u locked_windows=%u\n", live, locked);
	ane_h15_result(ane, 4, "PASS", live ? "dart-live-stream-found" : "dart-no-stream-enabled");
	return 0;
}

/* ---- stage 5 map_fw=1: give the core a DART mapping before RUN ----
 *
 * The pattern of the M6 module (ane/h18, PR 185): attach the ANE to its DART through the iommu API, so the DART
 * driver programs tunables, TTBR, TCR and the stream enable with the domains powered, then iommu_map() the two segments
 * the boot loader left in DRAM at the addresses the firmware is linked at. A later M6 board report, relayed without a
 * merged receipt: with the map in place the firmware sent HELLO and EPMAP and accepted replies. None of it has run on
 * an H15 part.
 *
 * What this module cannot know and the tester must give: the physical addresses, which iBoot picks at boot and which the
 * Linux device tree does not carry for the ANE (m1n1 writes asc-firmware reserved regions only for dcp, dcpext and ave).
 * They come from the live ADT segment-ranges of the same boot, as `fw_seg=TEXTPA:SIZE,DATAPA:SIZE`. A wrong address would
 * hand the core a window onto kernel memory, so each segment must be 16 KiB aligned, at most 64 MiB, disjoint from the
 * other and from system RAM (carved-out firmware memory is not in the RAM map). The IOVAs are not asked for: TEXT goes
 * to the RVBAR entry address (rvbar_expect without its valid bit, 0x10000000000 on the T6030) and DATA directly behind it,
 * which is how the M6 ADT lays out its two segments (remap = vm-base + cumulative offset; INFERRED the same on H15).
 * TEXT is read only, DATA read+write (the DART format has no execute bit).
 */
#define ANE_H15_MAP_MAX 0x4000000ull

static int ane_h15_parse_segs(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	unsigned long long pa[2], size[2];
	unsigned int i;

	if (!fw_seg || sscanf(fw_seg, "%llx:%llx,%llx:%llx", &pa[0], &size[0], &pa[1], &size[1]) != 4) {
		dev_err(ane->dev, "ane_h15 map_fw: fw_seg=TEXTPA:SIZE,DATAPA:SIZE (hex) is required\n");
		return -EINVAL;
	}
	for (i = 0; i < 2; i++) {
		if (!size[i] || size[i] > ANE_H15_MAP_MAX || ((pa[i] | size[i]) & (SZ_16K - 1)) || pa[i] + size[i] < pa[i]) {
			dev_err(ane->dev, "ane_h15 map_fw: segment %u pa=%#llx size=%#llx is empty, over 64 MiB, not 16 KiB aligned or wraps\n",
				i, pa[i], size[i]);
			return -EINVAL;
		}
		if (region_intersects(pa[i], size[i], IORESOURCE_SYSTEM_RAM, IORES_DESC_NONE) != REGION_DISJOINT) {
			dev_err(ane->dev, "ane_h15 map_fw: segment %u pa=%#llx size=%#llx touches system RAM: refusing to map it into the ANE\n",
				i, pa[i], size[i]);
			return -EBUSY;
		}
	}
	if (pa[0] < pa[1] + size[1] && pa[1] < pa[0] + size[0]) {
		dev_err(ane->dev, "ane_h15 map_fw: the two segments overlap\n");
		return -EINVAL;
	}
	ane->map_pa[0] = pa[0];
	ane->map_pa[1] = pa[1];
	ane->map_size[0] = size[0];
	ane->map_size[1] = size[1];
	ane->map_iova[0] = s->rvbar_expect & ~1ull;
	ane->map_iova[1] = ane->map_iova[0] + size[0];
	return 0;
}

/* Undo ane_h15_map_firmware(). NULL-safe. Never called while the core may be running. */
static void ane_h15_unmap_firmware(struct ane_h15 *ane)
{
	unsigned int i;

	if (!ane->domain)
		return;
	for (i = 0; i < ane->map_n; i++)
		iommu_unmap(ane->domain, ane->map_iova[i], ane->map_size[i]);
	ane->map_n = 0;
	iommu_detach_device(ane->domain, ane->dev);
	iommu_domain_free(ane->domain);
	ane->domain = NULL;
}

static int ane_h15_map_firmware(struct ane_h15 *ane)
{
	struct iommu_domain *dom;
	unsigned int i;
	int ret;

	dom = iommu_paging_domain_alloc(ane->dev);
	if (IS_ERR(dom)) {
		dev_err(ane->dev, "ane_h15 map_fw: no iommu domain: %ld\n", PTR_ERR(dom));
		return PTR_ERR(dom);
	}
	dev_crit(ane->dev, "ane_h15 attach the ANE to a new DART domain (the DART driver programs the DART now)\n");
	ret = iommu_attach_device(dom, ane->dev);
	if (ret) {
		dev_err(ane->dev, "ane_h15 map_fw: attach failed: %d\n", ret);
		iommu_domain_free(dom);
		return ret;
	}
	ane->domain = dom;
	for (i = 0; i < 2; i++) {
		dev_crit(ane->dev, "ane_h15 iommu_map iova=%#llx pa=%#llx size=%#llx %s\n", ane->map_iova[i], ane->map_pa[i],
			 ane->map_size[i], i ? "read+write" : "read");
		ret = iommu_map(dom, ane->map_iova[i], ane->map_pa[i], ane->map_size[i],
				IOMMU_READ | (i ? IOMMU_WRITE : 0), GFP_KERNEL);
		if (!ret) {
			ane->map_n = i + 1;
			if (iommu_iova_to_phys(dom, ane->map_iova[i]) != ane->map_pa[i])
				ret = -EIO;
		}
		if (ret) {
			dev_err(ane->dev, "ane_h15 map_fw: iommu_map of segment %u failed: %d (or reads back wrong)\n", i, ret);
			ane_h15_unmap_firmware(ane);
			return ret;
		}
	}
	return 0;
}

/* ---- stage 5: start the ASC core once, watch, stop it again ----
 *
 * Sources, commit-pinned, with H15 observations told apart from cross-generation inference:
 * ane/h15/run-stage-sources.md (m1n1 asc.py 97820568, macOS _runCPU decode sha d383f674, ane_t6021.h
 * 16cfa8720b80677dbcdf8fd1d04077a8b72768bf, omarchy-linux mailbox.c 57f8f6de, the M6 boot 235c8ff4).
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
 *   2. checks both live reg windows against the SoC row, waits for the five pmgr words to read ACTUAL=0xf,
 *      then runs the stage 4 DART scan and refuses if no stream is live (run_unmapped=1 overrides);
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

/* Stopped = STOPPED set and RUNNING clear (m1n1 R_CPU_STATUS; the M6 RUN write cleared STOPPED, RUNNING never read 1). */
static bool ane_h15_core_stopped(u32 st)
{
	return !(st & ANE_H15_ST_RUNNING) && (st & ANE_H15_ST_STOPPED);
}

/* ---- stage 6: an RTKit management session over the ASC mailbox ----
 *
 * The M6 module (ane/h18, PR 186 and 190) runs this session on the M6's Postoffice: the firmware sent HELLO (version
 * 12), an endpoint map (endpoints 0, 1 and 0x20), and acknowledged the power states; the host answered HELLO, the
 * map, started the system endpoints, answered the IOP power ON acknowledgement with AP power ON, started the
 * application endpoint, and shut the session down with AP QUIESCED then IOP SLEEP (restartable) before it cleared RUN.
 * That is the Linux apple-rtkit message sequence (drivers/soc/apple/rtkit.c, management endpoint 0): message type in
 * bits 59:52 of msg0, the endpoint in the low byte of msg1.
 *
 * What differs here: the transport. The M6 ANE (ascwrap-v8) uses a Postoffice window. The H15 ANE is
 * `iop,ascwrap-v6`, the ASC wrapper has its own mailbox at wrapper + 0x8000, and its two CTRL words read 0x20001 (empty)
 * on a T6030 (PR 175). The send and receive registers used here are the apple-mailbox v4 layout the other T6030 ASCs
 * use: A2I_SEND0 +0x800, A2I_SEND1 +0x808 (64-bit, SEND0 first), I2A_RECV0 +0x830, I2A_RECV1 +0x838 (64-bit, pop on
 * read). INFERRED for the ANE, never written on an H15 part. Whether the H15 ANE firmware talks RTKit on this mailbox
 * at all is what stage 5's raw message log would show: run stage 6 only after a stage 5 result with a message in it.
 *
 * Writes of this stage beyond stage 5: ONLY the two send registers, 16 messages at most, never when A2I_CTRL reads
 * FULL, never anything but the management endpoint (msg1 = 0). It answers no buffer request (the firmware's crash log
 * request on endpoint 1 stays unanswered, as on the M6) and nothing it does not recognise. Sources:
 * ane/h15/run-stage-sources.md and the h18 module.
 */
#define ANE_H15_MBOX_FULL	BIT(16)
#define ANE_H15_MBOX_A2I_SEND0	0x800u
#define ANE_H15_MBOX_A2I_SEND1	0x808u
#define ANE_H15_RTK_TYPE_SHIFT	52
#define ANE_H15_RTK_HELLO	1
#define ANE_H15_RTK_HELLO_REPLY	2
#define ANE_H15_RTK_STARTEP	5
#define ANE_H15_RTK_SET_IOP_PWR	6
#define ANE_H15_RTK_IOP_PWR_ACK	7
#define ANE_H15_RTK_EPMAP	8
#define ANE_H15_RTK_AP_PWR	0xb
#define ANE_H15_RTK_IOP_IDLE	0xc
#define ANE_H15_RTK_PWR_OFF	0x00
#define ANE_H15_RTK_PWR_SLEEP	0x01
#define ANE_H15_RTK_PWR_QUIESCED 0x10
#define ANE_H15_RTK_PWR_ON	0x20
#define ANE_H15_RTK_APP_EP	0x20
#define ANE_H15_RTK_VERSION	12
#define ANE_H15_RTK_MAXSEND	16
#define ANE_H15_RTK_MAXRX_TICK	4
#define ANE_H15_RTK_SHUTDOWN_MS	600

struct ane_h15_rtk {
	bool hello, epmap_last, ap_sent, ap_on, app_started, shutdown, ap_acked, iop_acked, iop_idle;
	u32 eps[8];
	u32 ap_state, iop_state;
	unsigned int sent, rx;
};

static int ane_h15_rtk_send(struct ane_h15 *ane, struct ane_h15_rtk *k, u64 m0)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 a2i = 0;
	unsigned int tries;
	int ret = 0;

	if (k->sent >= ANE_H15_RTK_MAXSEND) {
		dev_warn(ane->dev, "ane_h15 rtkit: send cap reached, not sending %#llx\n", m0);
		return -EBUSY;
	}
	if ((u64)s->mbox + ANE_H15_MBOX_A2I_SEND1 + 8 > s->engine_size || s->mbox + ANE_H15_MBOX_A2I_SEND0 < ANE_H15_WRAP_OFF ||
	    (u64)s->mbox + ANE_H15_MBOX_A2I_SEND1 + 8 > (u64)ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE)
		return -EPERM;
	for (tries = 0; tries < 10; tries++) {
		ret = ane_h15_read_word(ane, "MBOX_A2I_CTRL", s->mbox + 0x110, &a2i);
		if (ret)
			return ret;
		if (!(a2i & ANE_H15_MBOX_FULL))
			break;
		usleep_range(1000, 2000);
	}
	if (a2i & ANE_H15_MBOX_FULL) {
		dev_warn(ane->dev, "ane_h15 rtkit: A2I mailbox stays FULL, not sending %#llx\n", m0);
		return -EBUSY;
	}
	dev_crit(ane->dev, "ane_h15 write MBOX_A2I_SEND0=%#llx pa=%#llx then SEND1=0 (management endpoint)\n", m0,
		 s->engine_pa + s->mbox + ANE_H15_MBOX_A2I_SEND0);
	writeq_relaxed(m0, ane->engine + s->mbox + ANE_H15_MBOX_A2I_SEND0);
	writeq_relaxed(0, ane->engine + s->mbox + ANE_H15_MBOX_A2I_SEND1);
	k->sent++;
	return 0;
}

static bool ane_h15_rtk_system_ep(unsigned int ep)
{
	return ep == 1 || ep == 2 || ep == 3 || ep == 4 || ep == 8 || ep == 0xa;
}

/* One received message: reply to HELLO and the endpoint map, start the endpoints, walk the power states. */
static void ane_h15_rtk_handle(struct ane_h15 *ane, struct ane_h15_rtk *k, u64 m0, u64 m1)
{
	unsigned int type = (m0 >> ANE_H15_RTK_TYPE_SHIFT) & 0xff, i;

	dev_info(ane->dev, "ane_h15 rtkit rx type=%u ep=%#llx msg0=%#llx\n", type, m1 & 0xff, m0);
	if (m1 & 0xff)
		return;			/* not the management endpoint: logged, never answered */
	if (type == ANE_H15_RTK_HELLO) {
		unsigned int minv = m0 & 0xffff, maxv = (m0 >> 16) & 0xffff;

		k->hello = true;
		if (minv > ANE_H15_RTK_VERSION || maxv < ANE_H15_RTK_VERSION) {
			dev_warn(ane->dev, "ane_h15 rtkit: firmware versions %u..%u do not include %u, no reply\n", minv, maxv, ANE_H15_RTK_VERSION);
			return;
		}
		ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_HELLO_REPLY << ANE_H15_RTK_TYPE_SHIFT) |
				 ((u64)ANE_H15_RTK_VERSION << 16) | ANE_H15_RTK_VERSION);
	} else if (type == ANE_H15_RTK_EPMAP) {
		unsigned int base = (m0 >> 32) & 7;
		bool last = m0 & (1ULL << 51);

		for (i = 0; i < 32; i++)
			if (m0 & (1ULL << i)) {
				dev_info(ane->dev, "ane_h15 rtkit endpoint %#x discovered\n", base * 32 + i);
				k->eps[base] |= 1u << i;
			}
		if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_EPMAP << ANE_H15_RTK_TYPE_SHIFT) | (m0 & (7ULL << 32)) |
				     (last ? (1ULL << 51) : 1ULL)))
			return;
		if (last) {
			k->epmap_last = true;
			for (i = 0; i < 256; i++) {
				if (!(k->eps[i / 32] & (1u << (i % 32))) || !ane_h15_rtk_system_ep(i))
					continue;
				if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_STARTEP << ANE_H15_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1)))
					break;
			}
		}
	} else if (type == ANE_H15_RTK_IOP_PWR_ACK) {
		dev_info(ane->dev, "ane_h15 rtkit: IOP power state acknowledged: %#llx\n", m0 & 0xffff);
		if (k->shutdown && (m0 & 0xffff) == k->iop_state)
			k->iop_acked = true;
		else if (!k->shutdown && !k->ap_sent && (m0 & 0xff) == ANE_H15_RTK_PWR_ON &&
			 !ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_AP_PWR << ANE_H15_RTK_TYPE_SHIFT) | ANE_H15_RTK_PWR_ON))
			k->ap_sent = true;
	} else if (type == ANE_H15_RTK_AP_PWR) {
		dev_info(ane->dev, "ane_h15 rtkit: AP power state acknowledged: %#llx\n", m0 & 0xffff);
		if (k->shutdown && (m0 & 0xffff) == k->ap_state) {
			k->ap_acked = true;
			ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_SET_IOP_PWR << ANE_H15_RTK_TYPE_SHIFT) | k->iop_state);
		} else if (!k->shutdown && (m0 & 0xff) == ANE_H15_RTK_PWR_ON && !k->app_started) {
			k->ap_on = true;
			k->app_started = true;
			for (i = ANE_H15_RTK_APP_EP; i < 256; i++) {
				if (!(k->eps[i / 32] & (1u << (i % 32))))
					continue;
				if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_STARTEP << ANE_H15_RTK_TYPE_SHIFT) | ((u64)i << 32) | (1ULL << 1)))
					break;
			}
		}
	} else if (type == ANE_H15_RTK_IOP_IDLE) {
		dev_info(ane->dev, "ane_h15 rtkit: the firmware reports IOP idle\n");
		if (k->shutdown)
			k->iop_idle = true;
	}
}

/* Drain at most ANE_H15_RTK_MAXRX_TICK messages: I2A_CTRL, then RECV0 and RECV1 as 64-bit reads. */
static int ane_h15_rtk_poll_once(struct ane_h15 *ane, struct ane_h15_rtk *k)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 i2a = 0;
	u64 m0, m1;
	unsigned int n = 0;
	int ret;

	ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	while (!ret && !(i2a & ANE_H15_MBOX_EMPTY) && n < ANE_H15_RTK_MAXRX_TICK) {
		m0 = m1 = 0;
		ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV0", s->mbox + ANE_H15_MBOX_I2A_RECV0, &m0);
		if (!ret)
			ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV1", s->mbox + ANE_H15_MBOX_I2A_RECV1, &m1);
		if (ret)
			break;
		dev_info(ane->dev, "ane_h15 msg%u raw0=%#llx raw1=%#llx\n", k->rx++, m0, m1);
		ane_h15_rtk_handle(ane, k, m0, m1);
		n++;
		ret = ane_h15_read_word(ane, "MBOX_I2A_CTRL", s->mbox + 0x114, &i2a);
	}
	return ret;
}

/* Start phase until AP power ON is acknowledged and the application endpoints are started (plus 100 ms to log what
 * follows), then the shutdown phase. The caller clears RUN afterwards. */
static void ane_h15_rtk_session(struct ane_h15 *ane, struct ane_h15_rtk *k)
{
	unsigned int waited, settle = 0;

	for (waited = 0; waited < rtkit_wait_ms && settle < 100; waited += 10) {
		usleep_range(10000, 11000);
		if (ane_h15_rtk_poll_once(ane, k))
			return;
		if (k->app_started)
			settle += 10;
	}
	dev_info(ane->dev, "ane_h15 rtkit session: hello=%d epmap_last=%d ap_on=%d app_started=%d sent=%u after %ums\n",
		 k->hello, k->epmap_last, k->ap_on, k->app_started, k->sent, waited);
	if (!k->ap_on)
		return;			/* no session to shut down */
	k->shutdown = true;
	k->ap_state = rtkit_stop == 2 ? ANE_H15_RTK_PWR_OFF : ANE_H15_RTK_PWR_QUIESCED;
	k->iop_state = rtkit_stop == 2 ? ANE_H15_RTK_PWR_OFF : ANE_H15_RTK_PWR_SLEEP;
	k->sent = 0;			/* the shutdown has its own send budget */
	if (ane_h15_rtk_send(ane, k, ((u64)ANE_H15_RTK_AP_PWR << ANE_H15_RTK_TYPE_SHIFT) | k->ap_state))
		return;
	for (waited = 0; waited < ANE_H15_RTK_SHUTDOWN_MS && !k->iop_acked && !k->iop_idle; waited += 10) {
		usleep_range(10000, 11000);
		if (ane_h15_rtk_poll_once(ane, k))
			return;
	}
	dev_info(ane->dev, "ane_h15 rtkit shutdown: ap_acked=%d iop_acked=%d iop_idle=%d after %ums\n", k->ap_acked, k->iop_acked,
		 k->iop_idle, waited);
}

static int ane_h15_run_common(struct ane_h15 *ane, unsigned int stg)
{
	const struct ane_h15_soc *s = ane->soc;
	u32 st = 0, ctrl = 0, rv_lo = 0, rv_hi = 0, a2i = 0, i2a = 0, last = 0xffffffffu;
	u64 m0, m1;
	unsigned int waited, i, msgs = 0, started_seen = 0, posted = 0, live = 0, locked = 0;
	bool stopped = false;
	struct ane_h15_rtk rtk = { 0 };
	int ret;

	if (!confirm_run) {
		ane_h15_result(ane, stg, "REFUSED", "confirm_run=1 required");
		return -EPERM;
	}
	if (run_wait_ms < 10 || run_wait_ms > 2000) {
		ane_h15_result(ane, stg, "REFUSED", "run_wait_ms 10 to 2000");
		return -EINVAL;
	}
	if (stg == 6 && !confirm_rtkit) {
		ane_h15_result(ane, stg, "REFUSED", "confirm_rtkit=1 required");
		return -EPERM;
	}
	if (stg == 6 && (rtkit_wait_ms < 100 || rtkit_wait_ms > 10000 || rtkit_stop < 1 || rtkit_stop > 2)) {
		ane_h15_result(ane, stg, "REFUSED", "rtkit_wait_ms 100 to 10000, rtkit_stop 1 or 2");
		return -EINVAL;
	}
	if (!s->rvbar_expect) {
		ane_h15_result(ane, stg, "REFUSED", "no measured RVBAR for this SoC row: run stage 2 and report it");
		return -ENODEV;
	}
	if (map_fw) {
		if (!device_iommu_mapped(ane->dev)) {
			dev_err(ane->dev, "ane_h15 map_fw: the node has no iommus (no DART client): use the DART overlay\n");
			ane_h15_result(ane, stg, "REFUSED", "map_fw needs iommus in the node");
			return -ENODEV;
		}
		ret = ane_h15_parse_segs(ane);
		if (ret) {
			ane_h15_result(ane, stg, "REFUSED", "fw_seg missing or unsafe");
			return ret;
		}
	}
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size)) {
		ane_h15_result(ane, stg, "FAIL", "window-mismatch");
		return -EINVAL;
	}
	ret = ane_h15_ps_wait(ane);
	if (ret) {
		ane_h15_result(ane, stg, "FAIL", "pmgr-actual-stuck");
		return ret;
	}
	if (map_fw) {
		/* Attaching replaces the DART state a running core is fetching through: look first, and only on a stopped core. */
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (ret) {
			ane_h15_result(ane, stg, "FAIL", "read-refused");
			return ret;
		}
		if (!ane_h15_core_stopped(st)) {
			dev_err(ane->dev, "ane_h15 map_fw: cpu_status=%#x, the core is not stopped: not touching the DART\n", st);
			ane_h15_result(ane, stg, "FAIL", "core-not-stopped-before-map");
			return -EBUSY;
		}
		ret = ane_h15_map_firmware(ane);
		if (ret) {
			ane_h15_result(ane, stg, "FAIL", "fw-map-failed");
			return ret;
		}
	}
	/* The M6 lesson: RUN alone was silent and iBoot left no stream enabled. Without one the core cannot fetch. */
	ret = ane_h15_dart_scan(ane, &live, &locked);
	if (ret) {
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "dart-read-refused-or-not-a-dart");
		return ret;
	}
	dev_info(ane->dev, "ane_h15 dart summary live_streams=%u locked_windows=%u\n", live, locked);
	if (!live && !run_unmapped) {
		dev_err(ane->dev, "ane_h15 no ANE DART stream is enabled and mapped: the core could not fetch its firmware; nothing written (run_unmapped=1 overrides)\n");
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "no-dart-stream-enabled");
		return -ENODEV;
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
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "read-refused");
		return ret;
	}
	if ((st & ANE_H15_ST_RUNNING) || !(st & ANE_H15_ST_STOPPED) || (ctrl & ANE_H15_CPU_RUN) ||
	    (((u64)rv_hi << 32) | rv_lo) != s->rvbar_expect ||
	    !(a2i & ANE_H15_MBOX_EMPTY) || !(i2a & ANE_H15_MBOX_EMPTY)) {
		dev_err(ane->dev,
			"ane_h15 preflight: cpu_status=%#x cpu_control=%#x rvbar=%#llx a2i_ctrl=%#x i2a_ctrl=%#x (want stopped, RUN clear, rvbar %#llx, both mailbox words EMPTY)\n",
			st, ctrl, ((u64)rv_hi << 32) | rv_lo, a2i, i2a, s->rvbar_expect);
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "preflight-not-as-iboot-left-it");
		return -EBUSY;
	}

	/* Start the core. From here a reboot may be needed. */
	ane->fw_started = true;
	ane->fw_touched = true;
	ret = ane_h15_write_ctrl(ane, ctrl, ctrl | ANE_H15_CPU_RUN, "RUN");
	if (ret) {
		ane->fw_started = false;
		ane->fw_touched = false;
		ane_h15_unmap_firmware(ane);
		ane_h15_result(ane, stg, "FAIL", "write-refused");
		return ret;
	}
	if (stg == 6) {
		ane_h15_rtk_session(ane, &rtk);
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &st);
		if (!ret && !(st & ANE_H15_ST_STOPPED))
			started_seen = 1;
	}
	for (waited = 0; stg == 5 && waited < run_wait_ms; waited += 10) {
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
			ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV0", s->mbox + ANE_H15_MBOX_I2A_RECV0, &m0);
			if (!ret)
				ret = ane_h15_read_word64(ane, "MBOX_I2A_RECV1", s->mbox + ANE_H15_MBOX_I2A_RECV1, &m1);
			if (ret)
				break;
			dev_info(ane->dev, "ane_h15 msg%u raw0=%#llx raw1=%#llx\n", msgs, m0, m1);
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
		ane_h15_result(ane, stg, "FAIL", "core-not-stopped");
		/* Pin the module: ane_h15_remove() is void and cannot refuse an rmmod, and unloading would free the code that
		 * owns the power claim, the windows and (map_fw) the DART domain under a core that may still be running. The
		 * reference is never dropped: reboot to park the ANE. */
		__module_get(THIS_MODULE);
		/* 0, not an error: a failed probe would release the domains and the windows under a running core */
		return 0;
	}
	ane->fw_started = false;
	ane->fw_touched = false;
	ane_h15_unmap_firmware(ane);
	if (stg == 6) {
		if (!rtk.hello) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-no-hello");
			return -ETIMEDOUT;
		}
		if (!(rtk.epmap_last && rtk.ap_on && rtk.app_started)) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-session-incomplete");
			return -EIO;
		}
		if (!(rtk.ap_acked && rtk.iop_acked)) {
			ane_h15_result(ane, stg, "FAIL", "rtkit-shutdown-incomplete");
			return -EIO;
		}
		ane_h15_result(ane, stg, "PASS", "rtkit-session-and-shutdown");
		return 0;
	}
	ane_h15_result(ane, stg, "PASS", started_seen ? "core-started-and-stopped" : "core-never-left-stopped");
	return 0;
}

static int ane_h15_stage_run(struct ane_h15 *ane)
{
	return ane_h15_run_common(ane, 5);
}

static int ane_h15_stage_rtkit(struct ane_h15 *ane)
{
	return ane_h15_run_common(ane, 6);
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
	else if (stage_idx == 4)  ret = ane_h15_stage_dart(ane);
	else if (stage_idx == 5)  ret = ane_h15_stage_run(ane);
	else if (stage_idx == 6)  ret = ane_h15_stage_rtkit(ane);
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
MODULE_DESCRIPTION("OPT-IN EXPERIMENTAL Apple H15-family ANE bring-up module (dt/status/wrapper/boot/dart/run/rtkit; no inference)");
MODULE_LICENSE("Dual MIT/GPL");
