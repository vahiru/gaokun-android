/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 camera HAL: phase detection from the rear ov13b10's PD pixels.
 *
 * The ov13b10 has shielded phase-detection pixels in its full-size mode only
 * (the binned modes average them away): 16 per 32x32 block, all at the B sites
 * of its GRBG pattern, in pairs four rows apart. Measured on this unit's RAW:
 *   rows (1, 5) and (17, 21), columns 10 and 26: one orientation
 *   rows (9, 13) and (25, 29), columns 2 and 18: the opposite one
 * (block origin at the frame's top left, on the readout libcamera flips H+V for
 * the rear's DT rotation of 180; Session checks it). Along a pair of rows the two
 * members see the scene shifted horizontally by an amount that grows with
 * defocus and changes sign across focus.
 */
#pragma once

#include <cstdint>

namespace gaokun3 {

struct RawFrame {
	const uint8_t *data;   /* CSI-2 packed 10-bit Bayer */
	int stride;            /* bytes per line */
	int width, height;
};

struct PhaseResult {
	bool valid = false;
	/* Shift between the two members of the PD pairs, in units of 16 pixels
	 * (the sampling step along a pair of rows). Positive: the lens has to move
	 * to lower codes (towards infinity). */
	double disparity = 0.0;
	/* Normalised correlation at the peak, 0..1: how alike the two profiles
	 * are. Low on featureless scenes. */
	double confidence = 0.0;
};

/* Measure over the window x0..x1, y0..y1 given as fractions of the frame. */
PhaseResult measurePhase(const RawFrame &raw, float x0, float y0, float x1, float y1);

} /* namespace gaokun3 */
