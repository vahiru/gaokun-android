/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 camera HAL: phase detection from the rear camera's PD pixels.
 *
 * Both rear modules have half-shielded phase-detection pixels on B sites, in
 * pairs whose members are four rows apart and shielded on opposite sides.
 * The RAW carries them in the full-size mode only: the ov13b10's binned modes
 * average them away, and the s5k3l6 corrects them itself in its other modes.
 * Along a line of such pairs the two kinds of member see the scene shifted
 * horizontally by an amount that grows with defocus and changes sign across
 * focus. Positions are given on the readout libcamera flips H+V for the rear's
 * DT rotation of 180 (Session checks it), block origin at the frame's top left.
 *
 * ov13b10: 16 per 32x32 block, measured on this unit's RAW:
 *   rows (1, 5) and (17, 21), columns 10 and 26: one orientation
 *   rows (9, 13) and (25, 29), columns 2 and 18: the opposite one
 * s5k3l6: 32 per 64x64 block, the layout of the vendors' drivers, which RAW
 *   frames of an S5K3L6 unit confirm; the sensor corrects them itself unless
 *   the kernel driver turns that off (full-size mode).
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace gaokun3 {

struct RawFrame {
	const uint8_t *data;   /* CSI-2 packed 10-bit Bayer */
	int stride;            /* bytes per line */
	int width, height;
};

/*
 * One sample of a line of PD pairs, within a block: the column, and the rows
 * of the member that goes into the first profile (a) and of the one that goes
 * into the second (b). A row equal to the block size is row 0 of the next
 * block. Every a member of a layout is shielded on the same side.
 */
struct PdSite {
	int col;
	int rowA;
	int rowB;
};

/* The samples one block gives a line, left to right. */
struct PdLine {
	const PdSite *sites;
	int count;
};

struct PdLayout {
	const char *model;     /* libcamera's properties::Model */
	int block;             /* the layout repeats every block x block pixels */
	const PdLine *lines;
	int lineCount;
	bool gbrg;             /* RAW Bayer order under the flips: GBRG, else GRBG */
	/*
	 * What each profile loses before the correlation: 0, its mean; above
	 * that, at each sample the mean of the samples within this many of it.
	 * The local mean is for pixels whose response changes across the field
	 * (the two kinds of an s5k3l6 pair in opposite directions, by up to 1.6
	 * times over the frame), which would otherwise dominate the profiles of
	 * a low-contrast scene; 16 samples (256 pixels) keep the defocused texture
	 * that a phase search starts from.
	 */
	int detrend;
	/*
	 * Lens codes per unit of disparity for the first move of a phase search
	 * (AutoFocus.h); later moves use the measured slope.
	 */
	double codesPerUnit;
};

/* The layout of a rear sensor, or nullptr when its PD pixels are not known. */
const PdLayout *pdLayoutFor(std::string_view model);

struct PhaseResult {
	bool valid = false;
	/* Shift between the two profiles, in samples: 16 pixels on both layouts
	 * (two samples per 32 columns, four per 64). Positive: the lens has to
	 * move to lower codes (towards infinity). */
	double disparity = 0.0;
	/* Normalised correlation at the peak, 0..1: how alike the two profiles
	 * are. Low on featureless scenes. */
	double confidence = 0.0;
};

/* Measure over the window x0..x1, y0..y1 given as fractions of the frame. */
PhaseResult measurePhase(const RawFrame &raw, const PdLayout &layout,
			 float x0, float y0, float x1, float y1);

} /* namespace gaokun3 */
