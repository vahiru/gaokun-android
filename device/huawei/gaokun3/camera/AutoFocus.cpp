/* SPDX-License-Identifier: Apache-2.0 */
#include "AutoFocus.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <sys/system_properties.h>

#include <log/log.h>

namespace gaokun3 {

void AutoFocus::attach(Lens *lens)
{
	std::lock_guard<std::mutex> lk(m_);
	lens_ = lens;
	if (lens_)
		bestPos_ = lens_->position();
}

bool AutoFocus::ready() const
{
	std::lock_guard<std::mutex> lk(m_);
	return lens_ && lens_->usable();
}

bool AutoFocus::shouldScan() const
{
	switch (mode_) {
	case Mode::ContinuousPicture:
	case Mode::ContinuousVideo:
		return true;
	case Mode::Auto:
		return triggered_;
	default:
		return false;
	}
}

void AutoFocus::setMode(uint8_t androidAfMode)
{
	Mode nm = Mode::Off;
	switch (androidAfMode) {
	case ANDROID_CONTROL_AF_MODE_AUTO:
		nm = Mode::Auto;
		break;
	case ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE:
		nm = Mode::ContinuousPicture;
		break;
	case ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO:
		nm = Mode::ContinuousVideo;
		break;
	default:
		nm = Mode::Off;
		break;
	}

	std::lock_guard<std::mutex> lk(m_);
	if (startAge_ < 99)
		startAge_++;
	if (regionAge_ < 99)
		regionAge_++;
	if (nm == mode_)
		return;
	ALOGI("AF 模式 %u -> %d（0=OFF 1=AUTO 2=CONT_PIC 3=CONT_VID）",
	      androidAfMode, static_cast<int>(nm));
	/*
	 * CameraX taps to focus with the region and AF_TRIGGER_START in a request
	 * that still carries the continuous mode, and switches to AUTO in the next
	 * one. Keep that triggered search (or its lock) going in AUTO instead of
	 * dropping it to INACTIVE; only a START from the last kStartCarryRequests
	 * requests, not an old lock.
	 */
	const bool carry = triggered_ && startAge_ <= kStartCarryRequests &&
			   nm == Mode::Auto &&
			   (mode_ == Mode::ContinuousPicture || mode_ == Mode::ContinuousVideo);
	mode_ = nm;
	/* 手动对焦的位置只在这一段 OFF 里有效：再进 OFF 时没给距离就不动镜头 */
	manualPos_ = -1;
	if (carry) {
		ALOGI("AF 带着触发进入 AUTO（%s）", scanning_ ? "继续扫" : "保持锁定");
		return;
	}
	moveTarget_ = -1;
	triggered_ = false;
	scanning_ = false;
	converged_ = false;
	stable_ = 0;
	refScore_ = -1.0;
	/* 只有连续模式自己开始搜；AUTO 要等应用发 AF_TRIGGER=START（camera3 的 AUTO 状态机：
	 * 没有触发就停在 INACTIVE，镜头不动）。 */
	if (nm == Mode::ContinuousPicture || nm == Mode::ContinuousVideo)
		startScan();
}

void AutoFocus::trigger(uint8_t androidAfTrigger)
{
	std::lock_guard<std::mutex> lk(m_);
	/*
	 * 按 CaptureResult 文档里各模式的 AF 状态机（ANDROID_CONTROL_AF_STATE 那几张表）：
	 *   AUTO             START → ACTIVE_SCAN → FOCUSED/NOT_FOCUSED_LOCKED；CANCEL → INACTIVE。
	 *   CONTINUOUS_PIC   START：正在扫 → 扫完再锁；没在扫 → 按当前结果【立刻】锁。
	 *   CONTINUOUS_VIDEO START：立刻结束当前扫描并锁（点选除外：应用给的新区域开始的扫描，
	 *                    START 在区域变化后 kStartCarryRequests 个请求内到达 ⇒ 扫完再锁，
	 *                    否则点选的区域根本不会被扫）。
	 *   连续模式 CANCEL → 解锁、恢复连续对焦（不清掉已有的合焦结果）。
	 * ⚠️ 旧实现在连续模式下收到 START 也从头扫满全程（约 2 秒快门延迟，且报了连续模式
	 *    状态机里不存在的 ACTIVE_SCAN）—— 应用在拍照前几乎总会发一次 START。
	 */
	if (mode_ == Mode::Off)
		return;
	if (androidAfTrigger == ANDROID_CONTROL_AF_TRIGGER_START) {
		ALOGI("AF 触发 START（模式 %d，%s）", static_cast<int>(mode_),
		      scanning_ ? "正在扫" : "未在扫");
		triggered_ = true;
		if (mode_ != Mode::Auto)
			startAge_ = 0;
		if (mode_ == Mode::Auto) {
			startScan();
		} else if (scanning_ && mode_ == Mode::ContinuousVideo &&
			   !(regionScan_ && regionAge_ <= kStartCarryRequests)) {
			/* 视频：立刻停在目前最好的位置并锁。相位已判合焦、正在用对比度精调时，
			 * 按相位的结果算合焦（与精调走完时一样）。 */
			finishScan(pdDonePos_ >= 0);
		}
		/* A tap (a scan started by the app's new region, the START within
		 * kStartCarryRequests requests of it) runs to its end and then
		 * locks, as triggered_ is set. */
		/* 连续拍照且正在扫：什么都不做，finishScan() 时 triggered_ 已置位 ⇒ 直接锁定。
		 * 没在扫：triggered_ 置位即锁定，afState() 按 converged_ 报 FOCUSED/NOT_FOCUSED。 */
	} else if (androidAfTrigger == ANDROID_CONTROL_AF_TRIGGER_CANCEL) {
		ALOGI("AF 触发 CANCEL");
		triggered_ = false;
		stable_ = 0;
		if (mode_ == Mode::Auto) {
			scanning_ = false;
			converged_ = false;
			moveTarget_ = -1;
		}
	}
}

/* 调用者必须已持锁。 */
void AutoFocus::startScan()
{
	if (!lens_ || !lens_->usable())
		return;
	scanning_ = true;
	converged_ = false;
	moves_ = 0;
	bestScore_ = 0.0;
	bestPos_ = lens_->position();
	stable_ = 0;
	fineDone_ = false;
	samples_.clear();
	phaseMode_ = havePhase_;
	pdSteps_ = 0;
	pdOff_ = 0;
	pdGain_ = kPdCodesPerUnit;
	pdLastPos_ = -1;
	pdConfirm_ = false;
	pdDonePos_ = -1;
	pdAt_.clear();
	restarted_ = false;
	noRaw_ = 0;
	regionScan_ = false;
	buildSweep();
	settle_ = kSettleFrames;   /* 起始位置也要等流水线把画面换过来再采第一个点 */
}

/*
 * Coarse sweep over kLensLow..kLensHigh in kCoarseStep, from the end nearer the
 * lens; the fine sweep around the peak is generated when it ends. Caller holds
 * the lock.
 */
void AutoFocus::buildSweep()
{
	sweep_.clear();
	const int lo = std::max(lens_->minPos(), kLensLow);
	const int hi = std::min(lens_->maxPos(), kLensHigh);
	for (int p = lo; p < hi; p += kCoarseStep)
		sweep_.push_back(p);
	sweep_.push_back(hi);
	if (lens_->position() - lo > hi - lens_->position())
		std::reverse(sweep_.begin(), sweep_.end());
	sweepIdx_ = 0;
	fineDone_ = false;
	drops_ = 0;
	coarseBest_ = 0.0;
}

bool AutoFocus::apply(int pos)
{
	if (!lens_ || !lens_->setPosition(pos))
		return false;
	justMoved_ = true;
	return true;
}

/*
 * The dw9714 moves straight to each code, so a long move lands hard. Go at most
 * kMaxJump per frame; onFrame() continues the move and waits kSettleFrames
 * once it arrives. Caller holds the lock.
 */
bool AutoFocus::moveTo(int pos)
{
	const int cur = lens_->position();
	const int step = std::clamp(pos - cur, -kMaxJump, kMaxJump);
	if (!apply(cur + step)) {
		moveTarget_ = -1;
		return false;
	}
	moveTarget_ = cur + step == pos ? -1 : pos;
	settle_ = kSettleFrames;
	return true;
}

/* 调用者必须已持锁。 */
bool AutoFocus::focusable(double best) const
{
	if (samples_.size() < 2)
		return false;
	double lowest = best;
	for (const auto &[pos, score] : samples_)
		lowest = std::min(lowest, score);
	return best >= kHwMinRise * std::max(lowest, kHwFloor);
}

/* 调用者必须已持锁。 */
void AutoFocus::finishScan(bool byPhase)
{
	scanning_ = false;
	converged_ = byPhase || focusable(bestScore_);
	/* 峰在采样点之间：两侧相距 kFineStep 的点都采过时，取过最佳点和这两点的抛物线顶点。 */
	const int sampledBest = bestPos_;
	const auto l = samples_.find(bestPos_ - kFineStep);
	const auto r = samples_.find(bestPos_ + kFineStep);
	if (converged_ && l != samples_.end() && r != samples_.end()) {
		const double curv = l->second - 2 * bestScore_ + r->second;
		if (curv < 0) {
			const double off = std::clamp(0.5 * (l->second - r->second) / curv, -0.5, 0.5);
			bestPos_ += static_cast<int>(std::lround(off * kFineStep));
		}
	}
	stable_ = 0;
	/*
	 * 挪回最佳点之后，流水线里还有 kSettleFrames 帧是【细扫最后一个点】曝出来的。
	 * ⚠️ 旧实现这里 settle_=0，紧接着拿这几帧旧画面去判"画面变化"：若 best+64 那一点的
	 *    分数低于峰值的 70%，刚收敛就触发重扫，可能一轮接一轮地扫。
	 * ⇒ 等流水线换过来，再把第一帧当"收敛后的基准分数"（refScore_），重扫只跟它比。
	 */
	settle_ = 0;
	if (lens_ && lens_->position() != bestPos_)
		moveTo(bestPos_);
	refScore_ = -1.0;
	/* 相位重扫的基准：这次搜索在最终位置量到的错位，没量到为 0（见 pdRef_） */
	pdRef_ = 0.0;
	if (byPhase) {
		const auto at = pdAt_.find(sampledBest);
		if (at != pdAt_.end()) {
			pdRef_ = at->second;
			const int nb = bestPos_ > sampledBest ? sampledBest + kFineStep
							      : sampledBest - kFineStep;
			const auto nbAt = pdAt_.find(nb);
			if (bestPos_ != sampledBest && nbAt != pdAt_.end())
				pdRef_ += (nbAt->second - pdRef_) * (bestPos_ - sampledBest) /
					  static_cast<double>(nb - sampledBest);
		}
	}
	ALOGI("AF 搜索结束：最佳位置 %d 分数 %.1f（%s）共 %d 次移动",
	      bestPos_, bestScore_, converged_ ? "已合焦" : "未合焦（场景对比度太低）",
	      moves_);
}

/*
 * AF 区域里亮度的梯度能量：相邻像素差的平方和的均值，除以亮度均值的平方（与曝光、
 * 增益基本无关），乘 1e4。在区域里按 step 抽样，每个样本只用它和右边、下边的像素，
 * 全分辨率的细节都算进去，样本数不超过约 kGradSamples。
 */
double AutoFocus::gradientEnergy(const uint8_t *y, int w, int h, int stride, const float region[4],
				 double viewW)
{
	if (w < 4 || h < 4)
		return 0.0;
	/*
	 * 差分距离随画面宽度：相邻像素差在全尺寸画面上反映的是最细一级的细节和噪声，
	 * 失焦对它影响小。11-10 对着文字逐点钉住马达：4208 宽的画面隔 1 像素时偏离峰
	 * 20 码只低 3%，隔 4 像素低 7%；1952 宽的画面隔 1–2 像素低 9%。
	 */
	/* IFE 变焦时画面只是视场的一部分：按整个视场的等效宽度取（放大到 1:1 传感器像素时与全尺寸画面相同） */
	const double fullW = w / std::clamp(viewW, 0.05, 1.0);
	const int d = std::max(1, static_cast<int>(std::lround(fullW / 1000.0)));
	if (w < d + 2 || h < d + 2)
		return 0.0;
	const int x0 = std::clamp(static_cast<int>(region[0] * w), 0, w - d - 1);
	const int x1 = std::clamp(static_cast<int>(region[2] * w), x0 + 1, w - d);
	const int y0 = std::clamp(static_cast<int>(region[1] * h), 0, h - d - 1);
	const int y1 = std::clamp(static_cast<int>(region[3] * h), y0 + 1, h - d);
	const double area = static_cast<double>(x1 - x0) * (y1 - y0);
	const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(area / kGradSamples))));
	uint64_t g = 0, m = 0, n = 0;
	for (int yy = y0; yy < y1; yy += step) {
		const uint8_t *r0 = y + static_cast<size_t>(yy) * stride;
		const uint8_t *r1 = r0 + static_cast<size_t>(d) * stride;
		for (int xx = x0; xx < x1; xx += step) {
			const int dx = r0[xx + d] - r0[xx], dy = r1[xx] - r0[xx];
			g += static_cast<uint64_t>(dx * dx + dy * dy);
			m += r0[xx];
			n++;
		}
	}
	if (!n)
		return 0.0;
	const double mean = static_cast<double>(m) / n;
	return mean < 1.0 ? 0.0 : static_cast<double>(g) / n / (mean * mean) * 1e4;
}

void AutoFocus::onFrame(const uint8_t *pixels, int w, int h, int stride, bool sceneStable,
			const RawFrame *raw, double hwScore, double gain, const float *view)
{
	if (!pixels || w <= 0 || h <= 0)
		return;
	/* setRegion() writes region_ under the lock from the request thread. */
	float region[4];
	double viewW = 1.0;
	{
		std::lock_guard<std::mutex> lk(m_);
		std::copy(region_, region_ + 4, region);
		viewW = lastView_[2] - lastView_[0];
	}
	/* 画面只显示视场里的一块时，打分用的区域换算到这幅画面上（见 onFrame() 的说明） */
	float imgRegion[4];
	std::copy(region, region + 4, imgRegion);
	if (view && view[2] > view[0] && view[3] > view[1] &&
	    (view[0] > 0.0f || view[1] > 0.0f || view[2] < 1.0f || view[3] < 1.0f)) {
		for (int i = 0; i < 4; i++) {
			const int a = i & 1;   /* 0 = x，1 = y */
			imgRegion[i] = std::clamp((region[i] - view[a]) / (view[a + 2] - view[a]),
						  0.0f, 1.0f);
		}
		/*
		 * 区域（几乎）整个落在可见部分之外（例如点按边缘后放大）：Android 规定这样的区域
		 * 应当忽略，打分改用可见部分中间 70%，与默认区域相同。
		 */
		if (imgRegion[2] - imgRegion[0] < 0.02f || imgRegion[3] - imgRegion[1] < 0.02f) {
			imgRegion[0] = imgRegion[1] = 0.15f;
			imgRegion[2] = imgRegion[3] = 0.85f;
		}
	}
	/*
	 * 差分距离按登记过的视场（lastView_）取，不按这一帧：每边不到 kViewEps 的小幅变焦
	 * 不算视场变化，d 却可能跳档、分数跟着跳。视场一变就跳过几帧、重取基准。
	 */
	/* 增益高（暗处）且有 FocusFoM 时用它，否则用梯度能量（见 AutoFocus.h） */
	if (gain >= kFoMGainOn)
		highGain_ = true;
	else if (gain <= kFoMGainOff)
		highGain_ = false;
	const int source = hwScore >= 0.0 && highGain_ ? 1 : 2;
	const double score =
		source == 1 ? hwScore : gradientEnergy(pixels, w, h, stride, imgRegion, viewW);

	std::lock_guard<std::mutex> lk(m_);

	/* Phase only where it is used (rescan check, phase search), not in settle
	 * or move frames, nor while AUTO holds its lock. */
	auto measure = [&]() {
		return raw ? measurePhase(*raw, region_[0], region_[1], region_[2], region_[3])
			   : PhaseResult{};
	};
	auto trusted = [](const PhaseResult &r) {
		return r.valid && r.confidence >= kPdMinConfidence;
	};

	/* 镜头状态描述【这一帧曝光时】的马达：上一帧我们刚发过移动指令，
	 * 这一帧还没走到位 ⇒ MOVING；再下一帧起 STATIONARY。 */
	lensState_ = justMoved_ ? ANDROID_LENS_STATE_MOVING : ANDROID_LENS_STATE_STATIONARY;
	justMoved_ = false;

	/*
	 * 不同来源的分数量纲不同，不能放进同一条曲线比较。另一种来源只出现一两帧（例如某帧
	 * 统计出错、没有 FocusFoM）就跳过这几帧；连续 kHwSourceFrames 帧都是另一种来源才切换，
	 * 并重新搜索。
	 */
	if (source != scoreSource_) {
		if (++otherSource_ < kHwSourceFrames)
			return;
		otherSource_ = 0;
		scoreSource_ = source;
		ALOGI("AF score source: %s", source == 1 ? "hardware focus statistics (FocusFoM)"
							 : "luma gradient energy of the AF region");
		refScore_ = -1.0;
		if (scanning_) {
			/* 重新搜索仍是点按的那一次：保留点按标记（startScan() 会清掉），照常精调 */
			const bool tap = regionScan_;
			startScan();
			regionScan_ = tap;
			restarted_ = true;
		}
	} else {
		otherSource_ = 0;
	}

	/*
	 * IFE 变焦改变了打分画面的视场：梯度能量换了画面，不能和之前的分数比
	 * （FocusFoM 按整个视场统计，不受影响）。不在搜索时重取基准（变焦本身不触发重扫）；
	 * 在搜索时等视场连续 kViewSettleFrames 帧不变后重新开始这次搜索，保留点按标记。
	 */
	bool viewSkip = false;
	{
		const float v[4] = { view ? view[0] : 0.0f, view ? view[1] : 0.0f,
				     view ? view[2] : 1.0f, view ? view[3] : 1.0f };
		bool changed = false;
		for (int i = 0; i < 4; i++)
			changed = changed || std::fabs(v[i] - lastView_[i]) > kViewEps;
		if (changed) {
			std::copy(v, v + 4, lastView_);
			viewSettle_ = kViewSettleFrames;
			if (source != 1) {
				refScore_ = -1.0;
				stable_ = 0;
			}
			/*
			 * FocusFoM 由 IFE 在请求给的窗口里算，不随画面视场变，本不必重新开始；
			 * 但搜索中推迟了新的默认区域时，不重新开始的话整次搜索都按看不见的旧区域测。
			 */
			if (scanning_ && (source != 1 || hasPendingDefault_))
				viewRestart_ = true;
		}
		if (viewSettle_ > 0) {
			viewSettle_--;
			/* 跳过的是打分（在 shouldScan() 之后）：分步移动、手动对焦、调试钉住照常 */
			viewSkip = source != 1;
		} else if (viewRestart_) {
			viewRestart_ = false;
			if (scanning_) {
				ALOGI("AF view changed by zoom, searching again");
				const bool tap = regionScan_;
				/* 搜索中推迟的默认区域（新的可见部分）先换上 */
				if (hasPendingDefault_ && defaultRegion_)
					std::copy(pendingDefault_, pendingDefault_ + 4, region_);
				hasPendingDefault_ = false;
				startScan();
				regionScan_ = tap;
				restarted_ = true;
			}
		}
	}

	if (!lens_ || !lens_->usable())
		return;

	/*
	 * ── 调试口：把马达钉在指定位置（标定/排障用）──
	 *   setprop debug.gaokun3.camera.af.hold 700   → 停在 700，AF 不搜（报 INACTIVE）
	 *   setprop debug.gaokun3.camera.af.hold ""    → 恢复正常 AF（-1 同义）
	 * ★ 为什么必须留这个口子：自动搜索的"锐度曲线"分不清"焦点变了"和
	 *   "AE 漂了"——实测同一位置 1020 在 AGC 收敛前后差 3 倍。要证明马达真的
	 *   在动、以及焦点落在哪，唯一严谨的办法是把位置钉住逐点量图。
	 */
	{
		char buf[PROP_VALUE_MAX] = {};
		if (__system_property_get("debug.gaokun3.camera.af.hold", buf) > 0) {
			const int v = atoi(buf);
			holdPos_ = (lens_ && (v < lens_->minPos() || v > lens_->maxPos()))
					   ? -1 : v;
		} else {
			/* ★ 属性被清空（`setprop … ""`）或从未设置 ⇒ **必须解除钉住**。
			 *   早先这里只是"读不到就不改 holdPos_"，于是 setprop "" 之后
			 *   马达被永久钉在上一个位置 —— 实测 A/B 实验里"放开后画面纹丝不动",
			 *   排查半天才发现钉住根本没放开。 */
			holdPos_ = -1;
		}
	}
	if (holdPos_ >= 0) {
		if (!hold_)
			ALOGW("AF 被属性钉住：保持马达位置 %d，不再自动搜索", holdPos_);
		hold_ = true;
		scanning_ = false;
		moveTarget_ = -1;
		stableSeen_ = 0;
		if (lens_->position() != holdPos_) {
			if (lens_->setPosition(holdPos_)) {
				justMoved_ = true;
				settle_ = kSettleFrames;
			}
			return;
		}
		if (settle_ > 0) {
			settle_--;
			return;
		}
		/* ★ 钉住状态下每帧打分数 = 直接得到"位置 → 分数"曲线，
		 *   外部脚本只要逐个 setprop 就能把整条对焦曲线标出来。 */
		ALOGI("AF 钉住采样 pos=%d 分数=%.1f", holdPos_, score);
		return;
	}
	if (hold_) {
		ALOGI("AF 钉住解除，恢复自动搜索");
		hold_ = false;
		/* 连续模式：钉住期间镜头在别处，放开就重搜（不能把钉住位置的分数当成收敛基准）。 */
		if (mode_ == Mode::ContinuousPicture || mode_ == Mode::ContinuousVideo)
			startScan();
	}

	if (mode_ == Mode::Off) {
		/* 手动对焦：继续分步移动，或者开始一次 */
		if (moveTarget_ >= 0)
			moveTo(moveTarget_);
		else if (manualPos_ >= 0 && lens_->position() != manualPos_)
			moveTo(manualPos_);
		return;
	}

	/* A stepped move continues every frame, whatever else is going on. */
	if (moveTarget_ >= 0) {
		moveTo(moveTarget_);
		return;
	}

	if (!shouldScan()) {
		scanning_ = false;
		return;
	}
	/* 视场刚变（IFE 变焦），这几帧的分数不能用（见上） */
	if (viewSkip)
		return;

	/*
	 * AE 未收敛：这一帧的分数不可信（整帧 gamma 在变 ⇒ 锐度分数跟着漂）。
	 * ★★ 只【跳过这一帧】，绝不"作废重来"：
	 *   作废会把 step/dir/best/位置选择全部重置并立刻再挪马达，而 AGC 是
	 *   频繁台阶式调整的（实测 12 秒内十几次）⇒ 搜索被一直打断，最后被推
	 *   到行程端点、在一个平坦的低分平台上"收敛"（实测收敛到 896 / 34.9，
	 *   而真峰值在 320 / 347 —— 差 10 倍。这是把"等 AE"写成了"重启搜索"的错）。
	 *   跳过的话搜索状态原样保留，等稳定的帧接着推进，代价只是慢一点。
	 */
	if (!sceneStable) {
		if (!scanning_) {
			if (++stableSeen_ % 40 == 1)
				ALOGI("AF 等 AE 收敛中（第 %d 帧，画面还在变）", stableSeen_);
			return;
		}
		if (++stableSeen_ % 20 == 1)
			ALOGD("AF 跳过不稳定帧（第 %d 帧）", stableSeen_);
		return;
	}
	stableSeen_ = 0;

	if (!scanning_) {
		/* 连续模式：收敛后画面变了要能重新搜。
		 * 单次对焦(triggered_)时锁定不重搜，等应用发 CANCEL/START。 */
		if (triggered_)
			return;
		if (settle_ > 0) {
			settle_--;
			return;
		}
		if (refScore_ < 0.0) {
			refScore_ = score;        /* 收敛后第一帧稳定画面 = 基准 */
			stable_ = 0;
			return;
		}
		/*
		 * ★ 两个方向都要看：
		 *   变差（被摄物移动/换景）—— 旧实现只看这一个方向，而且是跟【扫描时】的峰值比；
		 *   变好（比如对着白墙收敛成"未合焦"、分数 15，再转向有纹理的物体：失焦的新物体
		 *   分数照样高于 0.7×15，旧实现永远不重扫，停在 PASSIVE_UNFOCUSED）。
		 * 阈值 = max(相对 kRescanRel，绝对 kHwFloor)：绝对下限挡住低分场景的噪声，
		 * 也让 refScore_≈0（全平场景、或钉住后刚放开）时照样能触发。
		 */
		/*
		 * Out of focus by the PD disparity: search again. 比较的是错位相对 pdRef_
		 * （这次搜索在最终位置量到的错位）的变化。
		 */
		const PhaseResult ph = pdRescanOk_ && raw ? measure() : PhaseResult{};
		pdOff_ = trusted(ph) && std::fabs(ph.disparity - pdRef_) > kPdRescan ? pdOff_ + 1 : 0;
		if (pdOff_ >= kRescanFrames) {
			ALOGI("AF phase shows defocus (disparity %+.3f, %+.3f when settled), searching again",
			      ph.disparity, pdRef_);
			startScan();
			return;
		}
		const double delta = std::fabs(score - refScore_);
		if (delta > std::max(refScore_ * kRescanRel, kHwFloor)) {
			if (++stable_ >= kRescanFrames) {
				ALOGI("AF 画面变化（基准 %.1f → %.1f），重新搜索", refScore_, score);
				startScan();
			}
		} else {
			stable_ = 0;
		}
		return;
	}

	/* ── 搜索中 ── */
	/*
	 * ★ 用【均匀粗扫 + 峰附近细扫】而不是爬山：
	 *   爬山是局部算法，实测会卡死 —— 起始 511 最佳、往 766 探到更差后反向，
	 *   只是从"已经走坏的那一侧"往回挪（639→702→671…），**峰在另一侧的
	 *   256~384 整片从未被探索**。粗扫从离马达近的一端走过起点，越过明显的峰
	 *   才提前结束，最多 9 点 + 细扫。
	 *   附带好处：整条"位置→分数"曲线都会打进日志，标定与排障直接看日志。
	 */
	const int cur = lens_->position();

	/*
	 * 流水线延迟：setPosition 之后，相机/ISP 流水线里还有 kSettleFrames 帧
	 * 是【旧位置】曝出来的。设完位置就采信下一帧的分数 = 把"位置"和"分数"
	 * 错配。所以刚移过就先扔掉几帧（扔掉的帧打 ALOGD，可从日志量真实延迟）。
	 */
	if (settle_ > 0) {
		settle_--;
		ALOGD("AF 等稳定：pos=%d 分数=%.1f（还需 %d 帧）", cur, score, settle_);
		return;
	}

	/* Phase detection: go where the disparity points, then measure again.
	 * A search started under a configuration without RAW (the AF state
	 * survives a reconfigure) switches to it when RAW frames come, as long as
	 * it has not sampled or moved yet. */
	if (!phaseMode_ && havePhase_ && raw && moves_ == 0 && samples_.empty())
		phaseMode_ = true;
	/*
	 * Without RAW frames (a binned configuration, or several lost buffers in a
	 * row) phase can't work: not a phase failure, so phase-based rescans stay
	 * as they are. One frame without its RAW is just skipped.
	 */
	if (phaseMode_ && raw)
		noRaw_ = 0;
	if (phaseMode_ && !raw && havePhase_ && ++noRaw_ < 3)
		return;
	if (phaseMode_ && !raw) {
		ALOGI("AF 没有 RAW 帧，改用对比度扫描");
		phaseMode_ = false;
		noRaw_ = 0;
		buildSweep();
	}
	if (phaseMode_) {
		const PhaseResult ph = measure();
		const bool phaseOk = trusted(ph);
		if (phaseOk)
			pdAt_[cur] = ph.disparity;
		const int lo = std::max(lens_->minPos(), kLensLow);
		const int hi = std::min(lens_->maxPos(), kLensHigh);
		/* A real sample: an interrupted or fallen-back search keeps it. */
		samples_[cur] = score;
		if (score > bestScore_) {
			bestScore_ = score;
			bestPos_ = cur;
		}
		const char *why = "untrusted";
		if (phaseOk) {
			/* Secant: codes per unit from the last two measurements. */
			if (pdLastPos_ >= 0 && cur != pdLastPos_) {
				const double dd = ph.disparity - pdLastD_;
				if (std::fabs(dd) >= kPdSecantMinDelta) {
					const double g = (cur - pdLastPos_) / dd;
					if (g >= kPdMinCodesPerUnit && g <= kPdMaxCodesPerUnit)
						pdGain_ = g;
				}
			}
			pdLastPos_ = cur;
			pdLastD_ = ph.disparity;
			const int want = static_cast<int>(std::lround(cur - ph.disparity * pdGain_));
			const int target = std::clamp(want, lo, hi);
			ALOGI("AF 相位 pos=%d 错位=%+.3f 可信度=%.2f 分数=%.1f 系数=%.0f → %d", cur,
			      ph.disparity, ph.confidence, score, pdGain_, target);
			if (std::fabs(ph.disparity) <= kPdDone && pdSteps_ == 0 && !triggered_ && !restarted_ &&
			    !regionScan_) {
				/* 连续模式的重搜，相位在原位置就判定合焦：镜头不动（见 kPdConfirmRise） */
				bestPos_ = cur;
				bestScore_ = score;
				pdRescanOk_ = true;
				finishScan(true);
				return;
			}
			if (std::fabs(ph.disparity) <= kPdDone) {
				/* 相位说合焦了：两侧各采一个点，用对比度精调（见 kPdConfirmRise） */
				bestPos_ = cur;
				bestScore_ = score;
				pdDonePos_ = cur;
				pdConfirm_ = true;
				/*
				 * 精调只看相位结果附近新采的点：相位步进时留下的旧样本若正好落在
				 * pdDonePos_ ± k·kFineStep 上，爬坡会把它当成已采过而不去，最后停在峰前一步。
				 */
				samples_.clear();
				samples_[cur] = score;
				phaseMode_ = false;
				sweep_.clear();
				for (int k : { 1, -1 }) {
					const int p = std::clamp(cur + k * kFineStep, lo, hi);
					if (p != cur)
						sweep_.push_back(p);
				}
				sweepIdx_ = 0;
				fineDone_ = true;
				ALOGI("AF phase settled at %d, checking contrast around it", cur);
			} else if (target != cur && pdSteps_ < kPdMaxSteps) {
				pdSteps_++;
				moves_++;
				if (!moveTo(target))
					finishScan();
				return;
			} else {
				why = target == cur ? "target outside the sweep range" : "out of steps";
			}
		}
		if (!pdConfirm_) {
			ALOGI("AF phase unusable (%s, confidence %.2f), contrast sweep instead", why,
			      ph.confidence);
			phaseMode_ = false;
			pdRescanOk_ = false;
			/* Sweep from where the phase steps left the lens. */
			buildSweep();
		}
	}

	/* 每个采样点都打一行（pos + 分数）——"马达到底动没动 / 焦点落在哪"的客观证据。 */
	ALOGI("AF 采样 pos=%d 分数=%.1f（当前最佳 %d / %.1f）",
	      cur, score, bestPos_, bestScore_);
	samples_[cur] = score;
	/* 精调的采样点也量一次相位：镜头可能停在这里，重扫的基准要用它（见 pdAt_） */
	if (pdDonePos_ >= 0 && raw) {
		const PhaseResult ph = measure();
		if (trusted(ph))
			pdAt_[cur] = ph.disparity;
	}
	if (score > bestScore_) {
		bestScore_ = score;
		bestPos_ = cur;
	}
	/*
	 * Past a clear peak: stop the coarse sweep early. Only the sweep's own
	 * points count (sweepIdx_ > 0): the sample where the search started may
	 * lie on the far side of the points the sweep has reached.
	 */
	if (!fineDone_ && sweepIdx_ > 0) {
		if (score > coarseBest_) {
			coarseBest_ = score;
			drops_ = 0;
		} else if (focusable(coarseBest_) && score < coarseBest_ * kDropRatio) {
			drops_++;
		} else {
			drops_ = 0;
		}
		if (drops_ >= kDropPoints) {
			ALOGI("AF 粗扫提前结束：已越过峰值 %d / %.1f", bestPos_, bestScore_);
			sweepIdx_ = static_cast<int>(sweep_.size());
		}
	}

	/* 取下一个【与当前位置不同】的探测点；粗扫走完就转到峰附近的细扫。 */
	while (true) {
		int target = -1;
		while (sweepIdx_ < static_cast<int>(sweep_.size())) {
			const int p = sweep_[sweepIdx_++];
			if (p != lens_->position()) {
				target = p;
				break;
			}
		}
		if (target >= 0 && moves_ >= kMaxMoves) {
			ALOGW("AF 探测次数到上限 %d，按当前最佳结束", kMaxMoves);
			finishScan();
			return;
		}
		if (target >= 0) {
			moves_++;
			if (!moveTo(target))
				finishScan();
			return;
		}
		if (pdConfirm_) {
			/*
			 * 精调相位的结果：两侧的点都采过了。三点平坦 ⇒ 对比度靠不住，停在相位的
			 * 结果附近（finishScan() 仍按这三点的抛物线细调，最多半步：曲线真实但较平时
			 * 它往真峰挪）；有峰 ⇒ 在最好的点附近细扫（下面 fineDone_ 的分支）。
			 */
			double hiS = 0.0, loS = 0.0;
			bool first = true;
			for (int p : { pdDonePos_ - kFineStep, pdDonePos_, pdDonePos_ + kFineStep }) {
				const auto it = samples_.find(std::clamp(p, std::max(lens_->minPos(), kLensLow),
									 std::min(lens_->maxPos(), kLensHigh)));
				if (it == samples_.end())
					continue;
				hiS = first ? it->second : std::max(hiS, it->second);
				loS = first ? it->second : std::min(loS, it->second);
				first = false;
			}
			const double rise = loS > 0.0 ? hiS / loS : 0.0;
			pdConfirm_ = false;
			if (rise < kPdConfirmRise) {
				ALOGI("AF contrast flat around the phase result %d (rise %.3f): keeping it, "
				      "parabola only",
				      pdDonePos_, rise);
				bestPos_ = pdDonePos_;
				const auto it = samples_.find(bestPos_);
				if (it != samples_.end())
					bestScore_ = it->second;
				pdRescanOk_ = true;
				finishScan(true);
				return;
			}
			ALOGI("AF contrast around the phase result %d rises %.3f: refining near %d",
			      pdDonePos_, rise, bestPos_);
		}
		if (fineDone_) {   /* 细扫也走完 → 收敛到最佳点 */
			/* A fine point won: go on outward in kFineStep steps until the
			 * best point's neighbours on both sides are sampled, so that
			 * finishScan() can fit the parabola. */
			const int lo = std::max(lens_->minPos(), kLensLow);
			const int hi = std::min(lens_->maxPos(), kLensHigh);
			for (int p : { bestPos_ - kFineStep, bestPos_ + kFineStep }) {
				if (p < lo || p > hi || samples_.count(p) || moves_ >= kMaxMoves)
					continue;
				moves_++;
				if (!moveTo(p))
					finishScan();
				return;
			}
			if (pdDonePos_ >= 0) {
				/*
				 * 相位的结果经对比度确认过：合焦（抛物线细调在 finishScan()）。峰离
				 * 相位的结果超过两个细扫步长时，相位对这个场景有偏差，之后不按相位
				 * 重扫（否则停在对比度峰上时相位一直报失焦，扫个不停）。
				 */
				pdRescanOk_ = std::abs(bestPos_ - pdDonePos_) <= 2 * kFineStep;
				ALOGI("AF contrast peak %d, phase result %d%s", bestPos_, pdDonePos_,
				      pdRescanOk_ ? "" : ": phase biased here, no phase rescans");
				finishScan(true);
				return;
			}
			finishScan();
			return;
		}
		/* Coarse sweep done: sample kFineStep on either side of the best
		 * point, nearer side first; finishScan() fits a parabola through
		 * the three. */
		const int lo = std::max(lens_->minPos(), kLensLow);
		const int hi = std::min(lens_->maxPos(), kLensHigh);
		const int dir = lens_->position() >= bestPos_ ? 1 : -1;
		sweep_.clear();
		for (int k : { 1, -1 }) {
			const int p = std::clamp(bestPos_ + dir * k * kFineStep, lo, hi);
			if (p != bestPos_)
				sweep_.push_back(p);
		}
		sweepIdx_ = 0;
		fineDone_ = true;
		ALOGI("AF 粗扫完成：最佳 %d 分数 %.1f → 细扫 ±%d", bestPos_, bestScore_, kFineStep);
	}
}

void AutoFocus::setRegion(float x0, float y0, float x1, float y1)
{
	applyRegion(x0, y0, x1, y1, true);
}

void AutoFocus::applyRegion(float x0, float y0, float x1, float y1, bool tap)
{
	/* At least 10% of the frame each way (so the grid has a few blocks),
	 * centred on the request, moved inside the frame if needed. */
	constexpr float kMin = 0.10f;
	const float w = std::clamp(x1 - x0, kMin, 1.0f), h = std::clamp(y1 - y0, kMin, 1.0f);
	const float left = std::clamp((x0 + x1 - w) / 2, 0.0f, 1.0f - w);
	const float top = std::clamp((y0 + y1 - h) / 2, 0.0f, 1.0f - h);
	const float region[4] = { left, top, left + w, top + h };

	std::lock_guard<std::mutex> lk(m_);
	if (std::equal(region, region + 4, region_)) {
		/* 又变回了现在用的默认区域：搜索中记下的另一个倍数的默认区域作废 */
		if (!tap && defaultRegion_)
			hasPendingDefault_ = false;
		return;
	}
	if (!tap && defaultRegion_) {
		/*
		 * 默认区域跟着变焦走（clearRegion() 每个请求都调）。数码变焦不改变
		 * 物距，不必重新搜索，只换分数的基准；搜索进行中先不换，免得一次
		 * 搜索的分数来自不同的区域，等下一个请求再换。
		 */
		if (scanning_) {
			std::copy(region, region + 4, pendingDefault_);
			hasPendingDefault_ = true;
			return;
		}
		std::copy(region, region + 4, region_);
		hasPendingDefault_ = false;
		refScore_ = -1.0;
		return;
	}
	std::copy(region, region + 4, region_);
	defaultRegion_ = !tap;
	hasPendingDefault_ = false;
	ALOGI("AF 区域 (%.2f,%.2f)-(%.2f,%.2f)", region_[0], region_[1], region_[2], region_[3]);
	regionChanged(tap);
}

void AutoFocus::region(float out[4]) const
{
	std::lock_guard<std::mutex> lk(m_);
	std::copy(region_, region_ + 4, out);
}

/* Back to the default region is not a tap: a CONTINUOUS_VIDEO START during
 * that scan locks at once. */
/* 默认区域取 (x0, y0)–(x1, y1)（变焦后的可见部分）中间的 70%。 */
void AutoFocus::clearRegion(float x0, float y0, float x1, float y1)
{
	const float mx = (x1 - x0) * 0.15f, my = (y1 - y0) * 0.15f;
	applyRegion(x0 + mx, y0 + my, x1 - mx, y1 - my, false);
}

/* Caller holds the lock. In AUTO the app's next trigger searches the new
 * region; the continuous modes search it now. */
void AutoFocus::regionChanged(bool tap)
{
	refScore_ = -1.0;
	if ((mode_ == Mode::ContinuousPicture || mode_ == Mode::ContinuousVideo) && !triggered_) {
		startScan();
		regionScan_ = tap;
		regionAge_ = 0;
	} else if (scanning_) {
		/*
		 * 已触发的搜索（AUTO 点按、连续模式 START 后）进行中换了区域（例如变焦后点按处
		 * 移出、移回可见部分）：从头再搜，免得前后采样点的分数来自两块区域。触发与点按
		 * 标记保留，搜完照样锁定。
		 */
		const bool wasTap = regionScan_;
		startScan();
		regionScan_ = wasTap || tap;
		restarted_ = true;
	}
}

void AutoFocus::setPhaseAvailable(bool available)
{
	std::lock_guard<std::mutex> lk(m_);
	havePhase_ = available;
}

bool AutoFocus::wantsRaw() const
{
	std::lock_guard<std::mutex> lk(m_);
	if (!lens_ || !lens_->usable() || hold_ || mode_ == Mode::Off)
		return false;
	return scanning_ || ((mode_ == Mode::ContinuousPicture ||
			      mode_ == Mode::ContinuousVideo) && !triggered_);
}

uint8_t AutoFocus::afState() const
{
	std::lock_guard<std::mutex> lk(m_);
	if (!lens_ || !lens_->usable() || mode_ == Mode::Off || hold_)
		return ANDROID_CONTROL_AF_STATE_INACTIVE;
	if (mode_ == Mode::Auto) {
		if (!triggered_)
			return ANDROID_CONTROL_AF_STATE_INACTIVE;
		if (scanning_)
			return ANDROID_CONTROL_AF_STATE_ACTIVE_SCAN;
		return converged_ ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
				  : ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
	}
	/* 连续模式：状态机里只有 PASSIVE_* 与 *_LOCKED，没有 ACTIVE_SCAN。
	 * 连续拍照收到 START 时若正在扫，按文档继续报 PASSIVE_SCAN，扫完直接进锁定态。 */
	if (scanning_)
		return ANDROID_CONTROL_AF_STATE_PASSIVE_SCAN;
	if (triggered_)
		return converged_ ? ANDROID_CONTROL_AF_STATE_FOCUSED_LOCKED
				  : ANDROID_CONTROL_AF_STATE_NOT_FOCUSED_LOCKED;
	return converged_ ? ANDROID_CONTROL_AF_STATE_PASSIVE_FOCUSED
			  : ANDROID_CONTROL_AF_STATE_PASSIVE_UNFOCUSED;
}

uint8_t AutoFocus::lensState() const
{
	std::lock_guard<std::mutex> lk(m_);
	return lensState_;
}

void AutoFocus::setManualFocus(float diopters)
{
	std::lock_guard<std::mutex> lk(m_);
	if (!lens_ || !lens_->usable())
		return;
	const int lo = lens_->minPos();
	const int hi = lens_->maxPos();
	if (hi <= lo)
		return;
	/* focusDistance() 的反函数 */
	const float t = std::clamp(diopters / kMinFocusDiopters, 0.0f, 1.0f);
	const int pos = lo + static_cast<int>(std::lround(t * (hi - lo)));
	if (pos != manualPos_) {
		manualPos_ = pos;
		/* 正在进行的移动改去新位置 */
		if (moveTarget_ >= 0 && mode_ == Mode::Off)
			moveTarget_ = pos;
	}
}

float AutoFocus::focusDistance() const
{
	std::lock_guard<std::mutex> lk(m_);
	if (!lens_ || !lens_->usable())
		return 0.0f;
	const int lo = lens_->minPos();
	const int hi = lens_->maxPos();
	if (hi <= lo)
		return 0.0f;
	/* 标称映射（UNCALIBRATED）：行程低端 = 无穷远(0 屈光度)，高端 = 最近。
	 * 声明为 UNCALIBRATED 后应用不该依赖它的绝对值，只要单调即可。 */
	return kMinFocusDiopters *
	       static_cast<float>(lens_->position() - lo) / static_cast<float>(hi - lo);
}

} /* namespace gaokun3 */
