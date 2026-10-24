/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_legtab.h: the slot logic of the driver's table of buffers the firmware asked for on its SHAREDMALLOC ring, factored out so the host unit test
 * (tools/test_t6021_legtab.c) compiles the rule the driver runs.
 *
 * The table has `cap` slots and a byte budget. A buffer is handed out for a MALLOC request and comes back with the firmware's FREE of its IOVA. Before, a freed
 * buffer stayed in the table until teardown, so every block the firmware freed and asked for again used a new slot and new bytes, and a few sessions of the
 * unload filled the 512 MiB budget (the next MALLOC then failed and the device was quarantined). Now a FREE releases the slot and its bytes, and a MALLOC takes
 * the first free slot below the high-water mark before it grows the table. A slot is free when its cpu pointer is NULL.
 */
#ifndef __ANE_T6021_LEGTAB_H__
#define __ANE_T6021_LEGTAB_H__

#include <linux/types.h>

struct ane_legacy_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/* The slot a new buffer takes: the first free one below `used`, else `used` itself while the table can grow, else -1. */
static inline int ane_legtab_pick(const struct ane_legacy_buffer *t, u32 used, u32 cap)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (!t[i].cpu)
			return (int)i;
	return used < cap ? (int)used : -1;
}

/* The slot that holds the buffer whose DMA address is `dma`, or -1. `keep` (may be NULL) is a buffer the driver itself still uses and that a FREE must
 * never match. */
static inline int ane_legtab_find(const struct ane_legacy_buffer *t, u32 used, u64 dma, const struct ane_legacy_buffer *keep)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (t[i].cpu && t[i].dma == dma && &t[i] != keep)
			return (int)i;
	return -1;
}

#endif /* __ANE_T6021_LEGTAB_H__ */
