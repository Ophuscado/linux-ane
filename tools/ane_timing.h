/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/*
 * Per-call timing helpers for ane-run --time and --time-raw.
 *
 * ane-run --time prints quantiles of the per-call exec times. When the calls land on steps of about 51 us (the poll cadence of the
 * finish-event wait), a quantile can only sit on a step, so a change smaller than a step moves it by one step or not at all. The
 * mean and the raw per-call values in call order do not have that limit: --time-raw FILE writes every call's exec time in
 * nanoseconds, one integer per line, in the order the calls were made, so blocks, drift and histograms can be computed afterwards.
 */
#ifndef ANE_TIMING_H
#define ANE_TIMING_H

#include <stdint.h>
#include <stdio.h>

static inline double ane_lat_mean(const double *lat, uint32_t n)
{
	double sum = 0.0;
	uint32_t i;

	for (i = 0; i < n; i++) {
		sum += lat[i];
	}
	return n ? sum / n : 0.0;
}

/* One decimal integer per line, call order. Returns 0, or -1 if the file cannot be written completely. */
static inline int ane_lat_write_raw(const char *path, const uint64_t *ns, uint32_t n)
{
	FILE *f = fopen(path, "w");
	uint32_t i;
	int bad = 0;

	if (!f) {
		return -1;
	}
	for (i = 0; i < n; i++) {
		bad |= fprintf(f, "%llu\n", (unsigned long long)ns[i]) < 0;
	}
	bad |= fclose(f) != 0;
	return bad ? -1 : 0;
}

#endif
