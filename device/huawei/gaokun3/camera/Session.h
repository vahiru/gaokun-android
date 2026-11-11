/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <functional>
#include <string>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <aidl/android/hardware/camera/device/BnCameraDeviceSession.h>
#include <cutils/native_handle.h>
#include <fmq/AidlMessageQueue.h>
#include <optional>

#include <libcamera/libcamera.h>

#include "AutoFocus.h"
#include "Exif.h"
#include "Lens.h"
#include "Metadata.h"

namespace gaokun3 {

class Session : public aidl::android::hardware::camera::device::BnCameraDeviceSession {
public:
	Session(std::shared_ptr<libcamera::Camera> cam, SensorFacts facts,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceCallback> cb);
	~Session() override;

	bool init();

	/*
	 * 输出流最大 maxW×maxH 时向硬件 ISP 要的 NV12 画面：传感器的宽高比，
	 * 盖住所有的流。
	 */
	static libcamera::Size hwIspFrameSize(const SensorFacts &f, int64_t maxW, int64_t maxH);

	::ndk::ScopedAStatus close() override;
	::ndk::ScopedAStatus configureStreams(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_cfg,
		std::vector<aidl::android::hardware::camera::device::HalStream> *out) override;
	/* V2 起新增；只是把 HalStream[] 包进一个 parcelable，委托给上面那个。 */
	::ndk::ScopedAStatus configureStreamsV2(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_cfg,
		aidl::android::hardware::camera::device::ConfigureStreamsRet *out) override;
	::ndk::ScopedAStatus constructDefaultRequestSettings(
		aidl::android::hardware::camera::device::RequestTemplate in_type,
		aidl::android::hardware::camera::device::CameraMetadata *out) override;
	::ndk::ScopedAStatus flush() override;
	::ndk::ScopedAStatus isReconfigurationRequired(
		const aidl::android::hardware::camera::device::CameraMetadata &in_oldSessionParams,
		const aidl::android::hardware::camera::device::CameraMetadata &in_newSessionParams,
		bool *out) override;
	::ndk::ScopedAStatus processCaptureRequest(
		const std::vector<aidl::android::hardware::camera::device::CaptureRequest> &in_requests,
		const std::vector<aidl::android::hardware::camera::device::BufferCache> &in_cachesToRemove,
		int32_t *out) override;
	::ndk::ScopedAStatus switchToOffline(
		const std::vector<int32_t> &in_streamsToKeep,
		aidl::android::hardware::camera::device::CameraOfflineSessionInfo *out_info,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraOfflineSession>
			*out) override;
	::ndk::ScopedAStatus repeatingRequestEnd(int32_t in_frameNumber,
						 const std::vector<int32_t> &in_streamIds) override;
	/* ── FMQ 元数据队列：我们不用它们（fmqResultSize 恒为 0），但接口是
	 *    纯虚的，必须实现。框架会在建会话时取一次描述符。 ── */
	::ndk::ScopedAStatus getCaptureRequestMetadataQueue(
		::aidl::android::hardware::common::fmq::MQDescriptor<
			int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
			*out) override;
	::ndk::ScopedAStatus getCaptureResultMetadataQueue(
		::aidl::android::hardware::common::fmq::MQDescriptor<
			int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>
			*out) override;
	/* 会话关闭时通知 Device（手电筒重新可用）。 */
	void setOnClosed(std::function<void()> f) { onClosed_ = std::move(f); }

	::ndk::ScopedAStatus signalStreamFlush(const std::vector<int32_t> &in_streamIds,
					       int32_t in_streamConfigCounter) override;

private:
	/* libcamera 的请求完成回调（在 CameraManager 线程上）。 */
	void onRequestCompleted(libcamera::Request *req);
	struct Pending;
	/* 按合同报一帧 ERROR_REQUEST：notify + 缓冲带 ERROR 状态还回去（只在完成回调里调，保证帧序）。 */
	void reportRequestError(const Pending &p);
	/* 以下两个调用者持 mutex_。 */
	void flashRequestDone(const Pending &p);
	void returnToPool(std::unique_ptr<libcamera::Request> r);

	/*
	 * 取得一路流某个 bufferId 对应的 gralloc 句柄（带缓存）。
	 * ★★ AIDL 的契约（StreamBuffer.aidl 原文）：同一个 bufferId **只在第一次**
	 *   带有效句柄，之后框架发过来的 `buffer` 字段是【空的】，
	 *   "HAL must look up the actual buffer handle to use from its own
	 *    bufferId to buffer handle map"。
	 *   漏了这个缓存的症状是第二帧起报 "Failed to importBuffer. Bad handle."
	 */
	buffer_handle_t getBuffer(
		const aidl::android::hardware::camera::device::StreamBuffer &sb);
	/* 按 cachesToRemove 释放；会话结束时全部释放。 */
	void dropCaches(const std::vector<
			aidl::android::hardware::camera::device::BufferCache> &caches);
	void dropAllCaches();

	/*
	 * libcamera 输出的一帧。软件 ISP（simple 管线）给打包的 RGB888（内存里是 B,G,R）；
	 * 硬件 ISP（qcom-ife 管线）给全范围 BT.601 的 NV12：data 是 Y 平面，uv 是交错的
	 * CbCr 平面。各平面的行宽都是 stride 字节。
	 */
	struct SrcFrame {
		const uint8_t *data = nullptr;
		const uint8_t *uv = nullptr;
		int stride = 0;
	};

	/* 把一帧（源尺寸 srcWidth_×srcHeight_）交付到一个 gralloc 缓冲，
	 * 必要时裁成该路流的宽高比、再缩放到它的尺寸。 */
	/* shared：这一帧另有一路流尺寸相同 */
	bool deliver(const SrcFrame &src, buffer_handle_t dst,
		     int32_t dstW, int32_t dstH, int chromaBlur, bool shared);
	/*
	 * 把硬件 ISP 的画面转成一种输出尺寸的 NV12，每帧每种尺寸只裁剪、缩放、
	 * 降噪一次：先 Y（dstW×dstH），紧接着 CbCr 平面。失败返回 nullptr。
	 */
	const uint8_t *renderNv12(const SrcFrame &src, int32_t dstW, int32_t dstH, int chromaBlur);
	/* 裁成 dstW×dstH 的宽高比、缩放后写成全范围的 4:2:0 平面（J420），
	 * Y 行宽 ys、色度行宽 cs。返回 libyuv 的结果，0 为成功。 */
	int toJ420(const SrcFrame &src, int32_t dstW, int32_t dstH,
		   uint8_t *dy, int ys, uint8_t *du, uint8_t *dv, int cs) const;
	/*
	 * 同上，但写成半平面（CbCr 成对交错在一个平面里，即 NV12），只接受 NV12
	 * 来源：裁剪，尺寸不同时再缩放，不做格式转换。
	 */
	int toNv12(const SrcFrame &src, int32_t dstW, int32_t dstH,
		   uint8_t *dy, int ys, uint8_t *duv, int cs) const;
	/* NV12 缩放成另一尺寸的 NV12，倍数大时分级 */
	int scaleNv12(const uint8_t *sy, int ss, const uint8_t *suv, int suvs, int sw, int sh,
		      uint8_t *dy, int ds, uint8_t *duv, int duvs, int dw, int dh) const;
	/* Centre crop of the source frame with the aspect ratio of dstW x dstH,
	 * even-aligned. */
	struct Crop {
		int x, y, w, h;
	};
	Crop cropFor(int32_t dstW, int32_t dstH) const;
	/*
	 * JPEG 流：把 RGB 编码成 JPEG 写进 BLOB 缓冲。
	 * jpegOrientation 是应用给的【顺时针】校正角（ANDROID_JPEG_ORIENTATION）。
	 * ⚠️ 90/270 时交付的 JPEG 实际宽高与流声明的 (dstW, dstH) 互换 ——
	 *    BLOB 流只声明缓冲字节数，这是 Android 允许且常见的行为。
	 */
	bool deliverJpeg(const SrcFrame &src, buffer_handle_t dst,
			 int32_t dstW, int32_t dstH, int32_t blobSize,
			 int quality, int32_t jpegOrientation);
	bool deliverJpegRgb(const uint8_t *rgb, buffer_handle_t dst,
			    int32_t dstW, int32_t dstH, int32_t blobSize,
			    int quality, int32_t jpegOrientation);
	/* NV12 不经过 RGB：裁剪缩放旋转成 J420，再以 YCbCr 原始数据交给 libjpeg。 */
	bool deliverJpegNv12(const SrcFrame &src, buffer_handle_t dst,
			     int32_t dstW, int32_t dstH, int32_t blobSize,
			     int quality, int32_t jpegOrientation);

	/*
	 * ── 闪光灯（#110 / #111）──
	 * LED 是 PM8350C 闪光模块的 torch 档（/sys/class/leds/white:flash/brightness），没有与传感器
	 * 曝光同步的 strobe，所以"闪光"= 从预闪触发起把灯点着，直到那张静态照片完成再灭：
	 * AGC 有几帧时间适应灯光，照片在灯下曝光，不会出现"灯亮了但那一帧已经曝完"的错位。
	 * ⚠️ 灯【不能】在下一个请求入队时就灭：框架会把预览请求排在拍照请求后面立刻发来，
	 *    那时拍照那一帧还没曝光 —— 所以用 firedPending_ 计数，等点过灯的请求都完成了才灭。
	 */
	void setLed(bool on);
	void parseFlashControls(const std::vector<uint8_t> &settings, uint8_t *trigger,
				uint8_t *intent);
	static constexpr int kDarkLuma = 50;          /* AUTO 闪光的"太暗"判据之一（0..255 采样均值） */
	static constexpr double kDarkGain = 4.0;      /* 之二：模拟增益 ≥ 4x 就算暗（libcamera 报的） */
	/* 预闪：至少 kPrecaptureMin 帧、且亮度连续 2 帧稳定才报收敛；最多等 kPrecaptureMax 帧。
	 * ★ 2026-09-14 第一版固定 4 帧，用户拍出来白墙全过曝：软件 ISP 的 AGC 要十几帧才收敛。 */
	static constexpr int kPrecaptureMin = 8;
	static constexpr int kPrecaptureMax = 15;
	static constexpr int kLumaStableDelta = 8;
	std::string flashLed_;          /* 空 = 这个相机没有闪光灯 */
	std::string ledMax_ = "255";    /* max_brightness 的原文 */
	bool ledOn_ = false;
	uint8_t aeMode_ = ANDROID_CONTROL_AE_MODE_ON;   /* 粘滞：请求里没带就沿用上一帧 */
	uint8_t flashMode_ = ANDROID_FLASH_MODE_OFF;
	bool flashArmed_ = false;       /* 预闪触发后 → 到静态拍照完成为止保持点亮 */
	bool precaptureActive_ = false; /* 预闪进行中（报 AE_STATE_PRECAPTURE） */
	bool precaptureLocked_ = false; /* 这次预拍触发时 AE 已经锁定：状态保持 LOCKED */
	int precaptureFrames_ = 0;
	int lumaStable_ = 0;            /* 连续几帧亮度变化 < kLumaStableDelta */
	int firedPending_ = 0;          /* 在途的、点着灯的请求数 */
	int lastLuma_ = 128;            /* 上一帧的采样平均亮度 */
	double lastGain_ = 1.0;         /* 上一帧 libcamera 报的模拟增益 */
	int32_t lastExposureUs_ = 0;

	/*
	 * ── 降噪（#112）：软件 ISP 里一个降噪算法都没有，高增益下噪点直接进照片 ──
	 * 静态照片（BLOB）：RGB 域 3×3 ε 滤波（只平均与中心差 ≤ T 的邻居，T 随增益走），多线程分行。
	 * 预览（YUV）：只对 U/V 平面做盒式模糊 —— 彩色噪点最难看、色度平面又只有 1/4 分辨率，几乎免费。
	 * 增益 < kDenoiseGain 一律不动：白天的图别被磨掉细节。
	 */
	static constexpr double kDenoiseGain = 2.0;
	static void epsilonFilterRgb(const uint8_t *src, uint8_t *dst, int w, int h, int threshold);
	/*
	 * 盒式模糊：h 行，每行 n 字节，行距 stride 字节；各通道交错，相隔 step
	 * 字节：单独的平面 step 为 1，NV12 的 CbCr 平面为 2（两个通道一起做）。
	 */
	void boxBlur(uint8_t *p, int n, int h, int stride, int step, int radius);
	int denoiseThreshold() const;   /* 0 = 不降噪 */
	int chromaBlurRadius() const;   /* 0 = 不模糊 */
	std::function<void()> onClosed_;

	/*
	 * 交付路径的每帧暂存区，跨帧保留：几 MB 的缓冲由 mmap() 分配，每帧重新
	 * 分配的话每 4 KB 一次缺页。只在 libcamera 的完成回调线程上使用。
	 */
	mutable std::vector<uint8_t> planeScratch_;   /* 复制前的 J420 或 CbCr */
	mutable std::vector<uint8_t> scaleScratch_;   /* toJ420()：缩放后的 NV12 */
	mutable std::vector<uint8_t> halfScratch_[2]; /* scaleNv12()：减半后的 NV12 */
	std::vector<uint8_t> blurSrc_;
	std::vector<uint16_t> blurCols_;
	struct RenderedNv12 {
		uint64_t frame = 0;     /* 所属的 renderFrame_，0 = 无 */
		int32_t w = 0, h = 0;
		int blur = 0;
		std::vector<uint8_t> data;
	};
	RenderedNv12 rendered_[2];
	uint64_t renderFrame_ = 0;

	/*
	 * libcamera 缓冲的 mmap()，按 dmabuf fd 记，第一次用到时映射，保留到缓冲
	 * 释放：20 MB 的全尺寸帧每帧映射、解除映射要占单核的百分之几。完成回调
	 * 线程读帧时持有 frameMapMutex_。
	 *
	 * 锁顺序：完成回调先拿 frameMapMutex_ 再拿 mutex_；close() 和
	 * configureStreams() 持着 mutex_ 调 unmapFrames()，顺序相反。不会死锁的
	 * 前提是 cam_->stop() 返回之后不再有完成回调：libcamera 在 CameraManager
	 * 线程上直接调用回调，stop() 也在这个线程上阻塞执行，返回前所有请求都已
	 * 完成或取消。所以 unmapFrames() 只能在 stop() 之后调用。
	 */
	std::map<int, std::pair<void *, size_t>> frameMaps_;
	std::mutex frameMapMutex_;
	/* 调用方持有 frameMapMutex_；mmap() 失败返回 nullptr */
	const uint8_t *mapFrame(int fd, size_t len);
	void unmapFrames();

	/*
	 * ── 自动对焦（后摄 only）──
	 * lens_ 是本会话独有的马达句柄：open() 打开子设备 fd 即上电，close() 关 fd 即断电。
	 * af_ 只在这两条线程上被访问：processCaptureRequest（解析模式/触发）与
	 * onRequestCompleted（喂帧、写位置）—— 内部自带锁，所以不必碰 mutex_。
	 * ⚠️ af_.attach()/Lens::close() 的顺序：先把 af_ 摘掉再关 fd，
	 *    否则完成回调可能对着已关闭的 fd 写（写失败无害，但会刷日志）。
	 */
	AutoFocus af_;
	std::unique_ptr<Lens> lens_;

	std::shared_ptr<libcamera::Camera> cam_;
	SensorFacts facts_;
	std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceCallback> cb_;

	std::mutex mutex_;
	std::condition_variable drainCv_;
	/* freeRequests_ 有空位时通知 processCaptureRequest（生产者/消费者），
	 * 这样池子瞬间枯竭时不会拒绝框架的请求（拒绝→框架永远等不到结果
	 * → waitUntilIdle 超时 → ERROR_CAMERA_DEVICE）。 */
	std::condition_variable freeCv_;
	bool streaming_ = false;
	bool closed_ = false;

	std::unique_ptr<libcamera::CameraConfiguration> config_;
	std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
	libcamera::Stream *stream_ = nullptr;
	/* Full-size RAW stream for phase detection (rear camera in full-size mode). */
	libcamera::Stream *rawStream_ = nullptr;
	bool rawForPhase_ = false;
	/*
	 * 输出画面的视场在像素阵列坐标里的位置（ScalerCropMaximum），仅当管线接受 AfWindows
	 * 时才有：AF 区域按它换算后交给对焦统计。
	 */
	std::optional<libcamera::Rectangle> afFov_;
	unsigned int rawStride_ = 0;
	int32_t rawWidth_ = 0, rawHeight_ = 0;

	/*
	 * 配置好的 Android 流。
	 * ★ 相机应用【一定】会配多路（预览 + 拍照，实测 Aperture 要 2 路）。
	 *   libcamera 这边只开一路（一个传感器模式的完整画面），再用 libyuv 裁剪缩放分发到每一路
	 *   —— 软件 ISP 是 CPU 瓶颈，开多路等于把同一份拜耳去马赛克多次；硬件 ISP 的 IFE
	 *   也只输出一路 NV12。
	 */
	struct HalStreamInfo {
		int32_t id = -1;
		int32_t width = 0, height = 0;
		bool isBlob = false;        /* JPEG 流 */
		int32_t blobSize = 0;       /* Stream::bufferSize */
	};
	std::vector<HalStreamInfo> halStreams_;
	/* libcamera 实际输出的尺寸：一个传感器模式的完整画面（见 configureStreams）。 */
	int32_t srcWidth_ = 0, srcHeight_ = 0;
	/* 源帧的行宽（字节）。 */
	int32_t srcStride_ = 0;
	/* libcamera 输出 NV12（硬件 ISP）还是 RGB888（软件 ISP），见 configureStreams。 */
	bool srcNv12_ = false;
	/* DMA_BUF_IOCTL_SYNC 失败时只记一次日志。 */
	bool syncWarned_ = false;

	/* 空闲的 libcamera 请求。 */
	std::deque<std::unique_ptr<libcamera::Request>> freeRequests_;
	/* 本次 configureStreams 建的 Request（裸指针，只作身份判断）。重配置后迟到的上一代
	 * Request 不在这里 ⇒ returnToPool() 直接释放，不会混进新池子。 */
	std::set<libcamera::Request *> configRequests_;
	/* 池子暂空时 processCaptureRequest 最多等多久（正常不会空，见那里的说明）。 */
	static constexpr int kPoolWaitMs = 1000;
	/* 上一个【带设置】的请求的设置，已去掉一次性触发（stripTriggers）。
	 * 请求不带设置 = 与上一帧相同 ⇒ 用它。 */
	std::vector<uint8_t> lastSettings_;
	/* 上一帧交给应用的 AF 状态（只为在变化时打一行日志）。 */
	uint8_t lastAfState_ = 0xff;
	/*
	 * libcamera Request* → Android frameNumber + 目标缓冲。
	 * ⚠️ 不能用 cookie：Request::cookie_ 是 const、只能在构造时给
	 *    （request.h:47,73 —— 没有 setCookie()），而我们的请求对象是
	 *    在 configureStreams 时一次建好反复复用的。所以用指针当键。
	 */
	/*
	 * ⚠️★ 这里【不能】存 AIDL 的 StreamBuffer：它含 NativeHandle，
	 *    里面是 vector<ndk::ScopedFileDescriptor> —— **move-only，不可拷贝**
	 *    （编译器报的是 vector::operator= 没有匹配的 assign）。
	 *    而 processCaptureRequest 的入参是 const&，move 不出来。
	 *    ⇒ 收到请求时就把 gralloc 缓冲导入，这里只存导入后的句柄和 id，
	 *      完成时直接往里写，然后 freeBuffer。这样也顺带省掉一次导入。
	 */
	struct PendingBuffer {
		int32_t streamId = -1;
		int64_t bufferId = 0;
		buffer_handle_t handle = nullptr;   /* 缓存里的，不拥有 */
		int32_t width = 0, height = 0;
		bool isBlob = false;
		int32_t blobSize = 0;
	};
	struct Pending {
		int32_t frameNumber = 0;
		std::vector<PendingBuffer> buffers;
		std::vector<uint8_t> settings;
		/* 闪光灯：这一帧灯亮着吗 / 完成后要不要解除预闪 / 要报的 3A 状态 */
		bool flashFired = false;
		bool endsFlash = false;
		uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
		uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
		uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
		uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
		int32_t evSteps = 0;    /* 这个请求的曝光补偿（kEvStepsPerStop 档） */
		bool sentGains = false; /* 这个请求给 libcamera 下发了手动的 ColourGains */
		/* 数码变焦：实际用的裁剪区域，有效阵列坐标 */
		int32_t crop[4] = {};
	};
	std::map<libcamera::Request *, Pending> pending_;
	/* 正在交付的帧的裁剪区域（cropFor()）；w 为 0 = 整幅 */
	int32_t cropRegion_[4] = {};
	/*
	 * 正在交付的帧的 EXIF（onRequestCompleted()）：尺寸和缩略图除外，那两项
	 * 由 JPEG 路径补上（exifApp1()）。
	 */
	ExifInfo exifFrame_;
	int32_t thumbW_ = 0, thumbH_ = 0;   /* 请求的尺寸，旋转之前；0 = 不要 */
	int thumbQuality_ = 85;
	int32_t thumbMade_[2] = { -1, -1 }; /* 做出的尺寸，旋转之后 */
	std::string exifMake_, exifModel_;
	/* JPEG（outW×outH，rotated：像素转过）的 APP1 内容；thumb 是缩略图的 B,G,R,A 像素 */
	std::vector<uint8_t> exifApp1(int32_t outW, int32_t outH, bool rotated,
				      const std::vector<uint8_t> &thumb, int32_t tw, int32_t th);
	/* 旋转 rotation 度的 JPEG 的缩略图尺寸（0x0：不要） */
	void thumbnailSize(int32_t rotation, int32_t *tw, int32_t *th) const;
	/* 应用给了 AF 区域；没给时区域跟着变焦走 */
	bool afAppRegion_ = false;

	/*
	 * ── AE/AWB 锁定与曝光补偿（持有 mutex_）──
	 * AE 锁定：曝光时间与增益切到手动，并显式下发最后报的值
	 * （lockedExposureUs_、lockedGain_），因为锁定期间曝光补偿照样生效
	 * （CaptureRequest 文档），要按它缩放。libcamera 的 AWB 进手动模式时从
	 * 单位增益开始，所以锁定时下发最后报的增益。
	 */
	void apply3aControls(Pending &p, libcamera::ControlList &ctrls);
	bool aeLocked_ = false;
	int32_t lockEvSteps_ = 0;
	int32_t lockedExposureUs_ = 0;
	double lockedGain_ = 0.0;
	/* 上一个完成的帧所属请求的曝光补偿：它报的曝光是在这个补偿下测的 */
	int32_t lastEvSteps_ = 0;
	bool awbLocked_ = false;
	std::array<float, 2> lockedColourGains_{};
	std::optional<std::array<float, 2>> lastColourGains_;
	/*
	 * 锁定时的色温，与增益一起下发：libipa 的 CCM 在 AWB 非自动时用手动矩阵，
	 * 只按 ColourTemperature 更新；不给的话矩阵会跳回配置时的 5000 K 或上次
	 * 预设的。AWB 有增益时不看色温。
	 */
	int32_t lockedColourTemp_ = 0;
	int32_t lastColourTemp_ = 0;
	/* 这个配置的曝光时间（微秒）与模拟增益范围 */
	int32_t expMinUs_ = 0, expMaxUs_ = 0;
	float gainMin_ = 1.0f, gainMax_ = 1.0f;

	/* (streamId, bufferId) → 已导入的句柄。见 getBuffer() 的说明。 */
	std::map<std::pair<int32_t, int64_t>, buffer_handle_t> bufferCache_;

	using MetadataQueue = ::android::AidlMessageQueue<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite>;
	std::shared_ptr<MetadataQueue> requestQueue_;
	std::shared_ptr<MetadataQueue> resultQueue_;
};

} /* namespace gaokun3 */
