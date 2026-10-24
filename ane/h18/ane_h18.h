/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h18.h: per-SoC table for the H18 ANE (T8152, Mac mini M6, board J873g).
 *
 * Sources of every number, by exact commit or file:
 * - Window addresses, sizes, mailbox number and PSD slots: the live M6 ADT dump (/arm-io/ane: reg,
 *   power-gates [30, 31], clock-gates [30, 31], service-gates, msgbox-mailbox-num 12, msgbox-using-postoffice
 *   true). Each ADT address plus 0x200000000 is the physical address, the translation the kernel tree's
 *   t8152-j873 device tree uses (linux-aurora, j873 tip 4513ee61d6a0, "translated ADT addresses").
 * - ASC wrapper register offsets (CPU_CONTROL 0x44, CPU_STATUS 0x48, ASC_STATUS 0x8048, IORVBAR at +0 of its block):
 *   UNVERIFIED by any checked-in receipt. They come from a decode of the macOS 27.0 (26A434) kernelcache
 *   AppleA7IOP-ASCWrap-v8 in the lab notebook (2026-10-19), which this repository does not carry. The board has
 *   confirmed only that reads of CPU_STATUS, ASC_STATUS and IORVBAR at these offsets return sane values with the
 *   PSD slots on (stage power) and that CPU_CONTROL reads 0 before the core runs (read_ctrl).
 * - PSD slot window layout and request protocol: drivers/pmdomain/apple/pmgr-psd.c of the same kernel tree,
 *   proven on this board for slot 71 (ATC0 USB).
 * INFERENCE, not measured: that ANE slots 30 and 31 accept state 0 (run) and what the ASC wrapper reads
 * once they are on. The ANE needs more than the PSD slots (clock-ids 329, 328 and 375 are not modeled).
 */
#ifndef __ANE_H18_H__
#define __ANE_H18_H__

#include <linux/types.h>

/* PSD slot window: 0x100 bytes at psd_base + (slot << 8). Offsets and states as in pmgr-psd.c. */
#define ANE_H18_PSD_BASE	0x303a10000ULL
#define ANE_H18_PSD_WIN_SIZE	0x100
#define ANE_H18_PSD_STATUS	0x80	/* bits 2:0 = current state; a read clears the acknowledge */
#define ANE_H18_PSD_REQUEST	0x90	/* write the requested state */
#define ANE_H18_PSD_DIAG	0xa0
#define ANE_H18_PSD_VALID	0xbc	/* bit 0 = status is valid */
#define ANE_H18_PSD_STATE_MASK	0x7u
#define ANE_H18_PSD_P0_RUN	0
#define ANE_H18_PSD_P6_RAIL	6
#define ANE_H18_NPSD		2

/* ASC wrapper (reg[0]) offsets. CPU_CONTROL (+0x44) is read only with read_ctrl=1 and is never written by this module. */
#define ANE_H18_ASC_CPU_CONTROL	0x0044
#define ANE_H18_ASC_CPU_STATUS	0x0048
#define ANE_H18_ASC_STATUS	0x8048
/* IORVBAR (reg[1]) is a 64-bit register at +0, read as two 32-bit words. */
#define ANE_H18_RVBAR_OFF	0x0
/* Postoffice (reg[2]): 0x80 bytes per channel, status word at +0x20 of the channel. */
#define ANE_H18_PO_STRIDE	0x80
#define ANE_H18_PO_STATUS	0x20
#define ANE_H18_PO_NCAND	3	/* candidate channels, see ane_h18_stage_power() */

struct ane_h18_soc {
	const char *name;
	u32 ane_type;
	u64 asc_pa;
	u32 asc_size;
	u64 rvbar_pa;
	u32 rvbar_size;
	u64 mbox_pa;
	u32 mbox_size;
	u64 engine_pa;
	u32 engine_size;
	u32 mbox_num;		/* ADT msgbox-mailbox-num */
	u32 psd_slot[ANE_H18_NPSD];
};

extern const struct ane_h18_soc ane_t8152_soc;

#endif /* __ANE_H18_H__ */
