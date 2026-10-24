// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_legtab.c: host unit test for ane/t6021/ane_t6021_legtab.h, the slot logic of the table of firmware MALLOC buffers. The M2 soak of the program unload
// failed on the fourth session because freed buffers kept their slot and bytes. Cases: a new buffer takes the first free slot below the high-water mark, the
// table grows only when no slot is free, a full table returns -1, a FREE finds its buffer by DMA address and never the buffer marked `keep`, an empty or
// freed slot never matches, and a free-then-malloc loop of many rounds never grows the table.
*/

#include <stdio.h>
#include <string.h>

#include "../ane/t6021/ane_t6021_legtab.h"

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
	struct ane_legacy_buffer t[8];
	char mem[8];
	u32 used = 0;
	int i, k, round;

	memset(t, 0, sizeof(t));
	check(ane_legtab_pick(t, 0, 8) == 0, "an empty table hands out slot 0");
	for (i = 0; i < 3; i++) {
		k = ane_legtab_pick(t, used, 8);
		check(k == i, "the table grows one slot at a time");
		t[k] = (struct ane_legacy_buffer){ &mem[i], 0x1000 + 0x1000 * i, 0x1000 };
		if ((u32)k == used)
			used++;
	}
	check(ane_legtab_find(t, used, 0x2000, NULL) == 1, "a FREE finds the buffer by DMA address");
	check(ane_legtab_find(t, used, 0x2000, &t[1]) == -1, "the kept buffer is never matched");
	check(ane_legtab_find(t, used, 0x9000, NULL) == -1, "an unknown address matches nothing");
	/* a slot with a stale DMA address but no cpu pointer is free and never matches */
	t[2].cpu = NULL;
	check(ane_legtab_find(t, used, 0x3000, NULL) == -1, "a slot without a cpu pointer never matches, whatever its stale address");
	t[2].cpu = &mem[2];
	/* free slot 1: it is the next one handed out, the table does not grow */
	memset(&t[1], 0, sizeof(t[1]));
	check(ane_legtab_find(t, used, 0x2000, NULL) == -1, "a freed slot no longer matches");
	check(ane_legtab_pick(t, used, 8) == 1, "the first free slot below the high-water mark is reused");
	/* fill up to the cap, then no slot */
	for (; used < 8;) {
		k = ane_legtab_pick(t, used, 8);
		t[k] = (struct ane_legacy_buffer){ &mem[k], 0x1000 * (k + 1), 0x1000 };
		if ((u32)k == used)
			used++;
	}
	check(ane_legtab_pick(t, used, 8) == -1, "a full table returns -1");
	/* many rounds of free and malloc of the same slot count never grow the table */
	for (round = 0; round < 1000; round++) {
		memset(&t[round % 8], 0, sizeof(t[0]));
		k = ane_legtab_pick(t, used, 8);
		check(k == round % 8, "free then malloc reuses the freed slot");
		t[k] = (struct ane_legacy_buffer){ &mem[k], 0x100000 + round, 0x1000 };
	}
	check(used == 8, "the high-water mark stays at 8 over 1000 rounds");
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_t6021_legtab: all checks passed\n");
	return 0;
}
