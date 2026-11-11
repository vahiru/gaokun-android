/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 自动对焦（对比度检测 CDAF，全尺寸模式下加相位检测）。
 *
 * ★ 软件 ISP（simple pipeline + softisp）没有 AF 统计：内核只把马达当 v4l2 子设备
 *   暴露，它的 IPA 只有 AGC/AWB。所以信号由 HAL 自己算：【帧本身的清晰度】（任何模式），
 *   以及全尺寸 RAW 里 ov13b10 的 PD 像素算出的相位错位（见下面 Phase detection 一段）。
 *   硬件 ISP（qcom-ife）单 IFE 的模式里，libcamera 报 IFE 对焦统计算出的 FocusFoM，
 *   用它代替帧本身的清晰度（见 onFrame()）；双 IFE 的全尺寸模式没有，仍用帧本身。
 *
 * ★ 判据（与曝光无关）：
 *   降采样亮度网格的拉普拉斯能量 / 亮度总和，再乘 1000（见 sharpness()）。
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
 * the score has fallen well below the best sweep point (the sample where the
 * search started does not count: it says nothing about the side the sweep has
 * not reached), so a scene in focus near the lens costs a few points, not the
 * whole range. Moves longer than kMaxJump go in steps, one per frame, rather
 * than in one jump. A parabola through the best point and its neighbours
 * kFineStep away gives the final position; while a fine point wins, the sweep
 * goes on outward in kFineStep steps (within the range and kMaxMoves) until its
 * outer neighbour has been sampled.
 *
 * AF region: one region (ANDROID_CONTROL_AF_REGIONS, rear camera only). Its
 * sharpness replaces that of the central 70% of the frame; a new region starts
 * a new search.
 *
 * Phase detection: with the full-size RAW frame (full-size sensor mode only,
 * see PhaseDetect.h) a search first moves straight to where the PD disparity
 * points and measures again after each move; it ends when the disparity is
 * below kPdDone. The first move uses kPdCodesPerUnit lens codes per unit of
 * disparity, later ones the slope between the last two measurements (secant):
 * the codes per unit change along the travel (the lens moves less per code
 * below the focus of a 30-50 cm target) and with the scene. It falls back to
 * the contrast sweep, from where the lens is, when the disparity is not
 * trustworthy (featureless scene), the target is outside the sweep range, or
 * it does not settle within kPdMaxSteps moves. In the continuous modes a
 * disparity above kPdRescan also starts a new search, unless the last phase
 * search fell back for one of those three reasons (running out of RAW frames,
 * e.g. in a binned configuration, does not count).
 */
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include <system/camera_metadata.h>

#include "Lens.h"
#include "PhaseDetect.h"

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

	/* 喂一帧（libcamera 的输出，srcWidth_×srcHeight_，行宽 stride 字节）：
	 * bytesPerPixel 为 3 时是软件 ISP 的 RGB24，为 1 时是硬件 ISP 输出的 NV12 的 Y 平面。
	 * sceneStable：AE 是否已经收敛（由会话按"连续 N 帧亮度稳定"给出）。
	 * ⚠️ 为什么必须等它：我们唯一的信号就是帧本身，AGC 还在爬的时候整帧的
	 *    gamma/对比度都在变，锐度分数会跟着漂 —— 实测同一马达位置（1020）
	 *   在 AGC 收敛前后能差 3 倍（612 → 211），那样的"曲线"里根本没有焦点信息。 */
	/*
	 * 分数的来源：
	 * - 硬件 ISP 的 NV12：增益不高时用 AF 区域里亮度的梯度能量（gradientEnergy()），
	 *   增益高（≥ kFoMGainOn，回到 ≤ kFoMGainOff 才换回）且有 FocusFoM 时用 hwScore，
	 *   即硬件 ISP 对焦统计在 AF 区域上算出的 FocusFoM。11-10 逐点钉住马达实测：亮处
	 *   文字上梯度能量偏离峰 20 码低 7–9%，FocusFoM 只低 2–3%；暗处（ISO 1550）白墙上
	 *   没有焦点信号，梯度能量被噪声主导，画面一变就跳 2–3 倍，FocusFoM 约 1.4 倍。
	 *   原来的块平均拉普拉斯（sharpness()）对文字只差约 5%，块平均把细节平均掉了。
	 * - 软件 ISP 的 RGB：块平均的拉普拉斯能量（sharpness()）。
	 * 前两种量纲不同，离焦时也有一层噪声的底，所以“可对焦”指峰值至少是最低采样点（不低
	 * 于 kHwFloor）的 kHwMinRise 倍，而不是高于 kMinScore；重扫按相对变化，绝对下限为
	 * kHwFloor。连续 kHwSourceFrames 帧换了来源才切换并重新搜索，偶尔一帧缺统计只跳过
	 * 那一帧。
	 */
	void onFrame(const uint8_t *pixels, int w, int h, int stride, int bytesPerPixel,
		     bool sceneStable, const RawFrame *raw = nullptr, double hwScore = -1.0,
		     double gain = 1.0);

	/* AF region as fractions of the frame (x0, y0, x1, y1), or none: the
	 * central 70%. A change starts a new search. */
	void setRegion(float x0, float y0, float x1, float y1);
	/*
	 * 应用没给区域：取画面可见部分（x0, y0, x1, y1，画面的比例；不变焦时是
	 * 整幅）中间的 70%。
	 */
	void clearRegion(float x0 = 0.0f, float y0 = 0.0f, float x1 = 1.0f, float y1 = 1.0f);
	/*
	 * 手动对焦（AF 模式 OFF）：镜头移到这个距离（屈光度，LENS_FOCUS_DISTANCE），
	 * 刻度同 focusDistance()，像搜索一样分步移动。
	 */
	void setManualFocus(float diopters);
	/* 当前的 AF 区域 (x0, y0, x1, y1)，画面的比例。 */
	void region(float out[4]) const;

	/* 结果元数据。 */
	uint8_t afState() const;
	/*
	 * Whether the RAW frame that comes with the frame about to be passed to
	 * onFrame() may be used for phase: while scanning or while a continuous
	 * mode is unlocked. A superset of the frames onFrame() measures (not settle
	 * or move frames); Session maps the RAW buffer only then.
	 */
	bool wantsRaw() const;
	/* Whether this stream configuration delivers RAW frames for phase. */
	void setPhaseAvailable(bool available);
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
	/* byPhase：相位错位收敛了（这本身要求场景有纹理）。 */
	void finishScan(bool byPhase = false);
	/* 最高分能否说明这个场景可以对焦。 */
	bool focusable(double best) const;
	bool apply(int pos);              /* 挪马达并记 justMoved_；失败返回 false */
	bool moveTo(int pos);             /* apply() in steps of at most kMaxJump */
	/* tap: the app's own region (a tap), not the default one */
	void regionChanged(bool tap);
	void applyRegion(float x0, float y0, float x1, float y1, bool tap);

	double sharpness(const uint8_t *pixels, int w, int h, int stride, int bytesPerPixel,
			 const float region[4]);
	static double gradientEnergy(const uint8_t *y, int w, int h, int stride,
				     const float region[4]);
	void buildSweep();

	/* 搜索参数。 */
	/*
	 * 扫描范围与步长（马达码值）。后摄马达在 0–128 推不动（弹簧预压，11-10 实测这一段
	 * FoM 完全相同），从 128 起扫，第一个点才真的动了镜头。
	 */
	static constexpr int kLensLow = 128;
	static constexpr int kLensHigh = 900;
	static constexpr int kCoarseStep = 100;
	static constexpr int kFineStep = 25;
	static constexpr int kMaxJump = 150;
	/* Coarse sweep stops early after kDropPoints points below kDropRatio of the peak. */
	static constexpr double kDropRatio = 0.75;
	static constexpr int kDropPoints = 2;
	static constexpr int kMaxMoves = 24;        /* 兜底：探测次数上限 */
	/*
	 * Phase detection (experiments/rear-pdaf-20261103/notes.md). On a textured
	 * target at 30-50 cm the disparity is zero at the contrast peak, about 300
	 * codes per unit above it and about 650 below it, and saturates (keeping
	 * its sign) more than ~200 codes away; a search on 11-04 measured 660-870
	 * codes per unit around 225-511. The first step uses kPdCodesPerUnit
	 * (short of the target rather than past it); later steps use the measured
	 * slope when the disparity changed by at least kPdSecantMinDelta; a slope
	 * outside kPdMinCodesPerUnit..kPdMaxCodesPerUnit (saturated disparity,
	 * noise) is ignored and the previous gain kept.
	 */
	static constexpr double kPdCodesPerUnit = 300.0;
	static constexpr double kPdMinCodesPerUnit = 150.0;
	static constexpr double kPdMaxCodesPerUnit = 1500.0;
	static constexpr double kPdSecantMinDelta = 0.05;
	static constexpr double kPdMinConfidence = 0.5;
	static constexpr double kPdDone = 0.06;     /* about 20-40 codes */
	static constexpr double kPdRescan = 0.15;   /* about 45-100 codes */
	static constexpr int kPdMaxSteps = 4;
	/*
	 * 相位判定合焦后用对比度精调（混合对焦）：相位的容差 kPdDone 约合 20–40 码，而屏幕
	 * 文字上偏离峰 20 码清晰度就掉约 25%（11-10 逐点钉住马达实测），只靠相位拍出来发虚。
	 * 在相位的结果两侧各采一个点（±kFineStep），峰不在中间就沿更高的一侧细扫到峰，
	 * 再抛物线细调。三点里最好的不比最差的高出 kPdConfirmRise 倍（没有纹理，比如
	 * 白墙）：对比度靠不住，停在相位的结果。连续模式里因画面变化而重搜、相位在原位置
	 * 就判定合焦时不精调：镜头不动，放着不动时不会反复前后挪。
	 */
	static constexpr double kPdConfirmRise = 1.04;
	/*
	 * 硬件分数：峰值至少是最低采样点的这么多倍才算可对焦。后摄 11-10 实测（FocusFoM，
	 * 1920x1080）：没有纹理时只有噪声，逐帧比值 1.02–1.03，扫描中 AE 漂移时可到约
	 * 1.1；光滑乳胶漆白墙 1.23–1.36（看最低采样点落在哪）；室内场景约 2.4 倍，文字页
	 * 约 5.3 倍。取 1.15，在噪声与弱纹理之间。
	 */
	static constexpr double kHwMinRise = 1.15;
	/*
	 * 硬件分数的绝对下限：最低采样点按不低于它算（全为 0 的曲线不能算合焦），也是连续
	 * 模式重扫的绝对下限。实测的噪声底都远高于它（暗处增益 15.5 时 272–279，白纸贴住
	 * 镜头 90–92，白墙 36–40），只在 FoM 约为 0（过曝、没有统计）时起作用。
	 */
	static constexpr double kHwFloor = 10.0;
	/* 分数来源连续换了这么多帧才切换。 */
	static constexpr int kHwSourceFrames = 3;
	/*
	 * 改用 FocusFoM / 换回梯度能量的模拟增益，带回差。只有真正暗（约 ISO 1000 以上）才
	 * 改用 FocusFoM：11-10 先试 4.5/3.5，室内灯光下增益正落在附近，十几秒里来回切了
	 * 五六次，每次都重新搜索。
	 */
	static constexpr double kFoMGainOn = 10.0;
	static constexpr double kFoMGainOff = 6.0;
	/* gradientEnergy() 在 AF 区域里最多抽这么多个样本（全尺寸画面也不到 1 ms） */
	static constexpr long kGradSamples = 100000;
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
	/*
	 * Requests after a START in a continuous mode within which a switch to AUTO
	 * keeps that search (setMode()), and after a tap's region change within
	 * which a CONTINUOUS_VIDEO START lets that scan finish (trigger()). CameraX
	 * switched to AUTO in the next request (30 ms, provider log 11-04
	 * 04:00:54); a repeating request queued in between must not drop the tap.
	 */
	static constexpr int kStartCarryRequests = 3;
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
	/* 探测点序列（粗扫最多 9 点 → 峰两侧细扫）与游标。
	 * ★ 用确定性扫描而不是爬山：爬山是局部的，实测会卡在"先探到坏的那一侧"
	 *   （反向也只是从那一侧往回挪），峰在另一侧整片探索不到。 */
	std::vector<int> sweep_;
	int sweepIdx_ = 0;
	bool fineDone_ = false;
	int drops_ = 0;                  /* sweep points in a row below kDropRatio of coarseBest_ */
	double coarseBest_ = 0.0;        /* best score among this sweep's points */
	bool regionScan_ = false;        /* this scan was started by the app's new AF region */
	int regionAge_ = 99;             /* setMode() calls since that region change */
	int startAge_ = 99;              /* setMode() calls since a START in a continuous mode */
	std::map<int, double> samples_;  /* this search's (position, score) samples */
	bool phaseMode_ = false;         /* this search follows the PD disparity */
	bool havePhase_ = false;         /* this configuration delivers RAW frames (setPhaseAvailable) */
	int noRaw_ = 0;                  /* phase-search frames in a row without their RAW frame */
	int pdSteps_ = 0;
	int pdOff_ = 0;                  /* frames in a row with the disparity above kPdRescan */
	double pdGain_ = kPdCodesPerUnit; /* lens codes per unit of disparity, this search */
	int pdLastPos_ = -1;             /* last phase measurement: lens position, disparity */
	double pdLastD_ = 0.0;
	bool pdRescanOk_ = true;         /* the last phase search settled: trust it for rescans */
	/*
	 * 镜头停下的位置上的相位错位：按相位重扫看错位离它多远，而不是离 0 多远。精调可以
	 * 把镜头停在离相位零点几十码的对比度峰上，那里错位并不为零；拿 0 比，停在峰上时相位
	 * 一直报失焦、一轮接一轮地重扫。
	 * 取自把镜头放到这里的那次搜索：搜索中每个采样点量到的可信错位记在 pdAt_，
	 * finishScan() 取最终位置的值（抛物线挪到两点之间时线性插值）。不能等收敛后再量：
	 * 那时画面可能已经变了（转向、锁定期间重新构图），失焦的错位成了基准就不再重扫。
	 * 搜索没量到最终位置的错位时（没按相位收敛、被 START 打断）取 0。
	 */
	double pdRef_ = 0.0;
	std::map<int, double> pdAt_;     /* 这次搜索在各采样位置量到的可信错位 */
	bool pdConfirm_ = false;         /* 这次搜索在用对比度确认相位的结果 */
	int pdDonePos_ = -1;             /* 相位判定合焦的位置，-1 = 没有 */
	int moveTarget_ = -1;            /* where a stepped move is heading, -1 = none */
	int manualPos_ = -1;             /* AF 模式 OFF 时要求的镜头位置，-1 = 无 */
	bool hwMetric_ = false;          /* 分数是相对量纲的（FocusFoM 或梯度能量） */
	int scoreSource_ = 0;            /* 0 拉普拉斯，1 FocusFoM，2 梯度能量 */
	bool highGain_ = false;          /* 增益高：有 FocusFoM 时用它 */
	int otherSource_ = 0;            /* 连续几帧的分数来自另一种来源 */
	/* AF region in fractions of the frame; default the central 70%. */
	float region_[4] = { 0.15f, 0.15f, 0.85f, 0.85f };
	bool defaultRegion_ = true;      /* region_ 是默认区域，不是应用给的 */
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
