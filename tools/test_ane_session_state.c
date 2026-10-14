// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_ane_session_state.c: host test of the ane-session STATE and RESET
// commands. The real ane_session_main runs over a command file against a fake
// DRM node whose EXEC is a deterministic fake device: every output byte of the
// tile (padding included, so it is nonzero garbage) is a function of all input
// BO bytes (padding included). The test then replays the same program on the
// host with the semantics the decoder has today (the input state tile is zeros
// plus the logical elements of the previous output) and compares every reply
// byte for byte.
//
// The fixture is prog000 (a lane input t1, a conv state input t5 of shape
// [1,1,6144,3] with row stride 64 B, so 90.6 percent of its tile is padding,
// and seven outputs, the last t38 the conv state out).
//
// Checks: the payload and reply shrink by exactly the state tiles; chained
// states equal the host-chained model over 5 steps; the device's output padding
// never reaches the next input; two names on one cached program stay isolated;
// RESET of one name and of all names; FREE then LOAD starts from zero;
// refusals (unloaded name, bad index, tile mismatch, run past the tile, double
// registration) leave the session failed and exit 1.
// Run with no argument: all checks. Mutants are compiled by
// test_ane_session_state_mutants.py and must each fail this test.
*/

#define _GNU_SOURCE
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <ane_accel.h>

#define ANE_SESSION_TEST
#include "ane-session.c"

#define FAKE_NODE "/dev/accel/accel0"
#define MAX_BO 4096

static int fake_fds[64];
static int n_fake_fds;
static int failures;
static void *bo_map[MAX_BO];
static int fail_exec_at = -1;	/* fail the EXEC with this 0-based index, -1 never */
static int exec_count;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

static int is_fake(int fd)
{
	int i;

	for (i = 0; i < n_fake_fds; i++) {
		if (fake_fds[i] == fd) {
			return 1;
		}
	}
	return 0;
}

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off);

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (!strcmp(path, FAKE_NODE)) {
		int fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);

		if (fd >= 0 && n_fake_fds < 64) {
			fake_fds[n_fake_fds++] = fd;
		}
		return fd;
	}
	if (!strncmp(path, "/dev/accel/", strlen("/dev/accel/"))) {
		errno = ENOENT;
		return -1;
	}
	return __real_open(path, flags, mode);
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off)
{
	void *m;

	if (!is_fake(fd)) {
		return __real_mmap(addr, len, prot, flags, fd, off);
	}
	m = __real_mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m != MAP_FAILED && (uint64_t)off >> 32 < MAX_BO) {
		bo_map[(uint64_t)off >> 32] = m;
	}
	return m;
}

/* ---- the fake device: out byte = f(all input bytes), padding included ---- */

static uint64_t fnv(uint64_t h, const uint8_t *b, uint64_t n)
{
	uint64_t i;

	for (i = 0; i < n; i++) {
		h = (h ^ b[i]) * 0x100000001b3ull;
	}
	return h;
}

/* in[]/out[] are tile buffers; the same function runs in the fake device and
 * in the host model. */
static void device_f(const uint8_t *const *in, const uint64_t *in_n,
		     unsigned nin, uint8_t *const *out, const uint64_t *out_n,
		     unsigned nout)
{
	uint64_t h = 0xcbf29ce484222325ull;
	unsigned i;
	uint64_t b;

	for (i = 0; i < nin; i++) {
		h = fnv(h, in[i], in_n[i]);
	}
	for (i = 0; i < nout; i++) {
		for (b = 0; b < out_n[i]; b++) {
			out[i][b] = (uint8_t)((h >> ((b * 7) & 31)) ^
					      (b * 131) ^ (i * 17) ^ 0xa5);
			if (!(b & 7)) {
				h = h * 6364136223846793005ull + 1442695040888963407ull;
			}
		}
	}
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
	if (!is_fake(fd)) {
		return __real_ioctl(fd, request, arg);
	}
	switch (request) {
	case DRM_IOCTL_VERSION: {
		drm_version_t *v = arg;

		if (v->name && v->name_len >= 3) {
			memcpy(v->name, "ane", 3);
		}
		v->name_len = 3;
		v->date_len = 0;
		v->desc_len = 0;
		v->version_major = ANE_ABI_M2_MAJOR;
		v->version_minor = 0;
		v->version_patchlevel = 0;
		return 0;
	}
	case DRM_IOCTL_ANE_BO_INIT: {
		struct drm_ane_bo_init *a = arg;
		static uint32_t next_handle;

		a->handle = ++next_handle;
		a->offset = (uint64_t)next_handle << 32;
		return 0;
	}
	case DRM_IOCTL_ANE_BO_FREE:
		return 0;
	case DRM_IOCTL_ANE_PROG_LOAD:
		((struct drm_ane_prog_load *)arg)->prog_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_PROC_CREATE:
		((struct drm_ane_proc_create *)arg)->proc_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_EXEC: {
		struct drm_ane_exec *a = arg;
		struct drm_ane_exec_io *io = (void *)(uintptr_t)a->io_ptr;
		const uint8_t *in[16];
		uint8_t *out[16];
		uint64_t in_n[16], out_n[16];
		unsigned nin = 0, nout = 0, i;

		if (exec_count++ == fail_exec_at) {
			errno = EIO;
			return -1;
		}
		for (i = 0; i < a->count; i++) {
			uint8_t *m = bo_map[io[i].bo_handle];

			if (io[i].type == 0) {
				in[nin] = m;
				in_n[nin++] = io[i].size;
			} else {
				out[nout] = m;
				out_n[nout++] = io[i].size;
			}
		}
		device_f(in, in_n, nin, out, out_n, nout);
		return 0;
	}
	case DRM_IOCTL_ANE_PROG_LOOKUP:
		((struct drm_ane_prog_lookup *)arg)->found_out = 0;
		((struct drm_ane_prog_lookup *)arg)->prog_id_out = 0;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	ret = fake_ioctl(fd, request, arg);
	if (ret < 0 && errno == 0) {
		errno = EINVAL;
	}
	return ret;
}

/* ---- command stream and session run ---- */

struct sbuf {
	uint8_t *p;
	size_t n, cap;
};

static void sput(struct sbuf *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void sprintf_buf(struct sbuf *b, const char *fmt, ...)
{
	char tmp[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	sput(b, tmp, (size_t)n);
}

static int run_session_bin(const struct sbuf *cmds, struct sbuf *reply)
{
	char cmdpath[] = "/tmp/test_state_cmds-XXXXXX";
	char outpath[] = "/tmp/test_state_out-XXXXXX";
	char lockpath[] = "/tmp/test_state_lock-XXXXXX";
	char *argv[3] = { (char *)"ane-session", (char *)"--lock", lockpath };
	int save_in = dup(0), save_out = dup(1), rc;
	int fd;
	FILE *fp;
	long len;

	fd = mkstemp(cmdpath);
	close(fd);
	fd = mkstemp(outpath);
	close(fd);
	fd = mkstemp(lockpath);
	close(fd);
	fp = fopen(cmdpath, "wb");
	fwrite(cmds->p, 1, cmds->n, fp);
	fclose(fp);
	if (!freopen(cmdpath, "rb", stdin) || !freopen(outpath, "wb", stdout)) {
		return -1;
	}
	rc = ane_session_main(3, argv);
	fflush(stdout);
	dup2(save_out, 1);
	dup2(save_in, 0);
	close(save_out);
	close(save_in);
	clearerr(stdin);
	fp = fopen(outpath, "rb");
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	reply->p = malloc((size_t)len + 1);
	reply->n = (size_t)len;
	if (fread(reply->p, 1, (size_t)len, fp) != (size_t)len) {
		reply->n = 0;
	}
	reply->p[reply->n] = 0;
	fclose(fp);
	unlink(cmdpath);
	unlink(outpath);
	unlink(lockpath);
	return rc;
}

/* ---- the host model (today's semantics) ---- */

#define LANE 16384u
#define CONV 393216u
#define NLANE_OUT 6u
#define ROWS 6144u
#define ROWB 6u
#define PITCH 64u

struct model {
	uint8_t state[CONV];	/* input state tile: zeros plus the logical elements */
};

static void model_step(struct model *m, const uint8_t lane[LANE],
		       uint8_t outs[NLANE_OUT][LANE])
{
	static uint8_t o[NLANE_OUT][LANE], conv_out[CONV];
	const uint8_t *in[2] = { lane, m->state };
	uint64_t in_n[2] = { LANE, CONV };
	uint8_t *out[7];
	uint64_t out_n[7];
	unsigned i, r;

	for (i = 0; i < NLANE_OUT; i++) {
		out[i] = o[i];
		out_n[i] = LANE;
	}
	out[6] = conv_out;
	out_n[6] = CONV;
	device_f(in, in_n, 2, out, out_n, 7);
	for (i = 0; i < NLANE_OUT; i++) {
		memcpy(outs[i], o[i], LANE);
	}
	memset(m->state, 0, CONV);
	for (r = 0; r < ROWS; r++) {
		memcpy(m->state + (uint64_t)r * PITCH, conv_out + (uint64_t)r * PITCH, ROWB);
	}
}

static void make_lane(uint8_t lane[LANE], unsigned seed)
{
	unsigned i;

	for (i = 0; i < LANE; i++) {
		lane[i] = (uint8_t)((i * 31 + seed * 97) ^ (seed << 3));
	}
}

static void add_state_cmd(struct sbuf *c, const char *name)
{
	uint64_t offs[ROWS];
	unsigned r;

	for (r = 0; r < ROWS; r++) {
		offs[r] = (uint64_t)r * PITCH;
	}
	sprintf_buf(c, "STATE %s 1 6 %u %u\n", name, ROWB, ROWS);
	sput(c, offs, sizeof(offs));
}

static void add_call(struct sbuf *c, const char *name, const uint8_t *lane)
{
	sprintf_buf(c, "CALL %s\n", name);
	sput(c, lane, LANE);
}

/* Parse "OK CALL name us\n" + NLANE_OUT*LANE bytes at *pos; 0 ok. */
static int take_call(const struct sbuf *r, size_t *pos, uint8_t outs[NLANE_OUT][LANE])
{
	const char *line = (const char *)r->p + *pos;
	const char *nl = memchr(line, '\n', r->n - *pos);
	size_t used;
	unsigned i;

	if (!nl || strncmp(line, "OK CALL ", 8)) {
		return -1;
	}
	used = (size_t)(nl - line) + 1;
	if (r->n - *pos - used < (size_t)NLANE_OUT * LANE) {
		return -2;
	}
	for (i = 0; i < NLANE_OUT; i++) {
		memcpy(outs[i], r->p + *pos + used + (size_t)i * LANE, LANE);
	}
	*pos += used + (size_t)NLANE_OUT * LANE;
	return 0;
}

static int take_ok(const struct sbuf *r, size_t *pos, const char *prefix)
{
	const char *line = (const char *)r->p + *pos;
	const char *nl = memchr(line, '\n', r->n - *pos);

	if (!nl || strncmp(line, prefix, strlen(prefix))) {
		return -1;
	}
	*pos += (size_t)(nl - line) + 1;
	return 0;
}

static const char *ANEC = "../fixtures/h14-anec/add/program-0.anec";
static const char *PORTS = "../tests/fixtures/prog000-ports.json";

static void load(struct sbuf *c, const char *name)
{
	sprintf_buf(c, "LOAD %s %s %s\n", name, ANEC, PORTS);
}

static void scenario_chain(void)
{
	struct sbuf c = { 0 }, r = { 0 };
	struct model m;
	uint8_t lane[LANE], want[NLANE_OUT][LANE], got[NLANE_OUT][LANE];
	size_t pos = 0;
	unsigned s;
	int rc;

	memset(&m, 0, sizeof(m));
	load(&c, "a");
	add_state_cmd(&c, "a");
	for (s = 1; s <= 5; s++) {
		make_lane(lane, s);
		add_call(&c, "a", lane);
	}
	sprintf_buf(&c, "RESET\n");	/* the start of a second prompt */
	for (s = 21; s <= 23; s++) {
		make_lane(lane, s);
		add_call(&c, "a", lane);
	}
	sprintf_buf(&c, "QUIT\n");
	rc = run_session_bin(&c, &r);
	check(rc == 0, "chain: session exit 0");
	check(!take_ok(&r, &pos, "OK LOAD a 2 7"), "chain: LOAD reports 2 inputs and 7 outputs");
	check(!take_ok(&r, &pos, "OK STATE a"), "chain: STATE accepted");
	for (s = 1; s <= 5; s++) {
		make_lane(lane, s);
		model_step(&m, lane, want);
		if (take_call(&r, &pos, got)) {
			check(0, "chain: a CALL reply of exactly 6 lane tiles");
			break;
		}
		check(!memcmp(got, want, sizeof(got)), "chain: step output equals the host-chained model");
	}
	check(!take_ok(&r, &pos, "OK RESET"), "chain: RESET answered between prompts");
	memset(&m, 0, sizeof(m));
	for (s = 21; s <= 23; s++) {
		make_lane(lane, s);
		model_step(&m, lane, want);
		if (take_call(&r, &pos, got)) {
			check(0, "chain: second-prompt CALL reply of exactly 6 lane tiles");
			break;
		}
		check(!memcmp(got, want, sizeof(got)), "chain: second prompt starts from a zero state and equals the model");
	}
	check(!take_ok(&r, &pos, "OK QUIT"), "chain: OK QUIT directly after the last reply (no state bytes on the wire)");
	free(c.p);
	free(r.p);
}

static void scenario_isolation_and_reset(void)
{
	struct sbuf c = { 0 }, r = { 0 };
	struct model ma, mb;
	uint8_t lane[LANE], want[NLANE_OUT][LANE], got[NLANE_OUT][LANE];
	size_t pos = 0;
	unsigned s, step;
	int rc, bad = 0;

	memset(&ma, 0, sizeof(ma));
	memset(&mb, 0, sizeof(mb));
	/* a and b share one cached program (same anec and ports bytes). */
	load(&c, "a");
	load(&c, "b");
	add_state_cmd(&c, "a");
	add_state_cmd(&c, "b");
	for (s = 1; s <= 3; s++) {
		make_lane(lane, s);
		add_call(&c, "a", lane);
		make_lane(lane, s + 50);
		add_call(&c, "b", lane);
	}
	sprintf_buf(&c, "RESET a\n");
	make_lane(lane, 7);
	add_call(&c, "a", lane);	/* a restarts from zero */
	make_lane(lane, 8);
	add_call(&c, "b", lane);	/* b continues */
	sprintf_buf(&c, "RESET\n");
	make_lane(lane, 9);
	add_call(&c, "a", lane);
	make_lane(lane, 10);
	add_call(&c, "b", lane);	/* both restart */
	sprintf_buf(&c, "FREE a\n");
	load(&c, "a");
	add_state_cmd(&c, "a");
	make_lane(lane, 11);
	add_call(&c, "a", lane);	/* reloaded name starts from zero */
	sprintf_buf(&c, "QUIT\n");
	rc = run_session_bin(&c, &r);
	check(rc == 0, "isolation: session exit 0");
	bad |= take_ok(&r, &pos, "OK LOAD a") | take_ok(&r, &pos, "OK LOAD b") |
	       take_ok(&r, &pos, "OK STATE a") | take_ok(&r, &pos, "OK STATE b");
	check(!bad, "isolation: both names loaded and registered");
	for (s = 1; s <= 3; s++) {
		make_lane(lane, s);
		model_step(&ma, lane, want);
		bad = take_call(&r, &pos, got);
		check(!bad && !memcmp(got, want, sizeof(got)), "isolation: name a interleaved with b equals its own model");
		make_lane(lane, s + 50);
		model_step(&mb, lane, want);
		bad = take_call(&r, &pos, got);
		check(!bad && !memcmp(got, want, sizeof(got)), "isolation: name b interleaved with a equals its own model");
	}
	check(!take_ok(&r, &pos, "OK RESET"), "reset: RESET a answered");
	memset(&ma, 0, sizeof(ma));
	make_lane(lane, 7);
	model_step(&ma, lane, want);
	bad = take_call(&r, &pos, got);
	check(!bad && !memcmp(got, want, sizeof(got)), "reset: a restarts from a zero state after RESET a");
	make_lane(lane, 8);
	model_step(&mb, lane, want);
	bad = take_call(&r, &pos, got);
	check(!bad && !memcmp(got, want, sizeof(got)), "reset: b is unaffected by RESET a");
	check(!take_ok(&r, &pos, "OK RESET"), "reset: RESET answered");
	memset(&ma, 0, sizeof(ma));
	memset(&mb, 0, sizeof(mb));
	make_lane(lane, 9);
	model_step(&ma, lane, want);
	bad = take_call(&r, &pos, got);
	check(!bad && !memcmp(got, want, sizeof(got)), "reset: a restarts after RESET all");
	make_lane(lane, 10);
	model_step(&mb, lane, want);
	bad = take_call(&r, &pos, got);
	check(!bad && !memcmp(got, want, sizeof(got)), "reset: b restarts after RESET all");
	check(!take_ok(&r, &pos, "OK FREE a") && !take_ok(&r, &pos, "OK LOAD a") &&
		      !take_ok(&r, &pos, "OK STATE a"),
	      "free: FREE, LOAD and STATE of a again answered");
	memset(&ma, 0, sizeof(ma));
	make_lane(lane, 11);
	model_step(&ma, lane, want);
	bad = take_call(&r, &pos, got);
	check(!bad && !memcmp(got, want, sizeof(got)), "free: a reloaded name starts from zero");
	(void)step;
	free(c.p);
	free(r.p);
}

/* A stream with one refused STATE: the reply text, the exit code 1. */
static void refused(const char *what, const char *state_line, size_t nruns, uint64_t off0,
		    const char *want_reason, int load_first)
{
	struct sbuf c = { 0 }, r = { 0 };
	uint64_t *offs = calloc(nruns, sizeof(*offs));
	size_t i;
	int rc;
	char msg[160];

	for (i = 0; i < nruns; i++) {
		offs[i] = off0 + i * PITCH;
	}
	if (load_first) {
		load(&c, "a");
	}
	sprintf_buf(&c, "%s\n", state_line);
	sput(&c, offs, nruns * sizeof(*offs));
	sprintf_buf(&c, "QUIT\n");
	rc = run_session_bin(&c, &r);
	snprintf(msg, sizeof(msg), "refusal %s: reply names '%s'", what, want_reason);
	check(strstr((char *)r.p, want_reason) != NULL, msg);
	snprintf(msg, sizeof(msg), "refusal %s: session exit 1", what);
	check(rc == 1, msg);
	free(offs);
	free(c.p);
	free(r.p);
}

static void scenario_refusals(void)
{
	refused("unloaded name", "STATE ghost 1 6 6 4", 4, 0, "not-loaded", 0);
	refused("input index past the inputs", "STATE a 2 6 6 4", 4, 0, "no-such-port", 1);
	refused("output index past the outputs", "STATE a 1 7 6 4", 4, 0, "no-such-port", 1);
	refused("tile mismatch lane in, conv out", "STATE a 0 6 6 4", 4, 0, "tile-bytes-differ", 1);
	refused("offset past the tile", "STATE a 1 6 6 4", 4, CONV - 3 * PITCH + 62, "run-out-of-tile", 1);
	refused("run crossing the end of the tile", "STATE a 1 6 6 1", 1, CONV - 3, "run-out-of-tile", 1);
	refused("zero run bytes", "STATE a 1 6 0 4", 4, 0, "bad-run-bytes", 1);
	refused("zero runs", "STATE a 1 6 6 0", 0, 0, "bad-run-count", 1);
	{
		struct sbuf c = { 0 }, r = { 0 };
		int rc;

		load(&c, "a");
		add_state_cmd(&c, "a");
		add_state_cmd(&c, "a");
		sprintf_buf(&c, "QUIT\n");
		rc = run_session_bin(&c, &r);
		check(strstr((char *)r.p, "ERR STATE a already-resident") != NULL,
		      "refusal double registration: reply names already-resident");
		check(rc == 1, "refusal double registration: session exit 1");
		free(c.p);
		free(r.p);
	}
}

static void scenario_exec_failure(void)
{
	struct sbuf c = { 0 }, r = { 0 };
	uint8_t lane[LANE];
	int rc;

	load(&c, "a");
	add_state_cmd(&c, "a");
	make_lane(lane, 1);
	add_call(&c, "a", lane);
	make_lane(lane, 2);
	add_call(&c, "a", lane);	/* this EXEC fails */
	sprintf_buf(&c, "QUIT\n");
	exec_count = 0;
	fail_exec_at = 1;
	rc = run_session_bin(&c, &r);
	fail_exec_at = -1;
	check(memmem(r.p, r.n, "ERR CALL a exec-failed", 22) != NULL, "exec failure: the failed CALL answers ERR exec-failed");
	check(rc == 1, "exec failure: the session exits 1 (fails closed)");
	check(memmem(r.p, r.n, "OK QUIT", 7) != NULL, "exec failure: the stream stays in step to QUIT");
	free(c.p);
	free(r.p);
}

int main(int argc, char **argv)
{
	/* Any argument: serve the real session protocol on stdin/stdout over the fake
	 * device (test_qwen_resident_states.py drives the decoder through it). */
	if (argc > 1) {
		return ane_session_main(argc, argv);
	}
	scenario_chain();
	scenario_isolation_and_reset();
	scenario_refusals();
	scenario_exec_failure();
	printf(failures ? "SESSION-STATE FAIL\n" : "SESSION-STATE PASS\n");
	return failures != 0;
}
