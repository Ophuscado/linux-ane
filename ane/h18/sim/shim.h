/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * sim/shim.h: userspace shim so the real ane_h18_main.c and ane_h18_soc.c compile and run unmodified in the
 * harness. A fake MMIO bus records every read and write; the PSD slot windows behave like the hardware
 * as pmgr-psd.c documents it (a REQUEST write sets VALID and STATUS, a STATUS read clears VALID), and the
 * ASC, IORVBAR and mailbox windows fault (return 0xdeadbeef and count a fault) unless the PSD slots they
 * depend on are running. A sim is not silicon: it proves control flow, ordering and gating of this module,
 * not any M6 behavior.
 */
#ifndef SIM_SHIM_H
#define SIM_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define __iomem

#define SIM_LOG_SZ (1 << 18)
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;

static inline void sim_log_reset(void) { sim_log_len = 0; sim_log[0] = '\0'; }

static inline void rec_log(const char *fmt, ...)
{
	va_list ap, ap2;
	int n;

	va_start(ap, fmt);
	if (sim_log_len + 1 < SIM_LOG_SZ) {
		va_copy(ap2, ap);
		n = vsnprintf(sim_log + sim_log_len, SIM_LOG_SZ - sim_log_len, fmt, ap2);
		va_end(ap2);
		if (n > 0)
			sim_log_len += (size_t)n;
	}
	va_end(ap);
}

#define dev_info(d, ...)  do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...)   do { (void)(d); rec_log("ERR: " __VA_ARGS__); } while (0)
#define dev_warn(d, ...)  do { (void)(d); rec_log("WARN: " __VA_ARGS__); } while (0)
#define dev_crit(d, ...)  do { (void)(d); rec_log("CRIT: " __VA_ARGS__); } while (0)
#define dev_emerg(d, ...) do { (void)(d); rec_log("EMERG: " __VA_ARGS__); } while (0)

/* ---- fake device tree node ---- */
#define SIM_MAX_REG 8
struct device_node {
	int n_reg;
	u64 reg_pa[SIM_MAX_REG];
	u64 reg_size[SIM_MAX_REG];
	const char *reg_name[SIM_MAX_REG];
	bool has_type;
	u32 ane_type;
	bool has_num;
	u32 mbox_num;
	bool has_power_domains;
	bool has_iommus;
};

struct device { struct device_node *of_node; };
struct platform_device { struct device dev; void *drvdata; };
struct resource { u64 start; u64 end; };

static inline u64 resource_size(const struct resource *r) { return r->end - r->start + 1; }

static inline int of_property_read_u32(const struct device_node *n, const char *name, u32 *v)
{
	if (!strcmp(name, "apple,ane-type") && n->has_type) { *v = n->ane_type; return 0; }
	if (!strcmp(name, "msgbox-mailbox-num") && n->has_num) { *v = n->mbox_num; return 0; }
	return -EINVAL;
}

static inline bool of_property_present(const struct device_node *n, const char *name)
{
	return (!strcmp(name, "power-domains") && n->has_power_domains) || (!strcmp(name, "iommus") && n->has_iommus);
}

#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((long)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline int of_property_match_string(const struct device_node *n, const char *prop, const char *s)
{
	int i;

	if (strcmp(prop, "reg-names"))
		return -EINVAL;
	for (i = 0; i < n->n_reg; i++)
		if (n->reg_name[i] && !strcmp(n->reg_name[i], s))
			return i;
	return -ENODATA;
}

static inline int of_address_to_resource(struct device_node *n, int idx, struct resource *r)
{
	if (idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return -EINVAL;
	r->start = n->reg_pa[idx];
	r->end = n->reg_pa[idx] + n->reg_size[idx] - 1;
	return 0;
}

struct of_device_id { const char *compatible; const void *data; };
struct driver_shim { const char *name; bool suppress_bind_attrs; const struct of_device_id *of_match_table; };
struct platform_driver {
	struct driver_shim driver;
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
};

#define module_param(n, t, p)
#define MODULE_PARM_DESC(n, d)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define module_platform_driver(x) extern char __sim_kmod_noop

#define GFP_KERNEL 0
static inline void *devm_kzalloc(struct device *d, size_t n, int f) { (void)d; (void)f; return calloc(1, n); }
static inline void platform_set_drvdata(struct platform_device *p, void *d) { p->drvdata = d; }
static inline void *platform_get_drvdata(struct platform_device *p) { return p->drvdata; }

/* ---- fake MMIO bus ---- */
#define SIM_MAX_EVENTS 8192
struct sim_event { int write; u64 pa; u32 val; };
extern struct sim_event sim_events[SIM_MAX_EVENTS];
extern int sim_n_events;
extern int sim_faults;
extern int sim_n_iomap, sim_n_iounmap;

#define SIM_NPSD 2
#define SIM_PSD_BASE 0x303a10000ULL
/* per-slot behavior */
enum { SIM_PSD_ACK = 0, SIM_PSD_NEVER = 1, SIM_PSD_WRONG_STATE = 2, SIM_PSD_IGNORE6 = 3 };
extern int sim_psd_mode[SIM_NPSD];
extern u32 sim_psd_slot[SIM_NPSD];	/* slot numbers, set by the harness */
extern u32 sim_psd_state[SIM_NPSD];	/* current state, 6 at start */
extern u32 sim_psd_valid[SIM_NPSD];
extern u32 sim_psd_initial_valid[SIM_NPSD];
/* ASC core model for stage boot */
extern u32 sim_cpu_status, sim_cpu_ctrl, sim_run_status;
extern u64 sim_rvbar_val;
extern int sim_core_running, sim_core_stuck;
extern int sim_ctrl_writes;
#define SIM_PO_CH 600	/* the alias bank channel of ANE0 is window offset 0x10c00 = channel 536 */
#define SIM_DART_PA 0x419800000ULL
#define SIM_DART_SIZE 0xc000
#define SIM_PO_Q 24
extern u32 sim_po_msg[SIM_PO_CH][SIM_PO_Q][4];
extern int sim_po_n[SIM_PO_CH], sim_po_head[SIM_PO_CH];
extern int sim_start_chan;		/* channel that receives sim_start_msgs messages when RUN is set; -1 = none */
extern int sim_start_msgs;
extern int sim_start_hello;		/* RUN queues an RTKit HELLO (min 11, max 12) and a last EPMAP (base 0, bitmap 3) on the alias channel */
extern u32 sim_po_out_off, sim_alias_credits, sim_hello_min, sim_hello_max, sim_hello_ep, sim_hello_type;
/* sim_start_hello == 3: a firmware that reacts to what the host writes to TX of the alias channel (RTKit management protocol) */
extern u32 sim_tx_w[4];
extern int sim_tx_got;
extern u64 sim_tx_msgs[32];
extern int sim_n_tx_msgs, sim_idle_sent, sim_stop_needs_idle, sim_no_shutdown_reply;
extern u32 sim_iop_ack_state;
extern u64 sim_asc_pa, sim_rvbar_pa, sim_mbox_pa;
extern u32 sim_asc_size, sim_rvbar_size, sim_mbox_size;

static inline void sim_fw_enqueue(int ch, u64 msg0)
{
	int slot = (sim_po_head[ch] + sim_po_n[ch]) % SIM_PO_Q;

	sim_po_msg[ch][slot][0] = (u32)msg0;
	sim_po_msg[ch][slot][1] = (u32)(msg0 >> 32);
	sim_po_msg[ch][slot][2] = 0;
	sim_po_msg[ch][slot][3] = 0;
	sim_po_n[ch]++;
}

/* the host finished a 16 byte TX message: let the modelled firmware react */
static inline void sim_fw_react(u64 msg0)
{
	unsigned int type = (msg0 >> 52) & 0xff;
	u64 state = msg0 & 0xffff;

	if (sim_n_tx_msgs < 32)
		sim_tx_msgs[sim_n_tx_msgs] = msg0;
	sim_n_tx_msgs++;
	if (type == 2) {
		sim_fw_enqueue(536, (8ULL << 52) | 3);
		sim_fw_enqueue(536, (8ULL << 52) | (1ULL << 51) | (1ULL << 32) | 1);
	} else if (type == 8 && (msg0 & (1ULL << 51))) {
		sim_fw_enqueue(536, (7ULL << 52) | sim_iop_ack_state);
	} else if (type == 0xb) {
		if (state == 0x20 || !sim_no_shutdown_reply)
			sim_fw_enqueue(536, (0xbULL << 52) | state);
	} else if (type == 6 && (state == 1 || state == 0) && !sim_no_shutdown_reply) {
		sim_fw_enqueue(536, (7ULL << 52) | state);
		sim_idle_sent = 1;	/* the modelled core accepts the RUN clear once the IOP acknowledged the shutdown state */
	}
}

static inline int sim_psd_of(u64 pa, u32 *off)
{
	int i;

	for (i = 0; i < SIM_NPSD; i++) {
		u64 b = SIM_PSD_BASE + ((u64)sim_psd_slot[i] << 8);

		if (pa >= b && pa < b + 0x100) {
			*off = (u32)(pa - b);
			return i;
		}
	}
	return -1;
}

static inline void sim_event(int write, u64 pa, u32 val)
{
	if (sim_n_events < SIM_MAX_EVENTS) {
		sim_events[sim_n_events].write = write;
		sim_events[sim_n_events].pa = pa;
		sim_events[sim_n_events].val = val;
	}
	sim_n_events++;
}

static inline bool sim_in(u64 pa, u64 base, u32 size) { return pa >= base && pa < base + size; }

static inline u32 bus_read(u64 pa)
{
	u32 off, v = 0xdeadbeef;
	int i = sim_psd_of(pa, &off);

	if (i >= 0) {
		if (off == 0xbc) {
			v = sim_psd_valid[i];
		} else if (off == 0x80) {
			v = sim_psd_state[i];
			sim_psd_valid[i] = 0;		/* a STATUS read clears the acknowledge */
		} else if (off == 0xa0) {
			v = 0xd1a9;
		} else {
			v = 0;
		}
	} else if (sim_in(pa, sim_asc_pa, sim_asc_size) || sim_in(pa, sim_rvbar_pa, sim_rvbar_size) ||
		   sim_in(pa, sim_mbox_pa, sim_mbox_size)) {
		/* the ASC blocks need PSD slot 0 powered (any state but 6) */
		if (sim_psd_state[0] == 6) {
			sim_faults++;
		} else if (pa == sim_asc_pa + 0x48) {
			v = sim_cpu_status;
		} else if (pa == sim_asc_pa + 0x44) {
			v = sim_cpu_ctrl;
		} else if (pa == sim_rvbar_pa) {
			v = (u32)sim_rvbar_val;
		} else if (pa == sim_rvbar_pa + 4) {
			v = (u32)(sim_rvbar_val >> 32);
		} else if (sim_in(pa, sim_mbox_pa, sim_mbox_size)) {
			u64 o = pa - sim_mbox_pa;
			u32 ch = (u32)(o / 0x80), r = (u32)(o % 0x80);

			v = 0;
			if (ch < SIM_PO_CH) {
				u32 oo = ch == 536 ? sim_po_out_off : 0;

				if (r == 0x20) {
					v = (u32)(sim_po_n[ch] > 15 ? 15 : sim_po_n[ch]);	/* a 4-bit count */
					if (ch == 536)
						v |= sim_alias_credits << 8;
				} else if (r >= oo && r < oo + 0x10 && sim_po_n[ch] > 0) {
					v = sim_po_msg[ch][sim_po_head[ch]][(r - oo) / 4];
					if (r == oo + 0xc) {		/* the last word read pops the message */
						sim_po_head[ch]++;
						sim_po_n[ch]--;
					}
				}
			}
		} else {
			v = (pa & 0xffff) | 0x10000;	/* distinguishable fake data */
		}
	} else if (sim_in(pa, SIM_DART_PA, SIM_DART_SIZE)) {
		/* the DART block is only reachable with both ANE PSD slots on */
		if (sim_psd_state[0] == 6 || sim_psd_state[1] == 6)
			sim_faults++;
		else
			v = 0xda000000u | (u32)(pa & 0xffff);
	} else {
		sim_faults++;
	}
	sim_event(0, pa, v);
	return v;
}

static inline void bus_write(u64 pa, u32 v)
{
	u32 off;
	int i = sim_psd_of(pa, &off);

	sim_event(1, pa, v);
	if (i >= 0 && off == 0x90) {
		if (sim_psd_mode[i] == SIM_PSD_ACK) {
			sim_psd_state[i] = v;
			sim_psd_valid[i] = 1;
		} else if (sim_psd_mode[i] == SIM_PSD_WRONG_STATE) {
			sim_psd_state[i] = 4;
			sim_psd_valid[i] = 1;
		} else if (sim_psd_mode[i] == SIM_PSD_IGNORE6 && v != 6) {
			sim_psd_state[i] = v;
			sim_psd_valid[i] = 1;
		}
		return;
	}
	if (i < 0 && pa == sim_asc_pa + 0x44 && sim_psd_state[0] != 6) {
		sim_ctrl_writes++;
		sim_cpu_ctrl = v;
		if (v & 0x10) {
			sim_core_running = 1;
			sim_cpu_status = sim_run_status;
			if (sim_start_chan >= 0 && sim_start_hello == 3) {
				sim_po_n[sim_start_chan] = 0;
				sim_po_head[sim_start_chan] = 0;
				sim_fw_enqueue(sim_start_chan, ((u64)sim_hello_type << 52) | ((u64)sim_hello_max << 16) | sim_hello_min);
			} else if (sim_start_chan >= 0 && sim_start_hello) {
				u64 hello = ((u64)sim_hello_type << 52) | ((u64)sim_hello_max << 16) | sim_hello_min;
				u64 epmap = (8ULL << 52) | (1ULL << 51) | 3;

				sim_po_msg[sim_start_chan][0][0] = (u32)hello; sim_po_msg[sim_start_chan][0][1] = (u32)(hello >> 32);
				sim_po_msg[sim_start_chan][0][2] = sim_hello_ep; sim_po_msg[sim_start_chan][0][3] = 0;
				sim_po_msg[sim_start_chan][1][0] = (u32)epmap; sim_po_msg[sim_start_chan][1][1] = (u32)(epmap >> 32);
				sim_po_msg[sim_start_chan][1][2] = 0; sim_po_msg[sim_start_chan][1][3] = 0;
				sim_po_n[sim_start_chan] = 2;
				sim_po_head[sim_start_chan] = 0;
			} else if (sim_start_chan >= 0) {
				int k;

				for (k = 0; k < sim_start_msgs && sim_po_n[sim_start_chan] < SIM_PO_Q; k++) {
					int slot = (sim_po_head[sim_start_chan] + sim_po_n[sim_start_chan]) % SIM_PO_Q;

					sim_po_msg[sim_start_chan][slot][0] = 0x1000 + k;
					sim_po_msg[sim_start_chan][slot][1] = 0;
					sim_po_msg[sim_start_chan][slot][2] = 0x100;
					sim_po_msg[sim_start_chan][slot][3] = 0;
					sim_po_n[sim_start_chan]++;
				}
			}
		} else if (!sim_core_stuck && !(sim_stop_needs_idle && !sim_idle_sent)) {
			sim_core_running = 0;
			sim_cpu_status = 0xa;
		}
		return;
	}
	if (i < 0 && sim_in(pa, sim_mbox_pa, sim_mbox_size) && sim_psd_state[0] != 6) {
		/* an inbox write with the ANE slot on is a legal bus access; a modelled firmware reads TX of the alias channel */
		if (sim_start_hello == 3 && pa >= sim_mbox_pa + 0x10c10 && pa < sim_mbox_pa + 0x10c20) {
			sim_tx_w[(pa - sim_mbox_pa - 0x10c10) / 4] = v;
			if (pa == sim_mbox_pa + 0x10c1c)
				sim_fw_react(((u64)sim_tx_w[1] << 32) | sim_tx_w[0]);
		}
		return;
	}
	if (i < 0)
		sim_faults++;
}

/* sim_find_node: the harness registers the node; of_iomap returns the physical address as the "mapping" */
static inline void *of_iomap(struct device_node *n, int idx)
{
	if (!n || idx < 0 || idx >= n->n_reg || !n->reg_size[idx])
		return NULL;
	sim_n_iomap++;
	return (void *)(uintptr_t)n->reg_pa[idx];
}
static inline void *ioremap(u64 pa, u32 size)
{
	(void)size;
	sim_n_iomap++;
	return (void *)(uintptr_t)pa;
}
static inline void iounmap(void *p) { (void)p; sim_n_iounmap++; }

#define readl(a) bus_read((u64)(uintptr_t)(a))
#define writel(v, a) bus_write((u64)(uintptr_t)(a), (v))

/* ---- runtime PM and iommu stubs: calls are recorded, ordered against the MMIO events by event index ---- */
extern int sim_rpm_enabled, sim_rpm_gets, sim_rpm_puts, sim_rpm_get_fail;
static inline int devm_pm_runtime_enable(struct device *d) { (void)d; sim_rpm_enabled++; return 0; }
static inline int pm_runtime_resume_and_get(struct device *d)
{
	(void)d;
	sim_event(2, 0xe0, 0);
	if (sim_rpm_get_fail)
		return -ETIMEDOUT;
	sim_rpm_gets++;
	return 0;
}
static inline int pm_runtime_put(struct device *d) { (void)d; sim_rpm_puts++; return 0; }

#define IOMMU_READ 1
#define IOMMU_WRITE 2
struct iommu_domain { int dummy; };
enum { SIM_IO_ALLOC = 1, SIM_IO_ATTACH, SIM_IO_MAP, SIM_IO_UNMAP, SIM_IO_DETACH, SIM_IO_FREE };
struct sim_iocall { int code; u64 iova, pa, size; int prot; int ev; };
extern struct sim_iocall sim_iocalls[64];
extern int sim_n_iocalls, sim_io_fail;		/* sim_io_fail: 1 alloc fails, 2 attach fails, 3 the second map fails */
extern int sim_n_dom_live;
static inline void sim_iorec(int code, u64 iova, u64 pa, u64 size, int prot)
{
	if (sim_n_iocalls < 64)
		sim_iocalls[sim_n_iocalls] = (struct sim_iocall){ code, iova, pa, size, prot, sim_n_events };
	sim_n_iocalls++;
}
static inline bool device_iommu_mapped(struct device *d) { return d->of_node && d->of_node->has_iommus; }
static inline struct iommu_domain *iommu_paging_domain_alloc(struct device *d)
{
	(void)d;
	sim_iorec(SIM_IO_ALLOC, 0, 0, 0, 0);
	if (sim_io_fail == 1)
		return ERR_PTR(-ENOMEM);
	sim_n_dom_live++;
	return calloc(1, sizeof(struct iommu_domain));
}
static inline int iommu_attach_device(struct iommu_domain *dom, struct device *d)
{
	(void)dom; (void)d;
	sim_iorec(SIM_IO_ATTACH, 0, 0, 0, 0);
	return sim_io_fail == 2 ? -EBUSY : 0;
}
static inline int iommu_map(struct iommu_domain *dom, unsigned long iova, u64 pa, size_t size, int prot, int gfp)
{
	(void)dom; (void)gfp;
	sim_iorec(SIM_IO_MAP, iova, pa, size, prot);
	if (sim_io_fail == 3) {
		int maps = 0, k;

		for (k = 0; k < sim_n_iocalls && k < 64; k++)
			maps += sim_iocalls[k].code == SIM_IO_MAP;
		if (maps == 2)
			return -ENOMEM;
	}
	return 0;
}
static inline size_t iommu_unmap(struct iommu_domain *dom, unsigned long iova, size_t size)
{
	(void)dom;
	sim_iorec(SIM_IO_UNMAP, iova, 0, size, 0);
	return size;
}
static inline void iommu_detach_device(struct iommu_domain *dom, struct device *d) { (void)dom; (void)d; sim_iorec(SIM_IO_DETACH, 0, 0, 0, 0); }
static inline void iommu_domain_free(struct iommu_domain *dom) { sim_iorec(SIM_IO_FREE, 0, 0, 0, 0); sim_n_dom_live--; free(dom); }

/* ---- virtual clock: 1 jiffy = 1 ms ---- */
extern unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline bool time_after(unsigned long a, unsigned long b) { return (long)(b - a) < 0; }
static inline void usleep_range(unsigned long lo, unsigned long hi) { sim_jiffies += (lo + hi) / 2000 + 1; }

#define ERANGE_UNUSED 0

#endif /* SIM_SHIM_H */
