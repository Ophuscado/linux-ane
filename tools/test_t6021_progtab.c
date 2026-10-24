// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_progtab.c: host unit test for the program table and unload rule in ane/t6021/ane_t6021_progtab.h (the header the
// driver compiles, over the stub kernel headers in ane_pool_shim/linux). Cases:
//   - find and lookup by id see LIVE programs only
//   - two files share one program: it unloads once, when the last file closes, process before program, then the buffers go back
//   - a program without a process skips the process termination (the op sees the no-process id)
//   - a failed unload leaves the program HELD: never found, never released, its slot never reused, other programs unaffected
//   - unload_on_close off keeps the old behaviour: references drop, the program stays loaded
//   - slot reuse after an unload, a full table, a full per-file list, one reference per file and program
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ane/t6021/ane_t6021_progtab.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

/* ops: record the order of calls and let a case choose which program id fails to unload */
static char calls[256][16];
static u32 call_prog[256];
static u32 call_proc[256];
static int ncalls;
static u32 fail_prog = 0xdeadbeefu;

static int op_unload(void *ctx, struct ane_t6021_prog *p)
{
	(void)ctx;
	snprintf(calls[ncalls], sizeof(calls[0]), "unload");
	call_prog[ncalls] = p->prog_id;
	call_proc[ncalls++] = p->proc_id;
	return p->prog_id == fail_prog ? -110 : 0;
}

static void op_release(void *ctx, struct ane_t6021_prog *p)
{
	(void)ctx;
	snprintf(calls[ncalls], sizeof(calls[0]), "release");
	call_prog[ncalls] = p->prog_id;
	call_proc[ncalls++] = p->proc_id;
}

static const struct ane_t6021_progtab_ops ops = { .unload = op_unload, .release = op_release };

static void reset_calls(void)
{
	ncalls = 0;
	fail_prog = 0xdeadbeefu;
}

static void dg(u8 *d, int n)
{
	memset(d, 0, 32);
	d[0] = (u8)n;
	d[1] = (u8)(n >> 8);
}

static struct ane_t6021_progtab *newtab(void)
{
	return calloc(1, sizeof(struct ane_t6021_progtab));
}

static struct ane_t6021_fdprogs *newfd(void)
{
	return calloc(1, sizeof(struct ane_t6021_fdprogs));
}

int main(void)
{
	struct ane_t6021_progtab *t = newtab();
	struct ane_t6021_fdprogs *a = newfd(), *b = newfd();
	struct ane_t6021_prog *p;
	u8 d1[32], d2[32], d3[32];
	int i;

	dg(d1, 1);
	dg(d2, 2);
	dg(d3, 3);

	/* find and by_id */
	check(!ane_t6021_progtab_find(t, d1), "empty table: no digest found");
	p = ane_t6021_progtab_add(t, d1, 7);
	check(p && p->state == ANE_T6021_PROG_LIVE && p->users == 0 && p->proc_id == ANE_T6021_PROG_NOPROC, "add: LIVE, no users, no process");
	check(ane_t6021_progtab_find(t, d1) == p && ane_t6021_progtab_by_id(t, 7) == p, "find and by_id return the program");
	check(!ane_t6021_progtab_find(t, d2) && !ane_t6021_progtab_by_id(t, 8), "other digest and id are not found");

	/* two files share one program: unload once, at the last close, process then program, then release */
	check(ane_t6021_fdprogs_ref(a, p) && p->users == 1, "file A takes a reference");
	check(ane_t6021_fdprogs_ref(a, p) && p->users == 1 && a->n == 1, "a second reference by the same file counts once");
	check(ane_t6021_fdprogs_ref(b, p) && p->users == 2, "file B (a lookup) takes a reference");
	p->proc_id = 3;
	reset_calls();
	check(ane_t6021_progtab_fd_close(t, a, true, &ops, NULL) == 0 && ncalls == 0 && p->users == 1 && p->state == ANE_T6021_PROG_LIVE, "closing A does not unload: B still holds it");
	check(a->n == 0, "A's list is empty after close");
	check(ane_t6021_progtab_fd_close(t, b, true, &ops, NULL) == 0, "closing B unloads without a hold");
	check(ncalls == 2 && !strcmp(calls[0], "unload") && !strcmp(calls[1], "release") && call_prog[0] == 7 && call_proc[0] == 3 && call_prog[1] == 7, "one unload with the process id, then one release, in that order");
	check(p->state == ANE_T6021_PROG_FREE && !ane_t6021_progtab_find(t, d1) && !ane_t6021_progtab_by_id(t, 7), "the slot is free and the program is gone");

	/* no process: the op sees the no-process id */
	p = ane_t6021_progtab_add(t, d1, 9);
	check(p == &t->progs[0], "the freed slot is reused");
	ane_t6021_fdprogs_ref(a, p);
	reset_calls();
	ane_t6021_progtab_fd_close(t, a, true, &ops, NULL);
	check(ncalls == 2 && call_proc[0] == ANE_T6021_PROG_NOPROC, "a program without a process is unloaded with the no-process id");

	/* a failed unload: HELD, never released, never found, slot never reused */
	p = ane_t6021_progtab_add(t, d1, 11);
	ane_t6021_fdprogs_ref(a, p);
	reset_calls();
	fail_prog = 11;
	check(ane_t6021_progtab_fd_close(t, a, true, &ops, NULL) == 1, "a failed unload is counted as held");
	check(ncalls == 1 && !strcmp(calls[0], "unload"), "no release after a failed unload");
	check(p->state == ANE_T6021_PROG_HELD && !ane_t6021_progtab_find(t, d1) && !ane_t6021_progtab_by_id(t, 11), "the held program is neither found nor returned by id");
	{
		struct ane_t6021_prog *q = ane_t6021_progtab_add(t, d1, 12);

		check(q && q != p && p->state == ANE_T6021_PROG_HELD, "a new program never takes the held slot");
	}
	reset_calls();
	/* partial failure across three programs of one file: the middle one is held, the others are released */
	memset(t, 0, sizeof(*t));
	{
		struct ane_t6021_prog *x = ane_t6021_progtab_add(t, d1, 21), *y = ane_t6021_progtab_add(t, d2, 22), *z = ane_t6021_progtab_add(t, d3, 23);

		ane_t6021_fdprogs_ref(a, x);
		ane_t6021_fdprogs_ref(a, y);
		ane_t6021_fdprogs_ref(a, z);
		fail_prog = 22;
		check(ane_t6021_progtab_fd_close(t, a, true, &ops, NULL) == 1, "one of three programs held");
		check(x->state == ANE_T6021_PROG_FREE && y->state == ANE_T6021_PROG_HELD && z->state == ANE_T6021_PROG_FREE, "the other two are freed");
		check(ncalls == 5, "five calls: unload and release for 21, unload for 22, unload and release for 23");
	}

	/* unload_on_close off */
	memset(t, 0, sizeof(*t));
	reset_calls();
	p = ane_t6021_progtab_add(t, d1, 31);
	ane_t6021_fdprogs_ref(a, p);
	check(ane_t6021_progtab_fd_close(t, a, false, &ops, NULL) == 0 && ncalls == 0, "unload_on_close off: no firmware call");
	check(p->state == ANE_T6021_PROG_LIVE && p->users == 0 && ane_t6021_progtab_find(t, d1) == p, "unload_on_close off: the program stays loaded and findable with no users");
	check(ane_t6021_fdprogs_ref(b, p) && p->users == 1, "a later file can take a reference on it");
	ane_t6021_progtab_fd_close(t, b, true, &ops, NULL);
	check(ncalls == 2 && p->state == ANE_T6021_PROG_FREE, "with unload_on_close on again, the last close unloads it");

	/* a file without a reference does not hold the program */
	memset(t, 0, sizeof(*t));
	p = ane_t6021_progtab_add(t, d1, 41);
	check(!ane_t6021_fdprogs_has(a, 41), "a file that never referenced a program does not have it");
	reset_calls();
	ane_t6021_progtab_fd_close(t, a, true, &ops, NULL);
	check(ncalls == 0 && p->state == ANE_T6021_PROG_LIVE, "closing a file without references unloads nothing");

	/* full table: has_room, add, and a freed slot opens room again */
	memset(t, 0, sizeof(*t));
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++) {
		u8 d[32];

		dg(d, 1000 + i);
		check(ane_t6021_progtab_has_room(t), "room while the table fills");
		check(ane_t6021_progtab_add(t, d, 100 + i) != NULL, "add while filling");
	}
	check(!ane_t6021_progtab_has_room(t), "no room when 250 programs are loaded");
	{
		u8 d[32];

		dg(d, 5000);
		check(ane_t6021_progtab_add(t, d, 999) == NULL, "add on a full table fails");
		ane_t6021_fdprogs_ref(a, &t->progs[17]);
		reset_calls();
		ane_t6021_progtab_fd_close(t, a, true, &ops, NULL);
		check(ane_t6021_progtab_has_room(t), "an unload opens a slot");
		check(ane_t6021_progtab_add(t, d, 999) == &t->progs[17], "the new program reuses that slot");
	}

	/* the per-file list is bounded */
	memset(a, 0, sizeof(*a));
	for (i = 0; i < ANE_T6021_MAX_PROGRAMS; i++)
		check(ane_t6021_fdprogs_ref(a, &t->progs[i]), "a file can hold 250 references");
	{
		struct ane_t6021_prog extra = { .prog_id = 4242, .state = ANE_T6021_PROG_LIVE };

		check(!ane_t6021_fdprogs_ref(a, &extra) && extra.users == 0, "the 251st reference is refused without a count");
	}

	free(t);
	free(a);
	free(b);
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_t6021_progtab: all checks passed\n");
	return 0;
}
