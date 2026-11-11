/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 亮度锐化，见 Sharpen.h。
 */
#include "Sharpen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>

#include <system/camera_metadata.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace gaokun3 {

SharpenParams sharpenParams(uint8_t edgeMode, double gain, double debugScale)
{
	SharpenParams p;
	if (edgeMode == ANDROID_EDGE_MODE_OFF || debugScale <= 0.0)
		return p;
	const bool hq = edgeMode == ANDROID_EDGE_MODE_HIGH_QUALITY;
	p.radius = hq ? 2 : 1;
	/*
	 * 增益 1 时 FAST 强度 2.0、HIGH_QUALITY 2.4，门限 2 级；增益每加 1，强度按
	 * 1/(1 + 0.15·(g − 1)) 减弱、门限加 0.75 级：ISO 1550 时强度约为三分之一、门限约 13 级。
	 * 强度由 11-10 盲测定（Aperture 预览，亮处文字）：1.0 与不锐化分不出，2.5 过头，
	 * 2.0 优于 1.5，2.0 与 1.7 分不出；暗处 2.0 与不锐化分不出（噪点都多）。
	 */
	const double g = std::max(gain, 1.0);
	const double amount = (hq ? 2.4 : 2.0) * debugScale / (1.0 + 0.15 * (g - 1.0));
	p.amountQ4 = std::min(static_cast<int>(std::lround(amount * 16.0)), 127);
	p.threshold = static_cast<int>(std::lround(2.0 + 0.75 * (g - 1.0)));
	if (p.amountQ4 <= 0)
		p.radius = 0;
	return p;
}

namespace {

/*
 * 一行：中心行 c、模糊后的列和 v（两侧各预填充 R 项），写出 out。全程 16 位
 * （NEON 一次 8 个像素）：列和最大 16 × 255，加权和最大 65280，差值 ±255，
 * 乘强度（amountQ4 ≤ 127，调试属性放大时也不超过）后最大约 ±32385，都不越界。
 */
template<int R>
void sharpenRow(const uint8_t *c, const uint16_t *v, uint8_t *out, int w, int16_t amountQ4,
		int16_t t)
{
	int x = 0;
#if defined(__ARM_NEON)
	/* 编译器自动向量化时乘法等会升到 32 位，一次只算 4 个像素；这里全程 16 位 */
	const int16x8_t tv = vdupq_n_s16(t), av = vdupq_n_s16(amountQ4), zero = vdupq_n_s16(0);
	for (; x + 8 <= w; x += 8) {
		uint16x8_t sum;
		if (R == 1) {
			sum = vaddq_u16(vaddq_u16(vld1q_u16(v + x - 1), vld1q_u16(v + x + 1)),
					vshlq_n_u16(vld1q_u16(v + x), 1));
			sum = vrshrq_n_u16(sum, 4);
		} else {
			const uint16x8_t m0 = vld1q_u16(v + x);
			sum = vaddq_u16(vld1q_u16(v + x - 2), vld1q_u16(v + x + 2));
			sum = vaddq_u16(sum, vshlq_n_u16(vaddq_u16(vld1q_u16(v + x - 1),
								   vld1q_u16(v + x + 1)), 2));
			sum = vaddq_u16(sum, vaddq_u16(vshlq_n_u16(m0, 2), vshlq_n_u16(m0, 1)));
			sum = vrshrq_n_u16(sum, 8);
		}
		const int16x8_t cc = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(c + x)));
		const int16x8_t d = vsubq_s16(cc, vreinterpretq_s16_u16(sum));
		const int16x8_t cd = vaddq_s16(vmaxq_s16(vsubq_s16(d, tv), zero),
					       vminq_s16(vaddq_s16(d, tv), zero));
		const int16x8_t o = vaddq_s16(cc, vrshrq_n_s16(vmulq_s16(cd, av), 4));
		vst1_u8(out + x, vqmovun_s16(o));
	}
#endif
	for (; x < w; x++) {
		uint16_t b;
		if (R == 1)
			b = static_cast<uint16_t>(v[x - 1] + 2 * v[x] + v[x + 1] + 8) >> 4;
		else
			b = static_cast<uint16_t>(v[x - 2] + 4 * v[x - 1] + 6 * v[x] + 4 * v[x + 1] +
						  v[x + 2] + 128) >> 8;
		const int16_t d = static_cast<int16_t>(c[x] - b);
		/* coring：|d| ≤ t 置 0，否则向 0 收 t */
		const int16_t hi = static_cast<int16_t>(d - t), lo = static_cast<int16_t>(d + t);
		const int16_t cd = static_cast<int16_t>((hi > 0 ? hi : 0) + (lo < 0 ? lo : 0));
		const int16_t o = static_cast<int16_t>(c[x] + ((cd * amountQ4 + 8) >> 4));
		out[x] = static_cast<uint8_t>(o < 0 ? 0 : o > 255 ? 255 : o);
	}
}

} /* namespace */

void sharpenLuma(const uint8_t *src, int ss, uint8_t *dst, int ds, int w, int h,
		 const SharpenParams &p, std::vector<uint16_t> &scratch)
{
	if (p.radius <= 0 || w < 8 || h < 8) {
		for (int y = 0; y < h; y++)
			std::copy(src + static_cast<size_t>(y) * ss, src + static_cast<size_t>(y) * ss + w,
				  dst + static_cast<size_t>(y) * ds);
		return;
	}
	const int r = p.radius;
	/*
	 * 门限不低于噪声：白噪声 σ 经 y − blur(y) 后约为 0.8σ（3×3）或 0.89σ（5×5），
	 * 门限取它的 2 倍，噪声幅度以内的起伏都不放大。
	 */
	SharpenParams q = p;
	const double sigma = estimateNoise(src, ss, w, h);
	q.threshold = std::max(p.threshold, static_cast<int>(std::lround(
						    2.0 * (r == 1 ? 0.80 : 0.89) * sigma)));
	scratch.resize(static_cast<size_t>(w) + 2 * r);
	uint16_t *v = scratch.data() + r;
	auto row = [&](int y) { return src + static_cast<size_t>(std::clamp(y, 0, h - 1)) * ss; };

	for (int y = 0; y < h; y++) {
		/* 竖直方向的二项式和，边缘取边上的行 */
		if (r == 1) {
			const uint8_t *a = row(y - 1), *b = row(y), *c = row(y + 1);
			for (int x = 0; x < w; x++)
				v[x] = static_cast<uint16_t>(a[x] + 2 * b[x] + c[x]);
		} else {
			const uint8_t *a = row(y - 2), *b = row(y - 1), *c = row(y), *d = row(y + 1),
				      *e = row(y + 2);
			for (int x = 0; x < w; x++)
				v[x] = static_cast<uint16_t>(a[x] + 4 * b[x] + 6 * c[x] + 4 * d[x] + e[x]);
		}
		/* 水平方向的边缘：重复边上的列和 */
		for (int q = 1; q <= r; q++) {
			v[-q] = v[0];
			v[w - 1 + q] = v[w - 1];
		}
		uint8_t *out = dst + static_cast<size_t>(y) * ds;
		if (r == 1)
			sharpenRow<1>(row(y), v, out, w, static_cast<int16_t>(q.amountQ4),
				      static_cast<int16_t>(q.threshold));
		else
			sharpenRow<2>(row(y), v, out, w, static_cast<int16_t>(q.amountQ4),
				      static_cast<int16_t>(q.threshold));
	}
}

double estimateNoise(const uint8_t *y, int stride, int w, int h)
{
	if (w < 8 || h < 8)
		return 0.0;
	/* 抽约 4 万个点 */
	const double area = static_cast<double>(w - 2) * (h - 2);
	const int step = std::max(1, static_cast<int>(std::sqrt(area / 40000.0)));
	/*
	 * |响应| 最大 16 × 255：直方图求中位数，与排序后取第 n/2 个（nth_element）结果相同，
	 * 但不用存样本、不用部分排序（预览每帧都算，nth_element 曾占 provider 的 7%）。
	 */
	constexpr int kMax = 16 * 255;
	std::vector<uint32_t> hist(kMax + 1, 0);
	uint32_t n = 0;
	for (int yy = 1; yy < h - 1; yy += step) {
		const uint8_t *a = y + static_cast<size_t>(yy - 1) * stride;
		const uint8_t *b = a + stride, *c = b + stride;
		for (int xx = 1; xx < w - 1; xx += step) {
			const int r = a[xx - 1] - 2 * a[xx] + a[xx + 1] - 2 * b[xx - 1] + 4 * b[xx] -
				      2 * b[xx + 1] + c[xx - 1] - 2 * c[xx] + c[xx + 1];
			hist[std::abs(r)]++;
			n++;
		}
	}
	if (!n)
		return 0.0;
	/* 第 n/2 个（从 0 数）：累计数超过 n/2 的最小值 */
	uint32_t acc = 0;
	int mid = 0;
	for (; mid < kMax; mid++) {
		acc += hist[mid];
		if (acc > n / 2)
			break;
	}
	/* 掩模的范数为 6；中位绝对值 × 1.4826 ≈ 标准差 */
	return 1.4826 * mid / 6.0;
}

void denoiseLuma(const uint8_t *src, int ss, uint8_t *dst, int ds, int w, int h, int t)
{
	auto work = [=](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const uint8_t *r0 = src + static_cast<size_t>(std::max(y - 1, 0)) * ss;
			const uint8_t *r1 = src + static_cast<size_t>(y) * ss;
			const uint8_t *r2 = src + static_cast<size_t>(std::min(y + 1, h - 1)) * ss;
			uint8_t *out = dst + static_cast<size_t>(y) * ds;
			for (int x = 0; x < w; x++) {
				const int xm = x > 0 ? x - 1 : x, xp = x < w - 1 ? x + 1 : x;
				const int c = r1[x];
				int sum = 0, n = 0;
				for (const uint8_t *r : { r0, r1, r2 })
					for (int xx : { xm, x, xp }) {
						const int d = r[xx] - c;
						if (d <= t && d >= -t) {
							sum += r[xx];
							n++;
						}
					}
				out[x] = static_cast<uint8_t>((sum + n / 2) / n);
			}
		}
	};
	if (t <= 0) {
		for (int y = 0; y < h; y++)
			std::copy(src + static_cast<size_t>(y) * ss, src + static_cast<size_t>(y) * ss + w,
				  dst + static_cast<size_t>(y) * ds);
		return;
	}
	constexpr int kThreads = 4;
	std::thread th[kThreads - 1];
	for (int i = 1; i < kThreads; i++)
		th[i - 1] = std::thread(work, h * i / kThreads, h * (i + 1) / kThreads);
	work(0, h / kThreads);
	for (auto &x : th)
		x.join();
}

} /* namespace gaokun3 */
