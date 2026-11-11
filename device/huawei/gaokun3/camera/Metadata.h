/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— CameraCharacteristics 构造。
 *
 * ★ 这一层之所以便宜，是因为 AIDL 的 CameraMetadata 就是
 *   camera_metadata_t 的序列化 blob（CameraMetadata.aidl 原文：
 *   "Access by casting to a camera_metadata*"），所以我们直接用
 *   AOSP 的 libcamera_metadata 建表，再把字节丢进 AIDL。
 */
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include <aidl/android/hardware/camera/device/StreamConfiguration.h>
#include <system/camera_metadata.h>

namespace gaokun3 {

/* 60 fps */
constexpr int64_t kMinFrameDuration60Ns = 16666667;

/*
 * 数码变焦：裁剪区域（SCALER_CROP_REGION）每个方向最小到视场的四分之一。
 * 用 CONTROL_ZOOM_RATIO 的应用由相机服务把倍率换算成裁剪区域。
 */
constexpr int32_t kMaxDigitalZoom = 4;

/* 曝光补偿每档的步数（ANDROID_CONTROL_AE_COMPENSATION_STEP） */
constexpr int32_t kEvStepsPerStop = 3;
/*
 * 镜头参数，取自华为 Windows 相机拍的照片的 EXIF。特性、每帧结果和 JPEG 的 EXIF 用同一份，
 * 应用和 CTS 会核对它们是否一致。
 */
struct LensParams {
	float focalLengthMm;
	float fNumber;
};
constexpr LensParams kLensBack = { 4.0f, 1.8f };     /* ov13b10 */
constexpr LensParams kLensFront = { 3.0f, 2.0f };    /* hi846 */
constexpr const LensParams &lensParams(bool frontFacing)
{
	return frontFacing ? kLensFront : kLensBack;
}
/* JPEG 缩略图尺寸（宽、高成对，0x0 = 不要缩略图） */
constexpr int32_t kThumbnailSizes[] = { 0, 0, 160, 90, 160, 120 };

/*
 * AWB 预设模式的色温（K）；AUTO 和没有色温的模式为 0。libcamera 按调优里的
 * 曲线换算成增益。
 */
constexpr int32_t awbPresetKelvin(int32_t awbMode)
{
	switch (awbMode) {
	case ANDROID_CONTROL_AWB_MODE_INCANDESCENT:
		return 2856;    /* CIE 标准光源 A */
	case ANDROID_CONTROL_AWB_MODE_WARM_FLUORESCENT:
		return 3000;
	case ANDROID_CONTROL_AWB_MODE_FLUORESCENT:
		return 4000;
	case ANDROID_CONTROL_AWB_MODE_DAYLIGHT:
		return 5500;
	case ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT:
		return 6500;
	case ANDROID_CONTROL_AWB_MODE_SHADE:
		return 7500;
	default:
		return 0;
	}
}

/* 从 libcamera 那边拿到的、构造 characteristics 需要的硬件事实。 */
struct SensorFacts {
	int32_t pixelArrayW = 0;
	int32_t pixelArrayH = 0;
	int32_t activeW = 0;
	int32_t activeH = 0;
	/*
	 * The field of view all outputs share: a centred part of the libcamera
	 * active area above (Device::init()). Android sees only this, as pixel,
	 * pre-correction and active array at (0,0); fovX/fovY place it in the
	 * libcamera frame.
	 */
	int32_t fovX = 0;
	int32_t fovY = 0;
	int32_t fovW = 0;
	int32_t fovH = 0;
	/* Pixel size in nm (libcamera properties::UnitCellSize); 0 = not reported. */
	int32_t unitCellW = 0;
	int32_t unitCellH = 0;
	int32_t orientation = 0;        /* 0/90/180/270 */
	bool    frontFacing = true;
	/* 闪光灯的 LED class 目录（如 /sys/class/leds/white:flash）；空 = 这个相机没有闪光灯。
	 * ★ #110：后摄的 LED 挂在 PM8350C 闪光模块 1+4 路，内核侧只暴露成一个 LED。 */
	std::string flashLed;
	/* ── 对焦马达 ──
	 * ★ 只有后摄有（i2c 1-000c 的 dw9714，内核把它暴露成独立的 v4l2 子设备）。
	 *   Device::init() 里按名字探测：找不到就保持 false，对焦相关的 tag 一律
	 *   退回"定焦"（AF_AVAILABLE_MODES={OFF}、MINIMUM_FOCUS_DISTANCE=0）。
	 *   ⚠️ 这两条必须【成对】一致：只把 MODES 放开而 MINIMUM_FOCUS_DISTANCE 还是 0，
	 *      框架会按"定焦但能设 AF 模式"理解，CameraX 会一直等一个不会到来的
	 *      FOCUSED_LOCKED。 */
	bool hasAf = false;
	/* 最近对焦距离（屈光度 = 1/m）。硬件手册没有可信值，按 10 diopter(10 cm) 声明，
	 * 并把 FOCUS_DISTANCE_CALIBRATION 报成 UNCALIBRATED —— 不谎称已标定。 */
	float minFocusDiopters = 10.0f;
	int64_t minFrameDurationNs = 33333333;
	/* 我们打算对外声明的输出尺寸（软件 ISP 产出，见 Provider.cpp 的说明）。 */
	std::vector<std::pair<int32_t, int32_t>> outputSizes;
	/* 每种输出尺寸各自的最短帧长；没有时都按 minFrameDurationNs。 */
	std::vector<int64_t> minFrameDurationsNs;
	/*
	 * libcamera 对这个相机接受的 3A 控制（Device::probeCamera()）：曝光补偿
	 * （ExposureValue）、AE 锁定（曝光时间与增益的手动模式）、AWB 锁定
	 * （AwbEnable 与 ColourGains）。
	 */
	bool evCompensation = false;
	bool aeLock = false;
	bool awbLock = false;
	/* AWB 预设：按色温的手动白平衡 */
	bool awbPresets = false;
	/*
	 * 手动曝光（MANUAL_SENSOR）：每种配置都接受的曝光时间、增益与帧长范围
	 * （Device::probeCamera()）。
	 */
	bool manualSensor = false;
	int64_t exposureMinNs = 0, exposureMaxNs = 0;
	int32_t isoMin = 0, isoMax = 0;
	int64_t maxFrameDurationNs = 1000000000;
};

/*
 * 建一份 static characteristics。
 * 返回的 vector 就是可以直接塞进 aidl CameraMetadata.metadata 的字节。
 */
std::vector<uint8_t> buildCharacteristics(const SensorFacts &f);

/*
 * 构造一帧的【结果】元数据。
 * ⚠️ 不能只把请求设置原样回传：框架要求结果里带上 SENSOR_TIMESTAMP 与几个
 *    3A 状态，缺了会让 CameraCaptureSession 判定这帧无效。
 */
struct FrameResultFacts {
	uint8_t flashState = ANDROID_FLASH_STATE_UNAVAILABLE;
	uint8_t aeState = ANDROID_CONTROL_AE_STATE_CONVERGED;
	uint8_t awbState = ANDROID_CONTROL_AWB_STATE_CONVERGED;
	/* 请求里可能不带这两项（沿用上一帧），结果里要按会话的粘滞值回显。 */
	uint8_t aeMode = ANDROID_CONTROL_AE_MODE_ON;
	uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
	/* libcamera 报回来的实际曝光/增益（软件 ISP 的 libipa Agc 每帧填 ExposureTime/AnalogueGain）。
	 * 0 = 这一帧没有。ISO 按 100 × 模拟增益折算（软件 ISP 没有别的定义）。 */
	int64_t exposureNs = 0;
	int32_t sensitivity = 0;
	int64_t frameDurationNs = 0;    /* 0 = 没报 */
	/* ── 对焦（#AF）：定焦相机恒为 INACTIVE / STATIONARY / 0。 ── */
	uint8_t afState = ANDROID_CONTROL_AF_STATE_INACTIVE;
	uint8_t lensState = ANDROID_LENS_STATE_STATIONARY;
	float focusDistance = 0.0f;     /* 屈光度；UNCALIBRATED ⇒ 只保证单调 */
	int32_t cropRegion[4] = {};     /* 实际用的裁剪区域 (x, y, w, h)；w 为 0 = 没设 */
	bool hasAf = false;             /* false 时不写 AF 相关动态条目（保持定焦语义） */
	bool manualSensor = false;      /* 声明了 MANUAL_SENSOR：结果里要有 BLACK_LEVEL_LOCK */
	LensParams lens = kLensBack;    /* 这个相机的镜头参数（lensParams()） */
	/* 这一帧实际的白平衡增益（R、G偶、G奇、B）与色彩矩阵（行优先），libcamera 报了才有 */
	bool hasCcGains = false;
	std::array<float, 4> ccGains{};
	bool hasCcTransform = false;
	std::array<float, 9> ccTransform{};
};
std::vector<uint8_t> buildResult(const std::vector<uint8_t> &requestSettings,
				 int64_t timestampNs, uint8_t pipelineDepth,
				 const FrameResultFacts &fr);

/*
 * 复制一份请求设置，把一次性的触发键（AF_TRIGGER / AE_PRECAPTURE_TRIGGER）改成 IDLE。
 * ★ 用途：请求不带设置 = "与上一帧相同"（CaptureRequest.aidl），HAL 要沿用上一帧的设置
 *   （JPEG 方向/质量、结果回显……），但【触发】只属于它出现的那一帧 —— 原样沿用会让
 *   AF_TRIGGER=START 在之后每一帧重复生效。
 */
std::vector<uint8_t> stripTriggers(const std::vector<uint8_t> &settings);

/* RequestTemplate → 默认请求设置。 */
std::vector<uint8_t> buildDefaultRequest(int templateId, const SensorFacts &f);

/*
 * Whether the HAL serves this template. ZERO_SHUTTER_LAG and MANUAL need the
 * PRIVATE/YUV_REPROCESSING and MANUAL_SENSOR capabilities, which it doesn't
 * declare, so those must be refused with ILLEGAL_ARGUMENT.
 */
bool templateSupported(int templateId, const SensorFacts &f);

/*
 * 从【请求设置】里读一个整型条目的第 index 个值，读不到就给 def。
 *
 * ★ 必要性：JPEG 的旋转角与质量是【应用】在每一帧的请求里给的
 *   （ANDROID_JPEG_ORIENTATION / ANDROID_JPEG_QUALITY），HAL 必须自己取。
 *   类型不匹配时返回 def 而不是硬转 —— ANDROID_JPEG_QUALITY 是 byte、
 *   ANDROID_JPEG_ORIENTATION 是 int32，读错类型会读出垃圾值。
 */
int32_t requestEntryInt(const std::vector<uint8_t> &requestSettings,
			uint32_t tag, int32_t def, size_t index = 0);
/* 同上，int64（曝光时间、帧长）与浮点的条目。 */
int64_t requestEntryInt64(const std::vector<uint8_t> &requestSettings, uint32_t tag, int64_t def);
float requestEntryFloat(const std::vector<uint8_t> &requestSettings, uint32_t tag, float def,
			size_t index = 0);

/*
 * 这套 HAL 能不能交付这组流？Device::isStreamCombinationSupported 与
 * Session::configureStreams 用的是【同一个】判据，与静态元数据一致：
 *   MAX_NUM_OUTPUT_STREAMS = {0 RAW, 2 非停顿(YUV/IMPL_DEFINED), 1 停顿(BLOB/JPEG)}，
 *   格式只限 kFormats 那三种（软件 ISP 出 RGB，由我们转 YUV / 编 JPEG，别的格式交付不了），
 *   只接受输出流、不旋转、尺寸不超过有效阵列。
 * why 不为空时写入拒绝理由（给日志）。
 */
bool streamCombinationSupported(
	const aidl::android::hardware::camera::device::StreamConfiguration &cfg,
	const SensorFacts &f, std::string *why);

/* 小工具：把 camera_metadata_t 打包成字节。 */
std::vector<uint8_t> pack(const camera_metadata_t *m);

} /* namespace gaokun3 */
