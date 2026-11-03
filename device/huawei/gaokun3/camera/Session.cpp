/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 会话：流配置、请求、以及把 libcamera 的 RGB 帧
 * 交付到 Android 的 gralloc 缓冲。
 *
 * ⚠️★ 设计上的一个硬约束：**软件 ISP 只输出 RGB 族**
 *    （libcamera debayer_cpu.cpp:436-441 —— 没有 YUV/NV12/YUYV），
 *    而 Android 要 YUV_420_888 / JPEG。色彩转换因此必须由本层用 libyuv 做。
 *    这正是 AOSP 自带的 ExternalCameraProvider 接不上的原因（[#91]）。
 */
#include "Session.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/device/ConfigureStreamsRet.h>
#include <aidl/android/hardware/camera/device/ErrorCode.h>
#include <aidl/android/hardware/camera/device/ErrorMsg.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraOfflineSession.h>
#include <aidl/android/hardware/camera/device/NotifyMsg.h>
#include <aidl/android/hardware/camera/device/ShutterMsg.h>
#include <aidl/android/hardware/camera/device/CameraBlob.h>
#include <aidl/android/hardware/camera/device/CameraBlobId.h>
#include <aidlcommonsupport/NativeHandle.h>
#include <jpeglib.h>
#include <hardware/gralloc.h>
#include <libyuv.h>
#include <log/log.h>
#include <sys/mman.h>
/* ⚠️ 不要在这里直接 #include <system/camera_metadata_tags.h>！
 * 该文件【没有 include guard】也没有 #pragma once（system/media/camera/include/system/），
 * 而 <system/camera_metadata.h> 内部本来就 include 了它 ⇒ 再写一次就是二次包含，
 * 立刻 20 个 "redefinition of 'camera_metadata_enum_android_control_*'"（实测：
 * 编到 HAL 那一步才炸）。
 * ANDROID_JPEG_ORIENTATION / ANDROID_JPEG_QUALITY 由 Metadata.h → camera_metadata.h 传递可见。 */
#include <system/graphics.h>
#include <ui/GraphicBufferMapper.h>
#include <ui/Rect.h>
#include <utils/Errors.h>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::device::BufferCache;
using ::aidl::android::hardware::camera::device::BufferStatus;
using ::aidl::android::hardware::camera::device::CameraBlob;
using ::aidl::android::hardware::camera::device::CameraBlobId;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::CaptureRequest;
using ::aidl::android::hardware::camera::device::CaptureResult;
using ::aidl::android::hardware::camera::device::ErrorCode;
using ::aidl::android::hardware::camera::device::ErrorMsg;
using ::aidl::android::hardware::camera::device::HalStream;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::NotifyMsg;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::ShutterMsg;
using ::aidl::android::hardware::camera::device::StreamBuffer;
using ::aidl::android::hardware::camera::device::StreamConfiguration;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}

/*
 * Android 的 JPEG 旋转角（顺时针）→ libyuv 的 RotationMode。
 *
 * ★★ 这里【不需要取反】，但理由必须写下来，因为它极容易被想当然搞错：
 *   ANDROID_JPEG_ORIENTATION 与 EXIF Orientation 都用【顺时针】；
 *   libyuv 的 rotate.h 原文注释也是顺时针 ——
 *     kRotate90  = 90,  // Rotate 90 degrees clockwise.
 *     kRotate270 = 270, // Rotate 270 degrees clockwise.
 *   而且它还留着把方向写死在名字里的历史别名：
 *     kRotateClockwise = 90 / kRotateCounterClockwise = 270
 *   ⇒ 直接一对一映射。
 *   ⚠️ 别拿 OpenCV / PIL 的直觉套过来：那些库的"旋转 90"是逆时针，
 *      照抄一遍会把前后摄一起转歪 180°。
 *
 * 非 0/90/180/270 的取值按"不旋转"处理并记日志 —— Android 只允许这四个值，
 * 出现别的说明上游算错了；静默接受会得到一个随机的方向。
 */
libyuv::RotationMode toLibyuvRotation(int32_t deg)
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
} /* namespace */

Session::Session(std::shared_ptr<libcamera::Camera> cam, SensorFacts facts,
		 std::shared_ptr<ICameraDeviceCallback> cb)
	: flashLed_(facts.flashLed), cam_(std::move(cam)), facts_(std::move(facts)),
	  cb_(std::move(cb))
{
	if (!flashLed_.empty()) {
		char buf[32] = {};
		int fd = open((flashLed_ + "/max_brightness").c_str(), O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);
			::close(fd);   /* 不是 Session::close() */
			if (n > 0) {
				std::string v(buf, static_cast<size_t>(n));
				while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
					v.pop_back();
				if (!v.empty())
					ledMax_ = v;
			}
		}
	}
}

void Session::setLed(bool on)
{
	if (flashLed_.empty() || on == ledOn_)
		return;
	int fd = open((flashLed_ + "/brightness").c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		ALOGE("闪光灯：打不开 %s/brightness: %s", flashLed_.c_str(), strerror(errno));
		return;
	}
	const std::string v = on ? ledMax_ : "0";
	if (write(fd, v.c_str(), v.size()) != static_cast<ssize_t>(v.size()))
		ALOGE("闪光灯：写 brightness 失败: %s", strerror(errno));
	else
		ledOn_ = on;
	::close(fd);
}

void Session::parseFlashControls(const std::vector<uint8_t> &settings, uint8_t *trigger,
				 uint8_t *intent)
{
	/* 请求不带设置 = 与上一帧相同（CaptureRequest.aidl 的约定），粘滞值不动。 */
	if (settings.empty())
		return;
	const camera_metadata_t *m =
		reinterpret_cast<const camera_metadata_t *>(settings.data());
	camera_metadata_ro_entry_t e;
	if (find_camera_metadata_ro_entry(m, ANDROID_CONTROL_AE_MODE, &e) == 0 && e.count)
		aeMode_ = e.data.u8[0];
	if (find_camera_metadata_ro_entry(m, ANDROID_FLASH_MODE, &e) == 0 && e.count)
		flashMode_ = e.data.u8[0];
	if (trigger &&
	    find_camera_metadata_ro_entry(m, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &e) == 0 &&
	    e.count)
		*trigger = e.data.u8[0];
	if (intent &&
	    find_camera_metadata_ro_entry(m, ANDROID_CONTROL_CAPTURE_INTENT, &e) == 0 && e.count)
		*intent = e.data.u8[0];
}

Session::~Session()
{
	close();
}

bool Session::init()
{
	if (cam_->acquire()) {
		ALOGE("libcamera Camera::acquire 失败（是不是已经被别人占了）");
		return false;
	}
	cam_->requestCompleted.connect(this, &Session::onRequestCompleted);
	/* 框架建会话时会取这两个描述符；我们不往里写（fmqResultSize 恒 0），
	 * 但必须是有效的队列。大小照抄 AOSP 的 ExternalCameraDeviceSession。 */
	requestQueue_ = std::make_shared<MetadataQueue>(1 << 20, false);
	resultQueue_ = std::make_shared<MetadataQueue>(1 << 20, false);
	if (!requestQueue_->isValid() || !resultQueue_->isValid()) {
		ALOGE("建 FMQ 元数据队列失败");
		return false;
	}

	/*
	 * 对焦马达：Device 只回答"有没有"（探测到 dw9714 子设备即 hasAf），
	 * 真正的打开放在这里 —— 有会话才需要它上电（打开子设备 fd 即上电，见 Lens.h）。
	 * ★ 打不开不算致命：如实降级成定焦，别让整个会话起不来。
	 *   ⚠️ 此时 characteristics 已经声明了 AF（那是建会话之前发出去的），
	 *      所以会短暂出现"声明有 AF 但马达不可用"——AutoFocus 拿不到马达时
	 *      一律报 INACTIVE / STATIONARY，行为等价于定焦。
	 */
	if (facts_.hasAf) {
		lens_ = std::make_unique<Lens>();
		if (lens_->open())
			af_.attach(lens_.get());
		else {
			ALOGW("声明了 AF 但打不开 VCM —— 本次会话退化为定焦");
			lens_.reset();
		}
	}
	return true;
}

ndk::ScopedAStatus Session::close()
{
	{
		std::unique_lock<std::mutex> lk(mutex_);
		if (closed_)
			return ndk::ScopedAStatus::ok();
		closed_ = true;

		if (streaming_) {
			streaming_ = false;
			/* 同 configureStreams：持锁 stop 会与完成回调争 mutex_ → 死锁。 */
			lk.unlock();
			cam_->stop();
			lk.lock();
		}
		cam_->requestCompleted.disconnect(this, &Session::onRequestCompleted);
		freeRequests_.clear();
		configRequests_.clear();
		pending_.clear();
		dropAllCaches();
		allocator_.reset();
		config_.reset();
		cam_->release();
		setLed(false);   /* 会话结束灯必须灭，别让 torch 请求把灯留着 */
		/* 对焦：先摘掉 AutoFocus 的句柄，再关 fd（顺序见 Session.h）。 */
		af_.attach(nullptr);
		if (lens_) {
			lens_->close();
			lens_.reset();
		}
		ALOGI("会话已关闭");
	}
	/* 在锁外通知 Device（它会拿自己的锁）。 */
	if (onClosed_)
		onClosed_();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::configureStreams(const StreamConfiguration &cfg,
					     std::vector<HalStream> *out)
{
	std::unique_lock<std::mutex> lk(mutex_);
	out->clear();

	{
		std::string why;
		if (!streamCombinationSupported(cfg, facts_, &why)) {
			ALOGE("configureStreams: 不支持的流组合（%s）", why.c_str());
			return err(Status::ILLEGAL_ARGUMENT);
		}
	}

	/*
	 * ★ 相机应用一定会配多路（预览 + 拍照）。libcamera 只开【一路】，
	 *   再用 libyuv 裁剪、缩放分发 —— 软件 ISP 是 CPU 瓶颈，开多路等于把同一份拜耳去马赛克多次。
	 */
	int32_t maxW = 0, maxH = 0;
	for (const auto &s2 : cfg.streams) {
		ALOGI("  请求流 id=%d %dx%d fmt=0x%x", s2.id, s2.width, s2.height,
		      static_cast<int>(s2.format));
		maxW = std::max(maxW, s2.width);
		maxH = std::max(maxH, s2.height);
	}

	if (streaming_) {
		/*
		 * ★★ 绝不能在【持 mutex_】时调 cam_->stop()：stop() 会把在途请求以
		 *   RequestCancelled 撤销并触发 onRequestCompleted，而回调第一件事就是
		 *   拿 mutex_ —— 一旦 stop() 要等回调返回，就是重入死锁。症状与实测
		 *   完全吻合：旋转/切换后的 reconfigure 卡住十几秒 → 框架
		 *   waitUntilIdle 超时(-110) → ERROR_CAMERA_DEVICE(CRITICAL)。
		 *   先放锁 stop，再上锁 drain/清理。
		 */
		streaming_ = false;
		lk.unlock();
		cam_->stop();
		lk.lock();
		/*
		 * 必须等 libcamera 把在途请求全部以 RequestCancelled 还回来，
		 * 才能清理 pending_/freeRequests_。否则已释放的 Request* 还会
		 * 被 onRequestCompleted 访问，造成 use-after-free / 请求泄漏，
		 * 最终 waitUntilIdle 超时 → ERROR_CAMERA_DEVICE。
		 */
		if (!pending_.empty()) {
			ALOGI("stop 后等待 %zu 个在途请求完成...", pending_.size());
			bool drained = drainCv_.wait_for(lk, std::chrono::milliseconds(3000),
							 [&] { return pending_.empty(); });
			if (!drained)
				ALOGW("stop 后 %zu 个在途请求仍未完成，强制清理", pending_.size());
		}
		freeRequests_.clear();
		configRequests_.clear();
		pending_.clear();
		allocator_.reset();
	}
	lastSettings_.clear();
	lastAfState_ = 0xff;

	/* Viewfinder role 会启用软件 ISP（Raw role 不会）。 */
	config_ = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
	if (!config_ || config_->empty()) {
		ALOGE("generateConfiguration 失败");
		return err(Status::INTERNAL_ERROR);
	}
	libcamera::StreamConfiguration &sc = config_->at(0);
	/*
	 * libcamera's software ISP crops, it does not scale: an output smaller
	 * than the sensor mode is a window out of its middle, with less field
	 * of view than the stills. Ask for the whole image of a mode and scale
	 * in deliver(): the 2x2 binned mode when every stream fits in half the
	 * array, full size otherwise. The CSI-2 packed debayer keeps a 4-pixel
	 * border on either side, so the binned mode's whole image is 8 pixels
	 * narrower than half the array (ov13b10 2104x1560 -> 2096x1560, hi846
	 * 1632x1224 -> 1624x1224). This also keeps libcamera, which picks the
	 * smallest mode that holds the output, off a 16:9 mode for 4:3 streams.
	 * A multiple of 8 keeps the RGB888 rows packed (checked below).
	 */
	const int32_t halfW = (facts_.activeW / 2 - 8) & ~7;
	const int32_t halfH = facts_.activeH / 2;
	sc.size = maxW <= halfW && maxH <= halfH ? libcamera::Size(halfW, halfH)
						 : libcamera::Size(facts_.activeW, facts_.activeH);
	/* 固定要 RGB888。⚠️ 用 3 字节的 RGB888 而不是 4 字节的 ABGR8888：
	 * 8 MP 下每帧少搬 8 MB，而软件去拜耳本来就是 CPU 瓶颈。 */
	sc.pixelFormat = libcamera::formats::RGB888;
	sc.bufferCount = 4;

	libcamera::CameraConfiguration::Status st = config_->validate();
	if (st == libcamera::CameraConfiguration::Invalid) {
		ALOGE("配置无效");
		return err(Status::ILLEGAL_ARGUMENT);
	}
	if (st == libcamera::CameraConfiguration::Adjusted)
		ALOGW("配置被调整为 %s", sc.toString().c_str());

	if (cam_->configure(config_.get())) {
		ALOGE("Camera::configure 失败");
		return err(Status::INTERNAL_ERROR);
	}

	/* The frame is read everywhere as packed RGB888, with no row padding. */
	if (sc.stride != sc.size.width * 3) {
		ALOGE("RGB888 行有填充（%s，stride %u），不支持", sc.size.toString().c_str(), sc.stride);
		return err(Status::INTERNAL_ERROR);
	}

	stream_ = sc.stream();
	srcWidth_ = sc.size.width;
	srcHeight_ = sc.size.height;

	/* 上一次配置若在建池之后失败（start 失败等），池里还是指向旧 allocator 缓冲的 Request ——
	 * 换 allocator 之前先清掉，别让新旧两代混在一个池里。此时没有在途请求（streaming_ 为假）。 */
	freeRequests_.clear();
	configRequests_.clear();
	allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(cam_);
	if (allocator_->allocate(stream_) < 0) {
		ALOGE("分配 libcamera 缓冲失败");
		return err(Status::INTERNAL_ERROR);
	}
	for (const auto &b : allocator_->buffers(stream_)) {
		auto r = cam_->createRequest();
		if (!r || r->addBuffer(stream_, b.get())) {
			ALOGE("建 libcamera 请求失败");
			return err(Status::INTERNAL_ERROR);
		}
		configRequests_.insert(r.get());
		freeRequests_.push_back(std::move(r));
	}

	ALOGI("空闲请求池已建：%zu 个（bufferCount=%d，allocator 缓冲=%zu 个）",
	      freeRequests_.size(), sc.bufferCount, allocator_->buffers(stream_).size());
	if (freeRequests_.empty())
		ALOGE("★★ 池子为空：allocator 没给出任何缓冲 —— 预览必然冻结，这就是池枯竭根因");

	if (cam_->start()) {
		ALOGE("Camera::start 失败");
		return err(Status::INTERNAL_ERROR);
	}
	streaming_ = true;

	halStreams_.clear();
	for (const auto &s2 : cfg.streams) {
		const bool isBlob =
			s2.format == aidl::android::hardware::graphics::common::PixelFormat::BLOB;
		/* ⚠️ BLOB 流的 bufferSize 由框架给（Stream::bufferSize）；
		 *    它是【字节数】，不是像素宽高。 */
		halStreams_.push_back({ s2.id, s2.width, s2.height, isBlob,
					isBlob ? s2.bufferSize : 0 });

		HalStream hs;
		hs.id = s2.id;
		/* IMPLEMENTATION_DEFINED 要由 HAL 定成具体格式；我们交付 YCbCr_420_888。 */
		hs.overrideFormat =
			(s2.format == aidl::android::hardware::graphics::common::PixelFormat::
					      IMPLEMENTATION_DEFINED)
				? aidl::android::hardware::graphics::common::PixelFormat::YCBCR_420_888
				: s2.format;
		hs.producerUsage =
			aidl::android::hardware::graphics::common::BufferUsage::CPU_WRITE_OFTEN;
		hs.consumerUsage =
			static_cast<aidl::android::hardware::graphics::common::BufferUsage>(0);
		hs.maxBuffers = 4;
		hs.overrideDataSpace = s2.dataSpace;
		hs.physicalCameraId = "";
		hs.supportOffline = false;
		hs.enableHalBufferManager = false;
		out->push_back(hs);
	}

	/*
	 * ★ 2026-09-14（#109）：libcamera 若没能建起软件 ISP（IPA 模块找不到、调优文件缺失……），
	 *   simple 流水线会"disabling software debayering"，然后把【裸拜耳】当成配置结果交回来；
	 *   下面的 deliver() 把它当 RGB24 喂 libyuv，越界读 ⇒ provider SIGSEGV 循环重启，
	 *   应用只看到"无法连接相机"。v0.6.0 正式镜像就是这样死的（IPA 装错目录）。
	 *   这里把它变成一条能读懂的错误。
	 */
	if (sc.pixelFormat != libcamera::formats::RGB888) {
		ALOGE("libcamera 配置出来的不是 RGB888 而是 %s —— 软件 ISP 没建起来？"
		      "看 libcamera.log 里有没有 'No IPA found' / 'Creating IPA for software ISP failed'",
		      sc.pixelFormat.toString().c_str());
		return err(Status::INTERNAL_ERROR);
	}

	ALOGI("流已配置：%zu 路，libcamera 源 %dx%d 输出 %s",
	      halStreams_.size(), srcWidth_, srcHeight_,
	      sc.pixelFormat.toString().c_str());
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::configureStreamsV2(
	const StreamConfiguration &cfg,
	aidl::android::hardware::camera::device::ConfigureStreamsRet *out)
{
	return configureStreams(cfg, &out->halStreams);
}

ndk::ScopedAStatus Session::constructDefaultRequestSettings(RequestTemplate type,
							    CameraMetadata *out)
{
	out->metadata = buildDefaultRequest(static_cast<int>(type), facts_);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::flush()
{
	/* 我们不排队积压请求，libcamera 自己会把在途的完成掉。 */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::isReconfigurationRequired(const CameraMetadata &,
						      const CameraMetadata &, bool *out)
{
	*out = false;
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::processCaptureRequest(const std::vector<CaptureRequest> &reqs,
						  const std::vector<BufferCache> &cachesToRemove,
						  int32_t *out)
{
	std::unique_lock<std::mutex> lk(mutex_);
	dropCaches(cachesToRemove);
	*out = 0;

	/*
	 * ★★ 契约：返回 OK 时 *out 必须等于 reqs.size()。
	 *   框架（AidlCamera3Device.cpp:1322-1349、Camera3Device.cpp:3546-3589）遇到"OK 但只处理了
	 *   n/N"时会把【整批】的在途 buffer 簿记弹掉，没提交的请求既不报错也不重试 —— 它们早已登记
	 *   为在途，于是之后的 waitUntilDrained 必然超时、ERROR_CAMERA_DEVICE。
	 *   所以只有两种结局：全收，或返回错误（框架按设备错误处理；它自己的注释：
	 *   "Bad metadata failures should come through notify"）。
	 *   ⚠️ PR #6 初版在 !streaming_ 时"接受 0 并返回 OK"、池空时"接受一部分"—— 正是上面那种。
	 */
	if (closed_ || !streaming_) {
		ALOGE("processCaptureRequest：会话%s，拒绝 %zu 个请求",
		      closed_ ? "已关闭" : "还没配置流", reqs.size());
		return err(Status::ILLEGAL_ARGUMENT);
	}

	for (const auto &r : reqs) {
		/*
		 * ★ 设置先读：FMQ 是一条顺序队列，这个请求的那一段不读走，下一个请求就会读到它的。
		 *   设置可能走 FMQ（fmqSettingsSize > 0 时 settings 字段是空的）——框架优先写队列。
		 */
		std::vector<uint8_t> explicitSettings;
		if (r.fmqSettingsSize > 0) {
			explicitSettings.resize(static_cast<size_t>(r.fmqSettingsSize));
			if (!requestQueue_->read(reinterpret_cast<int8_t *>(explicitSettings.data()),
						 static_cast<size_t>(r.fmqSettingsSize))) {
				ALOGE("从 FMQ 读请求设置失败（frame=%d，%lld 字节）", r.frameNumber,
				      (long long)r.fmqSettingsSize);
				return err(Status::INTERNAL_ERROR);
			}
		} else {
			explicitSettings = r.settings.metadata;
		}

		if (r.outputBuffers.empty()) {
			ALOGE("frame=%d 没有输出缓冲（请求不合法）", r.frameNumber);
			return err(Status::ILLEGAL_ARGUMENT);
		}

		/*
		 * 空闲请求：框架每路最多 maxBuffers(=池大小 4) 个在途，而完成回调在【交结果之前】就把
		 * Request 还回池（见 onRequestCompleted），所以正常情况下这里不会空。真空了就是
		 * 有请求卡在 libcamera 里 —— 有界地等一会儿，还是没有就如实报错，不做"只收一部分"。
		 */
		if (freeRequests_.empty()) {
			const bool got = freeCv_.wait_for(lk, std::chrono::milliseconds(kPoolWaitMs), [&] {
				return !freeRequests_.empty() || !streaming_;
			});
			if (!got || !streaming_) {
				ALOGE("frame=%d：等了 %d ms 仍没有空闲请求（在途 %zu）", r.frameNumber,
				      kPoolWaitMs, pending_.size());
				return err(Status::INTERNAL_ERROR);
			}
		}
		auto lreq = std::move(freeRequests_.front());
		freeRequests_.pop_front();

		lreq->reuse(libcamera::Request::ReuseBuffers);
		libcamera::Request *key = lreq.get();
		Pending p;
		p.frameNumber = r.frameNumber;
		/*
		 * ★ 设置为空 = "与上一帧相同"（CaptureRequest.aidl）。沿用上一帧的设置 —— JPEG 方向/质量、
		 *   结果回显都要它（PR #6 初版在空设置时把方向当 0°、质量当 90：连拍第 2 张起就歪）。
		 *   但沿用的那份先经 stripTriggers() 去掉一次性触发，否则 AF_TRIGGER=START 会在之后
		 *   每一帧重复生效。
		 */
		if (!explicitSettings.empty()) {
			lastSettings_ = stripTriggers(explicitSettings);
			p.settings = std::move(explicitSettings);
		} else {
			p.settings = lastSettings_;
		}

		/* ⚠️ 现在就导入每一路的缓冲：AIDL 的 StreamBuffer 不可拷贝
		 *    （见 Session.h 的说明），而入参是 const&，move 不出来。 */
		for (const auto &ob : r.outputBuffers) {
			const HalStreamInfo *hs = nullptr;
			for (const auto &h : halStreams_)
				if (h.id == ob.streamId)
					hs = &h;
			if (!hs) {
				ALOGE("frame=%d 用了未配置的流 id=%d", r.frameNumber, ob.streamId);
				freeRequests_.push_front(std::move(lreq));
				return err(Status::ILLEGAL_ARGUMENT);
			}
			buffer_handle_t hnd = getBuffer(ob);
			if (!hnd) {
				/* ⚠️ 不要在这里 free：句柄归缓存所有，下一帧还要用。 */
				freeRequests_.push_front(std::move(lreq));
				return err(Status::INTERNAL_ERROR);
			}
			p.buffers.push_back({ ob.streamId, ob.bufferId, hnd, hs->width, hs->height,
					      hs->isBlob, hs->blobSize });
		}

		/* ── 闪光灯策略（见 Session.h 里的说明）── */
		{
			uint8_t trigger = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
			uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
			parseFlashControls(p.settings, &trigger, &intent);
			if (!flashLed_.empty()) {
				bool hasBlob = false;
				for (const auto &b : p.buffers)
					hasBlob = hasBlob || b.isBlob;
				const bool torch = flashMode_ == ANDROID_FLASH_MODE_TORCH;
				/* "暗" = 画面均值低，或 AGC 已把模拟增益推到 kDarkGain 以上（后者更可靠：
				 * AGC 会把能救的场景拉亮，增益高就是它在硬撑）。 */
				const bool dark = lastLuma_ < kDarkLuma || lastGain_ >= kDarkGain;
				const bool flashAe =
					aeMode_ == ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH ||
					(aeMode_ == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH && dark);
				if (trigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START) {
					flashArmed_ = flashAe;
					precaptureActive_ = true;
					precaptureFrames_ = 0;
					lumaStable_ = 0;
				} else if (trigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL) {
					flashArmed_ = false;
					precaptureActive_ = false;
				}
				const bool still = hasBlob ||
						   intent == ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE ||
						   flashMode_ == ANDROID_FLASH_MODE_SINGLE;
				const bool fire = torch || (flashAe && (flashArmed_ || still));
				if (fire) {
					setLed(true);
					firedPending_++;
				} else if (firedPending_ == 0) {
					setLed(false);
				}
				p.flashFired = fire;
				p.endsFlash = flashArmed_ && still && !torch;
				/*
				 * 预闪收敛：灯是在这个请求点亮的，而在途的几帧还是没灯的曝光；等 libcamera 的
				 * AGC 在灯光下重新收敛（亮度连续 2 帧变化 < kLumaStableDelta）再报 CONVERGED，
				 * 应用才按快门。至少 kPrecaptureMin 帧兜住排队深度，最多 kPrecaptureMax 帧兜住不收敛。
				 */
				if (precaptureActive_) {
					precaptureFrames_++;
					if ((precaptureFrames_ >= kPrecaptureMin && lumaStable_ >= 2) ||
					    precaptureFrames_ >= kPrecaptureMax)
						precaptureActive_ = false;
				}
				if (precaptureActive_) {
					p.aeState = ANDROID_CONTROL_AE_STATE_PRECAPTURE;
				} else if (aeMode_ == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH && dark &&
					   !fire) {
					p.aeState = ANDROID_CONTROL_AE_STATE_FLASH_REQUIRED;
				} else {
					p.aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
				}
			}
			p.aeMode = aeMode_;
			p.flashMode = flashMode_;
		}

		/*
		 * ── 对焦控制（只后摄有马达）──
		 * ⚠️ 两个键都必须"请求里带了才动"：ANDROID_CONTROL_AF_MODE 是粘滞的
		 *    （框架通常只在模式变化那一帧才带），而 AF_TRIGGER 只在
		 *    START/CANCEL 那一刻出现，其余帧是 IDLE。
		 *    用 -1 当"没带"的哨兵 —— 0 是合法值（AF_MODE_OFF /
		 *    AF_TRIGGER_IDLE），不能用 0 兜底。
		 */
		if (facts_.hasAf) {
			const int32_t afMode = requestEntryInt(p.settings, ANDROID_CONTROL_AF_MODE, -1);
			if (afMode >= 0)
				af_.setMode(static_cast<uint8_t>(afMode));
			const int32_t afTrig = requestEntryInt(p.settings, ANDROID_CONTROL_AF_TRIGGER, -1);
			if (afTrig > 0)   /* 1=START 2=CANCEL；0=IDLE 不必下发 */
				af_.trigger(static_cast<uint8_t>(afTrig));
		}

		const bool fired = p.flashFired;
		pending_[key] = std::move(p);

		if (cam_->queueRequest(key)) {
			ALOGE("queueRequest 失败 frame=%d", r.frameNumber);
			pending_.erase(key);   /* 句柄归缓存，不在这里释放 */
			if (fired && --firedPending_ == 0 && !flashArmed_ &&
			    flashMode_ != ANDROID_FLASH_MODE_TORCH)
				setLed(false);
			freeRequests_.push_front(std::move(lreq));
			return err(Status::INTERNAL_ERROR);
		}
		/*
		 * ★ libcamera 从 queueRequest 起接管裸指针，完成回调里再包回
		 *   freeRequests_（见 onRequestCompleted）。
		 * ★ pending_ 只存簿记（键=裸指针），【不】持有 Request 所有权——
		 *   否则 close()/configureStreams() 里的 pending_.clear() 会析构
		 *   在途请求 → use-after-free。这条是硬约束，别再改回去。
		 */
		lreq.release();
		(*out)++;
	}
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::switchToOffline(
	const std::vector<int32_t> &,
	aidl::android::hardware::camera::device::CameraOfflineSessionInfo *,
	std::shared_ptr<aidl::android::hardware::camera::device::ICameraOfflineSession> *out)
{
	*out = nullptr;
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Session::repeatingRequestEnd(int32_t, const std::vector<int32_t> &)
{
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getCaptureRequestMetadataQueue(
	::aidl::android::hardware::common::fmq::MQDescriptor<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite> *out)
{
	*out = requestQueue_->dupeDesc();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getCaptureResultMetadataQueue(
	::aidl::android::hardware::common::fmq::MQDescriptor<
		int8_t, ::aidl::android::hardware::common::fmq::SynchronizedReadWrite> *out)
{
	*out = resultQueue_->dupeDesc();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::signalStreamFlush(const std::vector<int32_t> &, int32_t)
{
	/* HAL buffer manager 关着（enableHalBufferManager=false），无事可做。 */
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */

/* ────────────────────────── 帧交付 ────────────────────────── */

namespace gaokun3 {

buffer_handle_t Session::getBuffer(const StreamBuffer &sb)
{
	const auto key = std::make_pair(sb.streamId, sb.bufferId);
	auto it = bufferCache_.find(key);
	if (it != bufferCache_.end())
		return it->second;

	/*
	 * 第一次见到这个 bufferId —— 此时（且仅此时）框架给的句柄是有效的。
	 * ★ StreamBuffer.aidl 原文："If the bufferId has been sent to the HAL
	 *   before, this buffer handle must be empty and HAL must look up the
	 *   actual buffer handle to use from its own bufferId to buffer handle map."
	 */
	const native_handle_t *raw = ::android::makeFromAidl(sb.buffer);
	if (!raw) {
		ALOGE("makeFromAidl 失败 stream=%d buffer=%lld", sb.streamId,
		      (long long)sb.bufferId);
		return nullptr;
	}

	/*
	 * ⚠️★ 用 importBufferNoValidate()，不要用带宽高/格式/usage 的那个重载：
	 *   gralloc4 会拿传进去的描述符跟缓冲【实际分配时】的参数比对，对不上
	 *   返回 BAD_BUFFER(2)，而我们并不知道框架分配时用的确切 usage
	 *   （producerUsage | consumerUsage 再加框架自己的位，我们只声明了一半）。
	 *   ★ 判据：错误码 2 是 gralloc 的 BAD_BUFFER，不是 errno 的 ENOENT。
	 */
	auto &mapper = android::GraphicBufferMapper::get();
	buffer_handle_t imported = nullptr;
	android::status_t st = mapper.importBufferNoValidate(raw, &imported);
	/*
	 * ⚠️★ 只 delete、不 close：importBufferNoValidate() 成功后【接管了 fd】，
	 *   再 native_handle_close() 会去关已经被接管/关闭的描述符，
	 *   日志刷 "Could not close FD nn: Bad file descriptor"。
	 *   失败时才要自己关掉，否则漏 fd。
	 */
	if (st != android::OK || !imported)
		native_handle_close(const_cast<native_handle_t *>(raw));
	native_handle_delete(const_cast<native_handle_t *>(raw));
	if (st != android::OK || !imported) {
		ALOGE("importBuffer 失败: %d (stream=%d buffer=%lld)", st,
		      sb.streamId, (long long)sb.bufferId);
		return nullptr;
	}

	bufferCache_[key] = imported;
	return imported;
}

void Session::dropCaches(const std::vector<BufferCache> &caches)
{
	for (const auto &c : caches) {
		auto it = bufferCache_.find(std::make_pair(c.streamId, c.bufferId));
		if (it == bufferCache_.end())
			continue;
		android::GraphicBufferMapper::get().freeBuffer(it->second);
		bufferCache_.erase(it);
	}
}

void Session::dropAllCaches()
{
	for (auto &kv : bufferCache_)
		android::GraphicBufferMapper::get().freeBuffer(kv.second);
	bufferCache_.clear();
}

Session::Crop Session::cropFor(int32_t dstW, int32_t dstH) const
{
	int w = srcWidth_, h = srcHeight_;
	if (static_cast<int64_t>(w) * dstH > static_cast<int64_t>(h) * dstW)
		w = static_cast<int>(static_cast<int64_t>(h) * dstW / dstH) & ~1;
	else
		h = static_cast<int>(static_cast<int64_t>(w) * dstH / dstW) & ~1;
	return { ((srcWidth_ - w) / 2) & ~1, ((srcHeight_ - h) / 2) & ~1, w, h };
}

bool Session::deliver(const uint8_t *rgb, buffer_handle_t dst,
		      int32_t dstW, int32_t dstH, int chromaBlur)
{
	const int srcStride = srcWidth_ * 3;

	auto &mapper = android::GraphicBufferMapper::get();
	android_ycbcr ycbcr = {};
	android::status_t st = mapper.lockYCbCr(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
						android::Rect(dstW, dstH), &ycbcr);
	if (st != android::OK || !ycbcr.y) {
		ALOGE("deliver: lockYCbCr 失败: %d", st);
		return false;
	}

	bool ok = false;
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;
	std::vector<uint8_t> i420(static_cast<size_t>(dstW) * dstH +
				  static_cast<size_t>(cw) * chh * 2);
	uint8_t *dy = i420.data();
	uint8_t *du = dy + static_cast<size_t>(dstW) * dstH;
	uint8_t *dv = du + static_cast<size_t>(cw) * chh;

	/*
	 * ★★ 用 J420（全范围）而不是 I420（限制范围 16–235）。
	 *   Android 相机输出的 YUV_420_888 按约定是全范围 BT.601；
	 *   给限制范围会让预览发灰发闷（对比度被压掉约 13%）。
	 *   libyuv 的命名：I420 = 限制范围，J420 = 全范围（JPEG 范围）。
	 *   ⚠️ 这一条是【假说】，靠前后截图 A/B 验证，不是从文档抄来的。
	 *   字节序照旧：convert.h:959 注释明写 "RGB little endian (bgr in
	 *   memory) to J420"，与 libcamera 的 RGB888(=BGR24) 对得上。
	 */
	const Crop c = cropFor(dstW, dstH);
	const uint8_t *crop = rgb + static_cast<size_t>(c.y) * srcStride +
			      static_cast<size_t>(c.x) * 3;
	int rc;
	if (dstW == c.w && dstH == c.h) {
		rc = libyuv::RGB24ToJ420(crop, srcStride, dy, dstW, du, cw, dv, cw,
					 dstW, dstH);
	} else {
		/*
		 * 需要缩放：先在源尺寸上转成 I420，再缩放到目标尺寸。
		 * ⚠️ 顺序不能反 —— libyuv 没有"RGB24 直接缩放到 I420"的接口，
		 *    而先缩放 RGB 再转会多搬一次 3 字节/像素的数据。
		 * ⬜ 这条路每帧多一次全图拷贝；多路预览时值得再优化。
		 */
		const int scw = (c.w + 1) / 2, schh = (c.h + 1) / 2;
		std::vector<uint8_t> src(static_cast<size_t>(c.w) * c.h +
					 static_cast<size_t>(scw) * schh * 2);
		uint8_t *sy = src.data();
		uint8_t *su = sy + static_cast<size_t>(c.w) * c.h;
		uint8_t *sv = su + static_cast<size_t>(scw) * schh;
		rc = libyuv::RGB24ToJ420(crop, srcStride, sy, c.w, su, scw, sv, scw,
					 c.w, c.h);
		if (rc == 0)
			rc = libyuv::I420Scale(sy, c.w, su, scw, sv, scw, c.w, c.h,
					       dy, dstW, du, cw, dv, cw,
					       dstW, dstH, libyuv::kFilterBox);
	}

	if (rc == 0 && chromaBlur > 0) {
		/* 色度降噪：U/V 只有 1/4 分辨率，盒式模糊几乎免费，而彩色噪点正是最难看的那种。 */
		boxBlurPlane(du, cw, chh, cw, chromaBlur);
		boxBlurPlane(dv, cw, chh, cw, chromaBlur);
	}

	if (rc == 0) {
		for (int y = 0; y < dstH; y++)
			memcpy(static_cast<uint8_t *>(ycbcr.y) + y * ycbcr.ystride,
			       dy + static_cast<size_t>(y) * dstW, dstW);
		/* 按 chroma_step 同时支持平面(I420)与半平面(NV12/NV21)。 */
		for (int y = 0; y < chh; y++) {
			uint8_t *cb = static_cast<uint8_t *>(ycbcr.cb) + y * ycbcr.cstride;
			uint8_t *cr = static_cast<uint8_t *>(ycbcr.cr) + y * ycbcr.cstride;
			const uint8_t *su2 = du + static_cast<size_t>(y) * cw;
			const uint8_t *sv2 = dv + static_cast<size_t>(y) * cw;
			for (int x = 0; x < cw; x++) {
				cb[x * ycbcr.chroma_step] = su2[x];
				cr[x * ycbcr.chroma_step] = sv2[x];
			}
		}
		ok = true;
	} else {
		ALOGE("deliver: 色彩转换/缩放失败 rc=%d", rc);
	}

	mapper.unlock(dst);
	return ok;
}

/*
 * 把一帧 RGB 编成 JPEG 写进 Android 的 BLOB 缓冲。
 *
 * ★ Android 的约定（`CameraBlob.aidl` / `CameraBlobId.aidl`）：
 *   JPEG 数据从缓冲开头写，**缓冲末尾**放一个 8 字节的
 *   `CameraBlob{ blobId = JPEG(0x00FF), blobSizeBytes }`。
 *   框架按这个结构去找真实长度 —— 少了它，图片会被当成整个缓冲那么大。
 *
 * ⚠️ libcamera 给的是 B,G,R 顺序（formats::RGB888 → V4L2_PIX_FMT_BGR24，
 *    见 formats.cpp:185），而 libjpeg 的 JCS_EXT_BGR 正好对应它 ——
 *    用 JCS_RGB 会红蓝互换，而且不报任何错。
 *
 * ★★ 2026-09-19：补上 JPEG 旋转（照片方向不对的根因就在这一处）。
 *   见下面 "旋转" 那一段的说明。
 */
bool Session::deliverJpeg(const uint8_t *rgb, buffer_handle_t dst,
			  int32_t dstW, int32_t dstH, int32_t blobSize,
			  int quality, int32_t jpegOrientation)
{
	auto &mapper = android::GraphicBufferMapper::get();
	void *raw = nullptr;
	android::status_t st = mapper.lock(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
					   android::Rect(blobSize, 1), &raw);
	if (st != android::OK || !raw) {
		ALOGE("deliverJpeg: lock 失败: %d", st);
		return false;
	}

	/*
	 * ★★ 旋转：必须在【编码之前】把像素真的转过来。
	 *
	 *   ANDROID_JPEG_ORIENTATION 是应用按"传感器朝向 + 当前设备旋转"算出来的
	 *   顺时针校正角。HAL 的合同是把它落到交付物上，两个合法做法二选一：
	 *     ① 旋转像素，交付物里不要再有方向标记；
	 *     ② 不转像素，把这个值写进 EXIF 的 Orientation 标记
	 *        （上游 libcamera 的 Android HAL 走的就是这条：
	 *         post_processor_jpeg.cpp 里 exif.setOrientation(jpegOrientation)）。
	 *   本 HAL 选 ①，因为它是更强的保证 —— 不依赖看图程序是否解析 EXIF。
	 *   ⚠️ 两者只能选一个：又转像素又写 EXIF 的旋转值 = 转两次，反而歪。
	 *   像素已经转正，所以这里【不需要】libexif；缺 Orientation 标记 ≡ 1（正常），
	 *   这正是我们想要的结果，别事后又去补一个别样的标记。
	 *
	 * ⚠️ 90/270 会把宽高换过来：流的 (dstW, dstH) 是【传感器坐标系】里的尺寸，
	 *   转完之后 JPEG 的实际尺寸是 (dstH, dstW)。Android 允许这样 —— BLOB 流
	 *   只声明缓冲字节数，应用自己读 JPEG 头取真实尺寸。不要为了"跟流尺寸对上"
	 *   把宽高改回去，那就等于转了个寂寞。
	 */
	const libyuv::RotationMode rotMode = toLibyuvRotation(jpegOrientation);
	const bool swapDims = rotMode == libyuv::kRotate90 || rotMode == libyuv::kRotate270;
	const int32_t outW = swapDims ? dstH : dstW;
	const int32_t outH = swapDims ? dstW : dstH;

	/*
	 * 源尺寸与目标不同就得缩放；要旋转就得先有一个 4 通道缓冲。
	 * ⚠️ libyuv **没有** RGBScale（我一开始想当然写了，编译器拦下），
	 *    也没有 RGB24 的旋转 —— 两条路都走 ARGB 中间体：libyuv 的 "ARGB"
	 *    在内存里是 B,G,R,A，正好对上 libjpeg 的 JCS_EXT_BGRA，不用再转回
	 *    3 通道，也正好对上 ARGBScale / ARGBRotate 的入参。
	 */
	const Crop c = cropFor(dstW, dstH);
	const uint8_t *crop = rgb + static_cast<size_t>(c.y) * srcWidth_ * 3 +
			      static_cast<size_t>(c.x) * 3;
	const bool needScale = dstW != c.w || dstH != c.h;
	const bool needRotate = rotMode != libyuv::kRotate0;

	std::vector<uint8_t> argb;      /* dstW×dstH，已缩放、未旋转（BGRA） */
	std::vector<uint8_t> rotated;   /* outW×outH，已旋转（BGRA） */
	const uint8_t *src = crop;
	int srcStride = srcWidth_ * 3;
	int components = 3;
	J_COLOR_SPACE colorSpace = JCS_EXT_BGR;   /* ★ 字节序见上面的说明 */

	if (needScale || needRotate) {
		argb.resize(static_cast<size_t>(dstW) * dstH * 4);

		if (needScale) {
			std::vector<uint8_t> full(static_cast<size_t>(c.w) * c.h * 4);
			if (libyuv::RGB24ToARGB(crop, srcWidth_ * 3, full.data(),
						c.w * 4, c.w, c.h) != 0) {
				ALOGE("deliverJpeg: RGB24ToARGB 失败");
				mapper.unlock(dst);
				return false;
			}
			if (libyuv::ARGBScale(full.data(), c.w * 4, c.w, c.h,
					      argb.data(), dstW * 4, dstW, dstH,
					      libyuv::kFilterBox) != 0) {
				ALOGE("deliverJpeg: ARGBScale 失败");
				mapper.unlock(dst);
				return false;
			}
		} else {
			if (libyuv::RGB24ToARGB(crop, srcWidth_ * 3, argb.data(), dstW * 4,
						dstW, dstH) != 0) {
				ALOGE("deliverJpeg: RGB24ToARGB 失败");
				mapper.unlock(dst);
				return false;
			}
		}

		if (needRotate) {
			rotated.resize(static_cast<size_t>(outW) * outH * 4);
			/* ★ 目标 stride 用【旋转后】的宽度 outW ——
			 *   沿用 dstW 会让每一行都错位（画面斜切成条）。 */
			if (libyuv::ARGBRotate(argb.data(), dstW * 4, rotated.data(),
					       outW * 4, dstW, dstH, rotMode) != 0) {
				ALOGE("deliverJpeg: ARGBRotate(%d°) 失败", jpegOrientation);
				mapper.unlock(dst);
				return false;
			}
			src = rotated.data();
			srcStride = outW * 4;
		} else {
			src = argb.data();
			srcStride = dstW * 4;
		}
		components = 4;
		colorSpace = JCS_EXT_BGRA;
	}

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	/* 直接写进 gralloc 缓冲；留出末尾 8 字节给 CameraBlob。 */
	const size_t maxJpeg = static_cast<size_t>(blobSize) - sizeof(CameraBlob);
	unsigned char *out = static_cast<unsigned char *>(raw);
	unsigned long outSize = maxJpeg;
	jpeg_mem_dest(&cinfo, &out, &outSize);

	cinfo.image_width = outW;
	cinfo.image_height = outH;
	cinfo.input_components = components;
	cinfo.in_color_space = colorSpace;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, quality, TRUE);
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = const_cast<JSAMPROW>(
			src + static_cast<size_t>(cinfo.next_scanline) * srcStride);
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	const size_t jpegLen = outSize;
	jpeg_destroy_compress(&cinfo);

	bool ok = false;
	if (jpegLen > 0 && jpegLen <= maxJpeg) {
		/* jpeg_mem_dest 可能自己 malloc 了新缓冲（超出我们给的大小时），
		 * 那种情况下 out 不再指向 gralloc，要拷回去。 */
		if (out != raw)
			memcpy(raw, out, jpegLen);
		CameraBlob blob;
		blob.blobId = CameraBlobId::JPEG;
		blob.blobSizeBytes = static_cast<int32_t>(jpegLen);
		memcpy(static_cast<uint8_t *>(raw) + blobSize - sizeof(CameraBlob),
		       &blob, sizeof(CameraBlob));
		ok = true;
		ALOGD("JPEG %dx%d（请求 %dx%d，旋转 %d°）质量%d → %zu 字节",
		      outW, outH, dstW, dstH, jpegOrientation, quality, jpegLen);
	} else {
		ALOGE("deliverJpeg: JPEG 长度异常 %zu（上限 %zu）", jpegLen, maxJpeg);
	}
	if (out != raw)
		free(out);

	mapper.unlock(dst);
	return ok;
}

int Session::denoiseThreshold() const
{
	/* 调用方不持锁也没关系：lastGain_ 是个 double，读到旧值只是差一帧的强度。 */
	if (lastGain_ < kDenoiseGain)
		return 0;
	return std::clamp(static_cast<int>(lastGain_ * 3.0), 6, 36);
}

int Session::chromaBlurRadius() const
{
	if (lastGain_ < kDenoiseGain)
		return 0;
	return lastGain_ < 5.0 ? 1 : 2;
}

/*
 * 3×3 ε 滤波：只把与中心像素相差 ≤ T 的邻居算进平均 —— 平掉噪声、保住边缘（边缘两侧差值大于 T，
 * 不会被平均进来）。按行切给 4 个线程；13 MP 单线程约 1 秒，四线程约 0.3 秒，静态照片能接受。
 */
void Session::epsilonFilterRgb(const uint8_t *src, uint8_t *dst, int w, int h, int threshold)
{
	auto work = [=](int y0, int y1) {
		for (int y = y0; y < y1; y++) {
			const int ym = y > 0 ? y - 1 : y;
			const int yp = y < h - 1 ? y + 1 : y;
			const uint8_t *r0 = src + static_cast<size_t>(ym) * w * 3;
			const uint8_t *r1 = src + static_cast<size_t>(y) * w * 3;
			const uint8_t *r2 = src + static_cast<size_t>(yp) * w * 3;
			uint8_t *out = dst + static_cast<size_t>(y) * w * 3;
			for (int x = 0; x < w; x++) {
				const int xm = (x > 0 ? x - 1 : x) * 3;
				const int x0 = x * 3;
				const int xp = (x < w - 1 ? x + 1 : x) * 3;
				for (int c = 0; c < 3; c++) {
					const int center = r1[x0 + c];
					int sum = center, n = 1;
					const int nb[8] = { r0[xm + c], r0[x0 + c], r0[xp + c],
							    r1[xm + c],             r1[xp + c],
							    r2[xm + c], r2[x0 + c], r2[xp + c] };
					for (int v : nb) {
						if (std::abs(v - center) <= threshold) {
							sum += v;
							n++;
						}
					}
					out[x0 + c] = static_cast<uint8_t>((sum + n / 2) / n);
				}
			}
		}
	};
	const int nthreads = std::clamp(h / 256, 1, 4);
	std::vector<std::thread> pool;
	for (int t = 0; t < nthreads; t++) {
		const int y0 = h * t / nthreads, y1 = h * (t + 1) / nthreads;
		pool.emplace_back(work, y0, y1);
	}
	for (auto &t : pool)
		t.join();
}

/* 可分离盒式模糊（先横后纵），半径 radius，边界夹紧。用在 1/4 分辩率的色度平面上。 */
void Session::boxBlurPlane(uint8_t *plane, int w, int h, int stride, int radius)
{
	if (radius <= 0 || w <= 0 || h <= 0)
		return;
	std::vector<uint8_t> tmp(static_cast<size_t>(w) * h);
	const int win = 2 * radius + 1;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = plane + static_cast<size_t>(y) * stride;
		uint8_t *out = tmp.data() + static_cast<size_t>(y) * w;
		for (int x = 0; x < w; x++) {
			int sum = 0;
			for (int k = -radius; k <= radius; k++)
				sum += row[std::clamp(x + k, 0, w - 1)];
			out[x] = static_cast<uint8_t>((sum + win / 2) / win);
		}
	}
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) {
			int sum = 0;
			for (int k = -radius; k <= radius; k++)
				sum += tmp[static_cast<size_t>(std::clamp(y + k, 0, h - 1)) * w + x];
			plane[static_cast<size_t>(y) * stride + x] = static_cast<uint8_t>((sum + win / 2) / win);
		}
	}
}

void Session::reportRequestError(const Pending &p)
{
	/*
	 * ERROR_REQUEST 的合同（ErrorCode.aidl:33-39）："No output (metadata or buffers) must be
	 * produced for this request"，但缓冲仍要带 ERROR 状态还回去，框架据此把这一帧移出在途表。
	 * 不发 shutter —— 这一帧没有曝光时间戳，而 shutter 必须按帧号递增，乱发反而是设备错误。
	 * ★ 只在完成回调里调用：libcamera 按入队顺序完成请求，报错因此天然按帧序。
	 */
	NotifyMsg msg;
	ErrorMsg e;
	e.frameNumber = p.frameNumber;
	e.errorStreamId = -1;
	e.errorCode = ErrorCode::ERROR_REQUEST;
	msg.set<NotifyMsg::Tag::error>(e);
	std::vector<NotifyMsg> msgs;
	msgs.push_back(std::move(msg));
	cb_->notify(msgs);

	CaptureResult result;
	result.frameNumber = p.frameNumber;
	result.fmqResultSize = 0;
	result.partialResult = 0;   /* 只还缓冲、没有元数据 */
	for (const auto &b : p.buffers) {
		StreamBuffer sb;
		sb.streamId = b.streamId;
		sb.bufferId = b.bufferId;
		sb.status = BufferStatus::ERROR;
		result.outputBuffers.push_back(std::move(sb));
	}
	result.inputBuffer.streamId = -1;
	result.inputBuffer.bufferId = 0;
	std::vector<CaptureResult> results;
	results.push_back(std::move(result));
	cb_->processCaptureResult(results);
}

/* 调用者持 mutex_。点过灯的请求结束（无论成败）时的闪光灯簿记。 */
void Session::flashRequestDone(const Pending &p)
{
	if (p.endsFlash)
		flashArmed_ = false;
	if (p.flashFired && --firedPending_ == 0 && !flashArmed_ &&
	    flashMode_ != ANDROID_FLASH_MODE_TORCH)
		setLed(false);
}

/* 调用者持 mutex_。只收【本次配置】建的 Request；上一代的（重配置后迟到的）直接释放。 */
void Session::returnToPool(std::unique_ptr<libcamera::Request> r)
{
	if (configRequests_.count(r.get())) {
		freeRequests_.push_back(std::move(r));
		freeCv_.notify_one();
	}
}

void Session::onRequestCompleted(libcamera::Request *req)
{
	/*
	 * ★ 所有权模型（别再改）：idle 时 Request 归 freeRequests_；queueRequest
	 *   之后由 libcamera 持有裸指针；本回调收到时 libcamera 已交还 —— 这里立刻包回
	 *   unique_ptr，每条路径最后都经 returnToPool() 还池（或随作用域释放上一代的）。
	 *   丢一个 Request = 池子少一个 = 迟早枯竭。
	 */
	std::unique_ptr<libcamera::Request> owned(req);

	Pending pend;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		auto it = pending_.find(req);
		if (it == pending_.end()) {
			/*
			 * 不认识的请求：只可能是 configureStreams 强制清理（drain 超时）之后迟到的。
			 * ⚠️ 绝不能往下走去"交付"：它没有 frameNumber（PR #6 初版会给第 0 帧发 shutter
			 *    —— 框架里不存在的帧 ⇒ SET_ERR），它的 FrameBuffer 也可能已随旧 allocator 释放。
			 */
			ALOGW("完成了一个不认识的请求 %p（status=%d），丢弃", (void *)req,
			      static_cast<int>(req->status()));
			returnToPool(std::move(owned));
			return;
		}
		pend = std::move(it->second);
		pending_.erase(it);
		if (pending_.empty())
			drainCv_.notify_all();
	}

	/*
	 * libcamera 撤销的请求（stop() 里撤销在途请求，或 queueRequestDevice 失败）：
	 * 必须告诉框架 —— PR #6 初版只把 Request 还池，框架一直等这一帧，下一次
	 * configureStreams/close 超时 → ERROR_CAMERA_DEVICE。
	 */
	if (req->status() == libcamera::Request::RequestCancelled || req->buffers().empty()) {
		if (req->status() != libcamera::Request::RequestCancelled)
			ALOGE("frame=%d 完成的请求里没有 buffer", pend.frameNumber);
		reportRequestError(pend);
		std::lock_guard<std::mutex> lk(mutex_);
		flashRequestDone(pend);
		returnToPool(std::move(owned));
		return;
	}

	const libcamera::FrameBuffer *fb = req->buffers().begin()->second;
	const int64_t timestamp = fb->metadata().timestamp;

	/* mmap 一次，分发给所有流。 */
	const auto &planes = fb->planes();
	const uint8_t *rgb = nullptr;
	void *srcMap = MAP_FAILED;
	size_t srcLen = 0;
	if (!planes.empty()) {
		srcLen = planes[0].offset + planes[0].length;
		srcMap = mmap(nullptr, srcLen, PROT_READ, MAP_SHARED,
			      planes[0].fd.get(), 0);
		if (srcMap != MAP_FAILED)
			rgb = static_cast<const uint8_t *>(srcMap) + planes[0].offset;
		else
			ALOGE("mmap libcamera 帧失败: %s", strerror(errno));
		/* 第二道保险（#109）：帧不够 RGB24 的尺寸就整帧报 ERROR_BUFFER，绝不越界读。 */
		if (rgb && planes[0].length < static_cast<size_t>(srcWidth_) * srcHeight_ * 3) {
			ALOGE("libcamera 帧只有 %zu 字节，不够 %dx%d 的 RGB24 —— 丢弃这一帧",
			      static_cast<size_t>(planes[0].length), srcWidth_, srcHeight_);
			rgb = nullptr;
		}
	}

	/* 采样平均亮度（每 16 行 × 每 16 列），给 AUTO 闪光当"太暗"判据。软件 ISP 的 IPA
	 * 不往结果元数据里写曝光/增益，这是我们唯一现成的亮度信号。
	 * ★ 同一个信号还给自动对焦当"AE 已收敛"判据（见 AutoFocus.h 的 sceneStable）。 */
	int lumaStableFor = 0;
	if (rgb) {
		uint64_t sum = 0;
		uint32_t n = 0;
		for (int y = 0; y < srcHeight_; y += 16) {
			const uint8_t *px = rgb + static_cast<size_t>(y) * srcWidth_ * 3;
			for (int x = 0; x < srcWidth_; x += 16, px += 48) {
				sum += px[0] + 2u * px[1] + px[2];
				n++;
			}
		}
		if (n) {
			const int luma = static_cast<int>(sum / (4ull * n));
			std::lock_guard<std::mutex> lk(mutex_);
			lumaStable_ = (std::abs(luma - lastLuma_) < kLumaStableDelta) ? lumaStable_ + 1 : 0;
			lastLuma_ = luma;
			lumaStableFor = lumaStable_;
		}
	}

	/*
	 * ── 自动对焦：喂这一帧的清晰度，必要时把马达挪一步（见 AutoFocus.cpp）──
	 * ★ 时机的关键：这里挪位置，下一帧的请求是在 processCaptureRequest 里入队的，
	 *   到那时新位置已经写进马达，所以"下一帧的分数"确实属于新位置 —— 等价于
	 *   每帧采一个点。（[#95] 时每帧70–130 ms；现在全尺寸约33 ms。）
	 */
	if (rgb && facts_.hasAf)
		af_.onFrame(rgb, srcWidth_, srcHeight_, lumaStableFor >= 2);

	/* libcamera（libipa 的 Agc::fillMetadata）每帧报实际曝光与模拟增益：回填结果、驱动降噪强度与闪光判据。 */
	int64_t exposureNs = 0;
	int32_t sensitivity = 0;
	{
		const libcamera::ControlList &md = req->metadata();
		auto gain = md.get(libcamera::controls::AnalogueGain);
		auto expo = md.get(libcamera::controls::ExposureTime);
		std::lock_guard<std::mutex> lk(mutex_);
		if (gain && *gain > 0)
			lastGain_ = *gain;
		if (expo && *expo > 0)
			lastExposureUs_ = *expo;
		if (gain)
			sensitivity = static_cast<int32_t>(*gain * 100.0f + 0.5f);
		if (expo)
			exposureNs = static_cast<int64_t>(*expo) * 1000;
	}

	/* 降噪：静态照片走 RGB ε 滤波（有 BLOB 流且增益够高才算，13 MP 不便宜）；预览只模糊色度。 */
	const int denoiseT = denoiseThreshold();
	const int chromaBlur = chromaBlurRadius();
	std::vector<uint8_t> denoised;
	const uint8_t *rgbStill = rgb;
	if (rgb && denoiseT > 0) {
		bool hasBlob = false;
		for (const auto &pb : pend.buffers)
			hasBlob = hasBlob || pb.isBlob;
		if (hasBlob) {
			denoised.resize(static_cast<size_t>(srcWidth_) * srcHeight_ * 3);
			epsilonFilterRgb(rgb, denoised.data(), srcWidth_, srcHeight_, denoiseT);
			rgbStill = denoised.data();
			ALOGD("静态照片降噪：增益 %.2f → ε=%d", lastGain_, denoiseT);
		}
	}

	/* ── 顺序要求：先 shutter，后结果 ── */
	NotifyMsg msg;
	ShutterMsg shutter;
	shutter.frameNumber = pend.frameNumber;
	shutter.timestamp = timestamp;
	shutter.readoutTimestamp = timestamp;
	msg.set<NotifyMsg::Tag::shutter>(shutter);
	std::vector<NotifyMsg> msgs;
	msgs.push_back(std::move(msg));
	cb_->notify(msgs);

	/*
	 * ★★ 拍照参数来自【这一帧的请求】，不是静态元数据。
	 *   ANDROID_JPEG_ORIENTATION = 应用按"传感器朝向 + 当前设备旋转"算好的
	 *     顺时针校正角；ANDROID_JPEG_QUALITY = 它要的质量。
	 *   老代码这两个一个都没读（质量恒 90、方向恒 0）—— 这正是"前后摄照片
	 *   都固定歪一个角度"的直接原因：不管用户怎么拿机器，JPEG 永远是传感器
	 *   原始朝向。两条请求键都取不到时退回 0 / 90，行为与修前一致。
	 */
	const int32_t jpegOrientation =
		requestEntryInt(pend.settings, ANDROID_JPEG_ORIENTATION, 0);
	int32_t jpegQuality = requestEntryInt(pend.settings, ANDROID_JPEG_QUALITY, 90);
	/* libjpeg 只认 1..100；应用给 0 或越界时夹住，别让它产出垃圾。 */
	if (jpegQuality < 1)
		jpegQuality = 1;
	if (jpegQuality > 100)
		jpegQuality = 100;

	std::vector<bool> okv;
	for (auto &pb : pend.buffers) {
		bool ok = rgb && (pb.isBlob
				  ? deliverJpeg(rgbStill, pb.handle, pb.width, pb.height,
						pb.blobSize, jpegQuality, jpegOrientation)
				  : deliver(rgb, pb.handle, pb.width, pb.height, chromaBlur));
		okv.push_back(ok);
		if (!ok) {
			NotifyMsg emsg;
			ErrorMsg e;
			e.frameNumber = pend.frameNumber;
			e.errorStreamId = pb.streamId;
			e.errorCode = ErrorCode::ERROR_BUFFER;
			emsg.set<NotifyMsg::Tag::error>(e);
			std::vector<NotifyMsg> emsgs;
			emsgs.push_back(std::move(emsg));
			cb_->notify(emsgs);
		}
	}

	if (srcMap != MAP_FAILED)
		munmap(srcMap, srcLen);

	/*
	 * ★ req 到这里才算用完（buffers()/metadata() 都读过了、帧已 munmap）⇒ 先还池，再交结果：
	 *   框架拿回 buffer 会立刻发下一个请求，池子必须已经有位置（PR #6 初版在
	 *   processCaptureResult 之后才还，于是框架的下一个请求常常撞上空池）。
	 *   ⚠️ 也不能更早：还回去之后 processCaptureRequest 可能立刻取走并 reuse()
	 *   （实测崩过：tid name=CameraManager，栈顶 onRequestCompleted+1428）。
	 */
	{
		std::lock_guard<std::mutex> lk(mutex_);
		returnToPool(std::move(owned));
	}

	CaptureResult result;
	result.frameNumber = pend.frameNumber;
	result.fmqResultSize = 0;
	result.partialResult = 1;
	FrameResultFacts fr;
	fr.flashState = flashLed_.empty() ? ANDROID_FLASH_STATE_UNAVAILABLE
			: (pend.flashFired ? ANDROID_FLASH_STATE_FIRED : ANDROID_FLASH_STATE_READY);
	fr.aeState = pend.aeState;
	fr.aeMode = pend.aeMode;
	fr.flashMode = pend.flashMode;
	fr.exposureNs = exposureNs;
	fr.sensitivity = sensitivity;
	/*
	 * ★ 对焦结果组：应用（CameraX 的 FocusMeteringControl、各家相机的拍照前 AF）等的是
	 *   【结果里的】AF_STATE 到 FOCUSED_LOCKED。PR #6 初版 AutoFocus 算好了状态却从没填进来，
	 *   于是马达扫到了峰、应用却永远只看到 INACTIVE。
	 */
	fr.hasAf = facts_.hasAf;
	if (facts_.hasAf) {
		fr.afState = af_.afState();
		fr.lensState = af_.lensState();
		fr.focusDistance = af_.focusDistance();
		if (fr.afState != lastAfState_) {
			ALOGI("AF 状态 %u → %u（frame=%d，结果里交给应用的就是这个）",
			      lastAfState_, fr.afState, pend.frameNumber);
			lastAfState_ = fr.afState;
		}
	}
	result.result.metadata = buildResult(pend.settings, timestamp, /*pipelineDepth=*/4, fr);

	for (size_t i = 0; i < pend.buffers.size(); i++) {
		StreamBuffer sb;
		sb.streamId = pend.buffers[i].streamId;
		sb.bufferId = pend.buffers[i].bufferId;
		sb.status = okv[i] ? BufferStatus::OK : BufferStatus::ERROR;
		/* ⚠️ buffer/fence 都留空：框架按 bufferId 认，
		 *    回传句柄会被当成一次新导入。 */
		result.outputBuffers.push_back(std::move(sb));
	}
	result.inputBuffer.streamId = -1;
	result.inputBuffer.bufferId = 0;

	std::vector<CaptureResult> results;
	results.push_back(std::move(result));
	cb_->processCaptureResult(results);

	/* 闪光灯：点过灯的请求都完成了、而且没人还要灯，才灭。 */
	std::lock_guard<std::mutex> lk(mutex_);
	flashRequestDone(pend);
}

} /* namespace gaokun3 */
