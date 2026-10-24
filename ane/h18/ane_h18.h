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
 * - RTKit management protocol and power state values (drivers/soc/apple/rtkit.c) and the T8152 counted FIFO of a Postoffice channel (mailbox.c):
 *   linux-aurora commit dc7b5b9c539b85bdf2557f2a0eb44223a4219de9. Gen3 DART layout: drivers/iommu/apple-dart.c of the kernel tree this Image is
 *   built from, which no public commit carries yet; its sha256 is 2904d16b83d0ae065586e16157cc5b54e7233c174c51dcda1c70c2b02a637621.
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
/* CPU_CONTROL bit 4 starts the core (the AppleASCWrapV8 decode in the lab notebook; m1n1's R_CPU_CONTROL.RUN is the same bit).
 * CPU_STATUS: bit 0 RUNNING, bit 1 STOPPED (m1n1 R_CPU_STATUS). The board read 0xa before the core ever ran. */
#define ANE_H18_CPU_RUN		(1u << 4)
#define ANE_H18_ST_RUNNING	(1u << 0)
#define ANE_H18_ST_STOPPED	(1u << 1)
/* IORVBAR (reg[1]) is a 64-bit register at +0, read as two 32-bit words. */
#define ANE_H18_RVBAR_OFF	0x0
/* Postoffice (reg[2]): 0x80 bytes per channel, status word at +0x20 of the channel. */
#define ANE_H18_PO_STRIDE	0x80
#define ANE_H18_PO_STATUS	0x20
#define ANE_H18_PO_OUTBOX	0x00	/* 16 bytes, four 32-bit words; the last word read pops the message (m1n1 OUTBOX1) */
#define ANE_H18_PO_MAXMSG	8	/* at most this many messages are read per run */
#define ANE_H18_PO_NCAND	3	/* candidate channels, see ane_h18_stage_power() */
/* The alias bank: the same Postoffice window at +0x10000, channel 2 * msgbox-mailbox-num. For this ANE the ADT has msgbox-using-postoffice and no
 * msgbox-using-alias, which the AppleASCWrapV8 constructor turns into base + 0x10000 and channel 2 * num (0x316010c00 for ANE0). */
#define ANE_H18_PO_ALIAS_OFF	0x10000
/* ANE DART (dart,gen3, instance 0): read only liveness words, see ane_h18_read_dart_words(). */
#define ANE_H18_DART_NREAD	12
/* Postoffice channel layout used for the RTKit exchange, as drivers/soc/apple/mailbox.c has it for the T8152 counted FIFO: RX (the outbox, firmware to host)
 * at +0x00, TX (the inbox) at +0x10, status at +0x20 with the RX occupancy in bits 3:0 and the free TX slots in bits 11:8. A message is 16 bytes and is
 * moved with one paired access (ldp from RX, stp to TX); four separate 32-bit accesses are not what that driver does on this SoC. */
#define ANE_H18_PO_RX		0x00
#define ANE_H18_PO_TX		0x10
#define ANE_H18_PO_MAXSEND	12	/* at most this many management messages sent per run */
/* RTKit management protocol (the Linux apple-rtkit dialect): message type in bits 59:52 of the first 64-bit word, endpoint in the low byte of the second. */
#define ANE_H18_RTK_TYPE_SHIFT	52
#define ANE_H18_RTK_HELLO	1
#define ANE_H18_RTK_HELLO_REPLY	2
#define ANE_H18_RTK_STARTEP	5
#define ANE_H18_RTK_IOP_PWR_ACK	7
#define ANE_H18_RTK_EPMAP	8	/* the reply has the same type */
#define ANE_H18_RTK_AP_PWR	0xb	/* SET_AP_PWR_STATE */
#define ANE_H18_RTK_PWR_ON	0x20
#define ANE_H18_RTK_PWR_OFF	0x00	/* rtkit_stop=2: OFF on both sides, cannot be restarted (apple_rtkit_poweroff) */
#define ANE_H18_RTK_PWR_QUIESCED	0x10	/* the AP state of apple_rtkit_shutdown */
#define ANE_H18_RTK_PWR_SLEEP	0x01	/* the IOP state of apple_rtkit_shutdown: sleeping, can be restarted (OFF cannot) */
#define ANE_H18_RTK_IOP_IDLE	0xc
#define ANE_H18_RTK_SET_IOP_PWR	6
#define ANE_H18_RTK_STOP_WAIT_MS	600	/* how long the shutdown handshake waits for the IOP acknowledgement */
#define ANE_H18_RTK_APP_EP	0x20	/* endpoints from here are application endpoints, started only once the coprocessor runs */
#define ANE_H18_RTK_VERSION	12	/* the version macOS and Linux answer with on this firmware family */

/* A firmware segment the boot loader left in DRAM (ADT /arm-io/ane segment-ranges: physical address, size, and the IOVA the firmware is linked at in the
 * ANE DART's address space). The DART maps it before the core runs. TEXT is mapped read only, DATA read and write. */
struct ane_h18_fw_seg {
	u64 pa;
	u64 size;
	u64 iova;
	bool write;
};

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
	u64 dart_pa;		/* ANE DART instance 0 (ADT /arm-io/dart-ane reg[0]) */
	u32 dart_size;
	u32 mbox_num;		/* ADT msgbox-mailbox-num */
	u64 rvbar_expect;	/* IORVBAR as iBoot leaves it: entry 0x10000000000 (the segment-ranges remap) | valid bit. Measured on the board. */
	u32 psd_slot[ANE_H18_NPSD];
	struct ane_h18_fw_seg fw_seg[2];
};

extern const struct ane_h18_soc ane_t8152_soc;

#endif /* __ANE_H18_H__ */
