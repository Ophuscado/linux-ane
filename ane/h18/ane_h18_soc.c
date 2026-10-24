// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h18_soc.c: the T8152 ANE0 row. Sources in ane_h18.h. ANE1 (ADT /arm-io/ane1: asc 0x481600000,
 * rvbar 0x481050000, engine 0x480000000, mailbox number 13, PSD slots 32 and 33) is not a row yet: the
 * overlay does not describe it and no stage has run on ANE0.
 */
#include "ane_h18.h"

const struct ane_h18_soc ane_t8152_soc = {
	.name = "t8152",
	.ane_type = 784,
	.asc_pa = 0x419600000ULL,
	.asc_size = 0x74000,
	.rvbar_pa = 0x419050000ULL,
	.rvbar_size = 0x4000,
	.mbox_pa = 0x316000000ULL,
	.mbox_size = 0x1c000,
	.engine_pa = 0x418000000ULL,
	.engine_size = 0x2000000,
	.mbox_num = 12,
	.rvbar_expect = 0x10000000001ULL,
	.psd_slot = { 30, 31 },
};
