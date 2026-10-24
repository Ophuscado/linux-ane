// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * sim/sim_harness.c: drives the real ane_h18 probe through every stage against sim/shim.h.
 *   sim_h18 pos                positive controls (exit 0 iff all pass)
 *   sim_h18 neg-psd-never      a slot that never acknowledges: FAIL, undo, no ASC read
 *   sim_h18 neg-psd-wrong      a slot that acknowledges the wrong state: FAIL, no ASC read
 *   sim_h18 neg-dt             tampered windows or properties: refused before any bus access
 *   sim_h18 neg-args           wrong optin, unknown stage, bad psd_state or psd_slots: no bus access
 * A negative control exits 0 only when the module fails the induced fault correctly.
 */
#include "shim.h"

char sim_log[SIM_LOG_SZ];
size_t sim_log_len;
struct sim_event sim_events[SIM_MAX_EVENTS];
int sim_n_events, sim_faults, sim_n_iomap, sim_n_iounmap;
int sim_psd_mode[SIM_NPSD];
u32 sim_psd_slot[SIM_NPSD] = { 30, 31 };
u32 sim_psd_state[SIM_NPSD];
u32 sim_psd_valid[SIM_NPSD];
u32 sim_psd_initial_valid[SIM_NPSD];
u64 sim_asc_pa = 0x419600000ULL, sim_rvbar_pa = 0x419050000ULL, sim_mbox_pa = 0x316000000ULL;
u32 sim_asc_size = 0x74000, sim_rvbar_size = 0x4000, sim_mbox_size = 0x1c000;
unsigned long sim_jiffies;
u32 sim_cpu_status = 0xa, sim_cpu_ctrl, sim_run_status = 0x9;
u64 sim_rvbar_val = 0x10000000001ULL;
int sim_core_running, sim_core_stuck, sim_ctrl_writes;
u32 sim_po_msg[SIM_PO_CH][SIM_PO_Q][4];
int sim_po_n[SIM_PO_CH], sim_po_head[SIM_PO_CH];
u32 sim_tx_w[4];
int sim_tx_got;
u64 sim_tx_msgs[32];
int sim_n_tx_msgs, sim_idle_sent, sim_stop_needs_idle, sim_no_shutdown_reply;
u32 sim_iop_ack_state = 0x20;
int sim_rpm_enabled, sim_rpm_gets, sim_rpm_puts, sim_rpm_get_fail;
struct sim_iocall sim_iocalls[64];
int sim_n_iocalls, sim_io_fail, sim_n_dom_live;
int sim_start_chan = -1, sim_start_msgs, sim_start_hello;
u32 sim_po_out_off, sim_alias_credits = 1, sim_hello_min = 11, sim_hello_max = 12, sim_hello_ep, sim_hello_type = 1;

#include "../ane_h18_soc.c"
#include "../ane_h18_main.c"

static int fails;
static struct platform_device pdev;
static struct device_node node;

static void chk(int ok, const char *name)
{
	printf("  %-74s %s\n", name, ok ? "PASS" : "FAIL");
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

static int log_pos(const char *needle)
{
	const char *p = strstr(sim_log, needle);

	return p ? (int)(p - sim_log) : -1;
}

static bool result(const char *stg, const char *verdict, const char *reason)
{
	char want[160];

	snprintf(want, sizeof(want), "ane_h18 RESULT stage=%s soc=t8152 verdict=%s reason=%s", stg, verdict, reason);
	return strstr(sim_log, want) != NULL;
}

static int writes(void)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		n += sim_events[i].write;
	return n;
}

static int events_in(u64 base, u32 size, int write)
{
	int i, n = 0;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write == write && sim_events[i].pa >= base && sim_events[i].pa < base + size)
			n++;
	return n;
}

static u64 psd_pa(int i) { return SIM_PSD_BASE + ((u64)sim_psd_slot[i] << 8); }

static void reset_all(void)
{
	int i;

	sim_n_events = sim_faults = sim_n_iomap = sim_n_iounmap = 0;
	sim_jiffies = 0;
	sim_log_reset();
	for (i = 0; i < SIM_NPSD; i++) {
		sim_psd_mode[i] = SIM_PSD_ACK;
		sim_psd_state[i] = 6;
		sim_psd_valid[i] = sim_psd_initial_valid[i] = 0;
	}
	optin = "t8152";
	stage = "dt";
	confirm_power = false;
	psd_state = 0;
	psd_slots = 1;
	keep_on = false;
	read_mbox = false;
	read_ctrl = false;
	read_dart = false;
	alias_mbox = false;
	rtkit_hello = false;
	rtkit_start = false;
	rtkit_stop = 0;
	sim_n_tx_msgs = sim_idle_sent = sim_stop_needs_idle = sim_no_shutdown_reply = 0;
	sim_iop_ack_state = 0x20;
	map_fw = false;
	sim_rpm_enabled = sim_rpm_gets = sim_rpm_puts = sim_rpm_get_fail = 0;
	sim_n_iocalls = sim_io_fail = sim_n_dom_live = 0;
	sim_start_hello = 0;
	sim_po_out_off = 0;
	sim_alias_credits = 1;
	sim_hello_min = 11;
	sim_hello_max = 12;
	sim_hello_ep = 0;
	sim_hello_type = 1;
	confirm_boot = false;
	boot_wait_ms = 300;
	sim_cpu_status = 0xa;
	sim_cpu_ctrl = 0;
	sim_run_status = 0x9;
	sim_rvbar_val = 0x10000000001ULL;
	sim_core_running = sim_core_stuck = sim_ctrl_writes = 0;
	memset(sim_po_msg, 0, sizeof(sim_po_msg));
	memset(sim_po_n, 0, sizeof(sim_po_n));
	memset(sim_po_head, 0, sizeof(sim_po_head));
	sim_start_chan = -1;
	sim_start_msgs = 0;
	psd_timeout_ms = 1000;
	memset(&pdev, 0, sizeof(pdev));
	memset(&node, 0, sizeof(node));
	node.n_reg = 5;
	node.reg_pa[0] = 0x419600000ULL; node.reg_size[0] = 0x74000; node.reg_name[0] = "asc";
	node.reg_pa[1] = 0x419050000ULL; node.reg_size[1] = 0x4000; node.reg_name[1] = "rvbar";
	node.reg_pa[2] = 0x316000000ULL; node.reg_size[2] = 0x1c000; node.reg_name[2] = "mbox";
	node.reg_pa[3] = psd_pa(0); node.reg_size[3] = 0x100; node.reg_name[3] = "psd0";
	node.reg_pa[4] = psd_pa(1); node.reg_size[4] = 0x100; node.reg_name[4] = "psd1";
	node.has_type = node.has_num = true;
	node.ane_type = 784;
	node.mbox_num = 12;
	pdev.dev.of_node = &node;
}

static int probe_now(void) { return ane_h18_probe(&pdev); }

static void pos_dt(void)
{
	int ret;

	reset_all();
	stage = "dt";
	ret = probe_now();
	chk(ret == 0, "P1 dt: probe returns 0");
	chk(result("dt", "PASS", "dt-windows-match"), "P1 dt: RESULT PASS dt-windows-match");
	chk(sim_n_events == 0 && sim_faults == 0, "P1 dt: zero bus accesses");
	chk(sim_n_iomap == 0, "P1 dt: nothing mapped");
}

static void pos_psd(void)
{
	int ret;

	reset_all();
	stage = "psd";
	ret = probe_now();
	chk(ret == 0, "P2 psd (idle slots): probe returns 0");
	chk(result("psd", "PASS", "psd-read-only"), "P2 psd: RESULT PASS psd-read-only");
	chk(writes() == 0, "P2 psd: zero bus writes");
	chk(sim_faults == 0, "P2 psd: zero faults");
	chk(events_in(psd_pa(0), 0x100, 0) == 1 && events_in(psd_pa(1), 0x100, 0) == 1,
	    "P2 psd: one read per slot (VALID only, it is not valid)");
	chk(events_in(sim_asc_pa, sim_asc_size, 0) + events_in(sim_rvbar_pa, sim_rvbar_size, 0) + events_in(sim_mbox_pa, sim_mbox_size, 0) == 0,
	    "P2 psd: no ASC, IORVBAR or mailbox read");
	chk(log_count("not-valid") == 2, "P2 psd: both slots reported not-valid");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == 2 && sim_n_iounmap == 2, "P2 psd: two windows mapped and released on remove");

	reset_all();
	stage = "psd";
	sim_psd_valid[0] = sim_psd_state[0] ? 1 : 1;
	sim_psd_state[0] = 0;
	ret = probe_now();
	chk(ret == 0 && events_in(psd_pa(0), 0x100, 0) == 2 && log_count("state=P0-run") == 1,
	    "P2b psd: a valid slot also reads STATUS and reports P0-run");
	chk(writes() == 0, "P2b psd: still zero writes");
}

static void pos_refuse(void)
{
	int ret;

	reset_all();
	stage = "power";
	ret = probe_now();
	chk(ret == -EPERM && result("power", "REFUSED", "confirm_power=1 required"), "P3 power without confirm_power: REFUSED, -EPERM");
	chk(writes() == 0 && events_in(sim_asc_pa, sim_asc_size, 0) == 0, "P3 power without confirm_power: no write and no ASC read");
	chk(sim_n_iomap == sim_n_iounmap, "P3 power without confirm_power: windows released");
}

/* the sequence of writes as "pa:val" tokens */
static void write_seq(char *out, size_t n)
{
	int i;

	out[0] = 0;
	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			snprintf(out + strlen(out), n - strlen(out), "%llx=%u ", (unsigned long long)sim_events[i].pa, sim_events[i].val);
}

static int first_asc_read(void)
{
	int i;

	for (i = 0; i < sim_n_events; i++)
		if (!sim_events[i].write && (sim_in(sim_events[i].pa, sim_asc_pa, sim_asc_size) || sim_in(sim_events[i].pa, sim_rvbar_pa, sim_rvbar_size) ||
					     sim_in(sim_events[i].pa, sim_mbox_pa, sim_mbox_size)))
			return i;
	return -1;
}

static int last_write(void)
{
	int i, l = -1;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			l = i;
	return l;
}

static int first_write(void)
{
	int i;

	for (i = 0; i < sim_n_events; i++)
		if (sim_events[i].write)
			return i;
	return -1;
}

static void pos_power(void)
{
	char seq[256], want[256];
	int ret, i;

	/* one slot, default state 0 */
	reset_all();
	stage = "power";
	confirm_power = true;
	ret = probe_now();
	chk(ret == 0 && result("power", "PASS", "psd-acked+asc-read"), "P4 power slot 30 only: PASS");
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=0 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	chk(!strcmp(seq, want), "P4 power: writes are exactly REQUEST=0 on slot 30, then REQUEST=6 on slot 30");
	chk(events_in(psd_pa(1), 0x100, 1) == 0 && events_in(psd_pa(1), 0x100, 0) == 0, "P4 power: slot 31 never touched");
	chk(sim_faults == 0, "P4 power: zero faults (no ASC read before the acknowledge)");
	chk(first_asc_read() > first_write() && first_asc_read() < last_write(), "P4 power: ASC reads sit after the first write and before the rail-gate write");
	chk(events_in(sim_asc_pa, sim_asc_size, 0) == 2 && events_in(sim_rvbar_pa, sim_rvbar_size, 0) == 2, "P4 power: two ASC reads, two IORVBAR reads");
	chk(events_in(sim_mbox_pa, sim_mbox_size, 0) == 0, "P4 power: no mailbox read without read_mbox");
	chk(events_in(sim_asc_pa + 0x44, 4, 0) == 0 && events_in(sim_asc_pa, sim_asc_size, 1) == 0, "P4 power: CPU_CONTROL not read by default, ASC window never written");
	chk(log_count("CRIT: ane_h18 write") == writes(), "P4 power: every write was logged before it");
	chk(log_count("CRIT: ane_h18 read CPU_STATUS") == 1 && log_count("CRIT: ane_h18 read ASC_STATUS") == 1 &&
	    log_count("CRIT: ane_h18 read IORVBAR_LO") == 1 && log_count("CRIT: ane_h18 read IORVBAR_HI") == 1,
	    "P4 power: every read was logged");
	chk(log_pos("CRIT: ane_h18 write psd0 slot=30 REQUEST=0") < log_pos("state 0 acknowledged"), "P4 power: write logged before the acknowledge line");
	chk(sim_psd_state[0] == 6, "P4 power: slot 30 ends at state 6");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "P4 power: all windows released on remove");

	/* two slots */
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 2;
	ret = probe_now();
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=0 %llx=0 %llx=6 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
		 (unsigned long long)psd_pa(1) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	chk(ret == 0 && !strcmp(seq, want), "P5 power two slots: 30 on, 31 on, then 31 off, 30 off");

	/* keep_on */
	reset_all();
	stage = "power";
	confirm_power = true;
	keep_on = true;
	ret = probe_now();
	chk(ret == 0 && writes() == 1 && sim_psd_state[0] == 0, "P6 keep_on: one write, the slot stays at state 0");

	/* read_ctrl: CPU_CONTROL is read once, still never written */
	reset_all();
	stage = "power";
	confirm_power = true;
	read_ctrl = true;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_asc_pa + 0x44, 4, 0) == 1 && events_in(sim_asc_pa, sim_asc_size, 1) == 0 && log_count("CRIT: ane_h18 read CPU_CONTROL pa=0x419600044") == 1,
	    "P8 read_ctrl: CPU_CONTROL read once, logged before, ASC window never written");

	/* read_mbox: the three candidate channels, exact addresses */
	reset_all();
	stage = "power";
	confirm_power = true;
	read_mbox = true;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 0) == 3, "P7 read_mbox: three mailbox reads");
	for (i = 0; i < 3; i++) {
		static const unsigned int ch[3] = { 12, 24, 25 };
		bool found = false;
		int k;

		for (k = 0; k < sim_n_events; k++)
			if (!sim_events[k].write && sim_events[k].pa == sim_mbox_pa + ch[i] * 0x80 + 0x20)
				found = true;
		char nm[64];

		snprintf(nm, sizeof(nm), "P7 read_mbox: channel %u status at 0x%llx", ch[i], (unsigned long long)(sim_mbox_pa + ch[i] * 0x80 + 0x20));
		chk(found, nm);
	}
}

static int neg_psd_failure(int mode, int slots)
{
	int ret, bad = 0;
	char seq[256], want[256];

	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = slots;
	if (slots == 2) {
		sim_psd_mode[1] = mode;		/* slot 31 fails, slot 30 acks */
	} else {
		sim_psd_mode[0] = mode;
	}
	ret = probe_now();
	if (ret != -ETIMEDOUT) {
		printf("    expected -ETIMEDOUT, got %d\n", ret);
		bad = 1;
	}
	if (!result("power", "FAIL", "psd-not-acknowledged")) {
		printf("    expected RESULT FAIL psd-not-acknowledged\n");
		bad = 1;
	}
	if (first_asc_read() >= 0) {
		printf("    an ASC, IORVBAR or mailbox read happened without an acknowledge\n");
		bad = 1;
	}
	if (sim_faults) {
		printf("    bus faults: %d\n", sim_faults);
		bad = 1;
	}
	write_seq(seq, sizeof(seq));
	if (slots == 2)
		snprintf(want, sizeof(want), "%llx=0 %llx=0 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
			 (unsigned long long)psd_pa(0) + 0x90);
	else
		snprintf(want, sizeof(want), "%llx=0 ", (unsigned long long)psd_pa(0) + 0x90);
	if (strcmp(seq, want)) {
		printf("    write sequence '%s', want '%s'\n", seq, want);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    a failed probe leaked a window\n");
		bad = 1;
	}
	if (sim_jiffies < psd_timeout_ms) {
		printf("    the wait ended before the timeout (%lu ms)\n", sim_jiffies);
		bad = 1;
	}
	return bad;
}

/* a slot that acknowledges the run state and ignores the request for state 6: the stage must fail, never report PASS */
static int neg_undo6(void)
{
	int bad = 0, ret;

	reset_all();
	stage = "power";
	confirm_power = true;
	psd_state = 0;
	psd_slots = 1;
	sim_psd_mode[0] = SIM_PSD_IGNORE6;
	ret = probe_now();
	if (ret == 0 || !result("power", "FAIL", "psd-not-returned-to-state-6") || log_count("verdict=PASS") != 0) {
		printf("    a slot that ignores state 6: ret %d, no FAIL psd-not-returned-to-state-6 or a PASS line\n", ret);
		bad = 1;
	}
	if (log_count("did not return to state 6") != 1 || sim_psd_state[0] != 0) {
		printf("    the warning line or the slot state is not as expected (state %u)\n", sim_psd_state[0]);
		bad = 1;
	}
	if (sim_n_iomap != sim_n_iounmap) {
		printf("    leaked a window\n");
		bad = 1;
	}
	return bad;
}

static int neg_never(void)
{
	int bad = neg_psd_failure(SIM_PSD_NEVER, 1) | neg_psd_failure(SIM_PSD_NEVER, 2) | neg_undo6();

	printf("%s\n", bad ? "  neg-psd-never: harness FAILED the module (BAD)" : "  neg-psd-never: module fails as required");
	return bad;
}

static int neg_wrong(void)
{
	int bad = neg_psd_failure(SIM_PSD_WRONG_STATE, 1) | neg_psd_failure(SIM_PSD_WRONG_STATE, 2);

	printf("%s\n", bad ? "  neg-psd-wrong: harness FAILED the module (BAD)" : "  neg-psd-wrong: module fails as required");
	return bad;
}

static int neg_dt(void)
{
	int c, ret, bad = 0;

	for (c = 0; c < 8; c++) {
		const char *what = "";

		reset_all();
		stage = "power";
		confirm_power = true;
		switch (c) {
		case 0: node.reg_pa[0] += 0x1000000; what = "asc base moved"; break;
		case 1: node.reg_size[1] -= 0x1000; what = "rvbar shorter"; break;
		case 2: node.reg_pa[3] = psd_pa(0) + 0x100; what = "psd0 on the next slot"; break;
		case 3: node.reg_pa[4] = 0x303a15200ULL; what = "psd1 on a GPU slot"; break;
		case 4: node.mbox_num = 0; what = "mailbox number 0 (the first overlay's value)"; break;
		case 5: node.ane_type = 1; what = "wrong ane-type"; break;
		case 6: node.reg_name[2] = "mailbox"; what = "reg-names mbox missing"; break;
		case 7: node.has_num = false; what = "no mailbox number"; break;
		}
		ret = probe_now();
		if (ret == 0 || !strstr(sim_log, "verdict=FAIL reason=dt-window-mismatch")) {
			printf("    %s: probe returned %d without a dt-window-mismatch FAIL\n", what, ret);
			bad = 1;
		}
		if (sim_n_events || sim_n_iomap) {
			printf("    %s: bus access or mapping before the check (events %d, iomap %d)\n", what, sim_n_events, sim_n_iomap);
			bad = 1;
		}
	}
	printf("%s\n", bad ? "  neg-dt: harness FAILED the module (BAD)" : "  neg-dt: module refuses as required");
	return bad;
}


static void boot_setup(void)
{
	reset_all();
	stage = "boot";
	confirm_power = true;
	confirm_boot = true;
	psd_slots = 2;
	psd_state = 1;
}

static int ctrl_write_events(void) { return events_in(sim_asc_pa + 0x44, 4, 1); }

/* kernel-powered node: power-domains and iommus in the node, both PSD slots already on (the kernel PSD driver did it) */
static void kboot_setup(void)
{
	boot_setup();
	node.has_power_domains = true;
	node.has_iommus = true;
	sim_psd_state[0] = sim_psd_state[1] = 1;
	map_fw = true;
}

static int psd_write_events(void)
{
	int k, n = 0;

	for (k = 0; k < sim_n_events; k++)
		if (sim_events[k].write == 1 && sim_events[k].pa >= SIM_PSD_BASE && sim_events[k].pa < SIM_PSD_BASE + 0x40000)
			n++;
	return n;
}

static int iocalls_of(int code)
{
	int k, n = 0;

	for (k = 0; k < sim_n_iocalls && k < 64; k++)
		n += sim_iocalls[k].code == code;
	return n;
}

static int first_run_event(void)
{
	int k, run = -1;

	for (k = 0; k < sim_n_events; k++)
		if (sim_events[k].write == 1 && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
			run = k;
	return run;
}

static void pos_boot(void)
{
	char seq[512], want[512];
	int ret, i;

	/* happy path: the core starts, reports running, posts two messages on channel 25, stops on request */
	boot_setup();
	sim_start_chan = 25;
	sim_start_msgs = 2;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "B1 boot: PASS core-ran-and-stopped");
	write_seq(seq, sizeof(seq));
	snprintf(want, sizeof(want), "%llx=1 %llx=1 %llx=16 %llx=0 %llx=6 %llx=6 ", (unsigned long long)psd_pa(0) + 0x90, (unsigned long long)psd_pa(1) + 0x90,
		 (unsigned long long)sim_asc_pa + 0x44, (unsigned long long)sim_asc_pa + 0x44, (unsigned long long)psd_pa(1) + 0x90, (unsigned long long)psd_pa(0) + 0x90);
	if (strcmp(seq, want))
		printf("    got  %s\n    want %s\n", seq, want);
	chk(!strcmp(seq, want), "B1 boot: writes are exactly PSD 30 and 31 on, CONTROL=16 (0x10), CONTROL=0, PSD 31 and 30 off");
	chk(sim_faults == 0, "B1 boot: zero faults");
	chk(log_count("ane_h18 msg ch25 msg0=0x1000 msg1=0x100") == 1 && log_count("ane_h18 msg ch25 msg0=0x1001 msg1=0x100") == 1, "B1 boot: both messages logged with their raw words");
	chk(log_count("running_seen=1 messages=2") == 1 && sim_core_running == 0 && sim_cpu_status == 0xa, "B1 boot: running seen, two messages, core stopped at the end");
	{
		int first_run = -1, k;

		for (k = 0; k < sim_n_events; k++)
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				first_run = k;
		chk(first_run > 0 && first_run > first_asc_read(), "B1 boot: the RUN write comes after the preflight reads");
	}
	chk(log_count("CRIT: ane_h18 write CPU_CONTROL=0x10") == 1 && log_count("CRIT: ane_h18 write CPU_CONTROL=0 ") == 1, "B1 boot: both CPU_CONTROL writes logged before");
	chk(events_in(sim_rvbar_pa, sim_rvbar_size, 1) == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0, "B1 boot: IORVBAR and the mailbox are never written");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "B1 boot: windows released on remove after a clean stop");

	/* message cap */
	boot_setup();
	sim_start_chan = 25;
	sim_start_msgs = 20;
	ret = probe_now();
	if (log_count("ane_h18 msg ch25") != ANE_H18_PO_MAXMSG)
		printf("    msg lines: %d\n", log_count("ane_h18 msg ch25"));
	chk(ret == 0 && log_count("ane_h18 msg ch25") == ANE_H18_PO_MAXMSG, "B2 boot: at most 8 messages are read");

	/* messages on the other candidate channels are seen too */
	boot_setup();
	sim_start_chan = 12;
	sim_start_msgs = 1;
	ret = probe_now();
	chk(ret == 0 && log_count("ane_h18 msg ch12") == 1, "B3 boot: a message on candidate channel 12 is read");

	/* read_dart: twelve reads of the DART block, after both PSD acknowledges and before the RUN write, never a write */
	boot_setup();
	read_dart = true;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "D1 boot read_dart: PASS");
	chk(events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == ANE_H18_DART_NREAD && events_in(SIM_DART_PA, SIM_DART_SIZE, 1) == 0, "D1 boot read_dart: twelve DART reads, no DART write");
	chk(sim_faults == 0, "D1 boot read_dart: zero faults (the DART block was read with both slots on)");
	{
		int first_dart = -1, last_dart = -1, run = -1, psd_ack2 = -1, k;

		for (k = 0; k < sim_n_events; k++) {
			if (sim_in(sim_events[k].pa, SIM_DART_PA, SIM_DART_SIZE)) {
				if (first_dart < 0)
					first_dart = k;
				last_dart = k;
			}
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				run = k;
			if (sim_events[k].write && sim_events[k].pa == psd_pa(1) + 0x90 && sim_events[k].val == 1)
				psd_ack2 = k;
		}
		chk(first_dart > psd_ack2 && psd_ack2 >= 0 && last_dart < run && run > 0, "D1 boot read_dart: DART reads sit after PSD slot 31 on and before the RUN write");
	}
	chk(log_count("CRIT: ane_h18 read DART0_0 pa=0x419800000") == 1 && log_count("CRIT: ane_h18 read DART0_0x1438 pa=0x419801438") == 1, "D1 boot read_dart: each read is logged before it is done");
	ane_h18_remove(&pdev);
	chk(sim_n_iomap == sim_n_iounmap, "D1 boot read_dart: the DART mapping is released on remove");
	/* without read_dart the DART block is never touched, and no window is mapped for it */
	boot_setup();
	ret = probe_now();
	chk(ret == 0 && events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == 0 && events_in(SIM_DART_PA, SIM_DART_SIZE, 1) == 0, "D2 boot: no DART access without read_dart");
	/* alias bank: the status word at window + 0x10c20 is read only with alias_mbox, and a message there is logged raw */
	boot_setup();
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa + ANE_H18_PO_ALIAS_OFF, 0x1000, 0) == 0, "A1 boot: the alias bank is not read without alias_mbox");
	boot_setup();
	alias_mbox = true;
	sim_start_chan = 536;
	sim_start_msgs = 1;
	ret = probe_now();
	chk(ret == 0 && log_count("ane_h18 msg alias24 rx=0x1000,0,0x100,0") == 1, "A2 boot alias_mbox: a message on the alias channel is read and logged raw");
	{
		int k, st = 0;

		for (k = 0; k < sim_n_events; k++)
			if (!sim_events[k].write && sim_events[k].pa == sim_mbox_pa + 0x10c20)
				st++;
		chk(st >= 1 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0, "A2 boot alias_mbox: the status word at window + 0x10c20 is read, the mailbox is never written");
	}

	/* RTKit exchange: HELLO answered with version 12 and the last EPMAP answered with the same flag, in TX (+0x10) only; RX is +0x00 */
	{
		int k, hello_w = 0, epmap_w = 0, wrong_half = 0;

		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "H1 boot rtkit_hello: PASS");
		for (k = 0; k < sim_n_events; k++) {
			u64 pa = sim_events[k].pa;

			if (!sim_events[k].write || !sim_in(pa, sim_mbox_pa, sim_mbox_size))
				continue;
			if (pa >= sim_mbox_pa + 0x10c00 && pa < sim_mbox_pa + 0x10c10)
				wrong_half++;
			if (pa == sim_mbox_pa + 0x10c10 && sim_events[k].val == 0x000c000c)
				hello_w++;
			if (pa == sim_mbox_pa + 0x10c14 && sim_events[k].val == 0x00200000)
				hello_w++;
			if (pa == sim_mbox_pa + 0x10c14 && sim_events[k].val == 0x00880000)
				epmap_w++;
		}
		chk(hello_w == 2 && epmap_w == 1 && wrong_half == 0, "H1 boot rtkit_hello: HELLO_REPLY version 12 and the EPMAP reply go to TX (+0x10) only, never to RX");
		chk(events_in(sim_mbox_pa, sim_mbox_size, 1) == 8 && log_count("CRIT: ane_h18 write TX of alias24 pa=0x316010c10") == 2, "H1 boot rtkit_hello: two replies of four words, each logged before");
		if (log_count("ane_h18 rtkit rx type=") != 2 || sim_faults)
			printf("    rx lines %d, faults %d\n", log_count("ane_h18 rtkit rx type="), sim_faults);
		chk(log_count("ane_h18 rtkit rx type=") == 2 && sim_faults == 0, "H1 boot rtkit_hello: both messages decoded and logged, zero faults");
	}
	/* rtkit_start: the whole management boot against a firmware that reacts to what the host writes */
	{
		static const u64 want_start[6] = { 0x200000000c000cULL, 0x80000000000001ULL, 0x88000100000000ULL, 0x50000100000002ULL, 0xb0000000000020ULL, 0x50002000000002ULL };
		int k, same;

		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "R1 boot rtkit_start: PASS");
		same = sim_n_tx_msgs == 6;
		for (k = 0; k < 6 && k < sim_n_tx_msgs; k++)
			same &= sim_tx_msgs[k] == want_start[k];
		if (!same)
			for (k = 0; k < sim_n_tx_msgs && k < 32; k++)
				printf("    tx[%d] = %#llx\n", k, (unsigned long long)sim_tx_msgs[k]);
		chk(same, "R1 boot rtkit_start: HELLO_REPLY, two EPMAP replies, STARTEP ep 1, AP power ON, STARTEP ep 0x20, in that order and nothing else");
		chk(sim_faults == 0 && log_count("rtkit endpoint 0x20 discovered") == 1 && log_count("AP power state acknowledged: 0xb0000000000020") + log_count("AP power state acknowledged: 0x20") == 1, "R1 boot rtkit_start: endpoint 0x20 discovered, the AP power acknowledgement seen, zero faults");
		/* an IOP acknowledgement for another state does not get AP power ON */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_iop_ack_state = 0x10;
		ret = probe_now();
		chk(ret == 0 && sim_n_tx_msgs == 4, "R2 boot rtkit_start: an IOP acknowledgement for state 0x10 gets no AP power ON and no application endpoint start");
		/* without rtkit_start only HELLO and the maps are answered */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 3;
		ret = probe_now();
		chk(ret == 0 && sim_n_tx_msgs == 3, "R3 boot rtkit_hello alone: three replies, no STARTEP, no power state message");
		/* rtkit_stop: AP OFF, IOP OFF, IOP idle, and only then does the modelled core accept the RUN clear */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped") && sim_n_tx_msgs == 8 && sim_tx_msgs[6] == 0xb0000000000010ULL && sim_tx_msgs[7] == 0x60000000000001ULL,
		    "R4 boot rtkit_stop: AP power QUIESCED then IOP power SLEEP follow the boot messages and the core stops after the IOP acknowledgement");
		chk(log_count("rtkit shutdown: ap_acked=1 iop_acked=1 iop_idle=0") == 1, "R4 boot rtkit_stop: both acknowledgements seen");
		/* rtkit_stop=2: OFF then OFF, the pair the board stopped with */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 2;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		ret = probe_now();
		chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped") && sim_n_tx_msgs == 8 && sim_tx_msgs[6] == 0xb0000000000000ULL && sim_tx_msgs[7] == 0x60000000000000ULL,
		    "R6 boot rtkit_stop=2: AP power OFF then IOP power OFF, the core stops after the IOP acknowledgement");
		/* a firmware that ignores the shutdown while the core still accepts the RUN clear: the stage must not report PASS */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_no_shutdown_reply = 1;
		ret = probe_now();
		chk(ret != 0 && result("boot", "FAIL", "rtkit-shutdown-incomplete") && log_count("verdict=PASS") == 0, "R8 boot rtkit_stop: an unanswered shutdown is FAIL rtkit-shutdown-incomplete even when the core stopped");
		/* rtkit_stop=3 is refused */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 3;
		ret = probe_now();
		chk(ret == -EINVAL && !writes(), "R7 boot rtkit_stop=3 is refused before any write");
		/* a firmware that never answers the shutdown: no PASS, the slots stay, the core is reported as not stopped */
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		rtkit_start = true;
		rtkit_stop = 1;
		sim_start_chan = 536;
		sim_start_hello = 3;
		sim_stop_needs_idle = 1;
		sim_no_shutdown_reply = 1;
		ret = probe_now();
		chk(ret != 0 && result("boot", "FAIL", "core-not-stopped") && log_count("ap_acked=0 iop_acked=0") == 1 && sim_n_tx_msgs == 7, "R5 boot rtkit_stop: no shutdown answer gives FAIL core-not-stopped, AP QUIESCED sent once, no IOP SLEEP");
	}
	/* without rtkit_hello the messages are only logged */
	boot_setup();
	alias_mbox = true;
	sim_start_chan = 536;
	sim_start_hello = 1;
	ret = probe_now();
	chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == 0 && log_count("ane_h18 rtkit rx type=") == 2, "H2 boot alias_mbox without rtkit_hello: messages logged, nothing written");
	/* unknown type, another endpoint, firmware versions without 12, no inbox credit: no reply */
	for (i = 0; i < 4; i++) {
		boot_setup();
		alias_mbox = true;
		rtkit_hello = true;
		sim_start_chan = 536;
		sim_start_hello = 1;
		if (i == 0) sim_hello_type = 3;
		if (i == 1) sim_hello_ep = 1;
		if (i == 2) { sim_hello_min = 13; sim_hello_max = 14; }
		if (i == 3) sim_alias_credits = 0;
		ret = probe_now();
		chk(ret == 0 && events_in(sim_mbox_pa, sim_mbox_size, 1) == (i == 3 ? 0 : 4), i == 0 ? "H3 boot rtkit_hello: an unknown type gets no reply (only the EPMAP is answered)" : i == 1 ? "H3 boot rtkit_hello: a message for another endpoint gets no reply (only the EPMAP is answered)" : i == 2 ? "H3 boot rtkit_hello: firmware versions 13 to 14 get no HELLO reply (only the EPMAP is answered)" : "H3 boot rtkit_hello: no inbox credit, nothing written");
	}
	/* status read before RUN */
	boot_setup();
	alias_mbox = true;
	ret = probe_now();
	{
		int first_alias = -1, run = -1, k;

		for (k = 0; k < sim_n_events; k++) {
			if (first_alias < 0 && !sim_events[k].write && sim_events[k].pa == sim_mbox_pa + 0x10c20)
				first_alias = k;
			if (sim_events[k].write && sim_events[k].pa == sim_asc_pa + 0x44 && sim_events[k].val == 0x10)
				run = k;
		}
		chk(ret == 0 && first_alias > 0 && first_alias < run && log_count("ane_h18 alias status before RUN") == 1, "H4 boot alias_mbox: the alias STATUS word is read and logged before the RUN write");
	}

	/* kernel-powered node with map_fw: no PSD request, a runtime PM reference, then attach, two maps, the DART read again, RUN, and a clean undo */
	kboot_setup();
	read_dart = true;
	alias_mbox = true;
	rtkit_hello = true;
	sim_start_chan = 536;
	sim_start_hello = 1;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-ran-and-stopped"), "K1 boot map_fw (kernel-powered): PASS");
	chk(psd_write_events() == 0 && sim_rpm_enabled == 1 && sim_rpm_gets == 1 && sim_rpm_puts == 1, "K1 boot map_fw: no PSD write, one runtime PM get and put");
	chk(iocalls_of(SIM_IO_ALLOC) == 1 && iocalls_of(SIM_IO_ATTACH) == 1 && iocalls_of(SIM_IO_MAP) == 2 && iocalls_of(SIM_IO_UNMAP) == 2 &&
	    iocalls_of(SIM_IO_DETACH) == 1 && iocalls_of(SIM_IO_FREE) == 1 && sim_n_dom_live == 0, "K1 boot map_fw: alloc, attach, two maps, then two unmaps, detach and free after the clean stop");
	{
		int maps = 0, ok = 1, k, order_ok = 1, last_map = -1, attach = -1;

		for (k = 0; k < sim_n_iocalls && k < 64; k++) {
			if (sim_iocalls[k].code == SIM_IO_ATTACH)
				attach = sim_iocalls[k].ev;
			if (sim_iocalls[k].code != SIM_IO_MAP)
				continue;
			last_map = sim_iocalls[k].ev;
			if (maps == 0)
				ok &= sim_iocalls[k].iova == 0x10000000000ULL && sim_iocalls[k].pa == 0x100014e8000ULL && sim_iocalls[k].size == 0xc4000 && sim_iocalls[k].prot == IOMMU_READ;
			if (maps == 1)
				ok &= sim_iocalls[k].iova == 0x100000c4000ULL && sim_iocalls[k].pa == 0x100030c4000ULL && sim_iocalls[k].size == 0x2ac000 && sim_iocalls[k].prot == (IOMMU_READ | IOMMU_WRITE);
			maps++;
		}
		chk(maps == 2 && ok, "K1 boot map_fw: TEXT maps 0x100014e8000 at 0x10000000000 read only, DATA 0x100030c4000 at 0x100000c4000 read and write");
		order_ok = attach > 0 && last_map >= attach && first_run_event() > last_map;
		chk(order_ok, "K1 boot map_fw: the attach and both maps come before the RUN write");
	}
	chk(events_in(SIM_DART_PA, SIM_DART_SIZE, 0) == 2 * ANE_H18_DART_NREAD && log_count("CRIT: ane_h18 read DARTPOST_0 pa=0x419800000") == 1, "K1 boot map_fw: the DART words are read before and after the map");
	chk(sim_faults == 0 && log_count("ane_h18 rtkit rx type=") == 2, "K1 boot map_fw: zero faults, HELLO and EPMAP decoded");
	/* kernel-powered node without map_fw: still no PSD request */
	kboot_setup();
	map_fw = false;
	ret = probe_now();
	chk(ret == 0 && psd_write_events() == 0 && iocalls_of(SIM_IO_ALLOC) == 0 && sim_rpm_gets == 1 && sim_rpm_puts == 1, "K2 boot kernel-powered without map_fw: PASS, no PSD write, no iommu call");

	/* the core never reports running: PASS with its own reason, still stopped at the end */
	boot_setup();
	sim_run_status = 0x2a;
	ret = probe_now();
	chk(ret == 0 && result("boot", "PASS", "core-never-reported-running"), "B4 boot: no RUNNING bit gives PASS core-never-reported-running");
	(void)i;
}

static int neg_boot(void)
{
	int bad = 0, ret;
	char seq[256];

	/* refusals: no write of any kind */
	boot_setup();
	confirm_boot = false;
	ret = probe_now();
	if (ret != -EPERM || writes() || !result("boot", "REFUSED", "confirm_power=1 and confirm_boot=1 required")) { printf("    no confirm_boot\n"); bad = 1; }
	boot_setup();
	confirm_power = false;
	ret = probe_now();
	if (ret != -EPERM || writes()) { printf("    boot without confirm_power\n"); bad = 1; }
	boot_setup();
	boot_wait_ms = 5;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    boot_wait_ms 5\n"); bad = 1; }

	/* rtkit_start needs rtkit_hello, rtkit_stop needs rtkit_start: refused before any write */
	boot_setup();
	alias_mbox = true;
	rtkit_start = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_start=1 needs rtkit_hello=1")) { printf("    rtkit_start without rtkit_hello\n"); bad = 1; }
	boot_setup();
	alias_mbox = true;
	rtkit_hello = true;
	rtkit_stop = 1;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_stop=1 or 2 needs rtkit_start=1")) { printf("    rtkit_stop without rtkit_start\n"); bad = 1; }
	/* rtkit_hello without alias_mbox is refused before any write */
	boot_setup();
	rtkit_hello = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("boot", "REFUSED", "rtkit_hello=1 needs alias_mbox=1")) { printf("    rtkit_hello without alias_mbox\n"); bad = 1; }
	/* read_dart needs both slots: with one slot it is refused before any write or DART read */
	boot_setup();
	read_dart = true;
	psd_slots = 1;
	ret = probe_now();
	if (ret != -EINVAL || writes() || events_in(SIM_DART_PA, SIM_DART_SIZE, 0) || !result("boot", "REFUSED", "read_dart=1 needs psd_slots=2 (the DART block is read only with both ANE slots on)")) { printf("    read_dart with one slot\n"); bad = 1; }
	/* a preflight stop never reads the DART */
	boot_setup();
	read_dart = true;
	sim_rvbar_val = 0x10000000000ULL;
	ret = probe_now();
	if (ret != -EBUSY || events_in(SIM_DART_PA, SIM_DART_SIZE, 0) || ctrl_write_events()) { printf("    DART read after a failed preflight\n"); bad = 1; }

	/* preflight: each deviation stops before the RUN write and puts the slots back */
	{
		int c;

		for (c = 0; c < 4; c++) {
			const char *what = "";

			boot_setup();
			switch (c) {
			case 0: sim_rvbar_val = 0x10000000000ULL; what = "IORVBAR without the valid bit"; break;
			case 1: sim_rvbar_val = 0x20000000001ULL; what = "IORVBAR at another address"; break;
			case 2: sim_cpu_status = 0x9; what = "the core already reports RUNNING"; break;
			case 3: sim_cpu_ctrl = 0x10; what = "CPU_CONTROL.RUN already set"; break;
			}
			ret = probe_now();
			write_seq(seq, sizeof(seq));
			if (ret != -EBUSY || ctrl_write_events() || !result("boot", "FAIL", "preflight-not-as-iboot-left-it") || sim_psd_state[0] != 6 || sim_psd_state[1] != 6) {
				printf("    %s: ret %d, control writes %d, writes '%s'\n", what, ret, ctrl_write_events(), seq);
				bad = 1;
			}
			if (sim_n_iomap != sim_n_iounmap) { printf("    %s: leaked a window\n", what); bad = 1; }
		}
	}
	/* a slot that does not acknowledge: no ASC access, no CONTROL write */
	boot_setup();
	sim_psd_mode[1] = SIM_PSD_NEVER;
	ret = probe_now();
	if (ret != -ETIMEDOUT || ctrl_write_events() || first_asc_read() >= 0 || sim_psd_state[0] != 6) { printf("    PSD never acknowledges\n"); bad = 1; }

	/* map_fw: refusals and failures end before the RUN write and undo what was done */
	boot_setup();
	map_fw = true;
	ret = probe_now();
	if (ret != -EINVAL || writes() || sim_n_iocalls || !result("boot", "REFUSED", "map_fw=1 needs power-domains in the node (the kernel's PSD driver powers the slots for the DART)")) { printf("    map_fw without power-domains\n"); bad = 1; }
	{
		int c;

		for (c = 0; c < 5; c++) {
			const char *what = "";

			kboot_setup();
			switch (c) {
			case 0: node.has_iommus = false; what = "no iommus in the node"; break;
			case 1: sim_io_fail = 1; what = "domain alloc fails"; break;
			case 2: sim_io_fail = 2; what = "attach fails"; break;
			case 3: sim_io_fail = 3; what = "the second map fails"; break;
			case 4: sim_rpm_get_fail = 1; what = "the runtime PM get fails"; break;
			}
			ret = probe_now();
			if (ret == 0 || first_run_event() >= 0 || psd_write_events() || sim_n_dom_live != 0 || !result("boot", "FAIL", c == 4 ? "kernel-psd-power-on-failed" : "fw-map-failed")) {
				printf("    %s: ret %d, run event %d, psd writes %d, live domains %d\n", what, ret, first_run_event(), psd_write_events(), sim_n_dom_live);
				bad = 1;
			}
			if (c == 3 && (iocalls_of(SIM_IO_MAP) != 2 || iocalls_of(SIM_IO_UNMAP) != 1 || iocalls_of(SIM_IO_DETACH) != 1 || iocalls_of(SIM_IO_FREE) != 1)) { printf("    %s: undo incomplete\n", what); bad = 1; }
			if (c == 2 && (iocalls_of(SIM_IO_MAP) != 0 || iocalls_of(SIM_IO_FREE) != 1)) { printf("    %s: map after a failed attach or no free\n", what); bad = 1; }
			if (c == 4 && (sim_n_iocalls || events_in(sim_asc_pa, sim_asc_size, 0))) { printf("    %s: touched the ASC or the iommu\n", what); bad = 1; }
			if (c != 4 && sim_rpm_puts != 1) { printf("    %s: runtime PM reference not released\n", what); bad = 1; }
			if (sim_n_iomap != sim_n_iounmap) { printf("    %s: leaked a window\n", what); bad = 1; }
		}
	}
	/* a stuck core with map_fw: the mapping and the power reference stay, nothing is undone under a core that may run */
	kboot_setup();
	sim_core_stuck = 1;
	ret = probe_now();
	if (ret == 0 || !result("boot", "FAIL", "core-not-stopped") || iocalls_of(SIM_IO_UNMAP) || iocalls_of(SIM_IO_DETACH) || iocalls_of(SIM_IO_FREE) || sim_n_dom_live != 1 || sim_rpm_puts != 0) {
		printf("    stuck core with map_fw: ret %d unmaps %d live %d puts %d\n", ret, iocalls_of(SIM_IO_UNMAP), sim_n_dom_live, sim_rpm_puts);
		bad = 1;
	}

	/* a core that does not stop: FAIL, slots stay on, the module keeps its mappings and refuses to claim a clean state */
	boot_setup();
	sim_core_stuck = 1;
	ret = probe_now();
	write_seq(seq, sizeof(seq));
	if (ret == 0 || !result("boot", "FAIL", "core-not-stopped") || sim_psd_state[0] == 6 || sim_psd_state[1] == 6 || log_count("reboot required") < 1 || sim_n_iounmap != 0) {
		printf("    stuck core: ret %d writes '%s' psd %u %u iounmap %d\n", ret, seq, sim_psd_state[0], sim_psd_state[1], sim_n_iounmap);
		bad = 1;
	}
	printf("%s\n", bad ? "  neg-boot: harness FAILED the module (BAD)" : "  neg-boot: module refuses and fails as required");
	return bad;
}

static int neg_args(void)
{
	int bad = 0, ret;

	reset_all();
	optin = NULL;
	ret = probe_now();
	if (ret != -EPERM || sim_n_events || sim_n_iomap) { printf("    unset optin\n"); bad = 1; }
	reset_all();
	optin = "t8142";
	ret = probe_now();
	if (ret != -EPERM || sim_n_events || sim_n_iomap) { printf("    wrong optin\n"); bad = 1; }
	reset_all();
	stage = "run";
	ret = probe_now();
	if (ret != -EINVAL || sim_n_events || sim_n_iomap) { printf("    stage=run must not exist\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_state = 6;
	ret = probe_now();
	if (ret != -EINVAL || writes() || !result("power", "REFUSED", "psd_state must be 0 to 5 and psd_slots 1 or 2")) { printf("    psd_state 6\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 3;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    psd_slots 3\n"); bad = 1; }
	reset_all();
	stage = "power";
	confirm_power = true;
	psd_slots = 0;
	ret = probe_now();
	if (ret != -EINVAL || writes()) { printf("    psd_slots 0\n"); bad = 1; }
	printf("%s\n", bad ? "  neg-args: harness FAILED the module (BAD)" : "  neg-args: module refuses as required");
	return bad;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "pos";

	if (!strcmp(mode, "pos")) {
		printf("sim-h18 positive controls:\n");
		pos_dt();
		pos_psd();
		pos_refuse();
		pos_power();
		pos_boot();
		printf("%s\n", fails ? "SIM-TEST: FAILED" : "SIM-TEST: all positive controls PASS");
	} else if (!strcmp(mode, "neg-psd-never")) {
		fails = neg_never();
	} else if (!strcmp(mode, "neg-psd-wrong")) {
		fails = neg_wrong();
	} else if (!strcmp(mode, "neg-dt")) {
		fails = neg_dt();
	} else if (!strcmp(mode, "neg-boot")) {
		fails = neg_boot();
	} else if (!strcmp(mode, "neg-args")) {
		fails = neg_args();
	} else {
		fprintf(stderr, "usage: sim_h18 [pos|neg-psd-never|neg-psd-wrong|neg-dt|neg-boot|neg-args]\n");
		return 2;
	}
	return fails ? 1 : 0;
}
