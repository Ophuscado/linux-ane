// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_mring.c: host unit test for ane/t6021/ane_t6021_mring.h, the rule that tells a MALLOC, a FREE and an answered entry of the firmware's
// shared-memory ring apart. The M2 run of the program unload failed because a FREE (header = the IOVA, bit 0 clear) was taken for a malformed
// MALLOC. Cases: the old MALLOC shapes keep working, a FREE is a FREE, answered and idle slots are left alone, and shapes no request has are BAD.
*/

#include <stdio.h>

#include "../ane/t6021/ane_t6021_mring.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

int main(void)
{
	/* MALLOC: header 0, size, mode 0 or 1 */
	check(ane_t6021_mring_classify(0, 0x4000, 0) == ANE_T6021_MRING_MALLOC, "malloc mode 0");
	check(ane_t6021_mring_classify(0, 0x200000, 1) == ANE_T6021_MRING_MALLOC, "malloc mode 1");
	check(ane_t6021_mring_classify(0, 0x4000, 0x100000000ULL) == ANE_T6021_MRING_BAD, "a mode above u32 is bad, as before");
	/* an unwritten slot */
	check(ane_t6021_mring_classify(0, 0, 0) == ANE_T6021_MRING_IDLE, "header 0 and size 0 is idle");
	/* FREE or UNREMAP: the header is the IOVA, bit 0 clear, body zero */
	check(ane_t6021_mring_classify(0x1000023c000ULL, 0, 0) == ANE_T6021_MRING_FREE, "free of an iova");
	check(ane_t6021_mring_classify(0x4000, 0, 0) == ANE_T6021_MRING_FREE, "free of the lowest 16 KiB page");
	check(ane_t6021_mring_classify(0x1000023c000ULL, 0x4000, 0) == ANE_T6021_MRING_BAD, "an iova header with a size is bad");
	check(ane_t6021_mring_classify(0x1000023c000ULL, 0, 1) == ANE_T6021_MRING_BAD, "an iova header with a mode is bad");
	/* answered slots, whatever the rest holds */
	check(ane_t6021_mring_classify(1, 0, 0) == ANE_T6021_MRING_ANSWERED, "the free answer header 1");
	check(ane_t6021_mring_classify(0x1000023c001ULL, 0x4000, 7) == ANE_T6021_MRING_ANSWERED, "a malloc answer (dma | 1)");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_t6021_mring: all checks passed\n");
	return 0;
}
