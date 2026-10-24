// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* sim/sim_harness.c — drives the REAL ane_h15 module source through
 * every stage it implements, against the userspace shim, with positive
 * and negative controls (PMP vehicle harness pattern).
 *
 * Modes (argv[1]):
 *   pos                every positive control; exit 0 iff all pass
 *   neg-ps-stuck       ANE_CPU word stuck at 0x30; exit 0 iff the module
 *                      refuses (FAIL verdict, no engine touch, no PASS)
 *   neg-wrong-stage    unknown stage string + wrong optin; exit 0 iff
 *                      both refuse with NO RESULT line and no MMIO
 *   neg-truncated-dt   reg property missing/truncated; exit 0 iff
 *                      of_iomap NULL refusal, no invented defaults
 * The sim-negative-nopsguard make target rebuilds this harness against
 * a copy of the source with the PS guard call deleted and REQUIRES the
 * neg-ps-stuck run to fail (harness discriminates the guardless build).
 *
 * A sim is not silicon.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"

/* the module's real sources, unmodified */
#ifdef ANE15_SIM_FILL_FACTS
/* P7 build: compile the real soc file with its fact table renamed
 * aside, then substitute a synthetic fully-filled table, so the REAL
 * probe + stage_boot run their every-fact-filled fall-through. The
 * renamed real table stays in the binary for the substitution check.
 */
#define ane_h15_facts ane_h15_facts_real_unfilled
#define ane_h15_n_facts ane_h15_n_facts_real_unfilled
#include "../ane_h15_soc.c"
#undef ane_h15_facts
#undef ane_h15_n_facts
const struct ane_h15_fact ane_h15_facts[] = {
	{ "H1", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H2", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H3", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H4", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
	{ "H5", "synthetic row, filled for the P7 probe control",
	  "sim only, never a receipt", true },
};

const unsigned int ane_h15_n_facts = ARRAY_SIZE(ane_h15_facts);
#else
#include "../ane_h15_soc.c"
#endif
#include "../ane_h15_main.c"

/* ---- shim storage ---- */
struct sim_region sim_bus[SIM_MAX_REGIONS];
int sim_n_regions;
struct sim_event sim_events[SIM_MAX_EVENTS];
int sim_n_events;
int sim_bus_faults;
int sim_n_iomap;
int sim_n_iounmap;
char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
unsigned long sim_jiffies;
int sim_pm_get;
int sim_pm_put;
int sim_pm_get_fail;
int sim_pm_get_err;
int sim_pm_noidle;
struct device sim_pd_dev[SIM_MAX_PD];
struct device_link sim_pd_link[SIM_MAX_PD];
int sim_pd_state[SIM_MAX_PD];
int sim_link_state[SIM_MAX_PD];
int sim_pd_attach;
int sim_pd_detach;
int sim_link_add;
int sim_link_del;
int sim_pd_bad;
int sim_pd_attach_fail_at = -1;
int sim_link_fail_at = -1;
int sim_pd_attach_calls;
int sim_module_gets;
int sim_region_ram = REGION_DISJOINT;
struct sim_iocall sim_iocalls[64];
int sim_n_iocalls, sim_io_fail, sim_n_dom_live;
void (*sim_attach_hook)(void);
int sim_link_calls;

/* ---- helpers ---- */
static int fails;

static void chk(int ok, const char *name)
{
	printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
	if (!ok)
		fails++;
}

static int log_count(const char *needle)
{
	const char *p = sim_log;
	int n = 0;

	while ((p = strstr(p, needle))) {
		n++;
		p++;
	}
	return n;
}

/* Parse the one RESULT line: ane_h15 RESULT stage=%u soc=%s verdict=%s
 * reason=%s (reason runs to end of line). Returns false if absent.
 */
static bool parse_result(unsigned int *stage, char *verdict, size_t vsz,
			 char *reason, size_t rsz)
{
	const char *p = strstr(sim_log, "ane_h15 RESULT stage=");
	const char *v, *r, *e;
	char soc[32];

	if (!p)
		return false;
	if (sscanf(p, "ane_h15 RESULT stage=%u soc=%31s", stage, soc) != 2)
		return false;
	v = strstr(p, "verdict=");
	r = v ? strstr(v, " reason=") : NULL;
	if (!v || !r)
		return false;
	v += 8;				/* past "verdict=" */
	e = v;
	while (*e && *e != ' ' && *e != '\n')
		e++;
	snprintf(verdict, vsz, "%.*s", (int)(e - v), v);
	snprintf(reason, rsz, "%s", r + 8);
	{
		char *nl = strchr(reason, '\n');

		if (nl)
			*nl = '\0';
	}
	return true;
}

/* Collect `ane_h15 hole=Hx` ids in emission order. */
static int collect_holes(char ids[][8], int max)
{
	const char *p = sim_log;
	int n = 0;

	while (n < max && (p = strstr(p, "ane_h15 hole="))) {
		if (sscanf(p, "ane_h15 hole=%7s", ids[n]) != 1)
			break;
		n++;
		p++;
	}
	return n;
}

static int events_in(u64 base, u64 size, int writes_only)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].pa >= base && sim_events[i].pa < base + size &&
		    (!writes_only || sim_events[i].write))
			n++;
	return n;
}

/* Global invariants for every run: the h15 module this build ships is
 * read-only and refuses, so ANY bus write, ANY engine-window access,
 * or ANY fault (unmapped access) is a harness failure.
 */
static void chk_global_invariants(const struct ane_h15_soc *s,
				  const char *tag)
{
	char name[96];

	snprintf(name, sizeof(name), "%s: zero bus writes", tag);
	chk(events_in(0, ~0ULL, 1) == 0, name);
	snprintf(name, sizeof(name), "%s: zero engine-window events", tag);
	chk(s && events_in(s->engine_pa, s->engine_size, 0) == 0, name);
	snprintf(name, sizeof(name), "%s: zero unmapped-access faults", tag);
	chk(sim_bus_faults == 0, name);
}

/* ---- per-test scaffolding ---- */
static struct platform_device pdev;
static struct device_node node;
static const struct ane_h15_soc *cur_soc;

static void reset_all(void)
{
	sim_n_regions = 0;
	sim_n_events = 0;
	sim_bus_faults = 0;
	sim_n_iomap = 0;
	sim_n_iounmap = 0;
	sim_log_reset();
	sim_jiffies = 0;
	sim_pm_get = 0;
	sim_pm_put = 0;
	sim_pm_get_fail = 0;
	sim_pm_get_err = 0;
	sim_pm_noidle = 0;
	memset(sim_pd_state, 0, sizeof(sim_pd_state));
	memset(sim_link_state, 0, sizeof(sim_link_state));
	sim_pd_attach = sim_pd_detach = sim_link_add = sim_link_del = 0;
	sim_pd_bad = 0;
	sim_pd_attach_fail_at = -1;
	sim_link_fail_at = -1;
	sim_pd_attach_calls = 0;
	sim_link_calls = 0;
	memset(&pdev, 0, sizeof(pdev));
	memset(&node, 0, sizeof(node));
	optin = NULL;
	stage = "status";
	ps_wait_ms = 500;
	confirm_boot = false;
	confirm_read = false;
	confirm_run = false;
	run_unmapped = false;
	sim_module_gets = 0;
	map_fw = false;
	fw_seg = NULL;
	sim_region_ram = REGION_DISJOINT;
	sim_n_iocalls = sim_io_fail = sim_n_dom_live = 0;
	sim_attach_hook = NULL;
	run_wait_ms = 300;
	sim_write_hook = NULL;
	sim_read_hook = NULL;
	sim_write64_hook = NULL;
	confirm_rtkit = false;
	rtkit_wait_ms = 2000;
	rtkit_stop = 1;
	fw_path = NULL;
}

/* Build the fake DT node from the REAL soc row and resolve the soc row
 * through the REAL of_match_table, exactly as the OF core would.
 */
static void build_node(const struct ane_h15_soc *s, int nregs,
		       const char *compat)
{
	const struct of_device_id *m;

	for (m = ane_h15_of_match; m->compatible; m++)
		if (!strcmp(m->compatible, compat))
			node.match_data = m->data;
	node.compatible = compat;
	node.has_ane_type = true;
	node.ane_type = s->ane_type;
	node.n_reg = nregs;
	node.reg_pa[0] = s->engine_pa;
	node.reg_size[0] = s->engine_size;
	node.reg_pa[1] = s->pmgr_pa;
	node.reg_size[1] = s->pmgr_size;
	pdev.dev.of_node = &node;
	pdev.dev.name = s->name;
	cur_soc = s;
}

/* Pre-map the pmgr window and set the five ps words (0xffffffff = on);
 * stuck_idx <= 4 pins that word at 0x30 (TARGET 0x0, ACTUAL 0x3).
 */
static void set_ps_words(const struct ane_h15_soc *s, int stuck_idx)
{
	unsigned int i;

	sim_bus_map(s->pmgr_pa, s->pmgr_size);
	for (i = 0; i < 5; i++)
		sim_bus[sim_region_find(s->pmgr_pa) - sim_bus].mem[s->ps_off[i] >> 2] =
			((int)i == stuck_idx) ? 0x30u : 0xffffffffu;
}

static int probe_now(void)
{
	return ane_h15_probe(&pdev);
}

/* T6 test emitter: collect fact ids via the module's own helper. */
static char t6_ids[8][8];
static int t6_n;
static void t6_emit(void *ctx, const struct ane_h15_fact *f)
{
	(void)ctx;
	snprintf(t6_ids[t6_n], sizeof(t6_ids[t6_n]), "%s", f->id);
	t6_n++;
}

/* ---- positive controls ---- */
static void pos_dt_zero_mmio(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	reset_all();
	optin = "t8122";
	stage = "dt";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == 0, "P1 dt: probe returns 0");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 0 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "dt-parse-only"),
	    "P1 dt: RESULT stage=0 PASS dt-parse-only");
	chk(sim_n_events == 0, "P1 dt: ZERO bus events (hardware-silent)");
	chk(sim_n_regions == 0, "P1 dt: nothing even mapped");
	chk_global_invariants(cur_soc, "P1 dt");
}

static void pos_ps_pass(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	ret = probe_now();
	chk(ret == 0, "P2 ps: probe returns 0");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 1 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, "ps-guard+reads"),
	    "P2 ps: RESULT stage=1 PASS ps-guard+reads");
	chk(log_count("actual=0xf pass=true") == 5,
	    "P2 ps: five word lines with actual=0xf");
	chk(sim_pm_get == 1 && sim_pm_put == 1,
	    "P2 ps: genpd claim released (get==put==1)");
	chk_global_invariants(cur_soc, "P2 ps");

	/* remove() must release the windows the successful probe kept */
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P2 ps: remove unmaps both windows");
}

static void pos_refusals(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	char ids[8][8];
	int nids, ret;

	/* stage=2 wrapper: REFUSED before any engine read */
	reset_all();
	optin = "t8122";
	stage = "wrapper";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == -EPERM, "P3 wrapper: probe returns -EPERM");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 2 && !strcmp(verdict, "REFUSED") && reason[0],
	    "P3 wrapper: RESULT stage=2 REFUSED <reason>");
	chk(sim_n_events == 0,
	    "P3 wrapper: refusal precedes any MMIO access");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "P3 wrapper: power released");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P3 wrapper: refused probe unmaps its windows");
	chk_global_invariants(cur_soc, "P3 wrapper");

	/* stage=3 boot: REFUSED, enumerates exactly the unfilled facts */
	reset_all();
	optin = "t8122";
	stage = "boot";
	confirm_boot = true;
	fw_path = "/lib/firmware/synthetic-macho-for-sim";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	chk(ret == -ENOENT, "P4 boot: probe returns -ENOENT");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 3 && !strcmp(verdict, "REFUSED") &&
	    !strcmp(reason, "unfilled-facts"),
	    "P4 boot: RESULT stage=3 REFUSED unfilled-facts");
	nids = collect_holes(ids, 8);
	chk(nids == 5 && !strcmp(ids[0], "H1") && !strcmp(ids[1], "H2") &&
	    !strcmp(ids[2], "H3") && !strcmp(ids[3], "H4") &&
	    !strcmp(ids[4], "H5"),
	    "P4 boot: refusal enumerates exactly H1,H2,H3,H4,H5");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P4 boot: refused probe unmaps its windows");
	chk_global_invariants(cur_soc, "P4 boot");
}

/* T6 gate logic on synthetic tables: pure data walk, no code path that
 * releases anything exists anywhere in the module.
 */
static void pos_gate_logic(void)
{
	struct ane_h15_fact t[8];
	unsigned int i, n = ane_h15_n_facts;

	chk(n == 5, "T6: real table has exactly 5 fact rows");
	chk(!ane_h15_facts_ready(ane_h15_facts, n),
	    "T6: real table is NOT ready (gate closed)");
	chk(ane_h15_facts_report(ane_h15_facts, n, NULL, NULL) == 5,
	    "T6: real table reports 5 unfilled rows");

	memcpy(t, ane_h15_facts, n * sizeof(t[0]));
	t[2].filled = true;		/* synthetic: H3 measured */
	t6_n = 0;
	chk(ane_h15_facts_report(t, n, t6_emit, NULL) == 4,
	    "T6: synthetic H3-filled table reports 4 unfilled");
	chk(t6_n == 4 && !strcmp(t6_ids[0], "H1") &&
	    !strcmp(t6_ids[1], "H2") && !strcmp(t6_ids[2], "H4") &&
	    !strcmp(t6_ids[3], "H5"),
	    "T6: synthetic H3-filled enumeration is H1,H2,H4,H5");

	for (i = 0; i < n; i++)
		t[i].filled = true;
	chk(ane_h15_facts_ready(t, n),
	    "T6: fully-filled synthetic table WOULD open the gate (logic only)");
	chk(ane_h15_facts_report(t, n, t6_emit, NULL) == 0,
	    "T6: fully-filled table emits no refusal lines");
}

static void pos_other_socs(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
	} rows[] = {
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[64], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		reset_all();
		optin = (char *)rows[r].soc->name;
		stage = "status";
		build_node(rows[r].soc, 2, rows[r].compat);
		set_ps_words(rows[r].soc, -1);
		ret = probe_now();
		snprintf(tag, sizeof(tag), "P6 %s", rows[r].soc->name);
		snprintf(name, sizeof(name), "%s: probe returns 0", tag);
		chk(ret == 0, name);
		snprintf(name, sizeof(name), "%s: RESULT PASS ps-guard+reads",
			 tag);
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 1 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "ps-guard+reads"), name);
		snprintf(name, sizeof(name), "%s: five word actual=0xf lines",
			 tag);
		chk(log_count("actual=0xf pass=true") == 5, name);
		chk_global_invariants(cur_soc, tag);
	}
}


/* ---- stage 2 preflight (confirm_read=1): read-only ---- */
static void eng_set(const struct ane_h15_soc *s, u32 off, u32 v)
{
	sim_bus[sim_region_find(s->engine_pa + off) - sim_bus].mem[off >> 2] = v;
}

static void stage2_setup(const struct ane_h15_soc *s, const char *compat,
			 int stuck_ps)
{
	reset_all();
	optin = (char *)s->name;
	stage = "wrapper";
	confirm_read = true;
	build_node(s, 2, compat);
	set_ps_words(s, stuck_ps);
	sim_bus_map(s->engine_pa, s->engine_size);
	eng_set(s, s->cpu_status, 0x2a);
	eng_set(s, s->rvbar, 0x1);
	eng_set(s, s->rvbar + 4, 0x0);
	eng_set(s, s->mbox + 0x110, 0x20001);
	eng_set(s, s->mbox + 0x114, 0x20001);
}

/* every engine event, as a pa offset list; returns the count of reads
 * that hit `off` and of all engine events
 */
static int eng_events_at(const struct ane_h15_soc *s, u32 off)
{
	return events_in(s->engine_pa + off, 4, 0);
}

static void pos_stage2(void)
{
	static const struct {
		const struct ane_h15_soc *soc;
		const char *compat;
	} rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
		{ &ane_t6034_soc, "apple,t6034-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[96], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;

		stage2_setup(s, rows[r].compat, -1);
		ret = probe_now();
		snprintf(tag, sizeof(tag), "P9 %s", s->name);
		snprintf(name, sizeof(name), "%s: probe returns 0", tag);
		chk(ret == 0, name);
		snprintf(name, sizeof(name), "%s: RESULT stage=2 PASS preflight-read-only", tag);
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 2 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "preflight-read-only"), name);
		snprintf(name, sizeof(name), "%s: exactly five engine reads at the five words", tag);
		chk(events_in(s->engine_pa, s->engine_size, 0) == 5 &&
		    eng_events_at(s, s->cpu_status) == 1 &&
		    eng_events_at(s, s->rvbar) == 1 &&
		    eng_events_at(s, s->rvbar + 4) == 1 &&
		    eng_events_at(s, s->mbox + 0x110) == 1 &&
		    eng_events_at(s, s->mbox + 0x114) == 1, name);
		snprintf(name, sizeof(name), "%s: CPU_CONTROL never touched", tag);
		chk(eng_events_at(s, s->cpu_control) == 0, name);
		snprintf(name, sizeof(name), "%s: zero bus writes, zero faults", tag);
		chk(events_in(0, ~0ULL, 1) == 0 && sim_bus_faults == 0, name);
		snprintf(name, sizeof(name), "%s: each address logged before its read (5 lines)", tag);
		chk(log_count("ane_h15 read CPU_STATUS") == 1 &&
		    log_count("ane_h15 read RVBAR_LO") == 1 &&
		    log_count("ane_h15 read RVBAR_HI") == 1 &&
		    log_count("ane_h15 read MBOX_A2I_CTRL") == 1 &&
		    log_count("ane_h15 read MBOX_I2A_CTRL") == 1, name);
		snprintf(name, sizeof(name), "%s: values logged", tag);
		chk(log_count("cpu_status=0x2a rvbar=0x1 a2i_ctrl=0x20001 i2a_ctrl=0x20001 t6021_stopped_status=equal") == 1, name);
		snprintf(name, sizeof(name), "%s: pmgr words read before the first engine read", tag);
		{
			int first_eng = -1, last_pmgr = -1, i;

			for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
				if (sim_events[i].pa >= s->engine_pa &&
				    sim_events[i].pa < s->engine_pa + s->engine_size) {
					if (first_eng < 0)
						first_eng = i;
				} else {
					last_pmgr = i;
				}
			}
			chk(first_eng > last_pmgr && last_pmgr >= 4, name);
		}
		snprintf(name, sizeof(name), "%s: power claim released", tag);
		chk(sim_pm_get == 1 && sim_pm_put == 1, name);
		ane_h15_remove(&pdev);
		snprintf(name, sizeof(name), "%s: remove unmaps both windows", tag);
		chk(sim_n_iomap == 2 && sim_n_iounmap == 2, name);
	}
	/* a status word that differs from the T6021 stopped capture */
	stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
	eng_set(&ane_t6030_soc, ane_t6030_soc.cpu_status, 0x1);
	ret = probe_now();
	chk(ret == 0 && log_count("cpu_status=0x1 ") == 1 &&
	    log_count("t6021_stopped_status=differs") == 1,
	    "P9 differing CPU_STATUS is logged, not interpreted");
	ane_h15_remove(&pdev);
}

/* reg[0] or reg[1] that is not the window the SoC row describes (shifted
 * base, shorter, or another block): stage 2 refuses before any bus access
 */
static int neg_read_window(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int i, bad = 0;

	for (i = 0; i < 6; i++) {
		int ret;

		stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
		if (i == 0)
			node.reg_pa[0] += 0x1000000;	/* base moved */
		else if (i == 1)
			node.reg_size[0] -= 0x100000;	/* window shorter */
		else if (i == 2)
			node.reg_pa[0] = ane_t6030_soc.pmgr_pa + 0x100000;	/* some other block */
		else if (i == 3)
			node.reg_pa[1] += 0x10000;	/* pmgr base moved */
		else if (i == 4)
			node.reg_size[1] -= 0x4000;	/* pmgr window shorter */
		else
			node.reg_pa[1] = ane_t6030_soc.engine_pa + 0x100000;	/* pmgr is some other block */
		ret = probe_now();
		if (ret != -EINVAL) {
			printf("    NEG-READ-WINDOW case %d: expected -EINVAL, got %d\n", i, ret);
			bad = 1;
		}
		if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
		    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "window-mismatch")) {
			printf("    NEG-READ-WINDOW case %d: expected RESULT stage=2 FAIL window-mismatch\n", i);
			bad = 1;
		}
		if (sim_n_events != 0 || sim_bus_faults) {
			printf("    NEG-READ-WINDOW case %d: bus access before the window check\n", i);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap || sim_pm_get != sim_pm_put) {
			printf("    NEG-READ-WINDOW case %d: a refused probe leaked a window or the power claim\n", i);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-read-window: harness FAILED the module (BAD)"
			   : "  neg-read-window: module refuses as required");
	return bad;
}

/* stuck pmgr word: no engine read at all */
static int neg_read_ps_stuck(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, bad = 0;

	stage2_setup(&ane_t6030_soc, "apple,t6030-ane", 3);
	ret = probe_now();
	if (ret != -ETIMEDOUT) {
		printf("    NEG-READ-PS-STUCK: expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
	    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "pmgr-actual-stuck")) {
		printf("    NEG-READ-PS-STUCK: expected RESULT stage=2 FAIL pmgr-actual-stuck\n");
		bad = 1;
	}
	if (events_in(ane_t6030_soc.engine_pa, ane_t6030_soc.engine_size, 0) ||
	    events_in(0, ~0ULL, 1) || sim_bus_faults) {
		printf("    NEG-READ-PS-STUCK: engine access, write or fault on a stuck pmgr word\n");
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    NEG-READ-PS-STUCK: leaked a window\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-read-ps-stuck: harness FAILED the module (BAD)"
			   : "  neg-read-ps-stuck: module refuses as required");
	return bad;
}

/* a soc row whose word lies outside the two ADT windows, on CPU_CONTROL
 * or beyond the wrapper: the read is refused and never reaches the bus
 */
static int neg_read_forbidden(void)
{
	static struct ane_h15_soc bad_soc;
	struct {
		const char *what;
		u32 cpu_status, rvbar, mbox;
		u32 forbidden;
	} cases[] = {
		/* RVBAR in the T6021 fatal range */
		{ "rvbar-in-fatal-range", 0x1400048, 0x1854000, 0x1408000, 0x1854000 },
		/* a mailbox word that lands on CPU_CONTROL */
		{ "mbox-on-cpu-control", 0x1400048, 0x1050000, 0x1400044 - 0x110, 0x1400044 },
		/* CPU_STATUS past the end of the wrapper window */
		{ "status-past-wrapper", 0x146c000, 0x1050000, 0x1408000, 0x146c000 },
		/* RVBAR on the CoreSight line */
		{ "rvbar-on-coresight", 0x1400048, 0x1010000, 0x1408000, 0x1010000 },
	};
	unsigned int rstage;
	char verdict[32], reason[128];
	size_t i;
	int bad = 0;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		int ret;

		bad_soc = ane_t6030_soc;
		bad_soc.cpu_status = cases[i].cpu_status;
		bad_soc.rvbar = cases[i].rvbar;
		bad_soc.mbox = cases[i].mbox;
		stage2_setup(&ane_t6030_soc, "apple,t6030-ane", -1);
		node.match_data = &bad_soc;
		cur_soc = &bad_soc;
		ret = probe_now();
		if (ret != -EPERM) {
			printf("    NEG-READ-FORBIDDEN %s: expected -EPERM, got %d\n",
			       cases[i].what, ret);
			bad = 1;
		}
		if (!parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) ||
		    rstage != 2 || strcmp(verdict, "FAIL") || strcmp(reason, "read-refused")) {
			printf("    NEG-READ-FORBIDDEN %s: expected RESULT stage=2 FAIL read-refused\n",
			       cases[i].what);
			bad = 1;
		}
		if (events_in(bad_soc.engine_pa + cases[i].forbidden, 4, 0) ||
		    events_in(0, ~0ULL, 1) || sim_bus_faults) {
			printf("    NEG-READ-FORBIDDEN %s: the forbidden word reached the bus\n",
			       cases[i].what);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap || sim_pm_get != sim_pm_put) {
			printf("    NEG-READ-FORBIDDEN %s: a refused probe leaked a window or the power claim\n",
			       cases[i].what);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-read-forbidden: harness FAILED the module (BAD)"
			   : "  neg-read-forbidden: module refuses as required");
	return bad;
}

/* ---- P7 filled-table probe control (filled build only) ---- */
#ifdef ANE15_SIM_FILL_FACTS
/* P7: the gate logic driven through the REAL probe path with a fully
 * filled table. The filled-table run must refuse (-ENOSYS) with zero
 * bus writes: this build has no boot path, no CPU release.
 */
static void pos_filled_probe(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	reset_all();
	optin = "t8122";
	stage = "boot";
	confirm_boot = true;
	fw_path = "/lib/firmware/synthetic-macho-for-sim";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");

	chk(ane_h15_facts_ready(ane_h15_facts, ane_h15_n_facts),
	    "P7 filled: substituted table is ready (gate open)");
	chk(ane_h15_n_facts_real_unfilled == 5 &&
	    !ane_h15_facts_ready(ane_h15_facts_real_unfilled,
				 ane_h15_n_facts_real_unfilled),
	    "P7 filled: real table renamed aside, still 5 unfilled");

	ret = probe_now();
	chk(ret == -ENOSYS, "P7 filled: probe returns -ENOSYS");
	chk(parse_result(&rstage, verdict, sizeof(verdict),
			 reason, sizeof(reason)) &&
	    rstage == 3 && !strcmp(verdict, "REFUSED") &&
	    !strcmp(reason, "facts-filled-boot-path-not-built"),
	    "P7 filled: RESULT stage=3 REFUSED facts-filled-boot-path-not-built");
	chk(log_count("ane_h15 hole=") == 0,
	    "P7 filled: no hole lines from a filled table");
	chk(sim_pm_get == 1 && sim_pm_put == 1, "P7 filled: power released");
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
	    "P7 filled: refused probe unmaps its windows");
	chk_global_invariants(cur_soc, "P7 filled");
}
#endif

/* ---- negative controls: exit 0 iff the module correctly refuses ---- */
static int neg_ps_stuck(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, bad = 0;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, 2);	/* ANE_CPU stuck at 0x30 */
	ret = probe_now();

	if (ret != -ETIMEDOUT) {
		printf("    NEG-PS-STUCK: expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!parse_result(&rstage, verdict, sizeof(verdict),
			  reason, sizeof(reason)) ||
	    rstage != 1 || strcmp(verdict, "FAIL") ||
	    strcmp(reason, "pmgr-actual-stuck")) {
		printf("    NEG-PS-STUCK: expected RESULT stage=1 FAIL pmgr-actual-stuck\n");
		bad = 1;
	}
	if (log_count("pass=true") != 0) {
		printf("    NEG-PS-STUCK: a pass=true line was logged on a stuck word\n");
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    NEG-PS-STUCK: engine access, fault, or write on stuck word\n");
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    NEG-PS-STUCK: failed probe leaked a window (iomap=%d iounmap=%d)\n",
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-ps-stuck: harness FAILED the module (BAD)"
			   : "  neg-ps-stuck: module refuses as required");
	return bad;
}

static int neg_wrong_stage(void)
{
	unsigned int rstage;
	int ret, bad = 0;

	/* unknown stage string: -EINVAL, NO RESULT, nothing mapped */
	reset_all();
	optin = "t8122";
	stage = "7";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	ret = probe_now();
	if (ret != -EINVAL) {
		printf("    NEG-WRONG-STAGE: expected -EINVAL, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-WRONG-STAGE: a RESULT line leaked out\n");
		bad = 1;
	}
	if (sim_n_events || sim_n_regions) {
		printf("    NEG-WRONG-STAGE: MMIO happened before the stage check\n");
		bad = 1;
	}

	/* wrong optin: -EPERM, NO RESULT, nothing mapped */
	reset_all();
	optin = "t9999";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	ret = probe_now();
	if (ret != -EPERM) {
		printf("    NEG-WRONG-OPTIN: expected -EPERM, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-WRONG-OPTIN: a RESULT line leaked out\n");
		bad = 1;
	}
	if (sim_n_events || sim_bus_faults) {
		printf("    NEG-WRONG-OPTIN: MMIO happened before the optin check\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-wrong-stage: harness FAILED (BAD)"
			   : "  neg-wrong-stage: module refuses as required");
	return bad;
}

static int neg_truncated_dt(void)
{
	int ret, bad = 0;
	static const int nregs_variants[] = { 1, 0 };	/* pmgr missing; empty */
	size_t i;

	for (i = 0; i < ARRAY_SIZE(nregs_variants); i++) {
		reset_all();
		optin = "t8122";
		stage = "status";
		build_node(&ane_t8122_soc, nregs_variants[i],
			   "apple,t8122-ane");
		set_ps_words(&ane_t8122_soc, -1);
		ret = probe_now();
		if (ret != -ENXIO) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): expected -ENXIO, got %d\n",
			       nregs_variants[i], ret);
			bad = 1;
		}
		if (log_count("ane_h15 RESULT") != 0) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): RESULT on a broken tree\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (log_count("of_iomap failed") != 1) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): no of_iomap refusal line\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (sim_n_events) {
			printf("    NEG-TRUNCATED-DT(nreg=%d): MMIO on a broken tree\n",
			       nregs_variants[i]);
			bad = 1;
		}
		if (sim_n_iomap != sim_n_iounmap) {
			printf("    NEG-TRUNCATED-DT(%d): window leak (iomap=%d iounmap=%d)\n",
			       nregs_variants[i], sim_n_iomap,
			       sim_n_iounmap);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-truncated-dt: harness FAILED (BAD)"
			   : "  neg-truncated-dt: module refuses as required");
	return bad;
}

static int neg_rpm_fail(void)
{
	int ret, bad = 0;

	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	set_ps_words(&ane_t8122_soc, -1);
	sim_pm_get_fail = -EIO;		/* power-domains bring-up fails */
	ret = probe_now();

	if (ret != -EIO) {
		printf("    NEG-RPM-FAIL: expected -EIO, got %d\n", ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    NEG-RPM-FAIL: RESULT line before the stage ran\n");
		bad = 1;
	}
	if (sim_pm_get != 0 || sim_pm_put != 0) {
		printf("    NEG-RPM-FAIL: power claim taken on a failed get_sync\n");
		bad = 1;
	}
	if (sim_n_iomap != 2 || sim_n_iounmap != 2) {
		printf("    NEG-RPM-FAIL: failed probe leaked a window (iomap=%d iounmap=%d)\n",
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    NEG-RPM-FAIL: engine access, fault, or write\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-rpm-fail: harness FAILED (BAD)"
			   : "  neg-rpm-fail: module refuses as required");
	return bad;
}

/* ---- multiple power-domains (attach by id + runtime-PM device links) ---- */
static int pd_live(void)
{
	int i, n = 0;

	for (i = 0; i < SIM_MAX_PD; i++)
		n += sim_pd_state[i] == 1;
	return n;
}

static int link_live(void)
{
	int i, n = 0;

	for (i = 0; i < SIM_MAX_PD; i++)
		n += sim_link_state[i] == 1;
	return n;
}

/* A node with n_pd power-domains at stage 1; the caller sets the fault
 * knobs (sim_pd_attach_fail_at, sim_link_fail_at, sim_pm_get_fail)
 * after this returns and before probe_now().
 */
static void pd_setup(int n_pd)
{
	reset_all();
	optin = "t8122";
	stage = "status";
	build_node(&ane_t8122_soc, 2, "apple,t8122-ane");
	node.n_pd = n_pd;
	set_ps_words(&ane_t8122_soc, -1);
}

static void pos_multi_pd(void)
{
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret, variant;

	/* P8: five domains; variant 0 = get_sync succeeds, 1 = get_sync
	 * returns -EACCES (what T6030 stage 1 reported): both must pass
	 */
	for (variant = 0; variant < 2; variant++) {
		pd_setup(5);
		if (variant)
			sim_pm_get_fail = -EACCES;
		ret = probe_now();
		chk(ret == 0, variant ? "P8b pd/-EACCES: probe returns 0"
				      : "P8a pd: probe returns 0");
		chk(parse_result(&rstage, verdict, sizeof(verdict),
				 reason, sizeof(reason)) &&
		    rstage == 1 && !strcmp(verdict, "PASS") &&
		    !strcmp(reason, "ps-guard+reads"),
		    "P8 pd: RESULT stage=1 PASS ps-guard+reads");
		chk(sim_pd_attach == 5 && sim_link_add == 5,
		    "P8 pd: 5 domains attached and 5 links added");
		chk(pd_live() == 5 && link_live() == 5,
		    "P8 pd: all 5 attached and linked after probe");
		if (variant) {
			chk(sim_pm_get == 0 && sim_pm_get_err == 1 &&
			    sim_pm_put == 1,
			    "P8b pd/-EACCES: failing get_sync usage count dropped");
			chk(log_count("main get_sync -13 ignored") == 1,
			    "P8b pd/-EACCES: ignore line logged once");
		} else {
			chk(sim_pm_get == 1 && sim_pm_put == 1,
			    "P8a pd: get==put==1");
		}
		chk_global_invariants(cur_soc, "P8 pd");
		ane_h15_remove(&pdev);
		chk(sim_pd_detach == 5 && sim_link_del == 5,
		    "P8 pd: remove detaches 5 domains and deletes 5 links");
		chk(pd_live() == 0 && link_live() == 0 && sim_pd_bad == 0,
		    "P8 pd: nothing left attached, none released twice");
		chk(sim_n_iomap == 2 && sim_n_iounmap == 2,
		    "P8 pd: remove unmaps both windows");
	}

	/* P9: zero or one domain is the core's job: no attach, no link */
	for (variant = 0; variant < 2; variant++) {
		pd_setup(variant);
		ret = probe_now();
		chk(ret == 0 && sim_pd_attach_calls == 0 && sim_link_calls == 0,
		    variant ? "P9 one domain: module attaches nothing"
			    : "P9 no domain: module attaches nothing");
	}
}

/* Common refusal checks for a failed multi-domain probe. */
static int pd_refused(const char *tag, int ret, int want_ret)
{
	int bad = 0;

	if (ret != want_ret) {
		printf("    %s: expected %d, got %d\n", tag, want_ret, ret);
		bad = 1;
	}
	if (log_count("ane_h15 RESULT") != 0) {
		printf("    %s: RESULT line before the stage ran\n", tag);
		bad = 1;
	}
	if (pd_live() || link_live() || sim_pd_bad) {
		printf("    %s: leak or double release (domains live=%d links live=%d bad=%d)\n",
		       tag, pd_live(), link_live(), sim_pd_bad);
		bad = 1;
	}
	if (sim_pd_attach != sim_pd_detach || sim_link_add != sim_link_del) {
		printf("    %s: attach/detach %d/%d, link add/del %d/%d\n", tag,
		       sim_pd_attach, sim_pd_detach, sim_link_add, sim_link_del);
		bad = 1;
	}
	if (sim_pm_get_err != sim_pm_noidle) {
		printf("    %s: failed get_sync usage count not dropped (get_err=%d noidle=%d)\n", tag,
		       sim_pm_get_err, sim_pm_noidle);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    %s: window leak (iomap=%d iounmap=%d)\n", tag,
		       sim_n_iomap, sim_n_iounmap);
		bad = 1;
	}
	if (events_in(ane_t8122_soc.engine_pa, ane_t8122_soc.engine_size, 0) ||
	    sim_bus_faults || events_in(0, ~0ULL, 1)) {
		printf("    %s: engine access, fault, or write\n", tag);
		bad = 1;
	}
	return bad;
}

static int neg_pd_attach_fail(void)
{
	char tag[64];
	int i, bad = 0;

	for (i = 0; i < 5; i++) {
		pd_setup(5);
		sim_pd_attach_fail_at = i;
		snprintf(tag, sizeof(tag), "NEG-PD-ATTACH-FAIL(i=%d)", i);
		bad |= pd_refused(tag, probe_now(), -ENODEV);
		if (sim_pd_attach != i) {
			printf("    %s: %d domains attached before the failure, expected %d\n",
			       tag, sim_pd_attach, i);
			bad = 1;
		}
		if (sim_pm_get || sim_pm_put || sim_pm_get_err) {
			printf("    %s: runtime PM touched after a failed attach\n", tag);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-attach-fail: harness FAILED (BAD)"
			   : "  neg-pd-attach-fail: module refuses and releases as required");
	return bad;
}

static int neg_pd_link_fail(void)
{
	char tag[64];
	int i, bad = 0;

	for (i = 0; i < 5; i++) {
		pd_setup(5);
		sim_link_fail_at = i;
		snprintf(tag, sizeof(tag), "NEG-PD-LINK-FAIL(i=%d)", i);
		bad |= pd_refused(tag, probe_now(), -EINVAL);
		/* the domain whose link failed is attached and must be
		 * detached too: i+1 attaches, i links
		 */
		if (sim_pd_attach != i + 1 || sim_link_add != i) {
			printf("    %s: attach=%d link=%d, expected %d and %d\n",
			       tag, sim_pd_attach, sim_link_add, i + 1, i);
			bad = 1;
		}
		if (sim_pm_get || sim_pm_put || sim_pm_get_err) {
			printf("    %s: runtime PM touched after a failed link\n", tag);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-link-fail: harness FAILED (BAD)"
			   : "  neg-pd-link-fail: module refuses and releases as required");
	return bad;
}

/* get_sync fails with something other than -EACCES while five domains
 * are attached: the probe must fail and release everything
 */
static int neg_pd_rpm_err(void)
{
	int bad;

	pd_setup(5);
	sim_pm_get_fail = -EIO;
	bad = pd_refused("NEG-PD-RPM-ERR", probe_now(), -EIO);
	if (sim_pd_attach != 5 || sim_link_add != 5) {
		printf("    NEG-PD-RPM-ERR: attach=%d link=%d, expected 5 and 5\n",
		       sim_pd_attach, sim_link_add);
		bad = 1;
	}
	if (sim_pm_get != 0 || sim_pm_put != 0) {
		printf("    NEG-PD-RPM-ERR: power claim taken on a failed get_sync\n");
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-pd-rpm-err: harness FAILED (BAD)"
			   : "  neg-pd-rpm-err: module refuses as required");
	return bad;
}

/* -EACCES is ignored only with domains attached by the module: with
 * zero or one domain it must still fail the probe
 */
static int neg_pd_eacces_alone(void)
{
	int n, bad = 0;

	for (n = 0; n < 2; n++) {
		pd_setup(n);
		sim_pm_get_fail = -EACCES;
		bad |= pd_refused(n ? "NEG-PD-EACCES(one domain)"
				    : "NEG-PD-EACCES(no domain)",
				  probe_now(), -EACCES);
		if (sim_pd_attach_calls) {
			printf("    NEG-PD-EACCES(n=%d): module attached a domain\n", n);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-pd-eacces-alone: harness FAILED (BAD)"
			   : "  neg-pd-eacces-alone: module refuses as required");
	return bad;
}

/* ---- stage 4 (confirm_run=1): the only stage with writes ----
 *
 * A tiny device model behind the shim's bus hooks: a CPU_CONTROL write with RUN clears STOPPED in
 * CPU_STATUS (the M6 observation, 0xa -> 0x8), a write without it sets STOPPED again, unless the core
 * is "stuck". Optionally the core posts mailbox messages once started: I2A_CTRL loses EMPTY until the
 * module pops RECV1.
 */
void (*sim_write_hook)(u64 pa, u32 v);
u32 (*sim_read_hook)(u64 pa, u32 v);
void (*sim_write64_hook)(u64 pa, u64 v);

#define DM_RUN 0x10u
#define DM_STOPPED 0x2u
#define DM_EMPTY_WORD 0x20001u
static const struct ane_h15_soc *dm_soc;
static bool dm_stuck, dm_nostart;
static int dm_msgs_left;

static u32 eng_get(const struct ane_h15_soc *s, u32 off)
{
	return sim_bus[sim_region_find(s->engine_pa + off) - sim_bus].mem[off >> 2];
}

static void dm_write(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa != s->engine_pa + s->cpu_control)
		return;
	if (v & DM_RUN) {
		if (!dm_nostart)
			eng_set(s, s->cpu_status, eng_get(s, s->cpu_status) & ~DM_STOPPED);
		if (dm_msgs_left && !dm_nostart)
			eng_set(s, s->mbox + 0x114, 0x1);
	} else if (!dm_stuck) {
		eng_set(s, s->cpu_status, eng_get(s, s->cpu_status) | DM_STOPPED);
	}
}

static u32 dm_read(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa == s->engine_pa + s->mbox + 0x838) {	/* popping a message */
		if (dm_msgs_left > 0)
			dm_msgs_left--;
		if (!dm_msgs_left)
			eng_set(s, s->mbox + 0x114, DM_EMPTY_WORD);
	}
	return v;
}

/* ANE DART model: window 0 reads as a t8110 DART (PARAMS3 0x1c200301, 16 streams); stream 3 has TCR translate,
 * a valid TTBR and its ENABLE bit. Windows 1..3 read 0, so their version differs and they are not scanned. */
#define DART_P3 0x1c200301u
static void dart_set(const struct ane_h15_soc *s, bool live)
{
	u32 b = s->dart_off;

	eng_set(s, b + 0x08, DART_P3);
	eng_set(s, b + 0x0c, 0x00080010);
	eng_set(s, b + 0xc00, live ? 1u << 3 : 0);
	eng_set(s, b + 0x1000 + 4 * 3, live ? 0x1 : 0);
	eng_set(s, b + 0x1400 + 4 * 3, live ? 0x00abc001 : 0);
}

static void run_setup(const struct ane_h15_soc *s, const char *compat, u32 ctrl)
{
	stage2_setup(s, compat, -1);
	stage = "run";
	confirm_read = false;
	confirm_run = true;
	run_wait_ms = 50;
	eng_set(s, s->cpu_status, 0x30002a);
	eng_set(s, s->rvbar + 4, 0x100);
	eng_set(s, s->cpu_control, ctrl);
	eng_set(s, s->mbox + 0x830, 0xabc0);
	eng_set(s, s->mbox + 0x838, 0x1234);
	eng_set(s, s->mbox + 0x834, 0x11);	/* upper halves: a 32-bit read would lose them */
	eng_set(s, s->mbox + 0x83c, 0x22);
	dart_set(s, true);
	dm_soc = s;
	dm_stuck = dm_nostart = false;
	dm_msgs_left = 0;
	sim_write_hook = dm_write;
	sim_read_hook = dm_read;
}

/* engine writes in order; returns how many */
static int run_writes(const struct ane_h15_soc *s, u32 *vals, int max, int *first_idx)
{
	int i, n = 0;

	*first_idx = -1;
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		if (!sim_events[i].write || sim_events[i].pa < s->engine_pa ||
		    sim_events[i].pa >= s->engine_pa + s->engine_size)
			continue;
		if (*first_idx < 0)
			*first_idx = i;
		if (n < max)
			vals[n] = sim_events[i].val;
		n++;
	}
	return n;
}

/* no engine write at all, no write anywhere, no fault, and the failed probe released everything */
static int run_refused_clean(const struct ane_h15_soc *s, int ret, const char *want_verdict,
			     const char *want_reason_part, const char *tag)
{
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	int before = fails;

	snprintf(name, sizeof(name), "%s: probe fails, RESULT stage=5 %s", tag, want_verdict);
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, want_verdict) && strstr(reason, want_reason_part), name);
	snprintf(name, sizeof(name), "%s: zero bus writes, zero faults, no RECV read", tag);
	chk(events_in(0, ~0ULL, 1) == 0 && sim_bus_faults == 0 &&
	    eng_events_at(s, s->mbox + 0x830) == 0 && eng_events_at(s, s->mbox + 0x838) == 0, name);
	snprintf(name, sizeof(name), "%s: windows unmapped, power claim balanced", tag);
	chk(sim_n_iomap == sim_n_iounmap && sim_pm_get == sim_pm_put, name);
	return fails - before;
}

static void pos_run_case(const char *tag, u32 ctrl, int msgs_left, bool nostart, int want_msgs,
			 const u32 *want_w, int n_want_w)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	u32 w[8];
	int first, n, i, ok, ret;

	run_setup(s, "apple,t6030-ane", ctrl);
	dm_msgs_left = msgs_left;
	dm_nostart = nostart;
	ret = probe_now();
	snprintf(name, sizeof(name), "%s: probe returns 0, RESULT stage=5 PASS %s", tag,
		 nostart ? "core-never-left-stopped" : "core-started-and-stopped");
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") &&
	    !strcmp(reason, nostart ? "core-never-left-stopped" : "core-started-and-stopped"), name);
	n = run_writes(s, w, 8, &first);
	ok = n == n_want_w;
	for (i = 0; ok && i < n; i++)
		ok = w[i] == want_w[i];
	snprintf(name, sizeof(name), "%s: engine writes are exactly the %d CPU_CONTROL values", tag, n_want_w);
	chk(ok, name);
	snprintf(name, sizeof(name), "%s: every write is at CPU_CONTROL, nothing written elsewhere", tag);
	chk(events_in(s->engine_pa + s->cpu_control, 4, 1) == n && events_in(0, ~0ULL, 1) == n &&
	    sim_bus_faults == 0, name);
	snprintf(name, sizeof(name), "%s: preflight reads (6 words) all precede the first write", tag);
	{
		u32 want[6] = { s->cpu_status, s->cpu_control, s->rvbar, s->rvbar + 4,
				s->mbox + 0x110, s->mbox + 0x114 };
		int k, seen_all = 1;

		for (k = 0; k < 6; k++) {
			int found = 0;

			for (i = 0; i < first; i++)
				if (!sim_events[i].write && sim_events[i].pa == s->engine_pa + want[k])
					found = 1;
			seen_all &= found;
		}
		chk(first > 0 && seen_all, name);
	}
	snprintf(name, sizeof(name), "%s: pmgr words read before the first engine event", tag);
	{
		int first_eng = -1, last_pmgr = -1;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (sim_events[i].pa >= s->engine_pa && sim_events[i].pa < s->engine_pa + s->engine_size) {
				if (first_eng < 0)
					first_eng = i;
			} else {
				last_pmgr = i;
			}
		}
		chk(first_eng > last_pmgr && last_pmgr >= 4, name);
	}
	snprintf(name, sizeof(name), "%s: each write and the CPU_CONTROL read logged (%d write lines)", tag, n_want_w);
	chk(log_count("ane_h15 write CPU_CONTROL=") == n_want_w && log_count("ane_h15 read CPU_CONTROL") >= 1, name);
	snprintf(name, sizeof(name), "%s: %d message(s) popped, raw words logged, never a mailbox write", tag, want_msgs);
	chk(log_count("ane_h15 msg") == want_msgs &&
	    (want_msgs == 0 || log_count("raw0=0x110000abc0 raw1=0x2200001234") == want_msgs) &&
	    eng_events_at(s, s->mbox + 0x834) == 0 && eng_events_at(s, s->mbox + 0x83c) == 0 &&
	    eng_events_at(s, s->mbox + 0x830) == want_msgs && eng_events_at(s, s->mbox + 0x838) == want_msgs &&
	    events_in(s->engine_pa + s->mbox, 0x1000, 1) == 0, name);
	snprintf(name, sizeof(name), "%s: power claim released, no 'do NOT unload'", tag);
	chk(sim_pm_get == 1 && sim_pm_put == 1 && log_count("do NOT unload") == 0, name);
	ane_h15_remove(&pdev);
	snprintf(name, sizeof(name), "%s: remove unmaps both windows (core stopped again)", tag);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2 && sim_module_gets == 0, name);
}

static void pos_run(void)
{
	static const u32 w0[] = { 0x10, 0x0 };
	static const u32 w20[] = { 0x30, 0x20, 0x0 };

	pos_run_case("R1 T6030 ctrl=0", 0x0, 0, false, 0, w0, 2);
	pos_run_case("R2 T6030 ctrl=0x20 (bit 5 cleared too)", 0x20, 0, false, 0, w20, 3);
	pos_run_case("R3 T6030 two messages", 0x0, 2, false, 2, w0, 2);
	pos_run_case("R4 T6030 never-ending sender, capped at 4", 0x0, 99, false, 4, w0, 2);
	pos_run_case("R5 T6030 core does not leave STOPPED", 0x0, 0, true, 0, w0, 2);
}

static int neg_run(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	static const struct { const struct ane_h15_soc *soc; const char *compat; } others[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
		{ &ane_t6034_soc, "apple,t6034-ane" },
	};
	static const struct { const char *tag; u32 off; u32 val; } bad[] = {
		{ "N4 core already running", 0, 0x1 },
		{ "N5 core not stopped", 0, 0x8 },
		{ "N6 RVBAR_LO differs", 1, 0x2 },
		{ "N7 RVBAR_HI differs", 2, 0x101 },
		{ "N8 A2I mailbox not empty", 3, 0x1 },
		{ "N9 I2A mailbox not empty", 4, 0x1 },
		{ "N10 RUN already set", 5, 0x10 },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[128];
	u32 w[8];
	int first, i, ret, before;

	/* N1 no confirm_run: nothing touched at all */
	run_setup(s, "apple,t6030-ane", 0);
	confirm_run = false;
	ret = probe_now();
	run_refused_clean(s, ret, "REFUSED", "confirm_run=1", "N1 no confirm_run");
	chk(sim_n_events == 0, "N1 no confirm_run: zero bus events of any kind");
	/* N2 run_wait_ms out of range */
	for (i = 0; i < 2; i++) {
		run_setup(s, "apple,t6030-ane", 0);
		run_wait_ms = i ? 5000 : 5;
		ret = probe_now();
		run_refused_clean(s, ret, "REFUSED", "run_wait_ms", i ? "N2 run_wait_ms=5000" : "N2 run_wait_ms=5");
		chk(sim_n_events == 0, "N2 run_wait_ms: zero bus events of any kind");
	}
	/* N3 SoC rows with no measured RVBAR */
	for (i = 0; i < 3; i++) {
		run_setup(others[i].soc, others[i].compat, 0);
		ret = probe_now();
		snprintf(name, sizeof(name), "N3 %s no measured RVBAR", others[i].soc->name);
		run_refused_clean(others[i].soc, ret, "REFUSED", "no measured RVBAR", name);
		chk(sim_n_events == 0, "N3 no measured RVBAR: zero bus events of any kind");
	}
	/* N4..N10 preflight mismatches: read-only, FAIL, nothing written */
	for (i = 0; i < (int)ARRAY_SIZE(bad); i++) {
		run_setup(s, "apple,t6030-ane", 0);
		switch (bad[i].off) {
		case 0: eng_set(s, s->cpu_status, bad[i].val); break;
		case 1: eng_set(s, s->rvbar, bad[i].val); break;
		case 2: eng_set(s, s->rvbar + 4, bad[i].val); break;
		case 3: eng_set(s, s->mbox + 0x110, bad[i].val); break;
		case 4: eng_set(s, s->mbox + 0x114, bad[i].val); break;
		default: eng_set(s, s->cpu_control, bad[i].val); break;
		}
		ret = probe_now();
		run_refused_clean(s, ret, "FAIL", "preflight-not-as-iboot-left-it", bad[i].tag);
	}
	/* N11 stuck pmgr word: refuses before any engine access */
	run_setup(s, "apple,t6030-ane", 0);
	set_ps_words(s, 2);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "pmgr-actual-stuck", "N11 pmgr word stuck");
	chk(events_in(s->engine_pa, s->engine_size, 0) == 0, "N11 pmgr word stuck: zero engine events");
	/* N12 live reg[0] is not the SoC row's window */
	run_setup(s, "apple,t6030-ane", 0);
	node.reg_pa[0] += 0x1000;
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "window-mismatch", "N12 reg[0] shifted");
	chk(sim_n_events == 0, "N12 reg[0] shifted: zero bus events of any kind");
	/* N13 the core does not stop: probe succeeds on purpose, power and windows kept, remove refuses */
	run_setup(s, "apple,t6030-ane", 0);
	dm_stuck = true;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "FAIL") && !strcmp(reason, "core-not-stopped"),
	    "N13 stuck core: RESULT stage=5 FAIL core-not-stopped, probe returns 0");
	chk(run_writes(s, w, 8, &first) == 2 && w[0] == 0x10 && w[1] == 0x0,
	    "N13 stuck core: exactly one RUN and one clear-RUN write, no retry storm");
	chk(log_count("reboot required") >= 1 && log_count("do NOT unload") >= 1,
	    "N13 stuck core: reboot-required and do-NOT-unload logged");
	chk(sim_pm_get == 1 && sim_pm_put == 0 && sim_n_iounmap == 0,
	    "N13 stuck core: power claim and windows kept");
	chk(sim_module_gets == 1, "N13 stuck core: the module is pinned (rmmod cannot free it under the core)");
	before = sim_n_iounmap;
	ane_h15_remove(&pdev);
	chk(sim_n_iounmap == before && sim_pm_put == 0 && log_count("remove with the firmware released") == 1,
	    "N13 stuck core: remove() refuses");
	return fails;
}

/* ---- stage 4 (confirm_read=1): the read-only DART liveness scan ---- */
static void dart_setup(const struct ane_h15_soc *s, const char *compat, bool live)
{
	stage2_setup(s, compat, -1);
	stage = "dart";
	confirm_read = true;
	dart_set(s, live);
}

/* every engine event is a read inside the first DART window span, nothing is written anywhere */
static bool dart_only_reads(const struct ane_h15_soc *s)
{
	int i;

	if (events_in(0, ~0ULL, 1) || sim_bus_faults)
		return false;
	for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
		u64 pa = sim_events[i].pa;

		if (pa >= s->engine_pa && pa < s->engine_pa + s->engine_size &&
		    (pa < s->engine_pa + s->dart_off || pa >= s->engine_pa + s->dart_off + 0x24000))
			return false;
	}
	return true;
}

static void pos_dart(void)
{
	static const struct { const struct ane_h15_soc *soc; const char *compat; } rows[] = {
		{ &ane_t8122_soc, "apple,t8122-ane" },
		{ &ane_t6030_soc, "apple,t6030-ane" },
		{ &ane_t6031_soc, "apple,t6031-ane" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], name[128], tag[32];
	size_t r;
	int ret;

	for (r = 0; r < ARRAY_SIZE(rows); r++) {
		const struct ane_h15_soc *s = rows[r].soc;

		snprintf(tag, sizeof(tag), "D1 %s", s->name);
		dart_setup(s, rows[r].compat, true);
		ret = probe_now();
		snprintf(name, sizeof(name), "%s: RESULT stage=4 PASS dart-live-stream-found", tag);
		chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    rstage == 4 && !strcmp(verdict, "PASS") && !strcmp(reason, "dart-live-stream-found"), name);
		snprintf(name, sizeof(name), "%s: stream 3 and the summary logged, other windows not scanned", tag);
		chk(log_count("ane_h15 dart inst=0 sid=3 tcr=0x1 ttbr=0xabc001 enabled=1") == 1 &&
		    log_count("live_streams=1 locked_windows=0") == 1 &&
		    log_count("version differs from inst 0: not scanned") == 3, name);
		snprintf(name, sizeof(name), "%s: reads only, all inside the DART windows, none at CPU_CONTROL", tag);
		chk(dart_only_reads(s) && eng_events_at(s, s->cpu_control) == 0, name);
		snprintf(name, sizeof(name), "%s: groups logged before they are read", tag);
		chk(log_count("ane_h15 read DART inst=0 PARAMS3") == 1 &&
		    log_count("ane_h15 read DART inst=0 TCR[0..]") == 1, name);
		snprintf(name, sizeof(name), "%s: power claim released, windows unmapped on remove", tag);
		ane_h15_remove(&pdev);
		chk(sim_pm_get == 1 && sim_pm_put == 1 && sim_n_iomap == 2 && sim_n_iounmap == 2, name);
	}
	/* nothing enabled: the M6 situation, a PASS that says so */
	dart_setup(&ane_t6030_soc, "apple,t6030-ane", false);
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "PASS") && !strcmp(reason, "dart-no-stream-enabled") &&
	    log_count("live_streams=0") == 1 && dart_only_reads(&ane_t6030_soc),
	    "D2 no enabled stream: PASS dart-no-stream-enabled, reads only");
	ane_h15_remove(&pdev);
	/* the lock bit is reported */
	dart_setup(&ane_t6030_soc, "apple,t6030-ane", true);
	eng_set(&ane_t6030_soc, ane_t6030_soc.dart_off + 0x208, 1);
	ret = probe_now();
	chk(ret == 0 && log_count("locked_windows=1") == 1, "D3 PROTECT_LOCK bit 0 is counted");
	ane_h15_remove(&pdev);
	/* enabled stream whose TTBR is not valid, TCR without translate, enable bit missing: not live */
	{
		static const struct { const char *tag; u32 off; u32 val; } nl[] = {
			{ "D4 TTBR not valid", 0x1400 + 12, 0x00abc000 },
			{ "D4 TCR translate clear", 0x1000 + 12, 0x2 },
			{ "D4 ENABLE bit clear", 0xc00, 0 },
		};
		size_t i;

		for (i = 0; i < ARRAY_SIZE(nl); i++) {
			dart_setup(&ane_t6030_soc, "apple,t6030-ane", true);
			eng_set(&ane_t6030_soc, ane_t6030_soc.dart_off + nl[i].off, nl[i].val);
			ret = probe_now();
			snprintf(name, sizeof(name), "%s: not counted as live", nl[i].tag);
			chk(ret == 0 && log_count("live_streams=0") == 1, name);
			ane_h15_remove(&pdev);
		}
	}
}

static int neg_dart(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128];
	int ret;

	/* no confirm_read: nothing touched */
	dart_setup(s, "apple,t6030-ane", true);
	confirm_read = false;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "REFUSED") && sim_n_events == 0,
	    "ND1 no confirm_read: REFUSED, zero bus events");
	/* t6034: the ADT has no DART window for it */
	dart_setup(&ane_t6034_soc, "apple,t6034-ane", false);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "REFUSED") && strstr(reason, "no ADT DART window") &&
	    sim_n_events == 0, "ND2 t6034: REFUSED, zero bus events");
	/* stuck pmgr word: no engine access at all */
	dart_setup(s, "apple,t6030-ane", true);
	set_ps_words(s, 1);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "FAIL") && !strcmp(reason, "pmgr-actual-stuck") &&
	    events_in(s->engine_pa, s->engine_size, 0) == 0, "ND3 pmgr stuck: FAIL, zero engine events");
	/* shifted reg[0]: refused before any bus access */
	dart_setup(s, "apple,t6030-ane", true);
	node.reg_pa[0] += 0x1000;
	ret = probe_now();
	chk(ret != 0 && strstr(sim_log, "window mismatch") && sim_n_events == 0,
	    "ND4 reg[0] shifted: refused, zero bus events");
	/* window 0 does not read as a DART: stop after one read */
	dart_setup(s, "apple,t6030-ane", true);
	eng_set(s, s->dart_off + 0x08, 0);
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 4 && !strcmp(verdict, "FAIL") && events_in(s->engine_pa, s->engine_size, 0) == 1,
	    "ND5 PARAMS3 zero: FAIL after exactly one DART read");
	/* run (stage 5) with no live stream refuses without writing; run_unmapped=1 overrides */
	run_setup(s, "apple,t6030-ane", 0);
	dart_set(s, false);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "no-dart-stream-enabled", "ND6 run, no live DART stream");
	chk(events_in(s->engine_pa + s->cpu_control, 4, 0) == 0, "ND6 run, no live DART stream: CPU_CONTROL never read");
	run_setup(s, "apple,t6030-ane", 0);
	dart_set(s, false);
	run_unmapped = true;
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS"), "ND7 run_unmapped=1 runs without a live stream");
	ane_h15_remove(&pdev);
	/* run with an unreadable DART: refuses before the preflight */
	run_setup(s, "apple,t6030-ane", 0);
	eng_set(s, s->dart_off + 0x08, 0);
	ret = probe_now();
	run_refused_clean(s, ret, "FAIL", "dart-read-refused-or-not-a-dart", "ND8 run, window 0 is not a DART");
	return fails;
}

/* ---- stage 5 with map_fw=1: attach to the DART and iommu_map the two firmware segments before RUN ---- */
#define SEGS_OK "100014e8000:c4000,100030c4000:2ac000"

static void attach_live(void)
{
	dart_set(dm_soc, true);	/* the DART driver programmed stream 3 at the attach */
}

static void map_setup(const struct ane_h15_soc *s, const char *compat)
{
	run_setup(s, compat, 0);
	dart_set(s, false);	/* iBoot left nothing enabled (the M6 measurement) */
	node.has_iommus = true;
	map_fw = true;
	fw_seg = SEGS_OK;
	sim_attach_hook = attach_live;
}

/* the iommu call sequence, as a string of codes */
static void io_seq(char *out, size_t sz)
{
	int k;
	size_t o = 0;

	for (k = 0; k < sim_n_iocalls && k < 64 && o + 2 < sz; k++)
		out[o++] = "?ATMUDF"[sim_iocalls[k].code];
	out[o] = 0;
}

static void pos_map(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], seq[80];
	u32 w[8];
	int ret, first, n;

	map_setup(s, "apple,t6030-ane");
	ret = probe_now();
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "PASS") && !strcmp(reason, "core-started-and-stopped"),
	    "M1 map_fw: RESULT stage=5 PASS core-started-and-stopped");
	io_seq(seq, sizeof(seq));
	chk(!strcmp(seq, "AT" "MM" "UUDF"), "M1 map_fw: alloc, attach, map, map, then after the stop unmap, unmap, detach, free");
	chk(sim_n_iocalls >= 4 &&
	    sim_iocalls[2].iova == 0x10000000000ULL && sim_iocalls[2].pa == 0x100014e8000ULL &&
	    sim_iocalls[2].size == 0xc4000 && sim_iocalls[2].prot == IOMMU_READ &&
	    sim_iocalls[3].iova == 0x100000c4000ULL && sim_iocalls[3].pa == 0x100030c4000ULL &&
	    sim_iocalls[3].size == 0x2ac000 && sim_iocalls[3].prot == (IOMMU_READ | IOMMU_WRITE),
	    "M1 map_fw: TEXT 0x100014e8000 at the RVBAR entry 0x10000000000 read only, DATA 0x100030c4000 right behind it read+write");
	n = run_writes(s, w, 8, &first);
	chk(n == 2 && w[0] == 0x10 && w[1] == 0 && first >= 0 && sim_iocalls[3].ev <= first &&
	    sim_iocalls[1].ev >= 5, "M1 map_fw: both maps and the attach happen after the pmgr gate and before the RUN write");
	chk(log_count("live_streams=1") == 1 && sim_n_dom_live == 0, "M1 map_fw: the scan after the attach finds the live stream; the domain is gone after the stop");
	chk(events_in(0, ~0ULL, 1) == n && sim_pm_get == 1 && sim_pm_put == 1, "M1 map_fw: only CPU_CONTROL written, power claim released");
	ane_h15_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2, "M1 map_fw: remove unmaps both windows");
}

static int neg_map(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	static const struct { const char *tag; const char *seg; } bad[] = {
		{ "NM2 fw_seg absent", NULL },
		{ "NM3 fw_seg malformed", "zz" },
		{ "NM4 TEXT pa not 16 KiB aligned", "100014e9000:c4000,100030c4000:2ac000" },
		{ "NM4 DATA size not 16 KiB aligned", "100014e8000:c4000,100030c4000:2ac001" },
		{ "NM4 TEXT size zero", "100014e8000:0,100030c4000:2ac000" },
		{ "NM4 DATA over 64 MiB", "100014e8000:c4000,100030c4000:4004000" },
		{ "NM4 segments overlap", "100014e8000:c4000,100014f0000:2ac000" },
	};
	unsigned int rstage;
	char verdict[32], reason[128], seq[80], name[128];
	u32 w[8];
	int ret, first, i;

	/* NM1 no iommus in the node: refused before any bus access, no iommu call */
	map_setup(s, "apple,t6030-ane");
	node.has_iommus = false;
	ret = probe_now();
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 5 && !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0,
	    "NM1 no iommus: REFUSED, zero bus events, zero iommu calls");
	for (i = 0; i < (int)ARRAY_SIZE(bad); i++) {
		map_setup(s, "apple,t6030-ane");
		fw_seg = (char *)bad[i].seg;
		ret = probe_now();
		snprintf(name, sizeof(name), "%s: REFUSED, zero bus events, zero iommu calls", bad[i].tag);
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0, name);
	}
	/* NM5 a segment inside system RAM */
	for (i = REGION_INTERSECTS; i <= REGION_MIXED; i += 2) {
		map_setup(s, "apple,t6030-ane");
		sim_region_ram = i;
		ret = probe_now();
		snprintf(name, sizeof(name), "NM5 region_intersects=%d: REFUSED, zero bus events, zero iommu calls", i);
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "REFUSED") && sim_n_events == 0 && sim_n_iocalls == 0, name);
	}
	/* NM15 the core is already running (STOPPED clear, then RUNNING set): nothing mapped, nothing written */
	for (i = 0; i < 2; i++) {
		map_setup(s, "apple,t6030-ane");
		eng_set(s, s->cpu_status, i ? 0x30002b : 0x300028);
		ret = probe_now();
		snprintf(name, sizeof(name), "NM15 core %s: FAIL core-not-stopped-before-map, no iommu call, no write",
			 i ? "RUNNING" : "not STOPPED");
		chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
		    !strcmp(verdict, "FAIL") && !strcmp(reason, "core-not-stopped-before-map") &&
		    sim_n_iocalls == 0 && events_in(0, ~0ULL, 1) == 0, name);
	}
	/* NM14 a SoC row without a measured RVBAR: refused first */
	map_setup(&ane_t8122_soc, "apple,t8122-ane");
	ret = probe_now();
	chk(ret != 0 && sim_n_events == 0 && sim_n_iocalls == 0, "NM14 t8122 map_fw: refused (no measured RVBAR), no iommu call");
	/* NM6..NM10 failures inside the map: nothing written, everything undone */
	{
		static const struct { int fail; const char *seq; const char *tag; } f[] = {
			{ 1, "A", "NM6 domain alloc fails" },
			{ 2, "ATF", "NM7 attach fails" },
			{ 4, "ATMDF", "NM8 map of TEXT fails" },
			{ 3, "ATMMUDF", "NM9 map of DATA fails" },
			{ 5, "ATMUDF", "NM10 TEXT reads back wrong" },
		};

		for (i = 0; i < (int)ARRAY_SIZE(f); i++) {
			map_setup(s, "apple,t6030-ane");
			sim_io_fail = f[i].fail;
			ret = probe_now();
			io_seq(seq, sizeof(seq));
			snprintf(name, sizeof(name), "%s: FAIL fw-map-failed, call sequence %s, no write, no domain left", f[i].tag, f[i].seq);
			chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
			    !strcmp(verdict, "FAIL") && !strcmp(reason, "fw-map-failed") &&
			    !strcmp(seq, f[i].seq) &&
			    events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0, name);
		}
	}
	/* NM11 attached, but the DART still shows no live stream: refuse, unmap, write nothing */
	map_setup(s, "apple,t6030-ane");
	sim_attach_hook = NULL;
	ret = probe_now();
	io_seq(seq, sizeof(seq));
	chk(ret != 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    !strcmp(reason, "no-dart-stream-enabled") && !strcmp(seq, "ATMMUUDF") &&
	    events_in(0, ~0ULL, 1) == 0 && sim_n_dom_live == 0,
	    "NM11 attached, no live stream: FAIL, mapping undone, nothing written");
	/* NM12 a core that does not stop keeps the mapping, the power claim and the windows */
	map_setup(s, "apple,t6030-ane");
	dm_stuck = true;
	ret = probe_now();
	io_seq(seq, sizeof(seq));
	chk(ret == 0 && !strcmp(seq, "ATMM") && sim_n_dom_live == 1 && sim_pm_put == 0 &&
	    run_writes(s, w, 8, &first) == 2, "NM12 stuck core: the DART mapping is kept (no unmap, detach or free)");
	ane_h15_remove(&pdev);
	chk(sim_n_dom_live == 1 && sim_n_iounmap == 0, "NM12 stuck core: remove() refuses, mapping and windows stay");
	return fails;
}

/* ---- stage 6 (confirm_run=1 confirm_rtkit=1): the RTKit session over the wrapper mailbox ----
 *
 * Firmware model: RUN queues a HELLO (versions 11..12); the host's HELLO reply queues two endpoint-map messages
 * (endpoints 0 and 1, then 0x20 with the last flag); STARTEP of endpoint 1 queues a crash-log buffer request (endpoint 1,
 * never answered) and the IOP power ON acknowledgement; the host's AP power ON is acknowledged; STARTEP of 0x20 queues
 * an application message (never answered); AP QUIESCED/OFF and IOP SLEEP/OFF are acknowledged. The I2A words and the
 * RECV registers are derived from the queue; RECV1 is pop on read.
 */
struct fwmsg { u64 m0, m1; };
static struct fwmsg fq[64];
static int fq_n;
static u64 fw_pend0;
static u64 sent0[64], sent1[64];
static int n_sent, fw_ep1, fw_ep20;
static struct {
	bool silent, no_ap_ack, no_iop_ack, flood, a2i_full, no_epmap, quirk_ver;
} fwo;

#define RT(t) ((u64)(t) << 52)

static void fw_regs(void)
{
	const struct ane_h15_soc *s = dm_soc;

	eng_set(s, s->mbox + 0x114, fq_n ? 0x1 : DM_EMPTY_WORD);
	eng_set(s, s->mbox + 0x830, fq_n ? (u32)fq[0].m0 : 0);
	eng_set(s, s->mbox + 0x834, fq_n ? (u32)(fq[0].m0 >> 32) : 0);
	eng_set(s, s->mbox + 0x838, fq_n ? (u32)fq[0].m1 : 0);
	eng_set(s, s->mbox + 0x83c, fq_n ? (u32)(fq[0].m1 >> 32) : 0);
}

static void fw_push(u64 m0, u64 m1)
{
	if (fq_n < 64)
		fq[fq_n++] = (struct fwmsg){ m0, m1 };
	fw_regs();
}

static void fw_host_msg(u64 m0, u64 m1)
{
	unsigned int type = (m0 >> 52) & 0xff, ep, i;

	if (n_sent < 64) {
		sent0[n_sent] = m0;
		sent1[n_sent] = m1;
	}
	n_sent++;
	if (type == 2 && !fwo.no_epmap) {
		fw_push(RT(8) | 0x3, 0);
		fw_push(RT(8) | (1ULL << 32) | 1 | (1ULL << 51), 0);
		if (fwo.flood)
			for (i = 0; i < 30; i++)
				fw_push(RT(8) | (2ULL << 32) | 2, 0);
	} else if (type == 5) {
		ep = (m0 >> 32) & 0xff;
		if (ep == 1) {
			fw_ep1++;
			fw_push(RT(1) | 0x23c000, 1);		/* crash log buffer request on endpoint 1 */
			fw_push(RT(7) | 0x20, 0);		/* IOP power state ON acknowledged */
		} else if (ep == 0x20) {
			fw_ep20++;
			fw_push(RT(1) | (12ULL << 16) | 11, 0x20);	/* an application endpoint message that looks like a HELLO: must stay unanswered */
		}
	} else if (type == 0xb && !fwo.no_ap_ack) {
		fw_push(RT(0xb) | (m0 & 0xffff), 0);
	} else if (type == 6 && !fwo.no_iop_ack) {
		fw_push(RT(7) | (m0 & 0xffff), 0);
	}
}

static void rt_write(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;
	bool run = pa == s->engine_pa + s->cpu_control && (v & DM_RUN) && !dm_nostart;

	dm_write(pa, v);
	if (run) {
		if (fwo.a2i_full)
			eng_set(s, s->mbox + 0x110, 0x10000);
		if (!fwo.silent)
			fw_push(RT(1) | ((u64)(fwo.quirk_ver ? 14 : 12) << 16) | (fwo.quirk_ver ? 13 : 11), 0);
	}
}

static void rt_write64(u64 pa, u64 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa == s->engine_pa + s->mbox + 0x800)
		fw_pend0 = v;
	else if (pa == s->engine_pa + s->mbox + 0x808)
		fw_host_msg(fw_pend0, v);
}

static u32 rt_read(u64 pa, u32 v)
{
	const struct ane_h15_soc *s = dm_soc;

	if (pa == s->engine_pa + s->mbox + 0x838 && fq_n) {	/* pop on the read of RECV1 */
		int i;

		for (i = 1; i < fq_n; i++)
			fq[i - 1] = fq[i];
		fq_n--;
		fw_regs();
	}
	return v;
}

static void rt_setup(const struct ane_h15_soc *s, const char *compat)
{
	run_setup(s, compat, 0);
	stage = "rtkit";
	confirm_rtkit = true;
	fq_n = n_sent = fw_ep1 = fw_ep20 = 0;
	memset(&fwo, 0, sizeof(fwo));
	sim_write_hook = rt_write;
	sim_write64_hook = rt_write64;
	sim_read_hook = rt_read;
}

static int mbox_writes(const struct ane_h15_soc *s)
{
	return events_in(s->engine_pa + s->mbox + 0x800, 0x10, 1);
}

static void pos_rtkit_case(const char *tag, unsigned int stop, const u64 *want, int n_want, int map)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	u32 w[8];
	int ret, first, i, ok, cw;

	if (map) {
		map_setup(s, "apple,t6030-ane");
		stage = "rtkit";
		confirm_rtkit = true;
		fq_n = n_sent = fw_ep1 = fw_ep20 = 0;
		memset(&fwo, 0, sizeof(fwo));
		sim_write_hook = rt_write;
		sim_write64_hook = rt_write64;
		sim_read_hook = rt_read;
	} else {
		rt_setup(s, "apple,t6030-ane");
	}
	rtkit_stop = stop;
	ret = probe_now();
	snprintf(name, sizeof(name), "%s: probe 0, RESULT stage=6 PASS rtkit-session-and-shutdown", tag);
	chk(ret == 0 && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 6 && !strcmp(verdict, "PASS") && !strcmp(reason, "rtkit-session-and-shutdown"), name);
	ok = n_sent == n_want;
	for (i = 0; ok && i < n_want; i++)
		ok = sent0[i] == want[i] && sent1[i] == 0;
	snprintf(name, sizeof(name), "%s: the host sent exactly these %d management messages, SEND1 always 0", tag, n_want);
	chk(ok, name);
	cw = run_writes(s, w, 8, &first);
	snprintf(name, sizeof(name), "%s: every engine write is CPU_CONTROL [0x10, 0] or a send register (SEND0 then SEND1), nothing else", tag);
	{
		int cpu = events_in(s->engine_pa + s->cpu_control, 4, 1), mb = mbox_writes(s);

		chk(cpu == 2 && mb == 2 * n_want && cw == cpu + mb && events_in(0, ~0ULL, 1) == cw && sim_bus_faults == 0 &&
		    eng_get(s, s->cpu_control) == 0, name);
	}
	snprintf(name, sizeof(name), "%s: the crash-log request and the application message were not answered", tag);
	chk(fw_ep1 == 1 && fw_ep20 == 1 && n_sent == n_want, name);
	snprintf(name, sizeof(name), "%s: first mailbox write comes after the RUN write; core stopped; claim released", tag);
	{
		int run_idx = -1, mb_idx = -1;

		for (i = 0; i < sim_n_events && i < SIM_MAX_EVENTS; i++) {
			if (!sim_events[i].write)
				continue;
			if (run_idx < 0 && sim_events[i].pa == s->engine_pa + s->cpu_control)
				run_idx = i;
			if (mb_idx < 0 && sim_events[i].pa == s->engine_pa + s->mbox + 0x800)
				mb_idx = i;
		}
		chk(run_idx >= 0 && mb_idx > run_idx && (eng_get(s, s->cpu_status) & DM_STOPPED) && sim_pm_get == 1 && sim_pm_put == 1 &&
		    sim_module_gets == 0, name);
	}
	ane_h15_remove(&pdev);
}

static void pos_rtkit(void)
{
	const u64 base[] = {
		RT(2) | (12ULL << 16) | 12,			/* HELLO reply */
		RT(8) | 1,					/* map reply, base 0, more */
		RT(8) | (1ULL << 32) | (1ULL << 51),		/* map reply, base 1, last */
		RT(5) | (1ULL << 32) | 2,			/* STARTEP 1 */
		RT(0xb) | 0x20,				/* AP power ON */
		RT(5) | (0x20ULL << 32) | 2,			/* STARTEP 0x20 */
		RT(0xb) | 0x10,				/* AP QUIESCED */
		RT(6) | 0x01,				/* IOP SLEEP */
	};
	u64 off[8];

	memcpy(off, base, sizeof(off));
	off[6] = RT(0xb);				/* AP OFF */
	off[7] = RT(6);					/* IOP OFF */
	pos_rtkit_case("S1 rtkit_stop=1 (QUIESCED, SLEEP)", 1, base, 8, 0);
	pos_rtkit_case("S2 rtkit_stop=2 (OFF, OFF)", 2, off, 8, 0);
	pos_rtkit_case("S3 with map_fw=1", 1, base, 8, 1);
}

static void rt_refused(const char *tag, const char *want_verdict, const char *want_reason, int want_ret_nonzero, bool no_mbox, bool no_events)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	unsigned int rstage;
	char verdict[32], reason[128], name[160];
	int ret = probe_now();

	snprintf(name, sizeof(name), "%s: RESULT stage=6 %s %s", tag, want_verdict, want_reason);
	chk((ret != 0) == want_ret_nonzero && parse_result(&rstage, verdict, sizeof(verdict), reason, sizeof(reason)) &&
	    rstage == 6 && !strcmp(verdict, want_verdict) && strstr(reason, want_reason), name);
	snprintf(name, sizeof(name), "%s: no mailbox write%s", tag, no_events ? ", zero bus events" : "");
	chk((!no_mbox || mbox_writes(s) == 0) && (!no_events || sim_n_events == 0) && sim_bus_faults == 0, name);
}

static int neg_rtkit(void)
{
	const struct ane_h15_soc *s = &ane_t6030_soc;
	u32 w[8];
	int first, i;

	/* NR1 no confirm_rtkit */
	rt_setup(s, "apple,t6030-ane");
	confirm_rtkit = false;
	rt_refused("NR1 no confirm_rtkit", "REFUSED", "confirm_rtkit=1", 1, true, true);
	/* NR2 out-of-range budget and stop mode */
	for (i = 0; i < 3; i++) {
		rt_setup(s, "apple,t6030-ane");
		if (i == 0)
			rtkit_wait_ms = 50;
		else if (i == 1)
			rtkit_wait_ms = 20000;
		else
			rtkit_stop = 3;
		rt_refused("NR2 bad rtkit parameter", "REFUSED", "rtkit_wait_ms", 1, true, true);
	}
	/* NR3 silent firmware: only CPU_CONTROL is written, the verdict says so */
	rt_setup(s, "apple,t6030-ane");
	fwo.silent = true;
	rtkit_wait_ms = 300;
	rt_refused("NR3 silent firmware", "FAIL", "rtkit-no-hello", 1, true, false);
	chk(run_writes(s, w, 8, &first) == 2 && w[0] == 0x10 && w[1] == 0 && sim_pm_put == 1,
	    "NR3 silent firmware: RUN set and cleared, nothing else written, claim released");
	/* NR4 firmware versions that exclude 12: no reply */
	rt_setup(s, "apple,t6030-ane");
	fwo.quirk_ver = true;
	rtkit_wait_ms = 300;
	rt_refused("NR4 HELLO 13..14", "FAIL", "rtkit-session-incomplete", 1, true, false);
	chk(n_sent == 0, "NR4 HELLO 13..14: not one message sent");
	/* NR5 A2I mailbox FULL: nothing may be written to the send registers */
	rt_setup(s, "apple,t6030-ane");
	fwo.a2i_full = true;
	rtkit_wait_ms = 300;
	rt_refused("NR5 A2I FULL", "FAIL", "rtkit-session-incomplete", 1, true, false);
	/* NR6 the firmware never acknowledges the AP power state: shutdown incomplete, core still stopped */
	rt_setup(s, "apple,t6030-ane");
	fwo.no_ap_ack = true;
	rtkit_wait_ms = 300;
	rt_refused("NR6 no AP ack", "FAIL", "rtkit-session-incomplete", 1, false, false);
	/* NR7 no IOP ack at shutdown */
	rt_setup(s, "apple,t6030-ane");
	fwo.no_iop_ack = true;
	rt_refused("NR7 no IOP ack", "FAIL", "rtkit-shutdown-incomplete", 1, false, false);
	chk(eng_get(s, s->cpu_control) == 0 && (eng_get(s, s->cpu_status) & DM_STOPPED), "NR7 no IOP ack: RUN cleared and the core stopped regardless");
	/* NR8 an endpoint-map flood: the send cap holds */
	rt_setup(s, "apple,t6030-ane");
	fwo.flood = true;
	rtkit_wait_ms = 500;
	probe_now();
	chk(n_sent <= 16 + 2 && mbox_writes(s) <= 2 * (16 + 2), "NR8 map flood: at most 16 messages in the start phase and the shutdown budget");
	/* NR9 a stuck core after a complete session: kept, pinned, no PASS */
	rt_setup(s, "apple,t6030-ane");
	dm_stuck = true;
	rt_refused("NR9 stuck core", "FAIL", "core-not-stopped", 0, false, false);
	chk(sim_module_gets == 1 && sim_pm_put == 0, "NR9 stuck core: module pinned, power claim kept");
	/* NR10 stage 5 never writes the mailbox, with the same firmware model */
	rt_setup(s, "apple,t6030-ane");
	stage = "run";
	probe_now();
	chk(mbox_writes(s) == 0 && n_sent == 0, "NR10 stage 5 on a talking firmware: zero mailbox writes");
	return fails;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "pos";

	if (!strcmp(mode, "pos")) {
		printf("sim-h15 positive controls:\n");
		pos_dt_zero_mmio();
		pos_ps_pass();
		pos_refusals();
		pos_gate_logic();
		pos_other_socs();
		pos_multi_pd();
		pos_stage2();
		pos_run();
		pos_dart();
		pos_map();
		pos_rtkit();
		printf("%s\n", fails ? "SIM-TEST: FAILED"
				     : "SIM-TEST: all positive controls PASS");
	} else if (!strcmp(mode, "neg-ps-stuck")) {
		printf("sim-h15 negative control: ps-actual-stuck\n");
		fails = neg_ps_stuck();
	} else if (!strcmp(mode, "neg-wrong-stage")) {
		printf("sim-h15 negative control: wrong-stage / wrong-optin\n");
		fails = neg_wrong_stage();
	} else if (!strcmp(mode, "neg-truncated-dt")) {
		printf("sim-h15 negative control: truncated DT\n");
		fails = neg_truncated_dt();
#ifdef ANE15_SIM_FILL_FACTS
	} else if (!strcmp(mode, "pos-filled")) {
		printf("sim-h15 filled-table probe control (P7):\n");
		pos_filled_probe();
		printf("%s\n", fails ? "SIM-TEST: FAILED"
				     : "SIM-TEST: filled-table probe control PASS");
#endif
	} else if (!strcmp(mode, "neg-read-ps-stuck")) {
		printf("sim-h15 negative control: stage 2 with a stuck pmgr word\n");
		fails = neg_read_ps_stuck();
	} else if (!strcmp(mode, "neg-read-window")) {
		printf("sim-h15 negative control: stage 2 with a reg[0] that is not the SoC row's window\n");
		fails = neg_read_window();
	} else if (!strcmp(mode, "neg-read-forbidden")) {
		printf("sim-h15 negative control: stage 2 words outside the ADT windows or on CPU_CONTROL\n");
		fails = neg_read_forbidden();
	} else if (!strcmp(mode, "neg-rpm-fail")) {
		printf("sim-h15 negative control: pm_runtime_get_sync failure\n");
		fails = neg_rpm_fail();
	} else if (!strcmp(mode, "neg-pd-attach-fail")) {
		printf("sim-h15 negative control: power-domain attach fails at index i\n");
		fails = neg_pd_attach_fail();
	} else if (!strcmp(mode, "neg-pd-link-fail")) {
		printf("sim-h15 negative control: device_link_add fails at index i\n");
		fails = neg_pd_link_fail();
	} else if (!strcmp(mode, "neg-pd-rpm-err")) {
		printf("sim-h15 negative control: get_sync error other than -EACCES with domains attached\n");
		fails = neg_pd_rpm_err();
	} else if (!strcmp(mode, "neg-pd-eacces-alone")) {
		printf("sim-h15 negative control: -EACCES with zero or one domain\n");
		fails = neg_pd_eacces_alone();
	} else if (!strcmp(mode, "neg-rtkit")) {
		printf("sim-h15 negative control: stage 6 refusals, a silent or odd firmware, the stuck core\n");
		fails = neg_rtkit();
	} else if (!strcmp(mode, "neg-map")) {
		printf("sim-h15 negative control: stage 5 map_fw refusals, undo on each failure, the stuck core\n");
		fails = neg_map();
	} else if (!strcmp(mode, "neg-dart")) {
		printf("sim-h15 negative control: stage 4 DART scan refusals and the run stage DART gate\n");
		fails = neg_dart();
	} else if (!strcmp(mode, "neg-run")) {
		printf("sim-h15 negative control: stage 4 refusals and the stuck core\n");
		fails = neg_run();
	} else {
		fprintf(stderr,
			"usage: sim_h15 [pos|neg-run|neg-dart|neg-map|neg-rtkit|neg-ps-stuck|neg-wrong-stage|neg-truncated-dt|neg-rpm-fail|neg-pd-attach-fail|neg-pd-link-fail|neg-pd-rpm-err|neg-pd-eacces-alone|neg-read-ps-stuck|neg-read-window|neg-read-forbidden]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
