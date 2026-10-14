/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/* Host test of the ane-run --time-raw helpers. No device. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ane_timing.h"

static int bad;

static void expect(const char *name, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	bad |= !ok;
}

int main(void)
{
	const double lat[4] = {0.306, 0.307, 0.357, 0.256};
	const uint64_t ns[5] = {306000, 307001, 357002, 256003, 18446744073709551615ull};
	char path[] = "/tmp/test_ane_timing.XXXXXX";
	int fd = mkstemp(path);
	char line[64];
	FILE *f;
	unsigned long long got[5];
	int i;

	expect("mean of four calls", fabs(ane_lat_mean(lat, 4) - 0.3065) < 1e-12);
	expect("mean of no calls is 0", ane_lat_mean(lat, 0) == 0.0);
	expect("mean is not a quantile bin", ane_lat_mean(lat, 4) != lat[1] && ane_lat_mean(lat, 4) != lat[2]);
	if (fd < 0) {
		puts("FAIL mkstemp");
		return 1;
	}
	close(fd);
	expect("raw write succeeds", ane_lat_write_raw(path, ns, 5) == 0);
	f = fopen(path, "r");
	for (i = 0; f && i < 5 && fgets(line, sizeof(line), f); i++) {
		got[i] = strtoull(line, NULL, 10);
	}
	expect("five lines read back", f && i == 5);
	expect("values and call order preserved", i == 5 && !memcmp(got, ns, sizeof(ns)));
	expect("no sixth line", f && fgets(line, sizeof(line), f) == NULL);
	if (f) {
		fclose(f);
	}
	unlink(path);
	expect("an unwritable path returns -1", ane_lat_write_raw("/nonexistent-dir/x.raw", ns, 5) == -1);
	return bad;
}
