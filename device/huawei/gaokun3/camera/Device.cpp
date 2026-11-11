/* SPDX-License-Identifier: Apache-2.0 */
#include "Device.h"
#include "Provider.h"
#include "Session.h"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <fcntl.h>
#include <map>
#include <unistd.h>

#include <aidl/android/hardware/camera/common/CameraResourceCost.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceCallback.h>
#include <aidl/android/hardware/camera/device/ICameraDeviceSession.h>
#include <aidl/android/hardware/camera/device/ICameraInjectionSession.h>
#include <log/log.h>

#include <libcamera/control_ids.h>
#include <libcamera/formats.h>
#include <libcamera/property_ids.h>
#include <libcamera/stream.h>

using ::aidl::android::hardware::camera::common::CameraResourceCost;
using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::common::TorchModeStatus;
using ::aidl::android::hardware::camera::device::CameraMetadata;
using ::aidl::android::hardware::camera::device::ICameraDeviceCallback;
using ::aidl::android::hardware::camera::device::ICameraDeviceSession;
using ::aidl::android::hardware::camera::device::ICameraInjectionSession;
using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::StreamConfiguration;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}

/* 后摄闪光灯在内核里的名字（patches/kernel/dts/0006：PM8350C 闪光模块 1+4 路 → 一个 LED）。 */
constexpr char kRearFlashLed[] = "/sys/class/leds/white:flash";

bool writeSysfs(const std::string &path, const std::string &value)
{
	int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		ALOGE("打不开 %s: %s", path.c_str(), strerror(errno));
		return false;
	}
	ssize_t n = write(fd, value.c_str(), value.size());
	close(fd);
	if (n != static_cast<ssize_t>(value.size())) {
		ALOGE("写 %s 失败: %s", path.c_str(), strerror(errno));
		return false;
	}
	return true;
}

std::string readSysfs(const std::string &path)
{
	char buf[64] = {};
	int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return {};
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return {};
	std::string s(buf, static_cast<size_t>(n));
	while (!s.empty() && (s.back() == '\n' || s.back() == ' '))
		s.pop_back();
	return s;
}
} /* namespace */

Device::Device(std::shared_ptr<libcamera::Camera> cam, std::string name, Provider *provider)
	: cam_(std::move(cam)), name_(std::move(name)), provider_(provider)
{
}

Device::~Device() = default;

bool Device::init()
{
	const libcamera::ControlList &props = cam_->properties();

	/* ── 传感器阵列尺寸 ── */
	auto pixelArray = props.get(libcamera::properties::PixelArraySize);
	if (pixelArray) {
		facts_.pixelArrayW = pixelArray->width;
		facts_.pixelArrayH = pixelArray->height;
	} else {
		ALOGW("相机没有报 PixelArraySize");
	}

	/* 有效区优先用 PixelArrayActiveAreas，没有就退回全阵列。 */
	auto active = props.get(libcamera::properties::PixelArrayActiveAreas);
	if (active && !active->empty()) {
		facts_.activeW = (*active)[0].width;
		facts_.activeH = (*active)[0].height;
	} else {
		facts_.activeW = facts_.pixelArrayW;
		facts_.activeH = facts_.pixelArrayH;
	}
	if (facts_.activeW <= 0 || facts_.activeH <= 0) {
		ALOGE("拿不到有效的传感器尺寸（%dx%d）", facts_.activeW, facts_.activeH);
		return false;
	}

	/*
	 * Android's active array: the largest centred 4:3 part of the active
	 * area when that is within 2% of 4:3, all of it otherwise. Each output is
	 * the largest centred crop of the active array with its own aspect ratio
	 * (Session::cropFor()), and CameraX assumes the same when it lines up the
	 * streams of a session. With the ov13b10's 4208x3120 it took the 4:3
	 * preview to cover 4160 columns and the 16:9 video all 4208, and cropped
	 * the video to the 4160x2367 they share: 1080p recorded at 1898x1080.
	 * On an exact 4:3 array the 16:9 streams cover its full width.
	 */
	facts_.fovW = facts_.activeW;
	facts_.fovH = facts_.activeH;
	const int64_t w3 = static_cast<int64_t>(facts_.activeW) * 3;
	const int64_t h4 = static_cast<int64_t>(facts_.activeH) * 4;
	if (std::abs(w3 - h4) * 50 <= h4) {
		if (w3 > h4)
			facts_.fovW = static_cast<int32_t>(h4 / 3) & ~1;
		else
			facts_.fovH = static_cast<int32_t>(w3 / 4) & ~1;
	}
	facts_.fovX = (facts_.activeW - facts_.fovW) / 2 & ~1;
	facts_.fovY = (facts_.activeH - facts_.fovH) / 2 & ~1;

	/*
	 * Pixel size, for SENSOR_INFO_PHYSICAL_SIZE: the camera service builds
	 * the Camera1 (android.hardware.Camera) parameters with a field of view
	 * computed from it and refuses those clients without it ("unable to build
	 * defaults"), e.g. QQ.
	 */
	auto cell = props.get(libcamera::properties::UnitCellSize);
	if (cell) {
		facts_.unitCellW = cell->width;
		facts_.unitCellH = cell->height;
	} else {
		ALOGW("相机没有报 UnitCellSize：不声明物理尺寸，用 Camera1 的应用打不开它");
	}

	/* ── 朝向 ── */
	auto loc = props.get(libcamera::properties::Location);
	facts_.frontFacing = !loc || *loc == libcamera::properties::CameraLocationFront;

	/*
	 * ⚠️★★ 两个"方向"的约定是【相反的】，直接抄数值会差 180°。
	 *
	 *   libcamera properties::Rotation = 传感器相对机身的【物理安装角】，
	 *     用【逆时针】表达（property_ids 原文："the angular difference in the
	 *     counter-clockwise direction between the camera reference system 'Rc'
	 *     and the projected scene reference system 'Rp'"；内核
	 *     Documentation/devicetree/bindings/media/video-interface-devices.yaml
	 *     的 rotation 属性是同一套文字，libcamera 从这里继承 ——
	 *     camera_sensor_legacy.cpp 读的就是 V4L2_CID_CAMERA_SENSOR_ROTATION，
	 *     而那个控件由驱动 v4l2_ctrl_new_fwnode_properties() 从设备树填）。
	 *
	 *   ANDROID_SENSOR_ORIENTATION = 把输出图像转正所需的【顺时针】角。
	 *
	 *   ⇒ 逆时针换顺时针用 (360 - x) % 360，上游 libcamera 自己的 Android HAL
	 *     也是这一步（src/android/camera_device.cpp）：
	 *       "The Android orientation metadata specifies its rotation correction
	 *        value in clockwise direction whereas libcamera specifies the
	 *        rotation property in anticlockwise direction."
	 *     但 x 不能直接用设备树的值，还要先做下面两步。
	 *
	 *   ★ 两边的参照系差 90°（issue #23、#42）：
	 *     设备树按【横屏使用】的方向写（与下游 Linux 内核一致；绑定文档说"上"由使用环境决定，
	 *     笔记本摄像头的例子写 0）：后摄 180、前摄 0。Android 的自然方向却是面板原生的
	 *     【竖屏】，接键盘横放（前摄在上边框）时显示旋转是 ROTATION_90。SENSOR_ORIENTATION
	 *     要相对竖屏算：从横屏换到竖屏，前摄的安装角加 90°；后摄从机背那一面看，方向相反，
	 *     加 270°。
	 *
	 *   ★ 还要扣掉 libcamera 已经转掉的部分：会话不设 CameraConfiguration::orientation，
	 *     默认要 Rotate0，simple 流水线用 CameraSensor::computeTransform() 去凑。180 能靠
	 *     水平 + 垂直翻转消掉，后摄 ov13b10 支持翻转，就是被翻转读出的（日志里
	 *     transform hvflip）；90 / 270 要转置，消不掉。前摄 hi846 没有翻转控件。
	 *
	 *   ⇒ 本机：后摄 180 - 180 + 270 = 270 → SENSOR_ORIENTATION 90；
	 *           前摄 0 + 90 = 90 → SENSOR_ORIENTATION 270。
	 *     核对：接键盘横放（ROTATION_90，右边朝上，OrientationEventListener 报 270）时，
	 *     应用按 Android 文档的 getJpegOrientation 算出的 JPEG 角是后摄 (90 + 270) % 360 = 0、
	 *     前摄 (270 - 270) = 0，即交付的画面横放时本来就是正的，和设备树按横屏写的意思一致。
	 */
	auto rot = props.get(libcamera::properties::Rotation);
	const int32_t rawRotation = rot ? *rot : 0;
	if (!rot)
		ALOGW("libcamera 没报 Rotation 属性，按 0° 处理"
		      "（到 /dev/v4l-subdev* 上查 camera_sensor_rotation 控件）");
	int32_t ccw = ((rawRotation % 360) + 360) % 360;
	if (ccw % 90 != 0) {
		ALOGW("Rotation=%d 不是 90 的整数倍，按 0° 处理（Android 只认 0/90/180/270）",
		      rawRotation);
		ccw = 0;
	}

	/*
	 * libcamera 用翻转消掉了多少：生成一份配置，和 Session::configureStreams() 一样
	 * 不设 orientation（会话哪天要设，这里也得跟着改）。validate() 之后 orientation
	 * 还是 Rotate0，就是设备树的角度整个被消掉了；消不掉时它会把 orientation 改成
	 * 安装方向，不翻转。
	 * 不看 validate() 的返回值：simple 流水线一开头就算 orientation，后面即使因为
	 * 格式返回 Invalid，这个结果也已经定了，和会话用的格式无关。
	 */
	int32_t undone = 0;
	auto probe = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
	if (!probe || probe->empty()) {
		ALOGW("%s 生成不了配置，按 libcamera 不翻转处理", name_.c_str());
	} else {
		probe->validate();
		if (probe->orientation == libcamera::Orientation::Rotate0)
			undone = ccw;
	}

	/*
	 * 不再读 debug.gaokun3.camera.orientation.{rear,front}：那两个属性是给旧换算标定用的，
	 * 照 #42 设过的旧值（后 270、前 90）在这里会把方向带歪。要试别的角度就改设备树
	 * （rotation 控件是只读的，运行时改不了）。
	 *
	 * 先换到竖屏自然方向下的逆时针角，再按 (360 - 逆时针) 换成顺时针。
	 */
	const int32_t natural = (ccw - undone + (facts_.frontFacing ? 90 : 270) + 360) % 360;
	facts_.orientation = (360 - natural) % 360;
	ALOGI("%s 朝向：安装角 %d°（横屏参照系，逆时针），libcamera 翻转消掉 %d°，"
	      "竖屏自然方向下 %d° ⇒ SENSOR_ORIENTATION %d",
	      name_.c_str(), ccw, undone, natural, facts_.orientation);

	/*
	 * ── 闪光灯（#110）──
	 * 只有后摄有；LED 由内核暴露成 /sys/class/leds/white:flash（PM8350C 闪光模块 1+4 路）。
	 * 节点不在（旧内核 / 旧 dtb）就如实声明没有闪光灯，别让应用点一个不存在的灯。
	 * ⚠️ 写权限来自 ueventd.gaokun3.rc（brightness 0664 root camera）；HAL 以 cameraserver 跑。
	 */
	if (!facts_.frontFacing && access((std::string(kRearFlashLed) + "/brightness").c_str(),
					  W_OK) == 0)
		facts_.flashLed = kRearFlashLed;

	/*
	 * ── 对焦马达 ──
	 * 只有后摄有 VCM（i2c 1-000c 的 dw9714，内核把它暴露成独立 v4l2 子设备）。
	 * 这里【只找节点】、不开设备：打开 /dev/v4l-subdevN 本身就会给马达上电
	 * （dw9714_open → pm_runtime，见 Lens.h），相机还没开的时候不该占着它。真正的 open 在
	 * Session::init()（有会话才有必要）。
	 * 前摄 hi846 无马达 ⇒ hasAf 保持 false，元数据如实按定焦声明。
	 */
	if (!facts_.frontFacing && !Lens::findNode().empty()) {
		facts_.hasAf = true;
		ALOGI("%s 检测到对焦马达（VCM），将声明 AF", name_.c_str());
	}

	/*
	 * ── 对外声明的输出尺寸 ──
	 * ⚠️★ 不要只报满分辨率：流都不超过半个阵列时 HAL 向 libcamera 要合并模式的完整画面，
	 *    否则要全尺寸，每个输出流还要从整帧各转换一次（[#95] 时帧间隔约 130 ms；
	 *    2026-11 多线程去拜耳下全尺寸约 33 ms）。
	 *    这里给一组【降采样】的尺寸 + 满分辨率（留给静态拍照）。
	 */
	auto ratio = [&](int w) {
		return std::make_pair(w, static_cast<int>(
			static_cast<int64_t>(w) * facts_.fovH / facts_.fovW) & ~1);
	};
	/*
	 * Besides the sensor's own aspect ratio, offer the standard sizes (and
	 * 320x240, which camera2 requires for JPEG on every camera): CameraX's
	 * video qualities and the camcorder profiles in
	 * etc/media_profiles_V1_0.xml map onto 640x480, 1280x720, 1920x1080
	 * and so on, and without them a camera gets no video at all (the rear
	 * one had only 640x474 and 1280x948). deliver() crops to each stream's
	 * aspect ratio.
	 *
	 * Also exact 4:3 and 16:9 sizes at full resolution: CameraX takes the
	 * largest size that matches its aspect ratio within a few pixels. With
	 * 1280x960 on the list and no full-size 4:3, Aperture took photos at
	 * 1280x960.
	 */
	const int w43 = facts_.fovH * 4 / 3 & ~1;
	const std::pair<int, int> candidates[] = {
		{ 320, 240 }, ratio(640), { 640, 480 }, { 1280, 720 }, ratio(1280), { 1280, 960 },
		{ 1920, 1080 }, { w43, w43 * 9 / 16 & ~1 }, { w43, facts_.fovH },
		{ facts_.fovW, facts_.fovH },
	};
	facts_.outputSizes.clear();
	for (const auto &s : candidates) {
		if (s.first > facts_.fovW || s.second > facts_.fovH ||
		    std::find(facts_.outputSizes.begin(), facts_.outputSizes.end(), s) !=
			    facts_.outputSizes.end())
			continue;
		facts_.outputSizes.push_back(s);
	}

	/*
	 * 30 fps: the multithreaded CPU debayer keeps up with the sensor's
	 * 30 fps full-size mode (the preview was measured at about 29 fps at
	 * 4200x3120). Each extra output stream adds a conversion of the full
	 * frame in deliver(), which can lower the delivered rate. Aperture
	 * crashes when switching to video if no range reaches 24 or 30 fps.
	 */
	facts_.minFrameDurationNs = 33333333;   /* 30 fps */
	probeCamera();

	characteristics_ = buildCharacteristics(facts_);
	if (characteristics_.empty()) {
		ALOGE("characteristics 构造失败");
		return false;
	}

	ALOGI("%s 就绪：阵列 %dx%d 有效 %dx%d 视野 %dx%d+%d+%d 朝向 %d（libcamera rotation=%d，逆时针） %s%s%s",
	      name_.c_str(), facts_.pixelArrayW, facts_.pixelArrayH,
	      facts_.activeW, facts_.activeH, facts_.fovW, facts_.fovH, facts_.fovX,
	      facts_.fovY, facts_.orientation, rawRotation,
	      facts_.frontFacing ? "前摄" : "后摄",
	      facts_.flashLed.empty() ? "" : "（带闪光灯）",
	      facts_.hasAf ? "（带自动对焦）" : "");
	return true;
}

/*
 * libcamera 配置好之后这个相机能做什么；不开流。
 *
 * 帧率：硬件 ISP 路径上，每种输出尺寸的帧率取决于会话为它要的画面
 * （Session::hwIspFrameSize()）让 libcamera 选中的传感器模式：ov13b10 的合并
 * 模式能到 60 fps，全尺寸模式 30 fps。把画面复制到各路流跟得上 60 fps。
 * 软件 ISP 的 CPU 去马赛克仍是 30 fps。做法：每种不同的画面尺寸配置一次相机，
 * 读 libcamera 允许的最短帧长；失败的尺寸按 30 fps。
 *
 * 3A 控制：IPA 配置之后才列出它们，所以软件 ISP 也按默认流配置一次。
 * acquire 或配置失败的相机一个都不声明。
 */
void Device::probeCamera()
{
	facts_.minFrameDurationsNs.clear();
	facts_.evCompensation = facts_.aeLock = facts_.awbLock = facts_.awbPresets = false;
	facts_.manualSensor = false;
	ManualLimits limits;

	auto probe = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
	if (!probe || probe->empty())
		return;
	const auto fmts = probe->at(0).formats().pixelformats();
	auto has = [&](const libcamera::PixelFormat &f) {
		return std::find(fmts.begin(), fmts.end(), f) != fmts.end();
	};
	const bool hwIsp = has(libcamera::formats::NV12) && !has(libcamera::formats::RGB888);

	if (cam_->acquire()) {
		ALOGW("%s: camera busy, frame rates and 3A controls not probed", name_.c_str());
		return;
	}

	auto finish = [&]() {
		/*
		 * 手动曝光：用 AE 锁定的那套手动模式，范围取每种配置都满足的。
		 * ISO 100 对应单位增益。
		 */
		if (facts_.aeLock && limits.seen && limits.expMaxUs > limits.expMinUs &&
		    limits.gainMax >= limits.gainMin && limits.frameMaxUs < INT64_MAX) {
			facts_.maxFrameDurationNs = limits.frameMaxUs * 1000;
			facts_.exposureMinNs = std::max<int64_t>(limits.expMinUs, 1) * 1000;
			facts_.exposureMaxNs =
				std::min(limits.expMaxUs * 1000, facts_.maxFrameDurationNs);
			facts_.isoMin = static_cast<int32_t>(std::ceil(limits.gainMin * 100.0));
			facts_.isoMax = static_cast<int32_t>(std::floor(limits.gainMax * 100.0));
			facts_.manualSensor = facts_.exposureMaxNs > facts_.exposureMinNs &&
					      facts_.isoMax >= facts_.isoMin;
		}
		ALOGI("%s: manual sensor %d: exposure %lld-%lld ns, ISO %d-%d, frame up to %lld ns",
		      name_.c_str(), facts_.manualSensor, (long long)facts_.exposureMinNs,
		      (long long)facts_.exposureMaxNs, facts_.isoMin, facts_.isoMax,
		      (long long)facts_.maxFrameDurationNs);
		cam_->release();
	};

	if (!hwIsp) {
		if (probe->validate() != libcamera::CameraConfiguration::Invalid &&
		    !cam_->configure(probe.get())) {
			probe3aControls();
			accumulateLimits(limits);
		}
		finish();
		return;
	}

	std::map<std::pair<unsigned int, unsigned int>, int64_t> byFrame;
	for (const auto &s : facts_.outputSizes) {
		const libcamera::Size frame = Session::hwIspFrameSize(facts_, s.first, s.second);
		const auto key = std::make_pair(frame.width, frame.height);
		auto it = byFrame.find(key);
		if (it == byFrame.end()) {
			int64_t ns = facts_.minFrameDurationNs;
			auto cfg = cam_->generateConfiguration({ libcamera::StreamRole::Viewfinder });
			if (cfg && !cfg->empty()) {
				cfg->at(0).size = frame;
				cfg->at(0).pixelFormat = libcamera::formats::NV12;
				cfg->at(0).bufferCount = 4;
				if (cfg->validate() != libcamera::CameraConfiguration::Invalid &&
				    !cam_->configure(cfg.get())) {
					accumulateLimits(limits);
					const auto &ctrls = cam_->controls();
					auto ci = ctrls.find(&libcamera::controls::FrameDurationLimits);
					if (ci != ctrls.end()) {
						/*
						 * 单位微秒。最高按 60 fps；离 60 fps 不超过 1%
						 * 就算 60 fps，传感器的标称也是这么算的（ov13b10
						 * 合并模式最短帧 16.78 ms）。
						 */
						const int64_t us = ci->second.min().get<int64_t>();
						ns = us * 1000 <= kMinFrameDuration60Ns * 101 / 100
							     ? kMinFrameDuration60Ns
							     : std::min(us * 1000, facts_.minFrameDurationNs);
					}
				}
			}
			it = byFrame.emplace(key, ns).first;
			ALOGI("%s: frame %ux%u: shortest frame %.2f ms", name_.c_str(), frame.width,
			      frame.height, ns / 1e6);
		}
		facts_.minFrameDurationsNs.push_back(it->second);
	}
	probe3aControls();
	finish();
}

void Device::accumulateLimits(ManualLimits &l)
{
	const auto &ctrls = cam_->controls();
	auto e = ctrls.find(&libcamera::controls::ExposureTime);
	auto g = ctrls.find(&libcamera::controls::AnalogueGain);
	auto f = ctrls.find(&libcamera::controls::FrameDurationLimits);
	if (e == ctrls.end() || g == ctrls.end() || f == ctrls.end())
		return;
	l.expMinUs = std::max<int64_t>(l.expMinUs, e->second.min().get<int32_t>());
	l.expMaxUs = std::min<int64_t>(l.expMaxUs, e->second.max().get<int32_t>());
	l.gainMin = std::max<double>(l.gainMin, g->second.min().get<float>());
	l.gainMax = std::min<double>(l.gainMax, g->second.max().get<float>());
	l.frameMaxUs = std::min(l.frameMaxUs, f->second.max().get<int64_t>());
	l.seen = true;
}

/* 相机已配置：看 libcamera 接受哪些 3A 控制 */
void Device::probe3aControls()
{
	const auto &ctrls = cam_->controls();
	auto takes = [&](const libcamera::ControlId &id, int32_t value) {
		auto it = ctrls.find(&id);
		if (it == ctrls.end())
			return false;
		for (const auto &v : it->second.values())
			if (v.get<int32_t>() == value)
				return true;
		return false;
	};

	facts_.evCompensation = ctrls.count(&libcamera::controls::ExposureValue);
	facts_.aeLock = takes(libcamera::controls::ExposureTimeMode,
			      libcamera::controls::ExposureTimeModeManual) &&
			takes(libcamera::controls::AnalogueGainMode,
			      libcamera::controls::AnalogueGainModeManual);
	facts_.awbLock = ctrls.count(&libcamera::controls::AwbEnable) &&
			 ctrls.count(&libcamera::controls::ColourGains);
	facts_.awbPresets = ctrls.count(&libcamera::controls::AwbEnable) &&
			    ctrls.count(&libcamera::controls::ColourTemperature);
	facts_.antibanding = takes(libcamera::controls::AeFlickerMode,
				   libcamera::controls::FlickerManual) &&
			     ctrls.count(&libcamera::controls::AeFlickerPeriod);
	ALOGI("%s: 3A controls: exposure compensation %d, AE lock %d, AWB lock %d, AWB presets %d, "
	      "antibanding %d",
	      name_.c_str(), facts_.evCompensation, facts_.aeLock, facts_.awbLock,
	      facts_.awbPresets, facts_.antibanding);
}

ndk::ScopedAStatus Device::getCameraCharacteristics(CameraMetadata *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	out->metadata = characteristics_;
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::getPhysicalCameraCharacteristics(const std::string &,
							    CameraMetadata *)
{
	/* 不是逻辑多摄，没有物理子相机。 */
	return err(Status::ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus Device::getResourceCost(CameraResourceCost *out)
{
	out->resourceCost = 100;     /* 软件 ISP 吃 CPU，如实报满 */
	out->conflictingDevices.clear();
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::isStreamCombinationSupported(const StreamConfiguration &cfg,
							bool *out)
{
	/* 与 Session::configureStreams 同一个判据（Metadata.h），也与静态元数据的
	 * MAX_NUM_OUTPUT_STREAMS 一致。⚠️ PR #6 初版把这里从 <=1 悄悄改成 <=2，
	 * 既不看格式也不看路数构成，与声明的 {0,2,1} 互相矛盾。 */
	std::string why;
	*out = streamCombinationSupported(cfg, facts_, &why);
	if (!*out)
		ALOGI("isStreamCombinationSupported: 否（%s）", why.c_str());
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::open(const std::shared_ptr<ICameraDeviceCallback> &cb,
				std::shared_ptr<ICameraDeviceSession> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	if (!cb)
		return err(Status::ILLEGAL_ARGUMENT);

	auto session = ndk::SharedRefBase::make<Session>(cam_, facts_, cb);
	if (!session->init()) {
		ALOGE("Session::init 失败");
		return err(Status::INTERNAL_ERROR);
	}
	/* 相机被占用期间手电筒不可用（框架的约定）；灯由会话按请求控制。 */
	inUse_ = true;
	if (!facts_.flashLed.empty()) {
		setLed(false);
		provider_->notifyTorch(name_, TorchModeStatus::NOT_AVAILABLE);
	}
	session->setOnClosed([this] { onSessionClosed(); });
	*out = session;
	ALOGI("%s 已打开", name_.c_str());
	return ndk::ScopedAStatus::ok();
}

void Device::onSessionClosed()
{
	std::lock_guard<std::mutex> lk(mutex_);
	inUse_ = false;
	if (!facts_.flashLed.empty()) {
		setLed(false);
		provider_->notifyTorch(name_, TorchModeStatus::AVAILABLE_OFF);
	}
}

bool Device::setLed(bool on)
{
	if (facts_.flashLed.empty())
		return false;
	std::string max = readSysfs(facts_.flashLed + "/max_brightness");
	if (max.empty())
		max = "255";
	return writeSysfs(facts_.flashLed + "/brightness", on ? max : "0");
}

ndk::ScopedAStatus Device::openInjectionSession(
	const std::shared_ptr<ICameraDeviceCallback> &,
	std::shared_ptr<ICameraInjectionSession> *out)
{
	*out = nullptr;
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::setTorchMode(bool on)
{
	/*
	 * 手电筒（快捷设置的那块砖、以及 CameraManager.setTorchMode）。
	 * 契约（ICameraDevice.aidl）：没有闪光灯 → OPERATION_NOT_SUPPORTED；
	 * 相机正被会话占用 → CAMERA_IN_USE；成功后必须经 provider 回调报状态。
	 */
	std::lock_guard<std::mutex> lk(mutex_);
	if (facts_.flashLed.empty())
		return err(Status::OPERATION_NOT_SUPPORTED);
	if (inUse_)
		return err(Status::CAMERA_IN_USE);
	if (!setLed(on))
		return err(Status::INTERNAL_ERROR);
	provider_->notifyTorch(name_, on ? TorchModeStatus::AVAILABLE_ON
					 : TorchModeStatus::AVAILABLE_OFF);
	ALOGI("%s 手电筒 %s", name_.c_str(), on ? "开" : "关");
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::turnOnTorchWithStrengthLevel(int32_t)
{
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::getTorchStrengthLevel(int32_t *)
{
	return err(Status::OPERATION_NOT_SUPPORTED);
}

ndk::ScopedAStatus Device::constructDefaultRequestSettings(RequestTemplate type,
							   CameraMetadata *out)
{
	if (!templateSupported(static_cast<int>(type), facts_))
		return err(Status::ILLEGAL_ARGUMENT);
	out->metadata = buildDefaultRequest(static_cast<int>(type), facts_);
	if (out->metadata.empty())
		return err(Status::INTERNAL_ERROR);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Device::isStreamCombinationWithSettingsSupported(
	const StreamConfiguration &cfg, bool *out)
{
	return isStreamCombinationSupported(cfg, out);
}

ndk::ScopedAStatus Device::getSessionCharacteristics(const StreamConfiguration &,
						     CameraMetadata *out)
{
	/* 会话特性与静态特性一致（我们没有随配置变化的能力）。 */
	std::lock_guard<std::mutex> lk(mutex_);
	out->metadata = characteristics_;
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */
