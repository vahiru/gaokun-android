/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— 会话：流配置、请求、以及把 libcamera（qcom-ife 管线）给的 NV12 帧
 * 裁剪、缩放后交付到 Android 的 gralloc 缓冲，JPEG 由本层编码。
 */
#include "Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <fcntl.h>
#include <span>
#include <thread>
#include <sys/system_properties.h>
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
#include <linux/dma-buf.h>
#include <log/log.h>
#include <sys/ioctl.h>
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
#include <libcamera/orientation.h>
#include <libcamera/property_ids.h>
#include <libcamera/transform.h>

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
 * 把 jpeg_mem_dest 产出的 JPEG 放进 BLOB 缓冲 raw（blobSize 字节），末尾写 CameraBlob。
 * jpeg_mem_dest 超出给定大小时会自己 malloc 新缓冲，那时 out 不再指向 raw：拷回并释放。
 */
bool finishJpegBlob(void *raw, int32_t blobSize, unsigned char *out, size_t jpegLen)
{
	const size_t maxJpeg = static_cast<size_t>(blobSize) - sizeof(CameraBlob);
	bool ok = false;
	if (jpegLen > 0 && jpegLen <= maxJpeg) {
		if (out != raw)
			memcpy(raw, out, jpegLen);
		CameraBlob blob;
		blob.blobId = CameraBlobId::JPEG;
		blob.blobSizeBytes = static_cast<int32_t>(jpegLen);
		memcpy(static_cast<uint8_t *>(raw) + blobSize - sizeof(CameraBlob),
		       &blob, sizeof(CameraBlob));
		ok = true;
	} else {
		ALOGE("JPEG of %zu bytes is empty or larger than the BLOB buffer (%zu bytes)",
		      jpegLen, maxJpeg);
	}
	if (out != raw)
		free(out);
	return ok;
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
	/* EXIF 的厂商与型号，取 vendor 分区给设备的名字 */
	{
		char v[PROP_VALUE_MAX] = {};
		exifMake_ = __system_property_get("ro.product.vendor.manufacturer", v) > 0 ? v : "HUAWEI";
		exifModel_ = __system_property_get("ro.product.vendor.model", v) > 0 ? v : "gaokun3";
	}
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
	stillThread_ = std::thread([this] { stillWorker(); });
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
		/* JPEG 任务还在读 FULL 的映射、写缓存里的 JPEG 缓冲：等它们做完 */
		waitStillJobs(lk);
		freeRequests_.clear();
		configRequests_.clear();
		reqBuffers_.clear();
		fullFree_.clear();
		fullHeld_.clear();
		pending_.clear();
		dropAllCaches();
		unmapFrames();
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
	/* 任务都做完了（见上），工作线程空闲，在锁外停下 */
	{
		std::lock_guard<std::mutex> g(stillMutex_);
		stillStop_ = true;
	}
	stillCv_.notify_all();
	if (stillThread_.joinable())
		stillThread_.join();
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
	 * ★ 相机应用一定会配多路（预览 + 拍照）。libcamera 只开一路（拍照模式两路，见下），
	 *   再用 libyuv 裁剪、缩放分发到各路流。
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
		waitStillJobs(lk);
		freeRequests_.clear();
		configRequests_.clear();
		reqBuffers_.clear();
		fullFree_.clear();
		fullHeld_.clear();
		pending_.clear();
		unmapFrames();
		allocator_.reset();
	}
	lastSettings_.clear();
	lastAfState_ = 0xff;

	/*
	 * 画面来自硬件 ISP（libcamera 的 qcom-ife 管线）：IFE 输出全范围 BT.601 的 NV12，另可
	 * 配 RAW 流（RDI 写出的传感器原始帧）。
	 */
	{
		auto probe = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
		std::vector<libcamera::PixelFormat> fmts;
		if (probe && !probe->empty())
			fmts = probe->at(0).formats().pixelformats();
		if (std::find(fmts.begin(), fmts.end(), libcamera::formats::NV12) == fmts.end()) {
			ALOGE("libcamera offers no NV12 output: the qcom-ife pipeline is required");
			return err(Status::INTERNAL_ERROR);
		}
	}

	const int32_t halfW = (facts_.activeW / 2 - 8) & ~7;
	const int32_t halfH = facts_.activeH / 2;
	const bool fullSize = !(maxW <= halfW && maxH <= halfH);
	/*
	 * 相位对焦要 RAW 帧，而 ov13b10 只在全尺寸模式里有 PD 像素（见 PhaseDetect.h）：一并
	 * 要一路 RAW。它来自像素通路旁边的 RDI，与 NV12 是同一帧传感器数据。
	 */
	bool withRaw = fullSize && facts_.hasAf;

	/*
	 * 硬件 ISP 能同时出两路 NV12：大的一路走 FULL，给 JPEG；小的一路走显示路径，由 IFE
	 * 自带的缩放器缩小，给预览、录像和 YUV 流。拍照模式下预览就不必每帧由 CPU 从全尺寸
	 * 缩小，不拍照的帧也不用映射、同步全尺寸那一路。只在 JPEG 比其余各路大得多时用：
	 * 显示路径的画面不到 FULL 的一半大。管线做不到时（例如双 IFE）会去掉显示流、返回
	 * Adjusted，下面按实际的配置结果退回单路。
	 */
	int32_t dispW = 0, dispH = 0;
	bool anyBlob = false;
	for (const auto &s2 : cfg.streams) {
		if (s2.format == aidl::android::hardware::graphics::common::PixelFormat::BLOB) {
			anyBlob = true;
			continue;
		}
		dispW = std::max(dispW, s2.width);
		dispH = std::max(dispH, s2.height);
	}
	/*
	 * 有对焦马达时显示路径至少按 1280x960 要：低增益时对焦的梯度能量在显示路径上算，
	 * 预览很小（如 320x240）时不能拿那么小的画面对焦。
	 */
	if (facts_.hasAf && dispW > 0) {
		dispW = std::max(dispW, 1280);
		dispH = std::max(dispH, 960);
	}
	/*
	 * 显示路径的尺寸：与 hwIspFrameSize() 相同，但高度按宽度向下取整。validate 按高度
	 * 推宽度后再 16 对齐，向上取整多出的一行会让宽度多出 16（1296x962 → 1312x972），
	 * 裁出的流比要的大 1% 以上，每帧多缩放一次。
	 */
	auto dispSize = [&]() {
		libcamera::Size sz = hwIspFrameSize(facts_, dispW, dispH);
		const int64_t hf = static_cast<int64_t>(sz.width) * facts_.activeH / facts_.activeW;
		sz.height = static_cast<unsigned int>(std::max<int64_t>(dispH, hf) & ~1);
		return sz;
	};
	bool twoYuv = false;
	if (anyBlob && dispW > 0) {
		const libcamera::Size f = hwIspFrameSize(facts_, maxW, maxH);
		const libcamera::Size d = dispSize();
		twoYuv = static_cast<uint64_t>(d.width) * d.height * 2 <=
			 static_cast<uint64_t>(f.width) * f.height;
	}

	/* 要的画面见 hwIspFrameSize()：传感器模式由管线按它选，IFE 缩放到这个尺寸 */
	auto setUp = [&](bool two, bool raw) {
		libcamera::StreamConfiguration &vc = config_->at(0);
		vc.size = two ? dispSize() : hwIspFrameSize(facts_, maxW, maxH);
		vc.pixelFormat = libcamera::formats::NV12;
		vc.bufferCount = 4;
		size_t next = 1;
		if (two) {
			libcamera::StreamConfiguration &fc = config_->at(next++);
			fc.size = hwIspFrameSize(facts_, maxW, maxH);
			fc.pixelFormat = libcamera::formats::NV12;
			fc.bufferCount = vc.bufferCount;
		}
		if (raw) {
			libcamera::StreamConfiguration &rc = config_->at(next);
			rc.size = libcamera::Size(facts_.activeW, facts_.activeH);
			rc.bufferCount = vc.bufferCount;
		}
	};

	/* Without RAW if libcamera refuses it: phase detection is optional, the stream is not. */
	libcamera::CameraConfiguration::Status st;
	for (;;) {
		std::vector<libcamera::StreamRole> roles = { libcamera::StreamRole::Viewfinder };
		if (twoYuv)
			roles.push_back(libcamera::StreamRole::StillCapture);
		if (withRaw)
			roles.push_back(libcamera::StreamRole::Raw);
		config_ = cam_->generateConfiguration(roles);
		if (!config_ || config_->size() != roles.size()) {
			ALOGW("generateConfiguration gave %zu of %zu streams",
			      config_ ? config_->size() : 0, roles.size());
		} else {
			setUp(twoYuv, withRaw);
			std::string asked;
			for (const libcamera::StreamConfiguration &c : *config_)
				asked += " " + c.toString();
			st = config_->validate();
			int ret = -1;
			if (st != libcamera::CameraConfiguration::Invalid)
				ret = cam_->configure(config_.get());
			if (!ret)
				break;
			ALOGW("libcamera %s:%s", st == libcamera::CameraConfiguration::Invalid
							 ? "rejected the configuration"
							 : "could not configure",
			      asked.c_str());
		}
		if (twoYuv) {
			ALOGW("libcamera did not take two NV12 streams, using one");
			twoYuv = false;
			continue;
		}
		if (!withRaw) {
			ALOGE("generateConfiguration/validate/configure 失败");
			return err(Status::INTERNAL_ERROR);
		}
		ALOGW("libcamera 不接受 RAW 流：只用 Viewfinder，不做相位对焦");
		withRaw = false;
	}
	/*
	 * 按配置结果认流：validate 可能去掉了显示流。NV12 里大的是 FULL、小的是显示路径；
	 * 其余的是 RAW。
	 */
	const libcamera::PixelFormat procFmt = libcamera::formats::NV12;
	const libcamera::StreamConfiguration *yuvCfg[2] = {};
	const libcamera::StreamConfiguration *rawCfg = nullptr;
	size_t nYuv = 0;
	for (const libcamera::StreamConfiguration &c : *config_) {
		if (c.pixelFormat == procFmt && nYuv < 2)
			yuvCfg[nYuv++] = &c;
		else if (c.pixelFormat != procFmt)
			rawCfg = &c;
	}
	/* 没有 NV12（例如 IPA 没能加载）：报一条能读懂的错误，不把别的格式当 NV12 读 */
	if (nYuv == 0) {
		ALOGE("libcamera configured %s instead of %s: see libcamera.log for the IPA",
		      config_->at(0).pixelFormat.toString().c_str(), procFmt.toString().c_str());
		return err(Status::INTERNAL_ERROR);
	}
	const libcamera::StreamConfiguration *fullCfg = nullptr;
	if (nYuv == 2) {
		const auto area = [](const libcamera::StreamConfiguration *c) {
			return static_cast<uint64_t>(c->size.width) * c->size.height;
		};
		if (area(yuvCfg[0]) > area(yuvCfg[1]))
			std::swap(yuvCfg[0], yuvCfg[1]);
		fullCfg = yuvCfg[1];
	}
	withRaw = rawCfg != nullptr;
	const libcamera::StreamConfiguration &sc = *yuvCfg[0];
	if (st == libcamera::CameraConfiguration::Adjusted)
		ALOGW("Configuration adjusted to %s%s%s", sc.toString().c_str(),
		      fullCfg ? " + " : "", fullCfg ? fullCfg->toString().c_str() : "");

	/* 管线有硬件对焦统计时，AF 区域交给它 */
	afFov_.reset();
	if (facts_.hasAf && cam_->controls().count(&libcamera::controls::AfWindows) &&
	    cam_->controls().count(&libcamera::controls::AfMetering)) {
		if (auto fov = cam_->properties().get(libcamera::properties::ScalerCropMaximum);
		    fov && !fov->isNull())
			afFov_ = *fov;
	}


	/* NV12：裁剪偏移和色度成对取样要求偶数尺寸，行宽至少是画面宽度。 */
	for (const libcamera::StreamConfiguration *c : { &sc, fullCfg }) {
		if (c &&
		    (c->stride < c->size.width || (c->size.width | c->size.height) & 1)) {
			ALOGE("NV12 stream %s with stride %u is not supported",
			      c->size.toString().c_str(), c->stride);
			return err(Status::INTERNAL_ERROR);
		}
	}

	stream_ = sc.stream();
	srcWidth_ = sc.size.width;
	srcHeight_ = sc.size.height;
	srcStride_ = static_cast<int32_t>(sc.stride);
	fullStream_ = fullCfg ? fullCfg->stream() : nullptr;
	fullWidth_ = fullCfg ? static_cast<int32_t>(fullCfg->size.width) : 0;
	fullHeight_ = fullCfg ? static_cast<int32_t>(fullCfg->size.height) : 0;
	fullStride_ = fullCfg ? static_cast<int32_t>(fullCfg->stride) : 0;
	ifeZoom_ = false;
	zoomMetaWarned_ = false;
	scalerCropMax_ = {};
	if (fullStream_ && cam_->controls().count(&libcamera::controls::ScalerCrop)) {
		if (auto m = cam_->properties().get(libcamera::properties::ScalerCropMaximum);
		    m && !m->isNull()) {
			scalerCropMax_ = *m;
			ifeZoom_ = true;
		}
	}
	rawStream_ = nullptr;
	rawForPhase_ = false;
	if (withRaw) {
		const libcamera::StreamConfiguration &rc = *rawCfg;
		rawStream_ = rc.stream();
		rawStride_ = rc.stride;
		rawWidth_ = rc.size.width;
		rawHeight_ = rc.size.height;
		/*
		 * PhaseDetect reads CSI-2 packed 10-bit GRBG at full size, with the PD
		 * positions measured on a readout that libcamera flipped H+V (DT
		 * rotation 180). The driver keeps GRBG under flips, so the format
		 * does not show them: compare the configured orientation with the
		 * mounting one (libcamera falls back to no flips when it can't do the
		 * transform, and then reports the mounting orientation).
		 */
		const auto rot = cam_->properties().get(libcamera::properties::Rotation);
		const libcamera::Transform readout =
			config_->orientation / libcamera::orientationFromRotation(rot ? *rot : 0);
		rawForPhase_ = rc.pixelFormat == libcamera::formats::SGRBG10_CSI2P &&
			       rawWidth_ == facts_.activeW && rawHeight_ == facts_.activeH &&
			       readout == libcamera::Transform::HVFlip;
		ALOGI("RAW 流 %s stride %u 读出 %s%s", rc.toString().c_str(), rc.stride,
		      libcamera::transformToString(readout),
		      rawForPhase_ ? "，用于相位对焦" : "，格式或读出方向不符，不做相位对焦");
	}
	af_.setPhaseAvailable(rawForPhase_);

	/* 上一次配置若在建池之后失败（start 失败等），池里还是指向旧 allocator 缓冲的 Request ——
	 * 换 allocator 之前先清掉，别让新旧两代混在一个池里。此时没有在途请求（streaming_ 为假）。 */
	freeRequests_.clear();
	configRequests_.clear();
	reqBuffers_.clear();
	fullFree_.clear();

	/* 这个配置的曝光与增益范围，AE 锁定时用 */
	{
		const auto &ctrls = cam_->controls();
		auto e = ctrls.find(&libcamera::controls::ExposureTime);
		auto g = ctrls.find(&libcamera::controls::AnalogueGain);
		if (e != ctrls.end()) {
			expMinUs_ = e->second.min().get<int32_t>();
			expMaxUs_ = e->second.max().get<int32_t>();
		}
		if (g != ctrls.end()) {
			gainMin_ = g->second.min().get<float>();
			gainMax_ = g->second.max().get<float>();
		}
	}
	/*
	 * 锁定状态跨重新配置保留（拍照与录像之间切换时锁还在）：锁定时每个请求都
	 * 重新下发锁住的曝光、增益与色温，IPA 重新配置后照样生效。
	 */

	allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(cam_);
	std::vector<libcamera::Stream *> streams = { stream_ };
	if (fullStream_)
		streams.push_back(fullStream_);
	if (rawStream_)
		streams.push_back(rawStream_);
	size_t nbufs = SIZE_MAX;
	for (libcamera::Stream *s2 : streams) {
		if (allocator_->allocate(s2) < 0) {
			ALOGE("Allocating libcamera buffers failed");
			return err(Status::INTERNAL_ERROR);
		}
		nbufs = std::min(nbufs, allocator_->buffers(s2).size());
	}
	/*
	 * 每个请求固定带显示路径（或单路画面）与 RAW 的一个缓冲，按下标配对。FULL 的缓冲
	 * 放进 fullFree_，只在带 JPEG 的请求、变焦后预览要从 FULL 出的请求里加（管线允许请求不带
	 * FULL，只要至少带一路图像）。
	 */
	for (size_t i = 0; i < nbufs; i++) {
		auto r = cam_->createRequest();
		if (!r) {
			ALOGE("建 libcamera 请求失败");
			return err(Status::INTERNAL_ERROR);
		}
		auto &fixed = reqBuffers_[r.get()];
		for (libcamera::Stream *s2 : streams)
			if (s2 != fullStream_)
				fixed.emplace_back(s2, allocator_->buffers(s2)[i].get());
		configRequests_.insert(r.get());
		freeRequests_.push_back(std::move(r));
	}
	if (fullStream_)
		for (const auto &b : allocator_->buffers(fullStream_))
			fullFree_.push_back(b.get());

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

	if (fullStream_)
		ALOGI("Streams configured: %zu, libcamera display path %dx%d + full %dx%d %s",
		      halStreams_.size(), srcWidth_, srcHeight_, fullWidth_, fullHeight_,
		      sc.pixelFormat.toString().c_str());
	else
		ALOGI("Streams configured: %zu, libcamera frame %dx%d %s", halStreams_.size(),
		      srcWidth_, srcHeight_, sc.pixelFormat.toString().c_str());
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
	if (!templateSupported(static_cast<int>(type), facts_))
		return err(Status::ILLEGAL_ARGUMENT);
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

		/* 先清空上一次的缓冲（可能带过 FULL），再加这个请求固定的那几路 */
		lreq->reuse();
		for (const auto &[st2, fb2] : reqBuffers_[lreq.get()])
			lreq->addBuffer(st2, fb2);
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
				/* CONTROL_MODE_OFF 时 AE 也关了：闪光按 AE 模式 OFF 处理 */
				const bool ctrlOff = facts_.manualSensor &&
						     requestEntryInt(p.settings, ANDROID_CONTROL_MODE,
								     ANDROID_CONTROL_MODE_AUTO) ==
							     ANDROID_CONTROL_MODE_OFF;
				const uint8_t aeMode = ctrlOff ? ANDROID_CONTROL_AE_MODE_OFF : aeMode_;
				const bool flashAe =
					aeMode == ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH ||
					(aeMode == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH && dark);
				if (trigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_START) {
					flashArmed_ = flashAe;
					precaptureActive_ = true;
					precaptureLocked_ = aeLocked_;
					precaptureFrames_ = 0;
					lumaStable_ = 0;
				} else if (trigger == ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_CANCEL) {
					flashArmed_ = false;
					precaptureActive_ = false;
				}
				const bool still = hasBlob ||
						   intent == ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE ||
						   flashMode_ == ANDROID_FLASH_MODE_SINGLE;
				/* FLASH_MODE_SINGLE 在 AE 为 ON 或 OFF 时单独闪，带闪光的 AE 模式下不单独闪 */
				const bool single = flashMode_ == ANDROID_FLASH_MODE_SINGLE &&
						    (aeMode == ANDROID_CONTROL_AE_MODE_ON ||
						     aeMode == ANDROID_CONTROL_AE_MODE_OFF);
				const bool fire = torch || single || (flashAe && (flashArmed_ || still));
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
		/*
		 * 数码变焦：裁剪区域的大小限在视场的四分之一到全部之间，居中
		 * （SCALER_CROPPING_TYPE_CENTER_ONLY）。
		 */
		{
			const int32_t w = std::clamp(
				requestEntryInt(p.settings, ANDROID_SCALER_CROP_REGION, facts_.fovW, 2),
				facts_.fovW / kMaxDigitalZoom, facts_.fovW) & ~1;
			const int32_t h = std::clamp(
				requestEntryInt(p.settings, ANDROID_SCALER_CROP_REGION, facts_.fovH, 3),
				facts_.fovH / kMaxDigitalZoom, facts_.fovH) & ~1;
			p.crop[0] = ((facts_.fovW - w) / 2) & ~1;
			p.crop[1] = ((facts_.fovH - h) / 2) & ~1;
			p.crop[2] = w;
			p.crop[3] = h;
		}
		/*
		 * 两路输出时，变焦后的裁剪在显示路径上比某路非 JPEG 流小（要放大）：这个请求也带
		 * FULL，这些流改从 FULL 裁剪、缩小，画质与单路时相同。
		 */
		if (fullStream_ && !ifeZoom_) {
			SrcFrame disp;
			disp.w = srcWidth_;
			disp.h = srcHeight_;
			for (const auto &b : p.buffers) {
				if (b.isBlob)
					continue;
				const Crop c = cropIn(disp, p.crop, b.width, b.height);
				if (c.w < b.width || c.h < b.height) {
					p.previewFromFull = true;
					break;
				}
			}
		}

		if (facts_.hasAf) {
			/* CONTROL_MODE_OFF 时不管 AF_MODE 是什么，AF 都关 */
			const bool controlOff =
				facts_.manualSensor &&
				requestEntryInt(p.settings, ANDROID_CONTROL_MODE,
						ANDROID_CONTROL_MODE_AUTO) == ANDROID_CONTROL_MODE_OFF;
			const int32_t afMode = controlOff
						       ? ANDROID_CONTROL_AF_MODE_OFF
						       : requestEntryInt(p.settings, ANDROID_CONTROL_AF_MODE, -1);
			if (afMode >= 0)
				af_.setMode(static_cast<uint8_t>(afMode));
			/* 手动对焦 */
			if (afMode == ANDROID_CONTROL_AF_MODE_OFF) {
				const float d = requestEntryFloat(p.settings, ANDROID_LENS_FOCUS_DISTANCE, -1.0f);
				if (d >= 0.0f)
					af_.setManualFocus(d);
			}
			const int32_t afTrig = requestEntryInt(p.settings, ANDROID_CONTROL_AF_TRIGGER, -1);
			/*
			 * AF region (at most one, see Metadata.cpp): active array
			 * coordinates, with (0,0) at its top left. libcamera delivers the
			 * whole area around it, so map by fraction of that. Weight 0 means
			 * no region. Set before the trigger, which searches it.
			 */
			const int32_t rx0 = requestEntryInt(p.settings, ANDROID_CONTROL_AF_REGIONS, -1, 0);
			if (rx0 >= 0) {
				const int32_t ry0 = requestEntryInt(p.settings, ANDROID_CONTROL_AF_REGIONS, -1, 1);
				const int32_t rx1 = requestEntryInt(p.settings, ANDROID_CONTROL_AF_REGIONS, -1, 2);
				const int32_t ry1 = requestEntryInt(p.settings, ANDROID_CONTROL_AF_REGIONS, -1, 3);
				const int32_t rw = requestEntryInt(p.settings, ANDROID_CONTROL_AF_REGIONS, 0, 4);
				afAppRegion_ = rw > 0 && rx1 > rx0 && ry1 > ry0;
				afAppRect_[0] = rx0;
				afAppRect_[1] = ry0;
				afAppRect_[2] = rx1;
				afAppRect_[3] = ry1;
			}
			/*
			 * Android 规定落在 SCALER_CROP_REGION 之外的部分忽略：区域裁到与这个请求的
			 * 裁剪区域（变焦后的可见部分）的交集，相位、FocusFoM 与对比度都用这一块；
			 * 交集为空时按没有区域处理。变焦回来时重新用它（applyRegion() 对相同区域不做事）。
			 */
			const int32_t ax0 = std::max(afAppRect_[0], p.crop[0]);
			const int32_t ay0 = std::max(afAppRect_[1], p.crop[1]);
			const int32_t ax1 = std::min(afAppRect_[2], p.crop[0] + p.crop[2]);
			const int32_t ay1 = std::min(afAppRect_[3], p.crop[1] + p.crop[3]);
			const bool appRegion = afAppRegion_ && ax1 > ax0 && ay1 > ay0;
			if (appRegion)
				af_.setRegion(static_cast<float>(facts_.fovX + ax0) / facts_.activeW,
					      static_cast<float>(facts_.fovY + ay0) / facts_.activeH,
					      static_cast<float>(facts_.fovX + ax1) / facts_.activeW,
					      static_cast<float>(facts_.fovY + ay1) / facts_.activeH);
			/* 应用没给区域（或在可见部分之外）：用画面的可见部分 */
			if (!appRegion)
				af_.clearRegion(
					static_cast<float>(facts_.fovX + p.crop[0]) / facts_.activeW,
					static_cast<float>(facts_.fovY + p.crop[1]) / facts_.activeH,
					static_cast<float>(facts_.fovX + p.crop[0] + p.crop[2]) / facts_.activeW,
					static_cast<float>(facts_.fovY + p.crop[1] + p.crop[3]) / facts_.activeH);
			if (afTrig > 0)   /* 1=START 2=CANCEL；0=IDLE 不必下发 */
				af_.trigger(static_cast<uint8_t>(afTrig));
		}

		/*
		 * Frame rate: pass the requested AE target FPS range to libcamera
		 * as FrameDurationLimits. The soft ISP IPA sets the sensor's
		 * VBLANK from it, and its AGC may expose up to the maximum frame
		 * duration, so a {15, 30} range slows down to 15 fps in the dark. reuse()
		 * clears the controls; p.settings holds the full settings of this
		 * request or the last one, which include the range from the
		 * request template.
		 */
		const int32_t fpsMin =
			requestEntryInt(p.settings, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, 0, 0);
		const int32_t fpsMax =
			requestEntryInt(p.settings, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, 0, 1);
		if (fpsMin > 0 && fpsMax >= fpsMin)
			lreq->controls().set(libcamera::controls::FrameDurationLimits,
					     { static_cast<int64_t>(1000000 / fpsMax),
					       static_cast<int64_t>(1000000 / fpsMin) });

		/* AF 区域交给对焦统计（输入是画面的比例） */
		if (afFov_) {
			float rg[4];
			af_.region(rg);
			const libcamera::Rectangle win(
				afFov_->x + static_cast<int>(rg[0] * afFov_->width),
				afFov_->y + static_cast<int>(rg[1] * afFov_->height),
				static_cast<unsigned int>((rg[2] - rg[0]) * afFov_->width),
				static_cast<unsigned int>((rg[3] - rg[1]) * afFov_->height));
			lreq->controls().set(libcamera::controls::AfMetering,
					     libcamera::controls::AfMeteringWindows);
			lreq->controls().set(libcamera::controls::AfWindows,
					     std::span<const libcamera::Rectangle>(&win, 1));
		}

		/*
		 * IFE 变焦：裁剪区域（视场坐标，居中）换算到像素阵列坐标，每个请求都下发（不设时
		 * 管线沿用上一次）；不变焦时就是整个视场。
		 */
		if (ifeZoom_)
			lreq->controls().set(libcamera::controls::ScalerCrop, scalerCropFor(p.crop));

		apply3aControls(p, lreq->controls());

		/*
		 * 带 JPEG 的请求、预览要从 FULL 出的请求，再加一个 FULL 缓冲。只为预览要的不等：
		 * FULL 可能正被 JPEG 任务占着，拿不到时预览退回显示路径（放大，画质差些）。
		 */
		libcamera::FrameBuffer *fullBuf = nullptr;
		const bool wantsBlob = std::any_of(p.buffers.begin(), p.buffers.end(),
						   [](const PendingBuffer &b) { return b.isBlob; });
		if (fullStream_ && (p.previewFromFull || wantsBlob)) {
			if (fullFree_.empty() && wantsBlob &&
			    !freeCv_.wait_for(lk, std::chrono::milliseconds(kPoolWaitMs),
					      [&] { return !fullFree_.empty() || !streaming_; })) {
				ALOGE("frame=%d: no free full-size buffer within %d ms", r.frameNumber,
				      kPoolWaitMs);
			}
			if (!fullFree_.empty() && streaming_) {
				fullBuf = fullFree_.back();
				fullFree_.pop_back();
				lreq->addBuffer(fullStream_, fullBuf);
			}
		}

		const bool fired = p.flashFired;
		pending_[key] = std::move(p);

		if (cam_->queueRequest(key)) {
			ALOGE("queueRequest 失败 frame=%d", r.frameNumber);
			pending_.erase(key);   /* 句柄归缓存，不在这里释放 */
			if (fired && --firedPending_ == 0 && !flashArmed_ &&
			    flashMode_ != ANDROID_FLASH_MODE_TORCH)
				setLed(false);
			if (fullBuf)
				fullFree_.push_back(fullBuf);
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

libcamera::Size Session::hwIspFrameSize(const SensorFacts &f, int64_t maxW, int64_t maxH)
{
	/*
	 * 硬件 ISP 缩放不占 CPU：要一幅与传感器阵列同宽高比、刚好盖住每一路流的画面，
	 * 流与它同宽或同高时 deliver() 只需裁剪、拆分色度。宽按 16 对齐（IFE 写出的单位），
	 * 超出单个 IFE 能处理的尺寸时由管线处理器调小。
	 */
	/*
	 * 各路流是视场的裁剪（cropFor()），视场可能比画面窄：按视场缩放画面，
	 * 让视场以 1:1 盖住最大的那路流，这路流就不用再缩放。
	 */
	const int64_t fw = f.fovW > 0 ? f.fovW : f.activeW, fh = f.fovH > 0 ? f.fovH : f.activeH;
	int64_t w = maxW * fh >= maxH * fw ? (maxW * f.activeW + fw - 1) / fw
					   : (maxH * f.activeW + fh - 1) / fh;
	w = (std::max<int64_t>(w, maxW) + 15) & ~15;
	int64_t h = std::max<int64_t>(maxH, (w * f.activeH + f.activeW - 1) / f.activeW);
	h = (h + 1) & ~1;
	return libcamera::Size(static_cast<unsigned int>(w), static_cast<unsigned int>(h));
}

Session::Crop Session::cropFor(const SrcFrame &src, int32_t dstW, int32_t dstH) const
{
	return cropIn(src, cropRegion_, dstW, dstH);
}

libcamera::Rectangle Session::scalerCropFor(const int32_t crop[4]) const
{
	/* 视场坐标 → 有效区的比例 → ScalerCropMaximum（像素阵列坐标），与 AF 区域的换算相同 */
	const int32_t cw = crop[2] > 0 ? crop[2] : facts_.fovW;
	const int32_t ch = crop[3] > 0 ? crop[3] : facts_.fovH;
	const double fx0 = static_cast<double>(facts_.fovX + crop[0]) / facts_.activeW;
	const double fy0 = static_cast<double>(facts_.fovY + crop[1]) / facts_.activeH;
	const double fw = static_cast<double>(cw) / facts_.activeW;
	const double fh = static_cast<double>(ch) / facts_.activeH;
	const auto &m = scalerCropMax_;
	return libcamera::Rectangle(m.x + static_cast<int>(std::lround(fx0 * m.width)),
				    m.y + static_cast<int>(std::lround(fy0 * m.height)),
				    static_cast<unsigned int>(std::lround(fw * m.width)),
				    static_cast<unsigned int>(std::lround(fh * m.height)));
}

Session::Crop Session::cropIn(const SrcFrame &src, const int32_t region[4], int32_t dstW,
			      int32_t dstH) const
{
	/*
	 * The largest centred crop with the output's aspect ratio of the active
	 * array declared to Android (SensorFacts::fovW/fovH), which can be
	 * narrower than the frame. The frame is the libcamera active area, binned
	 * or not, less a few border columns of the debayer, so its height gives
	 * the scale.
	 */
	/*
	 * 要裁的范围（画面像素）：IFE 变焦时是请求的区域在这幅画面里的位置（SrcFrame::win，
	 * 由实际显示的矩形换算，中心随它）；否则取画面中间、裁剪区域大小的那部分。
	 */
	double cx = src.w / 2.0, cy = src.h / 2.0;
	int w, h;
	if (src.hasWin) {
		cx = src.win[0] + src.win[2] / 2.0;
		cy = src.win[1] + src.win[3] / 2.0;
		w = static_cast<int>(std::min<double>(src.w, src.win[2])) & ~1;
		h = static_cast<int>(std::min<double>(src.h, src.win[3])) & ~1;
	} else {
		const int64_t cw = region[2] > 0 ? region[2] : facts_.fovW;
		const int64_t ch = region[3] > 0 ? region[3] : facts_.fovH;
		w = static_cast<int>(std::min<int64_t>(src.w, cw * src.h / facts_.activeH)) & ~1;
		h = static_cast<int>(std::min<int64_t>(src.h, ch * src.h / facts_.activeH)) & ~1;
	}
	w = std::max(w, 2);
	h = std::max(h, 2);
	if (static_cast<int64_t>(w) * dstH > static_cast<int64_t>(h) * dstW)
		w = static_cast<int>(static_cast<int64_t>(h) * dstW / dstH) & ~1;
	else
		h = static_cast<int>(static_cast<int64_t>(w) * dstH / dstW) & ~1;
	/*
	 * 裁剪比流大不超过 1% 时，直接裁成流的尺寸，不把整幅画面缩小不到 1%。
	 * 代价：这路流的视场比同宽高比的最大居中裁剪少不到 1%（后摄 1920x1080
	 * 约 0.5%，每边约 10 列传感器像素），和同一会话里同宽高比、从别的画面
	 * 出的流（比如全尺寸 JPEG）不再逐像素对齐，肉眼看不出；换来的是纯复制。
	 */
	if (w >= dstW && h >= dstH && w * 100 <= dstW * 101 && h * 100 <= dstH * 101) {
		w = dstW;
		h = dstH;
	}
	/*
	 * IFE 变焦：请求的区域换算到画面上常常比流小一两个像素（管线按高度定缩放，宽度
	 * 1279.4 截成 1278）。差不超过 1% 且画面装得下时也直接用流的尺寸，否则每帧都要整幅放大。
	 */
	if (src.hasWin && dstW <= src.w && dstH <= src.h && w * 100 >= dstW * 99 &&
	    h * 100 >= dstH * 99 && w * 100 <= dstW * 101 && h * 100 <= dstH * 101) {
		w = dstW;
		h = dstH;
	}
	/* 以要裁的范围的中心为中心，不出画面 */
	const int x = std::clamp(static_cast<int>(std::lround(cx - w / 2.0)) & ~1, 0, (src.w - w) & ~1);
	const int y = std::clamp(static_cast<int>(std::lround(cy - h / 2.0)) & ~1, 0, (src.h - h) & ~1);
	return { x, y, w, h };
}

int Session::toJ420(const SrcFrame &src, const int32_t crop[4], int32_t dstW, int32_t dstH,
		    uint8_t *dy, int ys, uint8_t *du, uint8_t *dv, int cs, Scratch &s) const
{
	const Crop c = cropIn(src, crop, dstW, dstH);
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;

	/*
	 * IFE 的颜色转换按全范围 BT.601 编程，NV12 与 J420 是同一色彩空间：
	 * 只裁剪、缩放、把交错的 CbCr 拆成两个平面，不做颜色转换。
	 * cropIn() 给的坐标都是偶数，色度按 2×2 对齐。
	 */
	const uint8_t *sy = src.data + static_cast<size_t>(c.y) * src.stride + c.x;
	const uint8_t *suv = src.uv + static_cast<size_t>(c.y / 2) * src.stride + c.x;
	if (dstW == c.w && dstH == c.h)
		return libyuv::NV12ToI420(sy, src.stride, suv, src.stride, dy, ys, du, cs, dv, cs,
					  dstW, dstH);
	/* 先缩放 NV12（只有一个交错的色度平面要处理），再拆平面。 */
	s.scale.resize(static_cast<size_t>(dstW) * dstH + static_cast<size_t>(cw) * 2 * chh);
	uint8_t *ty = s.scale.data();
	uint8_t *tuv = ty + static_cast<size_t>(dstW) * dstH;
	int rc = scaleNv12(sy, src.stride, suv, src.stride, c.w, c.h, ty, dstW, tuv, cw * 2, dstW,
			   dstH, s);
	if (rc == 0)
		rc = libyuv::NV12ToI420(ty, dstW, tuv, cw * 2, dy, ys, du, cs, dv, cs, dstW, dstH);
	return rc;
}

int Session::toNv12(const SrcFrame &src, int32_t dstW, int32_t dstH,
		    uint8_t *dy, int ys, uint8_t *duv, int cs) const
{
	const Crop c = cropFor(src, dstW, dstH);
	const uint8_t *sy = src.data + static_cast<size_t>(c.y) * src.stride + c.x;
	const uint8_t *suv = src.uv + static_cast<size_t>(c.y / 2) * src.stride + c.x;

	if (dstW == c.w && dstH == c.h) {
		libyuv::CopyPlane(sy, src.stride, dy, ys, dstW, dstH);
		libyuv::CopyPlane(suv, src.stride, duv, cs, (dstW + 1) / 2 * 2, (dstH + 1) / 2);
		return 0;
	}
	return scaleNv12(sy, src.stride, suv, src.stride, c.w, c.h, dy, ys, duv, cs, dstW, dstH,
			 scratch_);
}

int Session::scaleNv12(const uint8_t *sy, int ss, const uint8_t *suv, int suvs, int sw, int sh,
		       uint8_t *dy, int ds, uint8_t *duv, int duvs, int dw, int dh, Scratch &s) const
{
	/*
	 * 设备上的 libyuv 缩小超过 2 倍时，盒式滤波走 C（全尺寸画面缩到 1280x960
	 * 预览约占单核 40%）。两个方向都还剩至少 2 倍时，先用 NEON 的 2x2 盒式
	 * 精确减半，剩下的用双线性。
	 */
	for (int pass = 0; sw >= 2 * dw && sh >= 2 * dh && sw % 4 == 0 && sh % 4 == 0; pass++) {
		const int hw = sw / 2, hh = sh / 2;
		std::vector<uint8_t> &t = s.half[pass & 1];
		t.resize(static_cast<size_t>(hw) * hh * 3 / 2);
		uint8_t *ty = t.data();
		uint8_t *tuv = ty + static_cast<size_t>(hw) * hh;
		const int rc = libyuv::NV12Scale(sy, ss, suv, suvs, sw, sh, ty, hw, tuv, hw, hw, hh,
						 libyuv::kFilterBox);
		if (rc != 0)
			return rc;
		sy = ty;
		suv = tuv;
		ss = suvs = hw;
		sw = hw;
		sh = hh;
	}
	return libyuv::NV12Scale(sy, ss, suv, suvs, sw, sh, dy, ds, duv, duvs, dw, dh,
				 libyuv::kFilterBox);
}

const uint8_t *Session::renderNv12(const SrcFrame &src, int32_t dstW, int32_t dstH,
				   int chromaBlur)
{
	const bool sharpen = sharpen_.radius > 0;
	for (const RenderedNv12 &r : rendered_)
		if (r.frame == renderFrame_ && r.w == dstW && r.h == dstH && r.blur == chromaBlur &&
		    r.sharpened == sharpen)
			return r.data.data();

	/* 取一个不属于这一帧的槽位 */
	RenderedNv12 &r = rendered_[0].frame == renderFrame_ ? rendered_[1] : rendered_[0];
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;
	r.data.resize(static_cast<size_t>(dstW) * dstH + static_cast<size_t>(cw) * 2 * chh);
	uint8_t *uv = r.data.data() + static_cast<size_t>(dstW) * dstH;
	/* 锐化时亮度先缩放到暂存区，再锐化写进缓存 */
	if (sharpen)
		lumaScratch_.resize(static_cast<size_t>(dstW) * dstH);
	uint8_t *y = sharpen ? lumaScratch_.data() : r.data.data();
	if (toNv12(src, dstW, dstH, y, dstW, uv, cw * 2) != 0) {
		r.frame = 0;
		return nullptr;
	}
	if (chromaBlur > 0)
		boxBlur(uv, cw * 2, chh, cw * 2, 2, chromaBlur, scratch_);
	if (sharpen)
		sharpenLuma(y, dstW, r.data.data(), dstW, dstW, dstH, sharpen_, scratch_.sharpen);
	r.frame = renderFrame_;
	r.w = dstW;
	r.h = dstH;
	r.blur = chromaBlur;
	r.sharpened = sharpen;
	return r.data.data();
}

bool Session::deliver(const SrcFrame &src, buffer_handle_t dst,
		      int32_t dstW, int32_t dstH, int chromaBlur, bool shared)
{
	auto &mapper = android::GraphicBufferMapper::get();
	android_ycbcr ycbcr = {};
	android::status_t st = mapper.lockYCbCr(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
						android::Rect(dstW, dstH), &ycbcr);
	if (st != android::OK || !ycbcr.y) {
		ALOGE("deliver: lockYCbCr 失败: %d", st);
		return false;
	}

	uint8_t *oy = static_cast<uint8_t *>(ycbcr.y);
	uint8_t *ocb = static_cast<uint8_t *>(ycbcr.cb);
	uint8_t *ocr = static_cast<uint8_t *>(ycbcr.cr);
	const int ys = static_cast<int>(ycbcr.ystride), cs = static_cast<int>(ycbcr.cstride);
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;
	/* 半平面布局：CbCr（NV12）或 CrCb（NV21）成对存在一个平面里 */
	const bool nv12 = ycbcr.chroma_step == 2 && ocr == ocb + 1;
	const bool nv21 = ycbcr.chroma_step == 2 && ocb == ocr + 1;
	const bool planar = ycbcr.chroma_step == 1;
	int rc;

	/*
	 * 缓冲只写一次、从不读：gralloc 给 CPU 的映射可能不带缓存。滤波和换序
	 * 都先在暂存区里做。
	 */
	if (nv12 || nv21) {
		/*
		 * NV12 原样裁剪、缩放后写入：直接写进缓冲；要降噪、
		 * 换序，或者这一帧另有一路流也要这个尺寸时，先渲染一次再复制。
		 */
		uint8_t *ouv = nv12 ? ocb : ocr;
		if (nv12 && chromaBlur <= 0 && !shared && sharpen_.radius == 0) {
			rc = toNv12(src, dstW, dstH, oy, ys, ouv, cs);
		} else if (const uint8_t *r = renderNv12(src, dstW, dstH, chromaBlur)) {
			const uint8_t *uv = r + static_cast<size_t>(dstW) * dstH;
			libyuv::CopyPlane(r, dstW, oy, ys, dstW, dstH);
			if (nv12)
				libyuv::CopyPlane(uv, cw * 2, ouv, cs, cw * 2, chh);
			else
				libyuv::SwapUVPlane(uv, cw * 2, ouv, cs, cw, chh);
			rc = 0;
		} else {
			rc = -1;
		}
	} else if (planar && chromaBlur <= 0 && sharpen_.radius == 0) {
		rc = toJ420(src, cropRegion_, dstW, dstH, oy, ys, ocb, ocr, cs, scratch_);
	} else {
		/* 平面或其他色度步长的布局，要做滤波、锐化时：经平面暂存区 */
		planeScratch_.resize(static_cast<size_t>(cw) * chh * 2);
		uint8_t *du = planeScratch_.data();
		uint8_t *dv = du + static_cast<size_t>(cw) * chh;
		const bool sharpen = sharpen_.radius > 0;
		if (sharpen)
			lumaScratch_.resize(static_cast<size_t>(dstW) * dstH);
		rc = toJ420(src, cropRegion_, dstW, dstH, sharpen ? lumaScratch_.data() : oy,
			    sharpen ? dstW : ys, du, dv, cw, scratch_);
		if (rc == 0 && sharpen)
			sharpenLuma(lumaScratch_.data(), dstW, oy, ys, dstW, dstH, sharpen_,
				    scratch_.sharpen);
		if (rc == 0 && chromaBlur > 0) {
			/* 色度降噪：U/V 只有 1/4 分辨率，盒式模糊几乎免费，而彩色噪点正是最难看的那种。 */
			boxBlur(du, cw, chh, cw, 1, chromaBlur, scratch_);
			boxBlur(dv, cw, chh, cw, 1, chromaBlur, scratch_);
		}
		if (rc == 0 && planar) {
			libyuv::CopyPlane(du, cw, ocb, cs, cw, chh);
			libyuv::CopyPlane(dv, cw, ocr, cs, cw, chh);
		} else if (rc == 0 && nv12) {
			libyuv::MergeUVPlane(du, cw, dv, cw, ocb, cs, cw, chh);
		} else if (rc == 0 && nv21) {
			libyuv::MergeUVPlane(dv, cw, du, cw, ocr, cs, cw, chh);
		} else if (rc == 0) {
			/* 其他色度步长 */
			for (int y = 0; y < chh; y++) {
				uint8_t *cb = ocb + static_cast<size_t>(y) * cs;
				uint8_t *cr = ocr + static_cast<size_t>(y) * cs;
				const uint8_t *su = du + static_cast<size_t>(y) * cw;
				const uint8_t *sv = dv + static_cast<size_t>(y) * cw;
				for (int x = 0; x < cw; x++) {
					cb[x * ycbcr.chroma_step] = su[x];
					cr[x * ycbcr.chroma_step] = sv[x];
				}
			}
		}
	}

	if (rc != 0)
		ALOGE("deliver: colour conversion/scaling failed, rc=%d", rc);

	mapper.unlock(dst);
	return rc == 0;
}

/*
 * JPEG 写进 Android 的 BLOB 缓冲（deliverJpeg()）。
 *
 * ★ Android 的约定（`CameraBlob.aidl` / `CameraBlobId.aidl`）：
 *   JPEG 数据从缓冲开头写，**缓冲末尾**放一个 8 字节的
 *   `CameraBlob{ blobId = JPEG(0x00FF), blobSizeBytes }`。
 *   框架按这个结构去找真实长度 —— 少了它，图片会被当成整个缓冲那么大。
 */
namespace {

/* 从 w×h 的中间裁出 tw×th 的宽高比 */
void thumbCrop(int w, int h, int tw, int th, int *cx, int *cy, int *cw, int *ch)
{
	*cw = w;
	*ch = h;
	if (static_cast<int64_t>(w) * th > static_cast<int64_t>(h) * tw)
		*cw = static_cast<int>(static_cast<int64_t>(h) * tw / th) & ~1;
	else
		*ch = static_cast<int>(static_cast<int64_t>(w) * th / tw) & ~1;
	*cx = ((w - *cw) / 2) & ~1;
	*cy = ((h - *ch) / 2) & ~1;
}

/* 从全范围 4:2:0 平面做 tw×th 的缩略图（B,G,R,A） */
std::vector<uint8_t> thumbFromJ420(const uint8_t *y, int ys, const uint8_t *u, const uint8_t *v,
				   int cs, int w, int h, int tw, int th)
{
	int cx, cy, cw, ch;
	thumbCrop(w, h, tw, th, &cx, &cy, &cw, &ch);
	const int tcw = (tw + 1) / 2, tch = (th + 1) / 2;
	std::vector<uint8_t> j(static_cast<size_t>(tw) * th + static_cast<size_t>(tcw) * tch * 2);
	uint8_t *ty = j.data(), *tu = ty + static_cast<size_t>(tw) * th, *tv = tu + tcw * tch;
	std::vector<uint8_t> out(static_cast<size_t>(tw) * th * 4);
	if (libyuv::I420Scale(y + static_cast<size_t>(cy) * ys + cx, ys,
			      u + static_cast<size_t>(cy / 2) * cs + cx / 2, cs,
			      v + static_cast<size_t>(cy / 2) * cs + cx / 2, cs, cw, ch,
			      ty, tw, tu, tcw, tv, tcw, tw, th, libyuv::kFilterBox) != 0 ||
	    libyuv::J420ToARGB(ty, tw, tu, tcw, tv, tcw, out.data(), tw * 4, tw, th) != 0)
		return {};
	return out;
}

/* 把 B,G,R,A 像素编成 JPEG */
std::vector<uint8_t> encodeThumbnail(const std::vector<uint8_t> &bgra, int w, int h, int quality)
{
	if (bgra.empty())
		return {};
	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);
	unsigned char *out = nullptr;
	unsigned long outSize = 0;
	jpeg_mem_dest(&cinfo, &out, &outSize);
	cinfo.image_width = w;
	cinfo.image_height = h;
	cinfo.input_components = 4;
	cinfo.in_color_space = JCS_EXT_BGRA;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, std::clamp(quality, 1, 100), TRUE);
	/* 缩略图不带 JFIF 头（EXIF 2.32，4.5.8） */
	cinfo.write_JFIF_header = FALSE;
	jpeg_start_compress(&cinfo, TRUE);
	while (cinfo.next_scanline < cinfo.image_height) {
		JSAMPROW row = const_cast<JSAMPROW>(
			bgra.data() + static_cast<size_t>(cinfo.next_scanline) * w * 4);
		jpeg_write_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);
	std::vector<uint8_t> jpeg(out, out + outSize);
	free(out);
	return jpeg;
}

} /* namespace */

void Session::thumbnailSize(const StillParams &sp, int32_t rotation, int32_t *tw, int32_t *th)
{
	const bool swap = rotation == 90 || rotation == 270;
	*tw = swap ? sp.thumbH : sp.thumbW;
	*th = swap ? sp.thumbW : sp.thumbH;
}

std::vector<uint8_t> Session::exifApp1(StillParams &sp, int32_t outW, int32_t outH, bool rotated,
				       const std::vector<uint8_t> &thumb, int32_t tw, int32_t th)
{
	ExifInfo info = sp.exif;
	info.width = outW;
	info.height = outH;
	info.rotated = rotated;
	info.thumbnail = encodeThumbnail(thumb, tw, th, sp.thumbQuality);
	bool withThumb = false;
	std::vector<uint8_t> app1 = buildExifApp1(info, &withThumb);
	/* 结果报实际放进去的缩略图：超出 64 KB 被丢掉时报 0x0 */
	sp.thumbMade[0] = withThumb ? tw : 0;
	sp.thumbMade[1] = withThumb ? th : 0;
	return app1;
}

bool Session::deliverJpeg(const SrcFrame &src, buffer_handle_t dst,
			  int32_t dstW, int32_t dstH, int32_t blobSize,
			  int quality, int32_t jpegOrientation, StillParams &sp, Scratch &s)
{
	/*
	 * 旋转：ANDROID_JPEG_ORIENTATION 是应用算好的顺时针校正角。HAL 的合同是把它落到交付物
	 * 上，可以转像素，也可以写进 EXIF 的方向标记，只能二选一（两样都做等于转两次）。这里转
	 * 像素，不依赖看图程序解析 EXIF；转过时 EXIF 方向写 0（未定义），0 度时写 1（见
	 * Exif.cpp）。90/270 度时 JPEG 的宽高与流声明的互换：BLOB 流只声明缓冲字节数，应用读
	 * JPEG 头取真实尺寸。
	 */
	const libyuv::RotationMode rotMode = toLibyuvRotation(jpegOrientation);
	const bool swapDims = rotMode == libyuv::kRotate90 || rotMode == libyuv::kRotate270;
	const int32_t outW = swapDims ? dstH : dstW;
	const int32_t outH = swapDims ? dstW : dstH;

	/*
	 * JPEG 的 YCbCr 就是全范围 BT.601，与 NV12 拆出来的 J420 相同：以原始数据交给 libjpeg，
	 * 全程不经过 RGB。原始数据输入时 libjpeg 每行按分量读满 8 的整数倍个样本，
	 * 所以平面行宽补齐到 16（亮度）和 8（色度）。
	 */
	const int cw = (dstW + 1) / 2, chh = (dstH + 1) / 2;
	const int ocw = (outW + 1) / 2, och = (outH + 1) / 2;
	const int ys = (outW + 15) & ~15, cs = (ocw + 7) & ~7;
	std::vector<uint8_t> img(static_cast<size_t>(ys) * outH + static_cast<size_t>(cs) * och * 2);
	uint8_t *oy = img.data();
	uint8_t *ou = oy + static_cast<size_t>(ys) * outH;
	uint8_t *ov = ou + static_cast<size_t>(cs) * och;

	int rc;
	if (rotMode == libyuv::kRotate0) {
		rc = toJ420(src, sp.crop, dstW, dstH, oy, ys, ou, ov, cs, s);
	} else {
		std::vector<uint8_t> t(static_cast<size_t>(dstW) * dstH +
				       static_cast<size_t>(cw) * chh * 2);
		uint8_t *ty = t.data();
		uint8_t *tu = ty + static_cast<size_t>(dstW) * dstH;
		uint8_t *tv = tu + static_cast<size_t>(cw) * chh;
		rc = toJ420(src, sp.crop, dstW, dstH, ty, dstW, tu, tv, cw, s);
		if (rc == 0)
			rc = libyuv::I420Rotate(ty, dstW, tu, cw, tv, cw, oy, ys, ou, cs, ov, cs,
						dstW, dstH, rotMode);
	}
	if (rc != 0) {
		ALOGE("deliverJpeg: crop, scale or rotation failed: %d", rc);
		return false;
	}
	/*
	 * 拍照的降噪（硬件 ISP 本身没有）：亮度做 3×3 ε 滤波，门限为噪声估计的 3.2 倍，
	 * 平坦处的颗粒抹掉、边缘保留；色度做 5×5 盒式模糊，对付成片的色斑。只在拍照时做，
	 * 1300 万像素约多 0.1–0.2 秒。门限由 11-10 照片盲测定（室内灯光，ISO 144–194）：
	 * 3.2 倍优于 2 倍和不降噪，也优于 4.8 倍（开始抹掉小字）。调试属性
	 * debug.gaokun3.camera.denoise 乘在门限上（0 = 关）。
	 */
	std::vector<uint8_t> denY;
	{
		char v[PROP_VALUE_MAX] = {};
		const double dn = __system_property_get("debug.gaokun3.camera.denoise", v) > 0
					  ? atof(v)
					  : 1.0;
		if (dn > 0.0) {
			const double sigma = estimateNoise(oy, ys, outW, outH);
			const int t = std::max(1, static_cast<int>(std::lround(3.2 * dn * sigma)));
			denY.resize(static_cast<size_t>(ys) * outH);
			denoiseLuma(oy, ys, denY.data(), ys, outW, outH, t);
			oy = denY.data();
			boxBlur(ou, ocw, och, cs, 1, 2, s);
			boxBlur(ov, ocw, och, cs, 1, 2, s);
			ALOGI("JPEG denoise: luma noise %.2f, epsilon threshold %d, chroma box 5x5", sigma, t);
		}
	}
	/* 锐化在缩放、旋转、降噪之后，按输出分辨率（核是对称的，旋转前后一样） */
	std::vector<uint8_t> sharpY;
	if (sp.sharpen.radius > 0) {
		sharpY.resize(static_cast<size_t>(ys) * outH);
		sharpenLuma(oy, ys, sharpY.data(), ys, outW, outH, sp.sharpen, s.sharpen);
		oy = sharpY.data();
	}

	/* 补出的列复制行末像素：填 0 会把右边缘那一列 8×8 块拉暗。 */
	auto padRows = [](uint8_t *p, int stride, int w, int h) {
		for (int y = 0; y < h && stride > w; y++) {
			uint8_t *row = p + static_cast<size_t>(y) * stride;
			memset(row + w, row[w - 1], stride - w);
		}
	};
	padRows(oy, ys, outW, outH);
	padRows(ou, cs, ocw, och);
	padRows(ov, cs, ocw, och);

	auto &mapper = android::GraphicBufferMapper::get();
	void *raw = nullptr;
	android::status_t st = mapper.lock(dst, GRALLOC_USAGE_SW_WRITE_OFTEN,
					   android::Rect(blobSize, 1), &raw);
	if (st != android::OK || !raw) {
		ALOGE("deliverJpeg: lock failed: %d", st);
		return false;
	}

	struct jpeg_compress_struct cinfo;
	struct jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	/* 直接写进 gralloc 缓冲；留出末尾 8 字节给 CameraBlob。 */
	unsigned char *out = static_cast<unsigned char *>(raw);
	unsigned long outSize = static_cast<size_t>(blobSize) - sizeof(CameraBlob);
	jpeg_mem_dest(&cinfo, &out, &outSize);

	cinfo.image_width = outW;
	cinfo.image_height = outH;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_YCbCr;
	jpeg_set_defaults(&cinfo);
	jpeg_set_colorspace(&cinfo, JCS_YCbCr);
	cinfo.raw_data_in = TRUE;
	cinfo.comp_info[0].h_samp_factor = 2;
	cinfo.comp_info[0].v_samp_factor = 2;
	for (int i = 1; i < 3; i++) {
		cinfo.comp_info[i].h_samp_factor = 1;
		cinfo.comp_info[i].v_samp_factor = 1;
	}
	jpeg_set_quality(&cinfo, quality, TRUE);
	/* 用 EXIF 代替 JFIF（jpeg_set_colorspace() 又把 JFIF 打开了） */
	cinfo.write_JFIF_header = FALSE;
	jpeg_start_compress(&cinfo, TRUE);
	{
		int32_t tw, th;
		thumbnailSize(sp, jpegOrientation, &tw, &th);
		const std::vector<uint8_t> thumb =
			tw > 0 && th > 0 ? thumbFromJ420(oy, ys, ou, ov, cs, outW, outH, tw, th)
					 : std::vector<uint8_t>();
		const std::vector<uint8_t> app1 =
			exifApp1(sp, outW, outH, rotMode != libyuv::kRotate0, thumb, tw, th);
		/* 超过一个段的上限时 libjpeg 的默认错误处理会 exit()：宁可不写 EXIF */
		if (app1.size() <= 65533)
			jpeg_write_marker(&cinfo, JPEG_APP0 + 1, app1.data(), app1.size());
	}

	/* 每次一个 iMCU 行：亮度 16 行、色度 8 行；图像下方不足的行重复最后一行。 */
	JSAMPROW yRows[16], uRows[8], vRows[8];
	JSAMPARRAY planes[3] = { yRows, uRows, vRows };
	while (cinfo.next_scanline < cinfo.image_height) {
		const int y0 = static_cast<int>(cinfo.next_scanline);
		for (int i = 0; i < 16; i++)
			yRows[i] = oy + static_cast<size_t>(std::min(y0 + i, outH - 1)) * ys;
		for (int i = 0; i < 8; i++) {
			const int r = std::min(y0 / 2 + i, och - 1);
			uRows[i] = ou + static_cast<size_t>(r) * cs;
			vRows[i] = ov + static_cast<size_t>(r) * cs;
		}
		if (jpeg_write_raw_data(&cinfo, planes, 16) != 16)
			break;
	}
	jpeg_finish_compress(&cinfo);
	const size_t jpegLen = outSize;
	jpeg_destroy_compress(&cinfo);

	const bool ok = finishJpegBlob(raw, blobSize, out, jpegLen);
	if (ok)
		ALOGD("JPEG %dx%d from NV12 (stream %dx%d, rotation %d), quality %d: %zu bytes",
		      outW, outH, dstW, dstH, jpegOrientation, quality, jpegLen);

	mapper.unlock(dst);
	return ok;
}

int Session::chromaBlurRadius() const
{
	/* 调用方不持锁也没关系：lastGain_ 是个 double，读到旧值只是差一帧的强度。 */
	if (lastGain_ < kDenoiseGain)
		return 0;
	return lastGain_ < 5.0 ? 1 : 2;
}

namespace {

/*
 * 半径 R、通道相隔 S 字节的盒式模糊的一行：输入是窗口内各行的列和（两侧各
 * 预填充 R * S 项边缘值），输出每 R * 2 + 1 个列和的平均。整行无分支，便于
 * 向量化。
 */
template<int R, int S>
void boxBlurRow(const uint16_t *col, uint8_t *out, int n, uint32_t recip)
{
	for (int i = 0; i < n; i++) {
		uint32_t sum = 0;
		for (int k = -R; k <= R; k++)
			sum += col[i + k * S];
		out[i] = static_cast<uint8_t>((sum * recip + 32768) >> 16);
	}
}

template<int R>
void columnSums(const uint8_t *const *rows, uint16_t *col, int n)
{
	for (int i = 0; i < n; i++) {
		uint16_t sum = 0;
		for (int k = 0; k <= 2 * R; k++)
			sum += rows[k][i];
		col[i] = sum;
	}
}

} /* namespace */

/*
 * 可分离的盒式模糊，边缘取边上的值：每个输出行先算窗口内的列和，再沿行
 * 滑动窗口。读平面的副本，写回平面。用在色度上，像素只有四分之一。
 */
void Session::boxBlur(uint8_t *p, int n, int h, int stride, int step, int radius, Scratch &s)
{
	if (radius <= 0 || n <= 0 || h <= 0 || (step != 1 && step != 2))
		return;
	radius = std::min(radius, 2);
	const int win = 2 * radius + 1;
	/* 窗口面积的 Q16 倒数：和不超过 255 * 25 */
	const uint32_t recip = (65536u + win * win / 2) / (win * win);
	const int pad = radius * step;

	s.blurSrc.resize(static_cast<size_t>(n) * h);
	libyuv::CopyPlane(p, stride, s.blurSrc.data(), n, n, h);
	s.blurCols.resize(static_cast<size_t>(n) + 2 * pad);
	uint16_t *col = s.blurCols.data() + pad;

	for (int y = 0; y < h; y++) {
		const uint8_t *rows[5];
		for (int k = 0; k < win; k++)
			rows[k] = s.blurSrc.data() +
				  static_cast<size_t>(std::clamp(y + k - radius, 0, h - 1)) * n;
		if (radius == 1)
			columnSums<1>(rows, col, n);
		else
			columnSums<2>(rows, col, n);

		/* 每个通道的边缘值重复填进预留区 */
		for (int q = 1; q <= radius; q++) {
			for (int c = 0; c < step; c++) {
				col[-q * step + c] = col[c];
				col[n - step + c + q * step] = col[n - step + c];
			}
		}

		uint8_t *out = p + static_cast<size_t>(y) * stride;
		if (radius == 1 && step == 1)
			boxBlurRow<1, 1>(col, out, n, recip);
		else if (radius == 1)
			boxBlurRow<1, 2>(col, out, n, recip);
		else if (step == 1)
			boxBlurRow<2, 1>(col, out, n, recip);
		else
			boxBlurRow<2, 2>(col, out, n, recip);
	}
}

void Session::reportRequestError(const Pending &p)
{
	/*
	 * ERROR_REQUEST 的合同（ErrorCode.aidl:33-39）："No output (metadata or buffers) must be
	 * produced for this request"，但缓冲仍要带 ERROR 状态还回去，框架据此把这一帧移出在途表。
	 * 不发 shutter —— 这一帧没有曝光时间戳，而 shutter 必须按帧号递增，乱发反而是设备错误。
	 * ★ 只在完成回调里调用：libcamera 按入队顺序完成请求，报错因此天然按帧序。
	 *   两路输出时前面的照片可能还在工作线程上编码：JPEG 缓冲也经任务队列返回（整帧
	 *   报错的任务，不另发 ERROR_BUFFER），才按帧号顺序；不在这里等，免得 CameraManager
	 *   线程停住、管线跟着停。
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
	std::optional<StillJob> job;
	for (const auto &b : p.buffers) {
		if (b.isBlob && fullStream_ && !job) {
			job.emplace();
			job->requestError = true;
			job->frameNumber = p.frameNumber;
			job->streamId = b.streamId;
			job->bufferId = b.bufferId;
			continue;
		}
		StreamBuffer sb;
		sb.streamId = b.streamId;
		sb.bufferId = b.bufferId;
		sb.status = BufferStatus::ERROR;
		result.outputBuffers.push_back(std::move(sb));
	}
	result.inputBuffer.streamId = -1;
	result.inputBuffer.bufferId = 0;
	/* 不带缓冲也不带元数据的结果不合法（只有 JPEG 一路时全交给了任务） */
	if (!result.outputBuffers.empty()) {
		std::vector<CaptureResult> results;
		results.push_back(std::move(result));
		sendResults(results);
	}
	if (job) {
		{
			std::lock_guard<std::mutex> lk(mutex_);
			stillInFlight_++;
		}
		{
			std::lock_guard<std::mutex> g(stillMutex_);
			stillJobs_.push_back(std::move(*job));
		}
		stillCv_.notify_one();
	}
}

void Session::sendResults(std::vector<CaptureResult> &results)
{
	std::lock_guard<std::mutex> g(resultMutex_);
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

void Session::waitStillJobs(std::unique_lock<std::mutex> &lk)
{
	if (stillInFlight_ == 0)
		return;
	/* 编码不依赖 libcamera 和框架之外的东西，每张不到 1 秒，所以不设超时：没做完就解除映射会崩 */
	ALOGI("Waiting for %d JPEG encode(s)", stillInFlight_);
	drainCv_.wait(lk, [&] { return stillInFlight_ == 0; });
}

void Session::stillWorker()
{
	pthread_setname_np(pthread_self(), "gk3cam-jpeg");
	Scratch s;
	std::unique_lock<std::mutex> lk(stillMutex_);
	for (;;) {
		stillCv_.wait(lk, [&] { return stillStop_ || !stillJobs_.empty(); });
		if (stillJobs_.empty())
			return;   /* 要停，且没有剩下的任务 */
		StillJob job = std::move(stillJobs_.front());
		stillJobs_.pop_front();
		lk.unlock();

		/* src 为空：这一帧没有能用的 FULL（或整帧报错），只是按顺序把 JPEG 缓冲报成出错 */
		bool ok = false;
		if (job.src.data) {
			int32_t tw = 0, th = 0;
			thumbnailSize(job.params, job.orientation, &tw, &th);
			ok = deliverJpeg(job.src, job.handle, job.width, job.height, job.blobSize,
					     job.quality, job.orientation, job.params, s);
			if (ok && tw > 0 && th > 0 && job.params.thumbMade[0] == 0)
				ALOGW("frame=%d: the thumbnail did not fit into the EXIF segment, but the "
				      "result already reported %dx%d", job.frameNumber, tw, th);
		}
		if (job.fullBuf) {
			struct dma_buf_sync sync = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
			ioctl(job.fullFd, DMA_BUF_IOCTL_SYNC, &sync);
			/* FULL 先还：框架拿回 JPEG 缓冲后可能立刻发下一张 */
			std::lock_guard<std::mutex> g(mutex_);
			if (fullHeld_.erase(job.fullBuf))
				fullFree_.push_back(job.fullBuf);
			freeCv_.notify_all();
		}

		/* 整帧报错的请求已发过 ERROR_REQUEST，不能再发单独的 ERROR_BUFFER */
		if (!ok && !job.requestError) {
			NotifyMsg emsg;
			ErrorMsg e;
			e.frameNumber = job.frameNumber;
			e.errorStreamId = job.streamId;
			e.errorCode = ErrorCode::ERROR_BUFFER;
			emsg.set<NotifyMsg::Tag::error>(e);
			std::vector<NotifyMsg> emsgs;
			emsgs.push_back(std::move(emsg));
			cb_->notify(emsgs);
		}
		/*
		 * 只带这一个缓冲的结果（partialResult 0 = 不带元数据）。元数据由完成回调按帧号顺序发，
		 * 多半已经发过；先于它到达也合合同（同一请求的元数据与缓冲不分先后）。
		 */
		CaptureResult result;
		result.frameNumber = job.frameNumber;
		result.fmqResultSize = 0;
		result.partialResult = 0;
		StreamBuffer sb;
		sb.streamId = job.streamId;
		sb.bufferId = job.bufferId;
		sb.status = ok ? BufferStatus::OK : BufferStatus::ERROR;
		result.outputBuffers.push_back(std::move(sb));
		result.inputBuffer.streamId = -1;
		result.inputBuffer.bufferId = 0;
		std::vector<CaptureResult> results;
		results.push_back(std::move(result));
		sendResults(results);

		{
			std::lock_guard<std::mutex> g(mutex_);
			if (--stillInFlight_ == 0)
				drainCv_.notify_all();
		}
		lk.lock();
	}
}

/* 调用者持 mutex_。只收【本次配置】建的 Request；上一代的（重配置后迟到的）直接释放。 */
void Session::returnToPool(std::unique_ptr<libcamera::Request> r)
{
	if (configRequests_.count(r.get())) {
		/*
		 * 这个请求带过的 FULL 缓冲还回去（帧已交付、DMA 同步已结束）；交给 JPEG 任务的
		 * 由工作线程编码完再还。
		 */
		if (fullStream_)
			if (libcamera::FrameBuffer *fb = r->findBuffer(fullStream_))
				if (!fullHeld_.count(fb))
					fullFree_.push_back(fb);
		freeRequests_.push_back(std::move(r));
		/* 等请求的和等 FULL 缓冲的可能是不同的线程 */
		freeCv_.notify_all();
	}
}

void Session::apply3aControls(Pending &p, libcamera::ControlList &ctrls)
{
	const int32_t ev =
		facts_.evCompensation
			? std::clamp(requestEntryInt(p.settings, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, 0),
				     -kEvStepsPerStop * 2, kEvStepsPerStop * 2)
			: 0;
	const bool aeLock = facts_.aeLock &&
			    requestEntryInt(p.settings, ANDROID_CONTROL_AE_LOCK,
					    ANDROID_CONTROL_AE_LOCK_OFF) == ANDROID_CONTROL_AE_LOCK_ON;
	const bool awbLock = facts_.awbLock &&
			     requestEntryInt(p.settings, ANDROID_CONTROL_AWB_LOCK,
					     ANDROID_CONTROL_AWB_LOCK_OFF) == ANDROID_CONTROL_AWB_LOCK_ON;

	if (facts_.evCompensation)
		ctrls.set(libcamera::controls::ExposureValue,
			  static_cast<float>(ev) / kEvStepsPerStop);

	/*
	 * 防频闪：交流供电的灯按电网频率的两倍闪烁，曝光时间是闪烁周期的整数倍时画面
	 * 没有条纹。50HZ/60HZ 让 libcamera 的 AE 把不短于一个周期的曝光向下取整到
	 * 10 ms / 8.333 ms 的整数倍、增益补足；AUTO 没有检测，按 kAntibandingAutoPeriodUs。
	 * 手动曝光不受影响；AE 锁定时锁住的曝光按补偿缩放后由下面自己取整（libipa 对手动
	 * 曝光不取整）。
	 */
	int32_t periodUs = 0;
	if (facts_.antibanding) {
		switch (requestEntryInt(p.settings, ANDROID_CONTROL_AE_ANTIBANDING_MODE,
					ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO)) {
		case ANDROID_CONTROL_AE_ANTIBANDING_MODE_50HZ:
			periodUs = 10000;
			break;
		case ANDROID_CONTROL_AE_ANTIBANDING_MODE_60HZ:
			periodUs = 8333;
			break;
		case ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO:
			periodUs = kAntibandingAutoPeriodUs;
			break;
		default:
			break;
		}
		ctrls.set(libcamera::controls::AeFlickerMode,
			  periodUs > 0 ? libcamera::controls::FlickerManual
				       : libcamera::controls::FlickerOff);
		if (periodUs > 0)
			ctrls.set(libcamera::controls::AeFlickerPeriod, periodUs);
	}

	/* 手动曝光：AE 模式 OFF，或整个 3A 关（CONTROL_MODE_OFF） */
	const bool controlOff = facts_.manualSensor &&
				requestEntryInt(p.settings, ANDROID_CONTROL_MODE,
						ANDROID_CONTROL_MODE_AUTO) == ANDROID_CONTROL_MODE_OFF;
	const bool manualAe =
		facts_.manualSensor && (controlOff || aeMode_ == ANDROID_CONTROL_AE_MODE_OFF);

	if (manualAe) {
		const int64_t expNs = requestEntryInt64(p.settings, ANDROID_SENSOR_EXPOSURE_TIME,
							static_cast<int64_t>(lastExposureUs_) * 1000);
		const int32_t iso = requestEntryInt(p.settings, ANDROID_SENSOR_SENSITIVITY,
						    static_cast<int32_t>(std::lround(lastGain_ * 100.0)));
		const int64_t frameNs = requestEntryInt64(p.settings, ANDROID_SENSOR_FRAME_DURATION, 0);
		const int64_t expUs = std::clamp<int64_t>(expNs / 1000, std::max(expMinUs_, 1),
							  std::max(expMaxUs_, std::max(expMinUs_, 1)));
		ctrls.set(libcamera::controls::ExposureTimeMode,
			  libcamera::controls::ExposureTimeModeManual);
		ctrls.set(libcamera::controls::ExposureTime, static_cast<int32_t>(expUs));
		ctrls.set(libcamera::controls::AnalogueGainMode,
			  libcamera::controls::AnalogueGainModeManual);
		ctrls.set(libcamera::controls::AnalogueGain,
			  std::clamp(iso / 100.0f, gainMin_, std::max(gainMax_, gainMin_)));
		/* 按要求的帧长，至少与曝光一样长 */
		const int64_t frameUs = std::max<int64_t>(frameNs / 1000, expUs);
		ctrls.set(libcamera::controls::FrameDurationLimits, { frameUs, frameUs });
		aeLocked_ = false;
		p.aeState = ANDROID_CONTROL_AE_STATE_INACTIVE;
	} else if (facts_.aeLock) {
		/*
		 * 还没有一帧报过曝光（会话刚打开）时先不锁，AE 照常跑，等有了值再锁：
		 * 不带值的手动模式会让 libipa 停在配置时的默认曝光。
		 */
		/*
		 * 没锁时开始的预拍（预闪）进行中也先不锁：等 AE 在闪光下收敛、预拍
		 * 做完再锁，否则锁住的是没开灯时测的曝光，闪光照片过曝。
		 */
		const bool lockNow =
			aeLock && (aeLocked_ || (lastExposureUs_ > 0 && lastGain_ > 0 &&
						 !(precaptureActive_ && !precaptureLocked_)));
		if (lockNow && !aeLocked_) {
			/* 锁住的曝光是在上一个完成帧的补偿下测的，补偿从那里算起 */
			lockEvSteps_ = lastEvSteps_;
			lockedExposureUs_ = lastExposureUs_;
			lockedGain_ = lastGain_;
			ALOGI("AE locked: exposure %d us, gain %.2f, EV %d/%d", lockedExposureUs_,
			      lockedGain_, lockEvSteps_, kEvStepsPerStop);
		}
		aeLocked_ = lockNow;
		ctrls.set(libcamera::controls::ExposureTimeMode,
			  lockNow ? libcamera::controls::ExposureTimeModeManual
				  : libcamera::controls::ExposureTimeModeAuto);
		ctrls.set(libcamera::controls::AnalogueGainMode,
			  lockNow ? libcamera::controls::AnalogueGainModeManual
				  : libcamera::controls::AnalogueGainModeAuto);
		if (lockNow) {
			/*
			 * 锁住的曝光按锁定以来的补偿缩放：先改曝光时间（不超过帧率
			 * 范围允许的帧长），不够再改增益。
			 */
			const double scale = std::exp2(static_cast<double>(ev - lockEvSteps_) /
						       kEvStepsPerStop);
			const int32_t fpsMin =
				requestEntryInt(p.settings, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, 0, 0);
			double maxUs = expMaxUs_ > 0 ? expMaxUs_ : lockedExposureUs_ * 4.0;
			if (fpsMin > 0)
				maxUs = std::min(maxUs, 1e6 / fpsMin);
			const double total = lockedExposureUs_ * lockedGain_ * scale;
			double exposure = std::clamp(lockedExposureUs_ * scale,
						     static_cast<double>(std::max(expMinUs_, 1)),
						     std::max(maxUs, 1.0));
			/*
			 * 防频闪：缩放后的曝光不再是闪烁周期的整数倍（例如 30 ms 加 1/3 档成
			 * 37.8 ms），向下取整到整数倍、差额由增益补足。锁住的曝光本身就是整数倍，
			 * 但报回来的值截断到了微秒（29997 us），留 1% 的余量，免得少一个周期。
			 */
			if (periodUs > 0 && exposure >= periodUs) {
				const double n = std::floor(exposure / periodUs + 0.01);
				exposure = std::max(n, 1.0) * periodUs;
			}
			const double gain = std::clamp(total / exposure, static_cast<double>(gainMin_),
						       static_cast<double>(std::max(gainMax_, gainMin_)));
			/*
			 * libcamera 报的曝光是截断到微秒的整数，libipa 换回行数时又截断：
			 * 原样下发会少一行（亮处曝光只有几行时差出百分之十几）。加 1 us
			 * 落回原来的行数（行长大于 1 us）。
			 */
			const int32_t expUs = std::min<int32_t>(
				static_cast<int32_t>(exposure) + 1,
				expMaxUs_ > 0 ? expMaxUs_ : std::numeric_limits<int32_t>::max());
			ctrls.set(libcamera::controls::ExposureTime, expUs);
			ctrls.set(libcamera::controls::AnalogueGain, static_cast<float>(gain));
		}
	}

	const int32_t awbMode =
		requestEntryInt(p.settings, ANDROID_CONTROL_AWB_MODE, ANDROID_CONTROL_AWB_MODE_AUTO);
	/* AWB 预设：按它的色温手动白平衡 */
	const int32_t presetK = facts_.awbPresets && !controlOff ? awbPresetKelvin(awbMode) : 0;
	/*
	 * AWB 关（或整个 3A 关）：用应用给的增益（R、Geven、Godd、B），换算成
	 * R、B 相对 G 的值；没给时保持最后的增益。
	 */
	const bool manualWb = facts_.awbLock && (controlOff || awbMode == ANDROID_CONTROL_AWB_MODE_OFF);
	const float cgR = requestEntryFloat(p.settings, ANDROID_COLOR_CORRECTION_GAINS, -1.0f, 0);
	const float cgGe = requestEntryFloat(p.settings, ANDROID_COLOR_CORRECTION_GAINS, -1.0f, 1);
	const float cgGo = requestEntryFloat(p.settings, ANDROID_COLOR_CORRECTION_GAINS, -1.0f, 2);
	const float cgB = requestEntryFloat(p.settings, ANDROID_COLOR_CORRECTION_GAINS, -1.0f, 3);
	const float cgG = (cgGe + cgGo) / 2.0f;
	if (manualWb && cgR > 0.0f && cgB > 0.0f && cgG > 0.0f) {
		const std::array<float, 2> gains = { cgR / cgG, cgB / cgG };
		if (!awbLocked_)
			lockedColourTemp_ = lastColourTemp_;
		ctrls.set(libcamera::controls::AwbEnable, false);
		ctrls.set(libcamera::controls::ColourGains, std::span<const float, 2>(gains));
		p.sentGains = true;
		if (lockedColourTemp_ > 0)
			ctrls.set(libcamera::controls::ColourTemperature, lockedColourTemp_);
		/* 之后不再给增益时保持 */
		lockedColourGains_ = gains;
		awbLocked_ = true;
	} else if (presetK) {
		ctrls.set(libcamera::controls::AwbEnable, false);
		ctrls.set(libcamera::controls::ColourTemperature, presetK);
		awbLocked_ = false;
	} else if (facts_.awbLock) {
		/* 有增益报回来之后才锁；AWB 关或整个 3A 关时也保持 */
		if ((awbLock || manualWb) && !awbLocked_ && lastColourGains_) {
			lockedColourGains_ = *lastColourGains_;
			lockedColourTemp_ = lastColourTemp_;
			awbLocked_ = true;
			ALOGI("AWB locked: gains R %.3f B %.3f, %d K", lockedColourGains_[0],
			      lockedColourGains_[1], lockedColourTemp_);
		} else if (!awbLock && !manualWb) {
			awbLocked_ = false;
		}
		ctrls.set(libcamera::controls::AwbEnable, !awbLocked_);
		if (awbLocked_) {
			ctrls.set(libcamera::controls::ColourGains,
				  std::span<const float, 2>(lockedColourGains_));
			p.sentGains = true;
			if (lockedColourTemp_ > 0)
				ctrls.set(libcamera::controls::ColourTemperature, lockedColourTemp_);
		}
	}

	/*
	 * AE 锁上之后报 LOCKED（还没锁上时照常报，那几帧曝光还在变）。预拍触发
	 * 时已经锁着：状态保持 LOCKED（状态表：触发被忽略），闪光照常可以闪。
	 * 预拍进行中才锁上：预拍做完之前报 PRECAPTURE。
	 */
	if (aeLocked_ && (precaptureLocked_ || p.aeState != ANDROID_CONTROL_AE_STATE_PRECAPTURE))
		p.aeState = ANDROID_CONTROL_AE_STATE_LOCKED;
	p.evSteps = ev;
	/* 不在 AUTO 模式时报 AWB_STATE_INACTIVE */
	p.awbState = manualWb || presetK ? ANDROID_CONTROL_AWB_STATE_INACTIVE
		     : awbLocked_		 ? ANDROID_CONTROL_AWB_STATE_LOCKED
						 : ANDROID_CONTROL_AWB_STATE_CONVERGED;
}

const uint8_t *Session::mapFrame(int fd, size_t len)
{
	auto it = frameMaps_.find(fd);
	if (it != frameMaps_.end() && it->second.second >= len)
		return static_cast<const uint8_t *>(it->second.first);
	if (it != frameMaps_.end()) {
		munmap(it->second.first, it->second.second);
		frameMaps_.erase(it);
	}
	void *p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		ALOGE("mmap of a libcamera frame failed: %s", strerror(errno));
		return nullptr;
	}
	frameMaps_[fd] = { p, len };
	return static_cast<const uint8_t *>(p);
}

void Session::unmapFrames()
{
	std::lock_guard<std::mutex> lk(frameMapMutex_);
	for (auto &[fd, m] : frameMaps_)
		munmap(m.first, m.second);
	frameMaps_.clear();
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

	const libcamera::FrameBuffer *fb = req->findBuffer(stream_);
	if (!fb) {
		ALOGE("frame=%d 完成的请求里没有处理过的帧", pend.frameNumber);
		reportRequestError(pend);
		std::lock_guard<std::mutex> lk(mutex_);
		flashRequestDone(pend);
		returnToPool(std::move(owned));
		return;
	}
	/*
	 * 驱动标为出错的帧（硬件 ISP 写坏或没写完，例如双 IFE 一侧的条带出错）不能当成好帧
	 * 交给应用、编成 JPEG 或拿去做 AF/曝光：整个请求按出错上报。
	 */
	if (fb->metadata().status != libcamera::FrameMetadata::FrameSuccess) {
		ALOGW("frame=%d: libcamera frame status %d, request reported as failed",
		      pend.frameNumber, static_cast<int>(fb->metadata().status));
		reportRequestError(pend);
		std::lock_guard<std::mutex> lk(mutex_);
		flashRequestDone(pend);
		returnToPool(std::move(owned));
		return;
	}
	const int64_t timestamp = fb->metadata().timestamp;

	/* 映射一次，分发给所有流。libcamera 把单个 dmabuf 里的 NV12 描述成同一 fd 上的两个平面。 */
	std::unique_lock<std::mutex> mapLock(frameMapMutex_);
	const auto &planes = fb->planes();
	SrcFrame src;
	const uint8_t *srcMap = nullptr;
	size_t srcLen = 0;
	int srcFd = -1;
	if (!planes.empty()) {
		srcFd = planes[0].fd.get();
		for (const auto &p : planes) {
			if (p.fd.get() == srcFd)
				srcLen = std::max<size_t>(srcLen, p.offset + p.length);
		}
		srcMap = mapFrame(srcFd, srcLen);
		if (srcMap) {
			/*
			 * IFE 用 DMA 写帧，CPU 读之前要让缓存失效，否则会读到旧的缓存行：
			 * 画面里出现 64 字节一段的旧数据（实测为黑段和绿段）。
			 */
			struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
			if (ioctl(srcFd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && !syncWarned_) {
				ALOGW("DMA_BUF_IOCTL_SYNC failed: %s", strerror(errno));
				syncWarned_ = true;
			}
			src.data = srcMap + planes[0].offset;
			src.stride = srcStride_;
			src.w = srcWidth_;
			src.h = srcHeight_;
			/*
			 * IFE 变焦：这一帧显示路径实际显示的矩形（元数据 ScalerCrop，像素阵列坐标）。
			 * 请求的区域按它换算到画面像素：(want − shown.xy) × 画面尺寸 / shown 的宽高，
			 * 取整偏差、宽高比差异、超出 IFE 上限都在这一步里处理。
			 */
			if (ifeZoom_ && !scalerCropMax_.isNull()) {
				const auto shown = req->metadata().get(libcamera::controls::ScalerCrop);
				if (!shown && !zoomMetaWarned_) {
					ALOGW("No ScalerCrop in the metadata: zoom falls back to a centred crop");
					zoomMetaWarned_ = true;
				}
				if (shown && shown->width > 0 && shown->height > 0) {
					const libcamera::Rectangle want = scalerCropFor(pend.crop);
					const double sx = static_cast<double>(src.w) / shown->width;
					const double sy = static_cast<double>(src.h) / shown->height;
					src.hasWin = true;
					src.win[0] = (want.x - shown->x) * sx;
					src.win[1] = (want.y - shown->y) * sy;
					src.win[2] = want.width * sx;
					src.win[3] = want.height * sy;
					const auto &m = scalerCropMax_;
					src.view[0] = static_cast<float>(shown->x - m.x) / m.width;
					src.view[1] = static_cast<float>(shown->y - m.y) / m.height;
					src.view[2] = static_cast<float>(shown->x + static_cast<int>(shown->width) - m.x) /
						      m.width;
					src.view[3] = static_cast<float>(shown->y + static_cast<int>(shown->height) - m.y) /
						      m.height;
				}
			}
		}
		/* 第二道保险（#109）：帧不够配置的尺寸就整帧报 ERROR_BUFFER，绝不越界读。 */
		if (src.data) {
			const size_t ySize = static_cast<size_t>(srcStride_) * srcHeight_;
			const size_t uvOffset = planes.size() > 1 && planes[1].fd.get() == srcFd
							? planes[1].offset
							: planes[0].offset + ySize;
			if (planes[0].offset + ySize > srcLen || uvOffset + ySize / 2 > srcLen) {
				ALOGE("NV12 frame of %zu bytes is too small for %dx%d, dropped", srcLen,
				      srcWidth_, srcHeight_);
				src.data = nullptr;
			} else {
				src.uv = srcMap + uvOffset;
			}
		}
	}

	/* 采样平均亮度（Y 平面每 16 行 × 每 16 列），给 AUTO 闪光当"太暗"判据。
	 * ★ 同一个信号还给自动对焦当"AE 已收敛"判据（见 AutoFocus.h 的 sceneStable）。 */
	int lumaStableFor = 0;
	if (src.data) {
		uint64_t sum = 0;
		uint32_t n = 0;
		for (int y = 0; y < srcHeight_; y += 16) {
			const uint8_t *px = src.data + static_cast<size_t>(y) * srcStride_;
			for (int x = 0; x < srcWidth_; x += 16, px += 16) {
				sum += px[0];
				n++;
			}
		}
		if (n) {
			const int luma = static_cast<int>(sum / n);
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
	if (src.data && facts_.hasAf) {
		/* The RAW frame of the same request, for phase detection. */
		const libcamera::FrameBuffer *rawFb =
			rawForPhase_ && af_.wantsRaw() ? req->findBuffer(rawStream_) : nullptr;
		const uint8_t *rawMap = nullptr;
		int rawFd = -1;
		RawFrame raw{};
		if (rawFb && rawFb->metadata().status == libcamera::FrameMetadata::FrameSuccess &&
		    !rawFb->planes().empty()) {
			const auto &rp = rawFb->planes()[0];
			rawFd = rp.fd.get();
			rawMap = mapFrame(rawFd, rp.offset + rp.length);
			if (rawMap && rp.length >= static_cast<size_t>(rawStride_) * rawHeight_) {
				/* 这帧由 RDI 用 DMA 写入：先丢掉 CPU 缓存里的旧数据 */
				struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
				ioctl(rawFd, DMA_BUF_IOCTL_SYNC, &sync);
				raw = { rawMap + rp.offset,
					static_cast<int>(rawStride_), rawWidth_, rawHeight_ };
			}
		}
		/* 硬件 ISP 在 AF 区域上算出的清晰度分数（有的话） */
		const auto fom = req->metadata().get(libcamera::controls::FocusFoM);
		af_.onFrame(src.data, srcWidth_, srcHeight_, srcStride_, lumaStableFor >= 2,
			    raw.data ? &raw : nullptr,
			    fom ? static_cast<double>(*fom) : -1.0, lastGain_, src.view);
		if (raw.data) {
			struct dma_buf_sync sync = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
			ioctl(rawFd, DMA_BUF_IOCTL_SYNC, &sync);
		}
	}

	/* libcamera（libipa 的 Agc::fillMetadata）每帧报实际曝光与模拟增益：回填结果、驱动降噪强度与闪光判据。 */
	int64_t exposureNs = 0;
	int32_t sensitivity = 0;
	int64_t frameDurationNs = 0;
	std::array<float, 4> ccGains{};
	std::array<float, 9> ccTransform{};
	bool haveCcGains = false, haveCcTransform = false;
	{
		const libcamera::ControlList &md = req->metadata();
		auto gain = md.get(libcamera::controls::AnalogueGain);
		auto expo = md.get(libcamera::controls::ExposureTime);
		auto cg = md.get(libcamera::controls::ColourGains);
		auto ct = md.get(libcamera::controls::ColourTemperature);
		auto ccm = md.get(libcamera::controls::ColourCorrectionMatrix);
		auto fd = md.get(libcamera::controls::FrameDuration);
		if (fd && *fd > 0)
			frameDurationNs = *fd * 1000;
		std::lock_guard<std::mutex> lk(mutex_);
		if (cg && (*cg)[0] > 0 && (*cg)[1] > 0)
			lastColourGains_ = std::array<float, 2>{ (*cg)[0], (*cg)[1] };
		/*
		 * 请求带了手动增益（AWB 锁定、AWB 关）的帧不取色温：这时 libipa 报的
		 * 是手动状态里留下的色温（配置时的 5000 K 或上次的预设），不是画面的。
		 * 预设模式的帧报的就是预设的色温，照取。
		 */
		if (ct && *ct > 0 && !pend.sentGains)
			lastColourTemp_ = *ct;
		lastEvSteps_ = pend.evSteps;
		if (gain && *gain > 0)
			lastGain_ = *gain;
		if (expo && *expo > 0)
			lastExposureUs_ = *expo;
		if (gain)
			sensitivity = static_cast<int32_t>(*gain * 100.0f + 0.5f);
		if (expo)
			exposureNs = static_cast<int64_t>(*expo) * 1000;
		/* 结果报这一帧实际用的白平衡增益与色彩矩阵（声明了 COLOR_CORRECTION_MODE） */
		if (cg && (*cg)[0] > 0 && (*cg)[1] > 0) {
			ccGains = { (*cg)[0], 1.0f, 1.0f, (*cg)[1] };
			haveCcGains = true;
		}
		if (ccm) {
			std::copy(ccm->begin(), ccm->end(), ccTransform.begin());
			haveCcTransform = true;
		}
	}

	bool hasBlob = false;
	for (const auto &pb : pend.buffers)
		hasBlob = hasBlob || pb.isBlob;

	/*
	 * 两路输出时的 FULL 那一路：只有带 JPEG 的请求、预览要从 FULL 出的请求（变焦）才带它的
	 * 缓冲，这里映射、同步。JPEG 从它编码；previewFromFull 时非 JPEG 流也从它出。
	 */
	SrcFrame full;
	const uint8_t *fullMap = nullptr;
	int fullFd = -1;
	if (fullStream_ && (hasBlob || pend.previewFromFull)) {
		const libcamera::FrameBuffer *ffb = req->findBuffer(fullStream_);
		if (!ffb || ffb->metadata().status != libcamera::FrameMetadata::FrameSuccess ||
		    ffb->planes().empty()) {
			/* 只为预览要 FULL 而没拿到时（见 processCaptureRequest）不算异常 */
			if (hasBlob || ffb)
				ALOGW("frame=%d: no usable full-size frame", pend.frameNumber);
		} else {
			const auto &fp = ffb->planes();
			fullFd = fp[0].fd.get();
			size_t fullLen = 0;
			for (const auto &p : fp)
				if (p.fd.get() == fullFd)
					fullLen = std::max<size_t>(fullLen, p.offset + p.length);
			const size_t ySize = static_cast<size_t>(fullStride_) * fullHeight_;
			const size_t uvOffset = fp.size() > 1 && fp[1].fd.get() == fullFd
							? fp[1].offset
							: fp[0].offset + ySize;
			if (fp[0].offset + ySize > fullLen || uvOffset + ySize / 2 > fullLen) {
				ALOGE("full-size NV12 frame of %zu bytes is too small for %dx%d", fullLen,
				      fullWidth_, fullHeight_);
			} else if ((fullMap = mapFrame(fullFd, fullLen))) {
				struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
				ioctl(fullFd, DMA_BUF_IOCTL_SYNC, &sync);
				full.data = fullMap + fp[0].offset;
				full.uv = fullMap + uvOffset;
				full.stride = fullStride_;
				full.w = fullWidth_;
				full.h = fullHeight_;
			}
		}
	}
	/* JPEG 的来源；两路输出却没拿到 FULL 时为空，JPEG 报 ERROR_BUFFER */
	const SrcFrame still = fullStream_ ? full : src;
	/* 非 JPEG 流的来源：FULL 拿不到时退回显示路径（放大，画质差些，但有画面） */
	const SrcFrame &view = pend.previewFromFull && full.data ? full : src;
	/* 预览、录像的色度降噪（照片的降噪在 deliverJpeg() 里） */
	const int chromaBlur = chromaBlurRadius();

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

	/* 这一帧的锐化参数：请求的 EDGE_MODE（没有时按 FAST）、这一帧的增益、调试属性的倍数 */
	{
		const auto edge = static_cast<uint8_t>(
			requestEntryInt(pend.settings, ANDROID_EDGE_MODE, ANDROID_EDGE_MODE_FAST));
		char v[PROP_VALUE_MAX] = {};
		const double scale = __system_property_get("debug.gaokun3.camera.sharpen", v) > 0
					     ? atof(v)
					     : 1.0;
		sharpen_ = sharpenParams(edge, sensitivity > 0 ? sensitivity / 100.0 : 1.0, scale);
	}
	std::vector<bool> okv;
	renderFrame_++;
	std::copy(pend.crop, pend.crop + 4, cropRegion_);

	/* 这一帧的照片参数（裁剪、锐化、EXIF、缩略图），给它的 JPEG 流用 */
	StillParams sp;
	std::copy(pend.crop, pend.crop + 4, sp.crop);
	sp.sharpen = sharpen_;
	{
		ExifInfo &exif = sp.exif;
		exif.make = exifMake_;
		exif.model = exifModel_;
		struct timespec now = {};
		clock_gettime(CLOCK_REALTIME, &now);
		exif.captureTime = now.tv_sec;
		exif.subsecMs = static_cast<int32_t>(now.tv_nsec / 1000000);
		exif.focalLengthMm = lensParams(facts_.frontFacing).focalLengthMm;
		exif.fNumber = lensParams(facts_.frontFacing).fNumber;
		exif.exposureNs = exposureNs;
		exif.iso = sensitivity;
		exif.hasFlash = !flashLed_.empty();
		exif.flashFired = pend.flashFired;
		/* EXIF 闪光模式：1 强制闪（手电筒也算），2 强制不闪，3 自动 */
		exif.flashMode = pend.aeMode == ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH ? 3
				 : pend.aeMode == ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH ||
						 pend.flashMode == ANDROID_FLASH_MODE_SINGLE ||
						 pend.flashMode == ANDROID_FLASH_MODE_TORCH
					 ? 1
					 : 2;
		const bool controlOff = requestEntryInt(pend.settings, ANDROID_CONTROL_MODE,
							ANDROID_CONTROL_MODE_AUTO) ==
					ANDROID_CONTROL_MODE_OFF;
		exif.manualExposure = controlOff || pend.aeMode == ANDROID_CONTROL_AE_MODE_OFF;
		exif.manualWhiteBalance =
			controlOff || requestEntryInt(pend.settings, ANDROID_CONTROL_AWB_MODE,
						      ANDROID_CONTROL_AWB_MODE_AUTO) !=
					      ANDROID_CONTROL_AWB_MODE_AUTO;
		/* 位置：应用要求记录时才写 */
		const double lat = requestEntryDouble(pend.settings, ANDROID_JPEG_GPS_COORDINATES, 0);
		const double lon = requestEntryDouble(pend.settings, ANDROID_JPEG_GPS_COORDINATES, 1);
		const double alt = requestEntryDouble(pend.settings, ANDROID_JPEG_GPS_COORDINATES, 2);
		if (std::isfinite(lat) && std::isfinite(lon)) {
			ExifGps g;
			g.latitude = lat;
			g.longitude = lon;
			g.altitude = std::isfinite(alt) ? alt : 0.0;
			g.timestamp = requestEntryInt64(pend.settings, ANDROID_JPEG_GPS_TIMESTAMP,
							exif.captureTime);
			g.method = requestEntryBytes(pend.settings, ANDROID_JPEG_GPS_PROCESSING_METHOD);
			exif.gps = g;
		}
		sp.thumbW = std::max(requestEntryInt(pend.settings, ANDROID_JPEG_THUMBNAIL_SIZE, 0, 0), 0);
		sp.thumbH = std::max(requestEntryInt(pend.settings, ANDROID_JPEG_THUMBNAIL_SIZE, 0, 1), 0);
		/*
		 * 只做声明过的尺寸（kThumbnailSizes）：别的尺寸按宽高比取最接近的
		 * 一个，免得按应用给的任意大小分配内存。
		 */
		if (sp.thumbW == 0 || sp.thumbH == 0) {
			sp.thumbW = sp.thumbH = 0;
		} else {
			int32_t bw = 0, bh = 0;
			double best = 1e9;
			for (size_t i = 0; i + 1 < std::size(kThumbnailSizes); i += 2) {
				const int32_t w = kThumbnailSizes[i], h = kThumbnailSizes[i + 1];
				if (w == sp.thumbW && h == sp.thumbH) {
					bw = w;
					bh = h;
					break;
				}
				const double d = w > 0 && h > 0
							 ? std::fabs(std::log(static_cast<double>(w) * sp.thumbH /
									       (static_cast<double>(h) * sp.thumbW)))
							 : 1e9;
				if (d < best) {
					best = d;
					bw = w;
					bh = h;
				}
			}
			sp.thumbW = bw;
			sp.thumbH = bh;
		}
		sp.thumbQuality = requestEntryInt(pend.settings, ANDROID_JPEG_THUMBNAIL_QUALITY, 85);
	}
	/*
	 * 两路输出、JPEG 从 FULL 编码时交给工作线程（见 Session.h 的 StillJob）：这里只记下任务，
	 * 请求还池之后再入队；FULL 那一路的预览（previewFromFull）在下面同步交付完。
	 */
	std::optional<StillJob> job;
	size_t jobIndex = SIZE_MAX;
	for (size_t i = 0; i < pend.buffers.size(); i++) {
		const PendingBuffer &pb = pend.buffers[i];
		const bool shared = std::count_if(pend.buffers.begin(), pend.buffers.end(),
						  [&](const PendingBuffer &o) {
							  return !o.isBlob && o.width == pb.width &&
								 o.height == pb.height;
						  }) > 1;
		bool ok;
		if (pb.isBlob && fullStream_ && !job) {
			/*
			 * 两路输出时每个 JPEG 都经任务队列交付，FULL 不能用时也是（src 为空，工作线程按
			 * 出错交付）：同一路流的缓冲必须按帧号顺序返回（ICameraDeviceCallback.aidl），
			 * 这里直接报错会抢在前一张还在编码的照片前面，框架随后把那张也当成丢失。
			 */
			job.emplace();
			job->src = full;
			if (full.data) {
				job->fullBuf = req->findBuffer(fullStream_);
				job->fullFd = fullFd;
			}
			job->frameNumber = pend.frameNumber;
			job->streamId = pb.streamId;
			job->bufferId = pb.bufferId;
			job->handle = pb.handle;
			job->width = pb.width;
			job->height = pb.height;
			job->blobSize = pb.blobSize;
			job->quality = jpegQuality;
			job->orientation = jpegOrientation;
			job->params = sp;
			jobIndex = i;
			ok = true;   /* 缓冲由工作线程编码后另发 */
		} else {
			ok = pb.isBlob ? still.data && deliverJpeg(still, pb.handle, pb.width, pb.height,
								   pb.blobSize, jpegQuality,
								   jpegOrientation, sp, scratch_)
				       : view.data && deliver(view, pb.handle, pb.width, pb.height,
							      chromaBlur, shared);
		}
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

	if (srcMap) {
		struct dma_buf_sync sync = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
		ioctl(srcFd, DMA_BUF_IOCTL_SYNC, &sync);
	}
	/* 交给任务的 FULL 由工作线程编码完再结束同步 */
	if (fullMap && !job) {
		struct dma_buf_sync sync = { DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
		ioctl(fullFd, DMA_BUF_IOCTL_SYNC, &sync);
	}
	mapLock.unlock();

	/*
	 * ★ req 到这里才算用完（buffers()/metadata() 都读过了、帧的 DMA 同步已结束）⇒ 先还池，再交结果：
	 *   框架拿回 buffer 会立刻发下一个请求，池子必须已经有位置（PR #6 初版在
	 *   processCaptureResult 之后才还，于是框架的下一个请求常常撞上空池）。
	 *   ⚠️ 也不能更早：还回去之后 processCaptureRequest 可能立刻取走并 reuse()
	 *   （实测崩过：tid name=CameraManager，栈顶 onRequestCompleted+1428）。
	 */
	bool staleJob = false;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		/*
		 * 请求已不属于当前配置（drain 超时后强制清理过，映射可能已解除）：不交给工作线程，
		 * JPEG 按出错交付。
		 */
		if (job && !configRequests_.count(req)) {
			job.reset();
			staleJob = true;
		}
		/* 先登记，returnToPool() 就不会把任务的 FULL 收回 fullFree_ */
		if (job) {
			if (job->fullBuf)
				fullHeld_.insert(job->fullBuf);
			stillInFlight_++;
		}
		returnToPool(std::move(owned));
	}
	if (staleJob) {
		okv[jobIndex] = false;
		NotifyMsg emsg;
		ErrorMsg e;
		e.frameNumber = pend.frameNumber;
		e.errorStreamId = pend.buffers[jobIndex].streamId;
		e.errorCode = ErrorCode::ERROR_BUFFER;
		emsg.set<NotifyMsg::Tag::error>(e);
		std::vector<NotifyMsg> emsgs;
		emsgs.push_back(std::move(emsg));
		cb_->notify(emsgs);
		jobIndex = SIZE_MAX;
	}
	/*
	 * 结果里的缩略图尺寸：同步编码时是实际做出的；交给任务时按要做的报（结果元数据必须按帧号
	 * 顺序发，不能等编码）。缩略图超过 EXIF 段的上限被丢掉时两者不同，工作线程会记一条警告。
	 */
	int32_t thumbSize[2] = { sp.thumbMade[0], sp.thumbMade[1] };
	if (job) {
		int32_t tw = 0, th = 0;
		thumbnailSize(sp, jpegOrientation, &tw, &th);
		thumbSize[0] = tw > 0 && th > 0 ? tw : 0;
		thumbSize[1] = tw > 0 && th > 0 ? th : 0;
		{
			std::lock_guard<std::mutex> g(stillMutex_);
			stillJobs_.push_back(std::move(*job));
		}
		stillCv_.notify_one();
	}

	CaptureResult result;
	result.frameNumber = pend.frameNumber;
	result.fmqResultSize = 0;
	result.partialResult = 1;
	FrameResultFacts fr;
	fr.flashState = flashLed_.empty() ? ANDROID_FLASH_STATE_UNAVAILABLE
			: (pend.flashFired ? ANDROID_FLASH_STATE_FIRED : ANDROID_FLASH_STATE_READY);
	fr.aeState = pend.aeState;
	fr.awbState = pend.awbState;
	fr.aeMode = pend.aeMode;
	fr.flashMode = pend.flashMode;
	fr.exposureNs = exposureNs;
	fr.frameDurationNs = frameDurationNs;
	std::copy(pend.crop, pend.crop + 4, fr.cropRegion);
	std::copy(thumbSize, thumbSize + 2, fr.thumbnailSize);
	fr.sensitivity = sensitivity;
	/*
	 * ★ 对焦结果组：应用（CameraX 的 FocusMeteringControl、各家相机的拍照前 AF）等的是
	 *   【结果里的】AF_STATE 到 FOCUSED_LOCKED。PR #6 初版 AutoFocus 算好了状态却从没填进来，
	 *   于是马达扫到了峰、应用却永远只看到 INACTIVE。
	 */
	fr.hasAf = facts_.hasAf;
	fr.manualSensor = facts_.manualSensor;
	fr.lens = lensParams(facts_.frontFacing);
	fr.hasCcGains = haveCcGains;
	fr.ccGains = ccGains;
	fr.hasCcTransform = haveCcTransform;
	fr.ccTransform = ccTransform;
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
		if (i == jobIndex)
			continue;   /* JPEG 由工作线程交付 */
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
	sendResults(results);

	/* 闪光灯：点过灯的请求都完成了、而且没人还要灯，才灭。 */
	std::lock_guard<std::mutex> lk(mutex_);
	flashRequestDone(pend);
}

} /* namespace gaokun3 */
