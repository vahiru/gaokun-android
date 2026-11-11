/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 亮度锐化（反锐化掩模，带门限）。
 *
 * 硬件 ISP 的锐化在 IPE 里，要经 ICP 才能用；IFE 输出的 NV12 没有锐化。这里在缩放到
 * 输出分辨率之后，只对 Y 平面做：out = y + amount × coring(y − blur(y))。blur 是二项式
 * 核（FAST 3×3，HIGH_QUALITY 5×5）；coring 把幅度不超过门限的差值压成 0、
 * 更大的减去门限，小幅的起伏（多半是噪点）不放大。门限取 threshold 与这幅图
 * 噪声估计（estimateNoise()）的约 2 倍中较大的一个：原尺寸照片的每像素噪声比
 * 缩小后的预览大得多，固定门限挡不住。增益越高，amount 越小。
 *
 * 拍照另有亮度降噪（denoiseLuma()，3×3 ε 滤波，门限按噪声估计）。
 */
#pragma once

#include <cstdint>
#include <vector>

namespace gaokun3 {

struct SharpenParams {
	int radius = 0;        /* 0 = 不锐化；1 = 3×3，2 = 5×5 */
	int amountQ4 = 0;      /* 强度，乘 16 */
	int threshold = 0;     /* 门限，亮度级 */
};

/*
 * 按 ANDROID_EDGE_MODE 与这一帧的模拟增益选参数。debugScale 乘在强度上
 * （调试属性 debug.gaokun3.camera.sharpen，盲测用；0 = 关）。
 */
SharpenParams sharpenParams(uint8_t edgeMode, double gain, double debugScale);

/*
 * 从 src（行宽 ss）读 w×h 的亮度，锐化后写到 dst（行宽 ds）。src 与 dst 不能重叠；
 * dst 只写不读（可以是 gralloc 的映射）。scratch 跨帧复用，免得每帧分配。
 */
void sharpenLuma(const uint8_t *src, int ss, uint8_t *dst, int ds, int w, int h,
		 const SharpenParams &p, std::vector<uint16_t> &scratch);

/*
 * 亮度噪声的标准差（亮度级）：Immerkær 的 3×3 拉普拉斯差分掩模在抽样点上的绝对值
 * 取中位数，按高斯噪声换算。纹理多的地方偏大，取中位数让平坦处说了算。
 */
double estimateNoise(const uint8_t *y, int stride, int w, int h);

/*
 * 3×3 ε 滤波：每个像素取与它相差不超过 t 的邻居（含自己）的平均，平坦处的噪点被
 * 平均掉，边缘两侧差值大于 t 不受影响。按行分给 4 个线程。src 与 dst 不能重叠。
 */
void denoiseLuma(const uint8_t *src, int ss, uint8_t *dst, int ds, int w, int h, int t);

} /* namespace gaokun3 */
