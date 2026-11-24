/* SPDX-License-Identifier: Apache-2.0 */
#include "PhaseDetect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace gaokun3 {

namespace {

constexpr int kMaxShift = 8;    /* in 16 px samples: +-128 px */

/*
 * ov13b10: each pair is a line of two samples per block. The pairs at rows
 * (9, 13) and (25, 29) have the opposite orientation, so their bottom member
 * goes into the first profile.
 */
constexpr PdSite kOv13b10Line0[] = { { 10, 1, 5 }, { 26, 1, 5 } };
constexpr PdSite kOv13b10Line1[] = { { 10, 17, 21 }, { 26, 17, 21 } };
constexpr PdSite kOv13b10Line2[] = { { 2, 13, 9 }, { 18, 13, 9 } };
constexpr PdSite kOv13b10Line3[] = { { 2, 29, 25 }, { 18, 29, 25 } };
constexpr PdLine kOv13b10Lines[] = {
	{ kOv13b10Line0, 2 },
	{ kOv13b10Line1, 2 },
	{ kOv13b10Line2, 2 },
	{ kOv13b10Line3, 2 },
};

/*
 * s5k3l6: the 16 pairs of a block sit at columns 7, 23, 43, 59 (rows 8-32)
 * and 11, 27, 39, 55 (rows 40-64), so each group of four pairs whose rows
 * are close makes a line of four samples per block, 12 to 20 columns apart.
 * The first profile takes the pixels the vendors' drivers call L, the
 * second those they call R: with this order the disparity has the sign of
 * PhaseResult, as a lens sweep over a textured target on an S5K3L6 unit
 * showed.
 */
constexpr PdSite kS5k3l6Line0[] = { { 7, 16, 12 }, { 23, 12, 8 }, { 43, 12, 8 }, { 59, 16, 12 } };
constexpr PdSite kS5k3l6Line1[] = { { 7, 24, 28 }, { 23, 28, 32 }, { 43, 28, 32 }, { 59, 24, 28 } };
constexpr PdSite kS5k3l6Line2[] = { { 11, 44, 40 }, { 27, 48, 44 }, { 39, 48, 44 }, { 55, 44, 40 } };
constexpr PdSite kS5k3l6Line3[] = { { 11, 60, 64 }, { 27, 56, 60 }, { 39, 56, 60 }, { 55, 60, 64 } };
constexpr PdLine kS5k3l6Lines[] = {
	{ kS5k3l6Line0, 4 },
	{ kS5k3l6Line1, 4 },
	{ kS5k3l6Line2, 4 },
	{ kS5k3l6Line3, 4 },
};

/*
 * codesPerUnit of the s5k3l6: the slope of the disparity over a lens sweep on
 * a target at 40-60 cm, measured over the central 70% of the frame (the
 * default AF region); linear from code 150 to 700, about 250 over the
 * central 30%.
 */
constexpr PdLayout kLayouts[] = {
	{ "ov13b10", 32, kOv13b10Lines, 4, false, 0, 300.0 },
	{ "s5k3l6", 64, kS5k3l6Lines, 4, true, 16, 270.0 },
};

/* One pixel of a CSI-2 packed 10-bit line: 4 pixels in 5 bytes, the fifth
 * holding the two low bits of each. */
inline int pixel(const uint8_t *line, int x)
{
	const uint8_t *g = line + (x >> 2) * 5;
	const int i = x & 3;
	return (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
}

/*
 * Subtract from each sample the mean of the samples within h of it (fewer at
 * the ends): what is left is the texture, without the slow change of the
 * pixels' response across the field.
 */
void removeTrend(std::vector<double> &v, int n, int h)
{
	std::vector<double> sum(n + 1, 0.0);
	for (int k = 0; k < n; k++)
		sum[k + 1] = sum[k] + v[k];
	for (int k = 0; k < n; k++) {
		const int lo = std::max(0, k - h), hi = std::min(n, k + h + 1);
		v[k] -= (sum[hi] - sum[lo]) / (hi - lo);
	}
}

} /* namespace */

const PdLayout *pdLayoutFor(std::string_view model)
{
	for (const PdLayout &l : kLayouts)
		if (model == l.model)
			return &l;
	return nullptr;
}

PhaseResult measurePhase(const RawFrame &raw, const PdLayout &layout,
			 float x0, float y0, float x1, float y1)
{
	PhaseResult res;
	const int blk = layout.block;
	if (!raw.data || raw.width < 4 * blk || raw.height < 4 * blk)
		return res;

	/* A row may reach into the next block: keep it inside the frame. */
	int reach = 0;
	for (int l = 0; l < layout.lineCount; l++)
		for (int s = 0; s < layout.lines[l].count; s++)
			reach = std::max({ reach, layout.lines[l].sites[s].rowA,
					   layout.lines[l].sites[s].rowB });

	const int bx0 = static_cast<int>(raw.width * x0) / blk;
	const int bx1 = std::min(static_cast<int>(raw.width * x1) / blk, raw.width / blk);
	const int by0 = static_cast<int>(raw.height * y0) / blk;
	const int by1 = std::min(static_cast<int>(raw.height * y1) / blk,
				 (raw.height - 1 - reach) / blk + 1);

	std::array<double, 2 * kMaxShift + 1> corr{};
	double norm = 0.0;
	std::vector<double> a, b;

	for (int l = 0; l < layout.lineCount; l++) {
		const PdLine &line = layout.lines[l];
		const int m = line.count;
		const int n = m * (bx1 - bx0);
		if (n <= 2 * kMaxShift || by1 <= by0)
			return res;
		a.resize(n);
		b.resize(n);
		for (int by = by0; by < by1; by++) {
			double ma = 0.0, mb = 0.0;
			for (int k = 0; k < n; k++) {
				const PdSite &s = line.sites[k % m];
				const int x = (bx0 + k / m) * blk + s.col;
				const uint8_t *la = raw.data + static_cast<size_t>(by * blk + s.rowA) * raw.stride;
				const uint8_t *lb = raw.data + static_cast<size_t>(by * blk + s.rowB) * raw.stride;
				a[k] = pixel(la, x);
				b[k] = pixel(lb, x);
				ma += a[k];
				mb += b[k];
			}
			double ea = 0.0, eb = 0.0;
			if (layout.detrend > 0) {
				removeTrend(a, n, layout.detrend);
				removeTrend(b, n, layout.detrend);
				for (int k = 0; k < n; k++) {
					ea += a[k] * a[k];
					eb += b[k] * b[k];
				}
			} else {
				ma /= n;
				mb /= n;
				for (int k = 0; k < n; k++) {
					a[k] -= ma;
					b[k] -= mb;
					ea += a[k] * a[k];
					eb += b[k] * b[k];
				}
			}
			for (int s = -kMaxShift; s <= kMaxShift; s++) {
				double c = 0.0;
				if (s >= 0) {
					for (int k = 0; k + s < n; k++)
						c += a[k] * b[k + s];
				} else {
					for (int k = -s; k < n; k++)
						c += a[k] * b[k + s];
				}
				corr[s + kMaxShift] += c;
			}
			norm += std::sqrt(ea * eb);
		}
	}
	if (norm <= 0.0)
		return res;

	const int i = static_cast<int>(std::max_element(corr.begin(), corr.end()) - corr.begin());
	double d = i - kMaxShift;
	if (i > 0 && i < 2 * kMaxShift) {
		const double den = corr[i - 1] - 2 * corr[i] + corr[i + 1];
		if (den < 0)
			d += 0.5 * (corr[i - 1] - corr[i + 1]) / den;
	}
	res.valid = i > 0 && i < 2 * kMaxShift;   /* a peak at the edge is out of range */
	res.disparity = d;
	res.confidence = corr[i] / norm;
	return res;
}

} /* namespace gaokun3 */
