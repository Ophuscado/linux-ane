/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/* Host test of the lane acceptance used by ane-run --check matvec and --check rms. No device. */
#include <math.h>
#include <stdio.h>

#include "ane_check_band.h"

static int bad;

static void expect(const char *name, int got, int want)
{
	if (got != want) {
		printf("FAIL %s: got %d, wanted %d\n", name, got, want);
		bad = 1;
	} else {
		printf("PASS %s\n", name);
	}
}

int main(void)
{
	/* Lanes with a scale: unchanged behaviour. */
	expect("2 ulp passes", ane_lane_in_band(2.0, 1.0, 1.0), 1);
	expect("4 condition units passes", ane_lane_in_band(100.0, 4.0 * 0x1p-11, 1.0), 1);
	expect("far outside both fails", ane_lane_in_band(100.0, 1.0, 1.0), 0);
	expect("NaN device value fails", ane_lane_in_band(NAN, NAN, 1.0), 0);
	expect("infinite device value fails", ane_lane_in_band(INFINITY, INFINITY, 1.0), 0);

	/* Lanes with no scale (exact sum of absolute terms is 0): only an exact match passes. */
	expect("zero lane, exact 0 passes", ane_lane_in_band(0.0, 0.0, 0.0), 1);
	expect("zero lane, output 1.0 fails", ane_lane_in_band(1e9, 1.0, 0.0), 0);
	expect("zero lane, tiny nonzero fails", ane_lane_in_band(1.0, 0x1p-24, 0.0), 0);
	expect("zero lane, NaN fails", ane_lane_in_band(NAN, NAN, 0.0), 0);
	expect("zero lane, infinity fails", ane_lane_in_band(INFINITY, INFINITY, 0.0), 0);

	/* The reported number. */
	expect("cond units with scale", ane_cond_units(1.0, 1.0) == 2048.0, 1);
	expect("cond units, zero lane exact is 0", ane_cond_units(0.0, 0.0) == 0.0, 1);
	expect("cond units, zero lane nonzero is infinite", isinf(ane_cond_units(1.0, 0.0)), 1);
	expect("cond units, zero lane NaN is infinite", isinf(ane_cond_units(NAN, 0.0)), 1);

	/* The rms reference and band for an all-zero input row (rscaled 0, gamma 1: sumabs = 1). */
	expect("rms reference of a normal channel", ane_rms_want(2.0, 3.0, 2.0) == 3.0, 1);
	expect("rms reference of an all-zero row is 0, not NaN", ane_rms_want(0.0, 1.0, 0.0) == 0.0, 1);
	expect("rms all-zero row, exact 0 output passes",
	       ane_lane_in_band(0.0, fabs(0.0 - ane_rms_want(0.0, 1.0, 0.0)), 1.0), 1);
	expect("rms all-zero row, output 1.0 fails",
	       ane_lane_in_band(1e9, fabs(1.0 - ane_rms_want(0.0, 1.0, 0.0)), 1.0), 0);
	expect("rms all-zero row, NaN output fails",
	       ane_lane_in_band(NAN, fabs(NAN - ane_rms_want(0.0, 1.0, 0.0)), 1.0), 0);
	expect("rms NaN scale keeps a NaN reference", isnan(ane_rms_want(1.0, 1.0, NAN)), 1);
	{
		double w = ane_rms_want(1.0, 1.0, NAN); /* an exact 0 device output against it */

		expect("rms NaN scale, exact 0 output fails", ane_lane_in_band(fabs(0.0 - w), fabs(0.0 - w), 1.0 + 1.0 + NAN), 0);
	}

	return bad;
}
