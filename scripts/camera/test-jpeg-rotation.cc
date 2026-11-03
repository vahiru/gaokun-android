/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * deliverJpeg 旋转逻辑的【真编译 + 真运行】自测（无第三方依赖）。
 *
 * 为什么要有这个：scripts/camera/test-jpeg-rotation.py 做的是【源码级】断言
 * —— 它保证 Session.cpp 里的表达式就是我们想的那几行；本文件做的是【运行级】
 * 检查 —— 把同一段表达式编译起来跑，用按 libyuv 契约实现的替身，验证调用
 * 形状真的对：实参顺序、源/目标 stride、90/270 的宽高互换、像素真的搬对了。
 *
 * 出 bug 的地方几乎都在调用形状上（比如目标 stride 顺手写成 dstW*4，画面会被
 * 斜切成条），而那种错在纯数学的模型里看不出来 —— 必须让数据真的流一遍。
 *
 * ⚠️ 这里用到的 libyuv 是【替身】，语义严格照 include/libyuv/rotate.h 的注释
 *    写死为顺时针：
 *       kRotate90  = 90,   // Rotate 90 degrees clockwise.
 *       kRotate270 = 270,  // Rotate 270 degrees clockwise.
 *    真机上换成真库即可，调用点不变。
 *
 * 构建与运行（WSL / 任意 Linux）：
 *     g++ -std=c++20 -Wall -Wextra -Werror -O1 \
 *         -o /tmp/t scripts/camera/test-jpeg-rotation.cc && /tmp/t
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

/* ───────────── libyuv 替身（只实现我们用到的三个函数） ───────────── */

namespace libyuv {

enum RotationMode {
	kRotate0 = 0,     /* No rotation. */
	kRotate90 = 90,   /* Rotate 90 degrees clockwise. */
	kRotate180 = 180, /* Rotate 180 degrees. */
	kRotate270 = 270, /* Rotate 270 degrees clockwise. */
};

enum FilterMode {
	kFilterNone = 0,
	kFilterBilinear = 2,
};

/* "ARGB" 在内存里其实是 B,G,R,A —— 与 libcamera 的 RGB888(=BGR24) 和
 * libjpeg 的 JCS_EXT_BGRA 都对得上，替身照这个布局来。 */
inline int RGB24ToARGB(const uint8_t *s, int ss, uint8_t *d, int ds, int w, int h)
{
	if (!s || !d || w <= 0 || h <= 0)
		return -1;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const uint8_t *sp = s + static_cast<size_t>(y) * ss + x * 3;
			uint8_t *dp = d + static_cast<size_t>(y) * ds + x * 4;
			dp[0] = sp[0];
			dp[1] = sp[1];
			dp[2] = sp[2];
			dp[3] = 0xFF;
		}
	}
	return 0;
}

/* 最近邻缩放：只为跑通"需要缩放"那条分支，精度不重要。 */
inline int ARGBScale(const uint8_t *s, int ss, int sw, int sh,
		     uint8_t *d, int ds, int dw, int dh, int)
{
	if (!s || !d || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
		return -1;
	for (int y = 0; y < dh; y++) {
		const int sy = y * sh / dh;
		for (int x = 0; x < dw; x++) {
			const int sx = x * sw / dw;
			memcpy(d + static_cast<size_t>(y) * ds + x * 4,
			       s + static_cast<size_t>(sy) * ss + sx * 4, 4);
		}
	}
	return 0;
}

/*
 * 顺时针旋转。width/height 是【源】尺寸 —— 与真库一致：目标 stride 由调用方
 * 按旋转后的宽度给，给错了就会错位，这正是本测试要抓的东西。
 *
 * 用"源像素正向散射"写，避免逆映射那套下标把自己绕进去：
 *   源 (sx,sy) 顺时针 90 度后落在 (dstH-1-sy, sx)      —— 源的上边变成 dst 的右边
 *   180 度落在 (dstW-1-sx, dstH-1-sy)
 *   270 度落在 (sy, dstW-1-sx)
 */
inline int ARGBRotate(const uint8_t *s, int ss, uint8_t *d, int ds,
		      int w, int h, RotationMode mode)
{
	if (!s || !d || w <= 0 || h <= 0)
		return -1;
	const bool swap = mode == kRotate90 || mode == kRotate270;
	const int ow = swap ? h : w;
	const int oh = swap ? w : h;
	for (int sy = 0; sy < h; sy++) {
		for (int sx = 0; sx < w; sx++) {
			int dx = sx, dy = sy;
			switch (mode) {
			case kRotate90:  dx = h - 1 - sy; dy = sx;         break;
			case kRotate180: dx = w - 1 - sx; dy = h - 1 - sy; break;
			case kRotate270: dx = sy;         dy = w - 1 - sx; break;
			default:                                           break;
			}
			memcpy(d + static_cast<size_t>(dy) * ds + dx * 4,
			       s + static_cast<size_t>(sy) * ss + sx * 4, 4);
		}
	}
	(void)ow;
	(void)oh;
	return 0;
}

} /* namespace libyuv */

#define ALOGW(...)                                                             \
	do {                                                                   \
		fprintf(stderr, "    W: " __VA_ARGS__);                        \
		fprintf(stderr, "\n");                                         \
	} while (0)

/* ──────────────── 被测代码：照抄 Session.cpp 的表达式 ──────────────── */

/*
 * ★ 逐字照抄 Session.cpp 的 toLibyuvRotation()。
 *   Python 那边（A2）会断言源码里就是这张表，所以这里和源码任何一边改错
 *   都会有人报错。
 */
static libyuv::RotationMode toLibyuvRotation(int32_t deg)
{
	switch (deg) {
	case 90:
		return libyuv::kRotate90;
	case 180:
		return libyuv::kRotate180;
	case 270:
		return libyuv::kRotate270;
	case 0:
		return libyuv::kRotate0;
	default:
		ALOGW("JPEG_ORIENTATION=%d 不是 0/90/180/270，按不旋转处理", deg);
		return libyuv::kRotate0;
	}
}

/*
 * 照抄 Session::deliverJpeg() 里"缩放 + 旋转"那一段（编码器换成"把最终像素
 * 与尺寸交回来"），表达式一字不改。
 */
struct JpegOut {
	std::vector<uint8_t> buf;   /* 按 stride 紧凑排布 */
	int32_t w = 0, h = 0;
	int channels = 0;           /* 3 = BGR（快捷路径），4 = BGRA */
};

static bool preparePixels(const uint8_t *rgb, int32_t srcWidth_, int32_t srcHeight_,
			  int32_t dstW, int32_t dstH, int32_t jpegOrientation,
			  JpegOut *out)
{
	const libyuv::RotationMode rotMode = toLibyuvRotation(jpegOrientation);
	const bool swapDims = rotMode == libyuv::kRotate90 || rotMode == libyuv::kRotate270;
	const int32_t outW = swapDims ? dstH : dstW;
	const int32_t outH = swapDims ? dstW : dstH;

	const bool needScale = dstW != srcWidth_ || dstH != srcHeight_;
	const bool needRotate = rotMode != libyuv::kRotate0;

	std::vector<uint8_t> argb;      /* dstW×dstH，已缩放、未旋转（BGRA） */
	std::vector<uint8_t> rotated;   /* outW×outH，已旋转（BGRA） */
	const uint8_t *src = rgb;
	int srcStride = srcWidth_ * 3;
	int components = 3;

	if (needScale || needRotate) {
		argb.resize(static_cast<size_t>(dstW) * dstH * 4);

		if (needScale) {
			std::vector<uint8_t> full(static_cast<size_t>(srcWidth_) *
						  srcHeight_ * 4);
			if (libyuv::RGB24ToARGB(rgb, srcWidth_ * 3, full.data(),
						srcWidth_ * 4, srcWidth_, srcHeight_) != 0)
				return false;
			if (libyuv::ARGBScale(full.data(), srcWidth_ * 4, srcWidth_,
					      srcHeight_, argb.data(), dstW * 4, dstW, dstH,
					      libyuv::kFilterBilinear) != 0)
				return false;
		} else {
			if (libyuv::RGB24ToARGB(rgb, srcWidth_ * 3, argb.data(), dstW * 4,
						dstW, dstH) != 0)
				return false;
		}

		if (needRotate) {
			rotated.resize(static_cast<size_t>(outW) * outH * 4);
			if (libyuv::ARGBRotate(argb.data(), dstW * 4, rotated.data(),
					       outW * 4, dstW, dstH, rotMode) != 0)
				return false;
			src = rotated.data();
			srcStride = outW * 4;
		} else {
			src = argb.data();
			srcStride = dstW * 4;
		}
		components = 4;
	}

	if (components == 3) {
		/* 快捷路径：直接拿 RGB24 编码，没有中间拷贝 */
		out->w = dstW;
		out->h = dstH;
		out->channels = 3;
		out->buf.assign(src, src + static_cast<size_t>(srcStride) * dstH);
		return true;
	}

	out->w = outW;
	out->h = outH;
	out->channels = 4;
	out->buf.assign(src, src + static_cast<size_t>(srcStride) * outH);
	return true;
}

/* ─────────────────────────── 断言框架 ─────────────────────────── */

static int g_checks = 0;
static int g_fail = 0;

static void check(bool ok, const char *label)
{
	g_checks++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
	if (!ok)
		g_fail++;
}

/* 源图：像素 (x,y) 的 (B,G,R) = (x+1, y+1, 0x3C) —— 坐标可反查 */
static uint8_t bOf(int x, int y)
{
	(void)y;   /* B 通道只编码 x，与像素布局对称，保留双参是为了调用点好看 */
	return static_cast<uint8_t>((x + 1) & 0xFF);
}
static uint8_t gOf(int x, int y)
{
	(void)x;   /* G 通道只编码 y */
	return static_cast<uint8_t>((y + 1) & 0xFF);
}

static std::vector<uint8_t> makeSource(int w, int h)
{
	std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 3);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *p = buf.data() + (static_cast<size_t>(y) * w + x) * 3;
			p[0] = bOf(x, y);
			p[1] = gOf(x, y);
			p[2] = 0x3C;
		}
	return buf;
}

static void testPixels(int srcW, int srcH, int dstW, int dstH, int deg,
		       bool expectScale)
{
	char label[256];
	snprintf(label, sizeof(label),
		 "源 %dx%d → 流 %dx%d、请求旋转 %3d 度：尺寸与每个像素都对",
		 srcW, srcH, dstW, dstH, deg);
	std::vector<uint8_t> src = makeSource(srcW, srcH);
	JpegOut out;
	if (!preparePixels(src.data(), srcW, srcH, dstW, dstH, deg, &out)) {
		check(false, label);
		return;
	}

	const bool swap = (deg == 90 || deg == 270);
	const int expW = swap ? dstH : dstW;
	const int expH = swap ? dstW : dstH;
	const int stride = out.channels == 4 ? 4 : 3;

	/* 期望图：先把源散射成"未旋转的 dst 图"（缩放用最近邻），
	 * 再按顺时针的【正向】映射散射到旋转后的帧里。正向写法不容易绕晕。 */
	const int kW = expW * expH;
	std::vector<int> expB(kW, -1), expG(kW, -1);
	bool ok = (out.w == expW && out.h == expH);

	for (int dy = 0; ok && dy < dstH; dy++) {
		for (int dx = 0; ok && dx < dstW; dx++) {
			const int ux = expectScale ? dx * srcW / dstW : dx;
			const int uy = expectScale ? dy * srcH / dstH : dy;

			int ox = dx, oy = dy;
			switch (deg) {
			case 90:  ox = dstH - 1 - dy; oy = dx;         break;
			case 180: ox = dstW - 1 - dx; oy = dstH - 1 - dy; break;
			case 270: ox = dy;            oy = dstW - 1 - dx; break;
			default:                                        break;
			}
			if (ox < 0 || ox >= expW || oy < 0 || oy >= expH) {
				ok = false;
				break;
			}
			expB[oy * expW + ox] = bOf(ux, uy);
			expG[oy * expW + ox] = gOf(ux, uy);
		}
	}

	for (int i = 0; ok && i < kW; i++) {
		if (expB[i] < 0) {
			ok = false;   /* 有像素没被覆盖到 */
			break;
		}
		const uint8_t *p = out.buf.data() + static_cast<size_t>(i) * stride;
		if (p[0] != static_cast<uint8_t>(expB[i]) ||
		    p[1] != static_cast<uint8_t>(expG[i]))
			ok = false;
	}
	check(ok, label);
}

/*
 * libcamera Rotation（设备树，横屏参照系，逆时针）→ ANDROID_SENSOR_ORIENTATION（顺时针），
 * 表达式照抄 Device::init()。undone 在 HAL 里来自 libcamera 的 validate()，这里按
 * computeTransform 的规则给：0 不用消；180 要传感器两个方向都能翻；90 / 270 要转置，消不掉。
 */
static int32_t androidOrientation(int32_t rawRotation, bool front, bool canFlip)
{
	int32_t ccw = ((rawRotation % 360) + 360) % 360;
	if (ccw % 90 != 0)
		ccw = 0;
	const int32_t undone = (ccw == 0 || (ccw == 180 && canFlip)) ? ccw : 0;
	const int32_t natural = (ccw - undone + (front ? 90 : 270) + 360) % 360;
	return (360 - natural) % 360;
}

int main()
{
	printf("=== C. 运行级：编译并执行 deliverJpeg 的缩放/旋转路径 ===\n");
	for (int deg : { 0, 90, 180, 270 }) {
		testPixels(8, 6, 8, 6, deg, false);   /* 不缩放、只旋转（含快捷路径） */
		testPixels(8, 6, 4, 3, deg, true);    /* 等比缩小一半再旋转 */
		testPixels(8, 6, 4, 6, deg, true);    /* 非等比缩放 */
	}

	printf("\n=== D. 运行级：Rotation → ANDROID_SENSOR_ORIENTATION 换算 ===\n");
	struct { int32_t in; bool front, canFlip; int32_t out; } cases[] = {
		/* 本机：后摄 ov13b10 能翻转，前摄 hi846 不能 */
		{ 180, false, true, 90 },    /* 后摄设备树 180，被翻转消掉 */
		{ 0, true, false, 270 },     /* 前摄设备树 0 */
		/* 其他角度：90 / 270 消不掉，180 在不能翻转的传感器上也消不掉 */
		{ 0, false, true, 90 }, { 90, false, true, 0 }, { 270, false, true, 180 },
		{ 90, true, false, 180 }, { 180, true, false, 90 }, { 270, true, false, 0 },
		{ 180, false, false, 270 },
		{ 360, true, false, 270 },   /* 归一化 */
		{ -90, true, false, 0 },     /* -90 逆时针 ≡ 270 逆时针 */
		{ 45, true, false, 270 },    /* 非法 ⇒ 按 0 */
	};
	for (auto &c : cases) {
		const int32_t got = androidOrientation(c.in, c.front, c.canFlip);
		char label[128];
		snprintf(label, sizeof(label), "Rotation=%d %s%s -> %d", c.in,
			 c.front ? "前摄" : "后摄", c.canFlip ? "（能翻转）" : "", got);
		check(got == c.out, label);
	}

	printf("\n=== 汇总 ===\n  检查项 %d，失败 %d\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
