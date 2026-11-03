/* SPDX-License-Identifier: Apache-2.0 */
#include "PhaseDetect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace gaokun3 {

namespace {

constexpr int kBlock = 32;
constexpr int kMaxShift = 8;    /* in 16 px samples: +-128 px */

struct PdPair {
	int top, bottom;     /* rows within the block */
	int col[2];          /* columns within the block */
	int sign;            /* orientation of the pair */
};

constexpr PdPair kPairs[] = {
	{ 1, 5, { 10, 26 }, 1 },
	{ 17, 21, { 10, 26 }, 1 },
	{ 9, 13, { 2, 18 }, -1 },
	{ 25, 29, { 2, 18 }, -1 },
};

/* One pixel of a CSI-2 packed 10-bit line: 4 pixels in 5 bytes, the fifth
 * holding the two low bits of each. */
inline int pixel(const uint8_t *line, int x)
{
	const uint8_t *g = line + (x >> 2) * 5;
	const int i = x & 3;
	return (g[i] << 2) | ((g[4] >> (2 * i)) & 3);
}

} /* namespace */

PhaseResult measurePhase(const RawFrame &raw, float x0, float y0, float x1, float y1)
{
	PhaseResult res;
	if (!raw.data || raw.width < 4 * kBlock || raw.height < 4 * kBlock)
		return res;

	const int bx0 = static_cast<int>(raw.width * x0) / kBlock;
	const int bx1 = std::min(static_cast<int>(raw.width * x1) / kBlock, raw.width / kBlock);
	const int by0 = static_cast<int>(raw.height * y0) / kBlock;
	const int by1 = std::min(static_cast<int>(raw.height * y1) / kBlock, raw.height / kBlock);
	const int n = 2 * (bx1 - bx0);
	if (n <= 2 * kMaxShift || by1 <= by0)
		return res;

	std::array<double, 2 * kMaxShift + 1> corr{};
	double norm = 0.0;
	std::vector<double> a(n), b(n);

	for (const PdPair &p : kPairs) {
		for (int by = by0; by < by1; by++) {
			const uint8_t *lt = raw.data + static_cast<size_t>(by * kBlock + p.top) * raw.stride;
			const uint8_t *lb = raw.data + static_cast<size_t>(by * kBlock + p.bottom) * raw.stride;
			double ma = 0.0, mb = 0.0;
			for (int k = 0; k < n; k++) {
				const int x = (bx0 + k / 2) * kBlock + p.col[k & 1];
				a[k] = pixel(lt, x);
				b[k] = pixel(lb, x);
				ma += a[k];
				mb += b[k];
			}
			ma /= n;
			mb /= n;
			double ea = 0.0, eb = 0.0;
			for (int k = 0; k < n; k++) {
				a[k] -= ma;
				b[k] -= mb;
				ea += a[k] * a[k];
				eb += b[k] * b[k];
			}
			for (int s = -kMaxShift; s <= kMaxShift; s++) {
				const int ss = s * p.sign;
				double c = 0.0;
				if (ss >= 0) {
					for (int k = 0; k + ss < n; k++)
						c += a[k] * b[k + ss];
				} else {
					for (int k = -ss; k < n; k++)
						c += a[k] * b[k + ss];
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
