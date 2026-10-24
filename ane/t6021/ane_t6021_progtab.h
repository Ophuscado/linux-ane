/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_progtab.h: the program table of the T6021 driver and the rule for when a firmware program is unloaded,
 * factored out so the host unit test (tools/test_t6021_progtab.c) compiles the code the driver runs (the pool header
 * pattern: stub kernel headers in tools/ane_pool_shim).
 *
 * A firmware program is shared by every client that loads byte-identical sections (the key is the SHA-256 digest of the
 * sections) or looks the digest up. Each client (a DRM file) holds one reference per program it loaded or looked up.
 * When the last reference goes with the file, the program is unloaded: the firmware is told to terminate its process
 * (command 0x203, programId at +8, processId at +0xc) and then to destroy the program (command 0x201, programId at +8).
 * Both commands complete with an acknowledgement; only after both did, the driver gives the program's section buffers
 * back. If either command fails or times out, the program is HELD: its buffers are never freed and its table slot is
 * never reused, because the firmware may still read them (a timed-out command also quarantines the device).
 *
 * The caller serialises every function here with the firmware command lock, the lock that already protects the table.
 */
#ifndef ANE_T6021_PROGTAB_H
#define ANE_T6021_PROGTAB_H

#include <linux/types.h>
#include <linux/string.h>

#define ANE_T6021_MAX_PROGRAMS	250	/* the firmware table holds 256; measured: the 257th load fails */
#define ANE_T6021_PROG_SECS	9	/* section slots of a program (ids 1 to 9) */
#define ANE_T6021_PROG_NOPROC	0xffffffffu

enum ane_t6021_prog_state {
	ANE_T6021_PROG_FREE = 0,
	ANE_T6021_PROG_LIVE,	/* loaded in the firmware, shared by its users */
	ANE_T6021_PROG_HELD,	/* an unload failed: kept forever, buffers never freed */
};

struct ane_t6021_prog {
	u8 digest[32];
	u32 prog_id;
	u32 proc_id;		/* ANE_T6021_PROG_NOPROC until a process exists */
	u32 users;		/* files that hold a reference */
	enum ane_t6021_prog_state state;
	void *sec[ANE_T6021_PROG_SECS];	/* the driver's reference on each section buffer, NULL when none */
};

struct ane_t6021_progtab {
	struct ane_t6021_prog progs[ANE_T6021_MAX_PROGRAMS];
	unsigned int hi;	/* slots below this index may be in use */
};

/* The programs one file holds a reference on. */
struct ane_t6021_fdprogs {
	u32 n;
	u32 ids[ANE_T6021_MAX_PROGRAMS];
};

struct ane_t6021_progtab_ops {
	/* Unload from the firmware: process first, then the program. Zero only when both were acknowledged. */
	int (*unload)(void *ctx, struct ane_t6021_prog *p);
	/* The firmware has dropped the program: give its section buffers back. */
	void (*release)(void *ctx, struct ane_t6021_prog *p);
};

/* A LIVE program with this digest, or NULL. HELD programs are never returned. */
static inline struct ane_t6021_prog *ane_t6021_progtab_find(struct ane_t6021_progtab *t, const u8 *digest)
{
	unsigned int i;

	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_LIVE && !memcmp(t->progs[i].digest, digest, sizeof(t->progs[i].digest)))
			return &t->progs[i];
	return NULL;
}

static inline struct ane_t6021_prog *ane_t6021_progtab_by_id(struct ane_t6021_progtab *t, u32 prog_id)
{
	unsigned int i;

	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_LIVE && t->progs[i].prog_id == prog_id)
			return &t->progs[i];
	return NULL;
}

/* True when a new program can be added: a free slot or room to grow. */
static inline bool ane_t6021_progtab_has_room(const struct ane_t6021_progtab *t)
{
	unsigned int i;

	if (t->hi < ANE_T6021_MAX_PROGRAMS)
		return true;
	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_FREE)
			return true;
	return false;
}

/* Add a program the firmware just loaded. The slot is a free one below hi, else a new one. Users start at zero: the caller takes the first reference. */
static inline struct ane_t6021_prog *ane_t6021_progtab_add(struct ane_t6021_progtab *t, const u8 *digest, u32 prog_id)
{
	struct ane_t6021_prog *p = NULL;
	unsigned int i;

	for (i = 0; i < t->hi; i++)
		if (t->progs[i].state == ANE_T6021_PROG_FREE) {
			p = &t->progs[i];
			break;
		}
	if (!p) {
		if (t->hi == ANE_T6021_MAX_PROGRAMS)
			return NULL;
		p = &t->progs[t->hi++];
	}
	memset(p, 0, sizeof(*p));
	memcpy(p->digest, digest, sizeof(p->digest));
	p->prog_id = prog_id;
	p->proc_id = ANE_T6021_PROG_NOPROC;
	p->state = ANE_T6021_PROG_LIVE;
	return p;
}

static inline bool ane_t6021_fdprogs_has(const struct ane_t6021_fdprogs *f, u32 prog_id)
{
	u32 i;

	for (i = 0; i < f->n; i++)
		if (f->ids[i] == prog_id)
			return true;
	return false;
}

/* The file takes a reference on the program (once per file). Returns false when the file's list is full. */
static inline bool ane_t6021_fdprogs_ref(struct ane_t6021_fdprogs *f, struct ane_t6021_prog *p)
{
	if (ane_t6021_fdprogs_has(f, p->prog_id))
		return true;
	if (f->n == ANE_T6021_MAX_PROGRAMS)
		return false;
	f->ids[f->n++] = p->prog_id;
	p->users++;
	return true;
}

/* The file is closing: drop its references. A program whose last reference this was is unloaded when unload_on_close is set; otherwise it stays
 * loaded forever (the behaviour before unload existed). Returns how many programs ended HELD. */
static inline unsigned int ane_t6021_progtab_fd_close(struct ane_t6021_progtab *t, struct ane_t6021_fdprogs *f, bool unload_on_close,
						      const struct ane_t6021_progtab_ops *ops, void *ctx)
{
	unsigned int held = 0;
	u32 i;

	for (i = 0; i < f->n; i++) {
		struct ane_t6021_prog *p = ane_t6021_progtab_by_id(t, f->ids[i]);

		if (!p || !p->users)
			continue;
		p->users--;
		if (p->users || !unload_on_close)
			continue;
		if (ops->unload(ctx, p)) {
			p->state = ANE_T6021_PROG_HELD;
			held++;
			continue;
		}
		ops->release(ctx, p);
		memset(p, 0, sizeof(*p));	/* state FREE */
	}
	f->n = 0;
	return held;
}

#endif /* ANE_T6021_PROGTAB_H */
