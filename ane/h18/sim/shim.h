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
extern u64 sim_asc_pa, sim_rvbar_pa, sim_mbox_pa;
extern u32 sim_asc_size, sim_rvbar_size, sim_mbox_size;

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
		/* the ASC blocks need PSD slot 0 running (the first requested slot) */
		if (sim_psd_state[0] != 0) {
			sim_faults++;
		} else {
			v = (pa & 0xffff) | 0x10000;	/* distinguishable fake data */
		}
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
static inline void iounmap(void *p) { (void)p; sim_n_iounmap++; }

#define readl(a) bus_read((u64)(uintptr_t)(a))
#define writel(v, a) bus_write((u64)(uintptr_t)(a), (v))

/* ---- virtual clock: 1 jiffy = 1 ms ---- */
extern unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline bool time_after(unsigned long a, unsigned long b) { return (long)(b - a) < 0; }
static inline void usleep_range(unsigned long lo, unsigned long hi) { sim_jiffies += (lo + hi) / 2000 + 1; }

#define ERANGE_UNUSED 0

#endif /* SIM_SHIM_H */
