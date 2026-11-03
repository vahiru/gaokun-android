/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 自动对焦（对比度检测 CDAF）。
 *
 * ★ 为什么只能做 CDAF：
 *   ov13b10 的成像链路里，相位/对比度统计【没有任何一层给】——
 *   内核只把马达当 v4l2 子设备暴露（无 AF 统计），软件 ISP（simple pipeline +
 *   softisp）的 IPA 只有 AGC/AWB，libcamera 也不产 AF 统计。
 *   所以唯一可用的信号就是【帧本身的清晰度】：读一帧 → 算清晰度 → 爬山搜马达位置。
 *
 * ★ 判据（与曝光无关）：
 *   梯度能量 / 亮度能量 = Σ(|gx|+|gy|) / Σ(luma)，再乘 1000。
 *   亮度整体伸缩（AGC 换曝光/增益）时分子分母同比例变，比值不动 ——
 *   否则"越亮越清晰"会把马达一路推到曝光最好而画面最糊的位置。
 *
 * ★ 搜索：**确定性扫描**（不是爬山）。按 kCoarseStep 粗扫搜索范围，
 *   再在最高分点附近按 kFineStep 细扫。
 *   ⚠️ 曾经用粗到细爬山，实测会卡死：起始点往一侧探到更差后反向，只是从
 *   那一侧往回挪，峰在另一侧的整片区域从未被探索（收敛到 896/34.9，而真峰
 *   在 320/347）。行程这么短、帧率这么高，没有理由用局部算法。
 *   每个探测点的 (位置, 分数) 都会打进日志，标定与排障直接看日志。
 *
 * Search range and order: the sweep covers kLensLow..kLensHigh, not the whole
 * travel. Below about 100 the lens does not move at all (it rests on its stop:
 * identical frames at 0 and 100), and the ends are where it audibly hits the
 * stops. The sweep starts from the end nearer the lens and stops early once
 * the score has fallen well below its peak, so a scene in focus near where the
 * lens already is costs a few points, not the whole range. Moves longer than
 * kMaxJump go in steps, one per frame, rather than in one jump.
 *
 * AF region: one region (ANDROID_CONTROL_AF_REGIONS, rear camera only). Its
 * sharpness replaces that of the central 70% of the frame; a new region starts
 * a new search.
 */
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <system/camera_metadata.h>

#include "Lens.h"

namespace gaokun3 {

class AutoFocus {
public:
	void attach(Lens *lens);

	/* 马达可用 = 有节点且能写。不可用时对焦相关元数据一律按"定焦"如实报。 */
	bool ready() const;

	/* 来自请求（粘滞：请求里没带就沿用上一次）。ANDROID_CONTROL_AF_MODE_* */
	void setMode(uint8_t androidAfMode);
	/* ANDROID_CONTROL_AF_TRIGGER_*；START 起一次单次对焦，CANCEL 取消锁定。 */
	void trigger(uint8_t androidAfTrigger);

	/* 喂一帧 RGB24（libcamera 的输出，srcWidth_×srcHeight_）。
	 * sceneStable：AE 是否已经收敛（由会话按"连续 N 帧亮度稳定"给出）。
	 * ⚠️ 为什么必须等它：我们唯一的信号就是帧本身，AGC 还在爬的时候整帧的
	 *    gamma/对比度都在变，锐度分数会跟着漂 —— 实测同一马达位置（1020）
	 *   在 AGC 收敛前后能差 3 倍（612 → 211），那样的"曲线"里根本没有焦点信息。 */
	void onFrame(const uint8_t *rgb, int w, int h, bool sceneStable);

	/* AF region as fractions of the frame (x0, y0, x1, y1), or none: the
	 * central 70%. A change starts a new search. */
	void setRegion(float x0, float y0, float x1, float y1);
	void clearRegion();

	/* 结果元数据。 */
	uint8_t afState() const;
	uint8_t lensState() const;
	float focusDistance() const;      /* 屈光度（1/m），UNCALIBRATED 下的标称值 */

private:
	enum class Mode : uint8_t {
		Off = 0,
		Auto,
		ContinuousPicture,
		ContinuousVideo,
	};

	bool shouldScan() const;          /* 当前模式下该不该跑搜索 */
	void startScan();
	void finishScan();
	bool apply(int pos);              /* 挪马达并记 justMoved_；失败返回 false */
	bool moveTo(int pos);             /* apply() in steps of at most kMaxJump */
	void regionChanged();

	double sharpness(const uint8_t *rgb, int w, int h);

	/* 搜索参数。 */
	/* Sweep range and steps (lens codes). */
	static constexpr int kLensLow = 100;
	static constexpr int kLensHigh = 900;
	static constexpr int kCoarseStep = 100;
	static constexpr int kFineStep = 25;
	static constexpr int kMaxJump = 150;
	/* Coarse sweep stops early after kDropPoints points below kDropRatio of the peak. */
	static constexpr double kDropRatio = 0.75;
	static constexpr int kDropPoints = 2;
	static constexpr int kMaxMoves = 24;        /* 兜底：探测次数上限 */
	static constexpr double kMinScore = 20.0;   /* "这场景没法对焦"（白墙）的下限；
						     ⚠️ 量纲随指标变（拉普拉斯/亮度）。实测本机
						     标准卡场景：真峰 347、远端平坦区 33、
						     近端 87 ⇒ 取 20 能把"平坦低分平台"
						     判成未合焦。要按实测再校准。 */
	/* 连续模式重扫：相对收敛后基准分数变化超过 30%（涨跌都算），且至少 kRescanAbs，
	 * 连续 kRescanFrames 帧。绝对下限 = kMinScore/2，挡住低分场景里的噪声。 */
	static constexpr double kRescanRel = 0.30;
	static constexpr double kRescanAbs = kMinScore / 2;
	static constexpr int kRescanFrames = 4;
	static constexpr float kMinFocusDiopters = 10.0f;  /* 与 Metadata.cpp 声明一致 */
	/* 降采样：网格步长 16 像素、每格内部再 4×4 平均。
	 * ⚠️ 曾经是"每 16 像素只取 1 个像素"，那样单像素噪声全进来 —— 实测噪声
	 * 比细节还大，曲线被噪声主导（同一场景 1023 的分数反而高于 512）。 */
	static constexpr int kBlock = 16;
	static constexpr int kAvg = 4;
	/* ⚠️ 流水线延迟：setPosition 之后相机流水线里还有 3–4 帧是旧位置曝的，
	 * 立刻用下一帧的分数就等于把"位置"和"分数"错配。必须等几帧再采样。 */
	/* 4: after moves of 50 codes or more the score was often still rising on the third frame. */
	static constexpr int kSettleFrames = 4;

	Lens *lens_ = nullptr;

	mutable std::mutex m_;
	Mode mode_ = Mode::Off;
	bool triggered_ = false;

	bool scanning_ = false;
	bool converged_ = false;
	/* 探测点序列（粗扫 9 点 → 峰附近细扫 5 点）与游标。
	 * ★ 用确定性扫描而不是爬山：爬山是局部的，实测会卡在"先探到坏的那一侧"
	 *   （反向也只是从那一侧往回挪），峰在另一侧整片探索不到。 */
	std::vector<int> sweep_;
	int sweepIdx_ = 0;
	bool fineDone_ = false;
	int drops_ = 0;                  /* coarse points in a row below kDropRatio of the peak */
	std::map<int, double> samples_;  /* this search's (position, score) samples */
	int moveTarget_ = -1;            /* where a stepped move is heading, -1 = none */
	/* AF region in fractions of the frame; default the central 70%. */
	float region_[4] = { 0.15f, 0.15f, 0.85f, 0.85f };
	/* ★ 调试口：`debug.gaokun3.camera.af.hold <pos>` 把马达钉在某个位置、不搜对焦。
	 *   标定"这个马达到底动没动 / 焦点在哪"必须靠它 —— 自动搜索的曲线分不清
	 *   "焦点变了"和"AGC 漂了"。置 -1 或空串恢复正常 AF（见 AutoFocus.cpp）。 */
	bool hold_ = false;
	int holdPos_ = -1;
	int stableSeen_ = 0;
	int settle_ = 0;                 /* 还要等几帧才能采信这一帧的分数 */
	std::vector<float> grid_;        /* 降采样后的亮度网格（复用，避免每帧分配） */
	int moves_ = 0;
	double bestScore_ = 0.0;
	int bestPos_ = 0;
	double refScore_ = -1.0;         /* 收敛后第一帧稳定画面的分数；<0 = 还没取 */
	bool justMoved_ = false;
	int stable_ = 0;
	uint8_t lensState_ = ANDROID_LENS_STATE_STATIONARY;
};

} /* namespace gaokun3 */
