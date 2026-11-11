/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gaokun3 相机 HAL —— CameraCharacteristics 构造。
 *
 * ⚠️ 这里每一个"必填"的 tag 都不是凭记忆列的：清单来自 AOSP 自己的
 *    CameraCharacteristics 文档与 libcamera 的 src/android/camera_capabilities.cpp
 *    （它为 camera3 做的是同一件事）。缺 tag 的后果不是报错，而是
 *    **相机被框架静默丢弃** —— 所以宁可多填。
 */
#include "Metadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include <aidl/android/hardware/camera/device/RequestTemplate.h>
#include <log/log.h>
#include <system/graphics.h>

using ::aidl::android::hardware::camera::device::RequestTemplate;
using ::aidl::android::hardware::camera::device::StreamConfiguration;
using ::aidl::android::hardware::camera::device::StreamRotation;
using ::aidl::android::hardware::camera::device::StreamType;
using ::aidl::android::hardware::graphics::common::PixelFormat;

namespace gaokun3 {

std::vector<uint8_t> pack(const camera_metadata_t *m)
{
	size_t n = get_camera_metadata_compact_size(m);
	std::vector<uint8_t> out(n);
	camera_metadata_t *dst = copy_camera_metadata(out.data(), n, m);
	if (!dst) {
		ALOGE("pack: copy_camera_metadata 失败 (size=%zu)", n);
		out.clear();
	}
	return out;
}

namespace {

const camera_metadata_ro_entry_t *findEntry(const std::vector<uint8_t> &requestSettings,
					    uint32_t tag, uint8_t type,
					    camera_metadata_ro_entry_t *e)
{
	if (requestSettings.empty())
		return nullptr;
	const camera_metadata_t *m =
		reinterpret_cast<const camera_metadata_t *>(requestSettings.data());
	if (find_camera_metadata_ro_entry(m, tag, e) != 0 || !e->count || e->type != type)
		return nullptr;
	return e;
}

} /* namespace */

double requestEntryDouble(const std::vector<uint8_t> &requestSettings, uint32_t tag, size_t index)
{
	camera_metadata_ro_entry_t e;
	return findEntry(requestSettings, tag, TYPE_DOUBLE, &e) && e.count > index ? e.data.d[index]
										  : NAN;
}

std::string requestEntryBytes(const std::vector<uint8_t> &requestSettings, uint32_t tag)
{
	camera_metadata_ro_entry_t e;
	if (!findEntry(requestSettings, tag, TYPE_BYTE, &e))
		return {};
	std::string s(reinterpret_cast<const char *>(e.data.u8), e.count);
	s.erase(std::find(s.begin(), s.end(), '\0'), s.end());
	return s;
}

int64_t requestEntryInt64(const std::vector<uint8_t> &requestSettings, uint32_t tag, int64_t def)
{
	camera_metadata_ro_entry_t e;
	return findEntry(requestSettings, tag, TYPE_INT64, &e) ? e.data.i64[0] : def;
}

float requestEntryFloat(const std::vector<uint8_t> &requestSettings, uint32_t tag, float def,
			size_t index)
{
	camera_metadata_ro_entry_t e;
	return findEntry(requestSettings, tag, TYPE_FLOAT, &e) && e.count > index ? e.data.f[index]
										   : def;
}

int32_t requestEntryInt(const std::vector<uint8_t> &requestSettings,
			uint32_t tag, int32_t def, size_t index)
{
	if (requestSettings.empty())
		return def;

	/* ★ CameraMetadata.aidl 原文：这就是 camera_metadata_t 的序列化 blob，
	 *   可以就地当成 camera_metadata* 用（与 buildResult() 同一手法）。 */
	const camera_metadata_t *m =
		reinterpret_cast<const camera_metadata_t *>(requestSettings.data());
	camera_metadata_ro_entry_t e;
	if (find_camera_metadata_ro_entry(m, tag, &e) != 0)
		return def;

	if (e.count <= index)
		return def;
	if (e.type == TYPE_INT32)
		return e.data.i32[index];
	if (e.type == TYPE_BYTE)
		return e.data.u8[index];

	ALOGW("请求里的 tag %u 类型是 %d，不是整数类型 —— 用默认值 %d",
	      tag, static_cast<int>(e.type), def);
	return def;
}

namespace {

/* 我们对外声明支持的像素格式。
 *
 * ★ 软件 ISP 只出 RGB 族（libcamera debayer_cpu.cpp:436-441），
 *   Android 要的是 YUV_420_888 / JPEG / IMPLEMENTATION_DEFINED，
 *   所以这一层的色彩转换由本 HAL 用 libyuv 做，JPEG 用 libjpeg 做。
 *   ⇒ 这里声明的是【我们能交付的】格式，不是 libcamera 直接产出的格式。
 */
constexpr int32_t kFormats[] = {
	HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED,
	HAL_PIXEL_FORMAT_YCBCR_420_888,
	HAL_PIXEL_FORMAT_BLOB,          /* = JPEG */
};

void addStreamConfigs(camera_metadata_t **m, const SensorFacts &f)
{
	std::vector<int32_t> cfgs;
	for (int32_t fmt : kFormats) {
		for (const auto &s : f.outputSizes) {
			cfgs.push_back(fmt);
			cfgs.push_back(s.first);
			cfgs.push_back(s.second);
			cfgs.push_back(ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT);
		}
	}
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,
				  cfgs.data(), cfgs.size());

	std::vector<int64_t> dur, st;
	for (int32_t fmt : kFormats) {
		for (const auto &s : f.outputSizes) {
			const size_t i = &s - f.outputSizes.data();
			dur.push_back(fmt);
			dur.push_back(s.first);
			dur.push_back(s.second);
			dur.push_back(i < f.minFrameDurationsNs.size() ? f.minFrameDurationsNs[i]
								       : f.minFrameDurationNs);

			st.push_back(fmt);
			st.push_back(s.first);
			st.push_back(s.second);
			/* ★ 只有 BLOB(JPEG) 有 stall —— 软件 JPEG 编码是阻塞的。
			 *   其余格式必须是 0，否则框架会以为每帧都要停顿。 */
			st.push_back(fmt == HAL_PIXEL_FORMAT_BLOB ? 200000000LL : 0LL);
		}
	}
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_MIN_FRAME_DURATIONS,
				  dur.data(), dur.size());
	add_camera_metadata_entry(*m, ANDROID_SCALER_AVAILABLE_STALL_DURATIONS,
				  st.data(), st.size());
}

} /* namespace */

std::vector<uint8_t> buildCharacteristics(const SensorFacts &f)
{
	/* 条目数与数据量给足，add_ 失败是静默的，宁可浪费几 KB。 */
	camera_metadata_t *m = allocate_camera_metadata(128, 16384);
	if (!m) {
		ALOGE("allocate_camera_metadata 失败");
		return {};
	}

	/* ── 传感器几何 ── */
	/*
	 * The outputs' shared field of view (SensorFacts::fov*) is all Android
	 * sees of the sensor: pixel array, pre-correction and active array alike,
	 * at (0,0). Without distortion correction the two arrays must be equal,
	 * and CameraX uses the active array's left edge as an offset for its
	 * metering regions. The HAL maps back into the libcamera frame itself.
	 */
	const int32_t activeArray[] = { 0, 0, f.fovW, f.fovH };
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_ACTIVE_ARRAY_SIZE,
				  activeArray, 4);
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_PRE_CORRECTION_ACTIVE_ARRAY_SIZE,
				  activeArray, 4);
	const int32_t pixelArray[] = { f.fovW, f.fovH };
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_PIXEL_ARRAY_SIZE,
				  pixelArray, 2);
	/* The declared pixel array times the pixel size, in mm (see Device::init()). */
	if (f.unitCellW > 0 && f.unitCellH > 0) {
		const float physicalSize[] = { f.fovW * (f.unitCellW / 1e6f),
					       f.fovH * (f.unitCellH / 1e6f) };
		add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_PHYSICAL_SIZE,
					  physicalSize, 2);
	}
	const int32_t orientation = f.orientation;
	add_camera_metadata_entry(m, ANDROID_SENSOR_ORIENTATION, &orientation, 1);

	const uint8_t facing = f.frontFacing ? ANDROID_LENS_FACING_FRONT
					     : ANDROID_LENS_FACING_BACK;
	add_camera_metadata_entry(m, ANDROID_LENS_FACING, &facing, 1);

	/* [raw, processed(非stall), processedStall] —— 我们不提供 RAW。
	 * ⚠️ 不用复合字面量：那是 GNU 扩展，-Werror 下会被拦。 */
	const int32_t maxOutStreams[] = { 0, 2, 1 };
	add_camera_metadata_entry(m, ANDROID_REQUEST_MAX_NUM_OUTPUT_STREAMS,
				  maxOutStreams, 3);

	/* ── 流配置 ── */
	addStreamConfigs(&m, f);

	/* 每种配置下 libcamera 都允许的最长帧（探测过时） */
	const int64_t maxFrameDuration = f.manualSensor ? f.maxFrameDurationNs : 1000000000LL;
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_MAX_FRAME_DURATION,
				  &maxFrameDuration, 1);
	if (f.manualSensor) {
		const int64_t expRange[] = { f.exposureMinNs, f.exposureMaxNs };
		add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_EXPOSURE_TIME_RANGE, expRange, 2);
		const int32_t isoRange[] = { f.isoMin, f.isoMax };
		add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_SENSITIVITY_RANGE, isoRange, 2);
		/* 全是模拟增益 */
		add_camera_metadata_entry(m, ANDROID_SENSOR_MAX_ANALOG_SENSITIVITY, &f.isoMax, 1);
	}

	/* ── 能力声明：只做 BACKWARD_COMPATIBLE，硬件等级 LIMITED ──
	 * ⚠️ 不要声明 LEGACY：LEGACY 走的是框架里那条兼容路径，对 HAL 的
	 *    行为假设更多更旧；LIMITED 才是"实现了 camera2 基本面"的正确说法。 */
	/* MANUAL_SENSOR 总是带着 READ_SENSOR_SETTINGS（能力的定义）：结果里报实际的传感器与镜头设置 */
	const uint8_t caps[] = { ANDROID_REQUEST_AVAILABLE_CAPABILITIES_BACKWARD_COMPATIBLE,
				 ANDROID_REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR,
				 ANDROID_REQUEST_AVAILABLE_CAPABILITIES_READ_SENSOR_SETTINGS };
	add_camera_metadata_entry(m, ANDROID_REQUEST_AVAILABLE_CAPABILITIES, caps,
				  f.manualSensor ? 3 : 1);
	const uint8_t level = ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED;
	add_camera_metadata_entry(m, ANDROID_INFO_SUPPORTED_HARDWARE_LEVEL, &level, 1);

	const int32_t partialCount = 1;
	add_camera_metadata_entry(m, ANDROID_REQUEST_PARTIAL_RESULT_COUNT, &partialCount, 1);
	const uint8_t pipelineDepth = 4;
	add_camera_metadata_entry(m, ANDROID_REQUEST_PIPELINE_MAX_DEPTH, &pipelineDepth, 1);

	/* ── 3A：软件 ISP 只有 AE/AWB；AF 是【马达有才有】（后摄 dw9714，见 Lens.h） ──
	 * ⚠️ AF 的两处声明必须同时成立，否则框架会陷入"等一个永不到来的对焦结果"：
	 *    ① AF_AVAILABLE_MODES 里除了 OFF 还有别的值；
	 *    ② LENS_INFO_MINIMUM_FOCUS_DISTANCE > 0。
	 *    只做①不做② → 框架认为这是"定焦镜头"（min=0），但也可能把 AF 模式发下来，
	 *    我们若没有马达就会一直报 INACTIVE；只做②不做① → 应用永远不会去设 AF 模式。
	 *    CameraX（Aperture 用的就是它）判"能不能对焦"看的就是 min focus distance。 */
	/* 有闪光灯的相机（后摄，#110）才声明两个闪光 AE 模式：框架规定 FLASH_INFO_AVAILABLE=true
	 * 时 AE 模式必须含 ON_AUTO_FLASH / ON_ALWAYS_FLASH。 */
	const bool hasFlash = !f.flashLed.empty();
	std::vector<uint8_t> aeModes = { ANDROID_CONTROL_AE_MODE_ON };
	if (hasFlash)
		aeModes.insert(aeModes.end(), { ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH,
						ANDROID_CONTROL_AE_MODE_ON_ALWAYS_FLASH });
	if (f.manualSensor)
		aeModes.push_back(ANDROID_CONTROL_AE_MODE_OFF);
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_MODES, aeModes.data(),
				  aeModes.size());
	std::vector<uint8_t> awbModes = { ANDROID_CONTROL_AWB_MODE_AUTO };
	if (f.awbPresets)
		awbModes.insert(awbModes.end(), { ANDROID_CONTROL_AWB_MODE_INCANDESCENT,
						  ANDROID_CONTROL_AWB_MODE_WARM_FLUORESCENT,
						  ANDROID_CONTROL_AWB_MODE_FLUORESCENT,
						  ANDROID_CONTROL_AWB_MODE_DAYLIGHT,
						  ANDROID_CONTROL_AWB_MODE_CLOUDY_DAYLIGHT,
						  ANDROID_CONTROL_AWB_MODE_SHADE });
	/*
	 * OFF：用应用给的白平衡增益（COLOR_CORRECTION_GAINS，见
	 * Session::apply3aControls()）。色彩矩阵仍取调优里的、按关掉 AWB 时的
	 * 色温插值；不应用 COLOR_CORRECTION_TRANSFORM。
	 */
	if (f.awbLock)
		awbModes.push_back(ANDROID_CONTROL_AWB_MODE_OFF);
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_AVAILABLE_MODES, awbModes.data(),
				  awbModes.size());
	/* 只声明我们真实现的四种：OFF / AUTO（单次）/ CONTINUOUS_PICTURE / CONTINUOUS_VIDEO。
	 * ⚠️ 不声明 MACRO 和 EDOF：MACRO 在 AOSP 里是"近距单次对焦"，我们的 CDAF
	 *    不区分行程区间，声明了就得额外实现一套近距优先逻辑，容易出假报。 */
	const uint8_t afModesOn[] = { ANDROID_CONTROL_AF_MODE_OFF,
				      ANDROID_CONTROL_AF_MODE_AUTO,
				      ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE,
				      ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO };
	const uint8_t afModesOff[] = { ANDROID_CONTROL_AF_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_AVAILABLE_MODES,
				  f.hasAf ? afModesOn : afModesOff, f.hasAf ? 4 : 1);
	/* OFF 配合手动曝光：AE、AF 关，AWB 保持（Session::apply3aControls()） */
	const uint8_t controlModes[] = { ANDROID_CONTROL_MODE_AUTO, ANDROID_CONTROL_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_MODES, controlModes,
				  f.manualSensor ? 2 : 1);
	const uint8_t sceneModes[] = { ANDROID_CONTROL_SCENE_MODE_DISABLED };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_SCENE_MODES, sceneModes, 1);
	const uint8_t effects[] = { ANDROID_CONTROL_EFFECT_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_EFFECTS, effects, 1);
	/* 50HZ/60HZ/AUTO：libcamera 的 AE 把曝光取整到闪烁周期（Session::apply3aControls()） */
	const uint8_t antibanding[] = { ANDROID_CONTROL_AE_ANTIBANDING_MODE_OFF,
					ANDROID_CONTROL_AE_ANTIBANDING_MODE_50HZ,
					ANDROID_CONTROL_AE_ANTIBANDING_MODE_60HZ,
					ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_ANTIBANDING_MODES,
				  antibanding, f.antibanding ? 4 : 1);
	const uint8_t vstabModes[] = { ANDROID_CONTROL_VIDEO_STABILIZATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES,
				  vstabModes, 1);
	/*
	 * 曝光补偿按 1/3 档，±2 EV（libcamera 接受 ±8；camera2 要求支持补偿时
	 * 至少 ±2）。
	 */
	const int32_t aeCompRange[] = { f.evCompensation ? -kEvStepsPerStop * 2 : 0,
					f.evCompensation ? kEvStepsPerStop * 2 : 0 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_COMPENSATION_RANGE, aeCompRange, 2);
	const camera_metadata_rational aeCompStep = { 1, f.evCompensation ? kEvStepsPerStop : 1 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_COMPENSATION_STEP, &aeCompStep, 1);
	const uint8_t aeLockAvail = f.aeLock ? ANDROID_CONTROL_AE_LOCK_AVAILABLE_TRUE
					     : ANDROID_CONTROL_AE_LOCK_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_LOCK_AVAILABLE, &aeLockAvail, 1);
	const uint8_t awbLockAvail = f.awbLock ? ANDROID_CONTROL_AWB_LOCK_AVAILABLE_TRUE
					       : ANDROID_CONTROL_AWB_LOCK_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_LOCK_AVAILABLE, &awbLockAvail, 1);

	/*
	 * HAL 把请求的帧率范围作为 FrameDurationLimits 交给 libcamera（见
	 * processCaptureRequest），由它据此设置传感器的帧长。HAL 要的模式最高
	 * 60 fps（ov13b10 合并模式）或 30 fps（全尺寸、hi846 合并模式），所以
	 * 30 fps 总能达到。只有某个尺寸能到 60 fps 时才声明 60 fps 的范围
	 * （Device::init()），具体哪些尺寸由最短帧长告诉应用。
	 */
	std::vector<int32_t> fpsRanges = { 15, 30, 30, 30 };
	const bool has60 = std::any_of(f.minFrameDurationsNs.begin(), f.minFrameDurationsNs.end(),
				       [](int64_t d) { return d <= kMinFrameDuration60Ns; });
	if (has60)
		fpsRanges.insert(fpsRanges.end(), { 30, 60, 60, 60 });
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES,
				  fpsRanges.data(), fpsRanges.size());

	/* AE/AWB 不支持区域；AF 在有马达的后摄上支持一个区域（AutoFocus::setRegion）。 */
	const int32_t maxRegions[] = { 0, 0, f.hasAf ? 1 : 0 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_MAX_REGIONS, maxRegions, 3);

	/* ── 闪光灯 / 镜头 ── */
	const uint8_t flashAvailable = hasFlash ? ANDROID_FLASH_INFO_AVAILABLE_TRUE
					       : ANDROID_FLASH_INFO_AVAILABLE_FALSE;
	add_camera_metadata_entry(m, ANDROID_FLASH_INFO_AVAILABLE, &flashAvailable, 1);
	/* Camera1 rebuilds the view angles from it and SENSOR_INFO_PHYSICAL_SIZE. */
	const LensParams &lens = lensParams(f.frontFacing);
	const float focalLengths[] = { lens.focalLengthMm };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_FOCAL_LENGTHS,
				  focalLengths, 1);
	const float apertures[] = { lens.fNumber };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_APERTURES, apertures, 1);
	const float filterDensities[] = { 0.0f };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_FILTER_DENSITIES,
				  filterDensities, 1);
	const uint8_t opticalStab[] = { ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION,
				  opticalStab, 1);
	const float hyperfocal = 0.0f;
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_HYPERFOCAL_DISTANCE, &hyperfocal, 1);
	const float minFocus = f.hasAf ? f.minFocusDiopters : 0.0f;   /* 0 = 定焦 */
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_MINIMUM_FOCUS_DISTANCE, &minFocus, 1);
	const uint8_t focusCalib = ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION_UNCALIBRATED;
	add_camera_metadata_entry(m, ANDROID_LENS_INFO_FOCUS_DISTANCE_CALIBRATION,
				  &focusCalib, 1);

	/* ── 缩放 / 裁剪 ── */
	const float maxZoom = static_cast<float>(kMaxDigitalZoom);
	add_camera_metadata_entry(m, ANDROID_SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, &maxZoom, 1);
	const uint8_t croppingType = ANDROID_SCALER_CROPPING_TYPE_CENTER_ONLY;
	add_camera_metadata_entry(m, ANDROID_SCALER_CROPPING_TYPE, &croppingType, 1);

	/* ── JPEG ── */
	/* 16:9 与 4:3 两种 JPEG 的缩略图，放在 EXIF 里（Exif.h） */
	add_camera_metadata_entry(m, ANDROID_JPEG_AVAILABLE_THUMBNAIL_SIZES, kThumbnailSizes,
				  std::size(kThumbnailSizes));
	const int32_t maxJpegSize = f.fovW * f.fovH * 3 / 2 + 65536;
	add_camera_metadata_entry(m, ANDROID_JPEG_MAX_SIZE, &maxJpegSize, 1);

	/* ── 统计 ── */
	const uint8_t faceModes[] = { ANDROID_STATISTICS_FACE_DETECT_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_FACE_DETECT_MODES,
				  faceModes, 1);
	const int32_t maxFaces = 0;
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_MAX_FACE_COUNT, &maxFaces, 1);
	const uint8_t hotPixelMapModes[] = { ANDROID_STATISTICS_HOT_PIXEL_MAP_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_HOT_PIXEL_MAP_MODES,
				  hotPixelMapModes, 1);
	const uint8_t lensShadingMapModes[] = { ANDROID_STATISTICS_LENS_SHADING_MAP_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_STATISTICS_INFO_AVAILABLE_LENS_SHADING_MAP_MODES,
				  lensShadingMapModes, 1);

	/* ── 时间戳 / 同步 ──
	 * ⚠️ UNKNOWN 而不是 REALTIME：libcamera 给的是 CLOCK_MONOTONIC，
	 *    而 REALTIME 在 Android 指的是 CLOCK_BOOTTIME（含休眠时间）。
	 *    声明错了会让框架把时间戳与其它传感器错误对齐。 */
	const uint8_t timestampSource = ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE_UNKNOWN;
	add_camera_metadata_entry(m, ANDROID_SENSOR_INFO_TIMESTAMP_SOURCE, &timestampSource, 1);
	const int32_t syncLatency = ANDROID_SYNC_MAX_LATENCY_UNKNOWN;
	add_camera_metadata_entry(m, ANDROID_SYNC_MAX_LATENCY, &syncLatency, 1);

	/* ── 其余必填 ── */
	const uint8_t noiseModes[] = { ANDROID_NOISE_REDUCTION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_NOISE_REDUCTION_AVAILABLE_NOISE_REDUCTION_MODES,
				  noiseModes, 1);
	/* 亮度锐化（Sharpen.h）：FAST 3×3，HIGH_QUALITY 5×5 */
	const uint8_t edgeModes[] = { ANDROID_EDGE_MODE_OFF, ANDROID_EDGE_MODE_FAST,
				      ANDROID_EDGE_MODE_HIGH_QUALITY };
	add_camera_metadata_entry(m, ANDROID_EDGE_AVAILABLE_EDGE_MODES, edgeModes, 3);
	const uint8_t abModes[] = { ANDROID_COLOR_CORRECTION_ABERRATION_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_COLOR_CORRECTION_AVAILABLE_ABERRATION_MODES,
				  abModes, 1);
	const uint8_t tonemapModes[] = { ANDROID_TONEMAP_MODE_FAST };
	add_camera_metadata_entry(m, ANDROID_TONEMAP_AVAILABLE_TONE_MAP_MODES, tonemapModes, 1);
	const int32_t maxCurvePoints = 2;
	add_camera_metadata_entry(m, ANDROID_TONEMAP_MAX_CURVE_POINTS, &maxCurvePoints, 1);
	const uint8_t testPatternModes[] = { ANDROID_SENSOR_TEST_PATTERN_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_SENSOR_AVAILABLE_TEST_PATTERN_MODES,
				  testPatternModes, 1);
	const uint8_t shadingModes[] = { ANDROID_SHADING_MODE_OFF };
	add_camera_metadata_entry(m, ANDROID_SHADING_AVAILABLE_MODES, shadingModes, 1);

	/*
	 * ── 键清单（LIMITED 必填；PR #6 文档第 7 章的建议 2）──
	 * 缺 AVAILABLE_CHARACTERISTICS_KEYS 时 cameraserver 每次枚举都报
	 * "addDynamicDepthTags: Supported camera characteristics is empty!"；应用侧的
	 * CaptureRequest/CaptureResult.getKeys() 与 CameraCharacteristics.getKeys() 也全靠这三张表。
	 *   请求键 = HAL 真正会读的（Session.cpp 的 parseFlashControls / AF / JPEG / 结果回显）；
	 *   结果键 = buildResult() 真正会写的，外加回显的请求键；
	 *   特性键 = 上面已经写进 m 的全部条目 —— 枚举生成，不手抄，免得两边漂移。
	 */
	std::vector<int32_t> reqKeys = {
		ANDROID_CONTROL_MODE,
		ANDROID_CONTROL_AE_MODE,
		ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER,
		ANDROID_CONTROL_AE_TARGET_FPS_RANGE,
		ANDROID_CONTROL_AWB_MODE,
		ANDROID_CONTROL_CAPTURE_INTENT,
		ANDROID_FLASH_MODE,
		ANDROID_JPEG_ORIENTATION,
		ANDROID_JPEG_QUALITY,
		ANDROID_JPEG_THUMBNAIL_SIZE,
		ANDROID_JPEG_THUMBNAIL_QUALITY,
		ANDROID_JPEG_GPS_COORDINATES,
		ANDROID_JPEG_GPS_PROCESSING_METHOD,
		ANDROID_JPEG_GPS_TIMESTAMP,
		ANDROID_SCALER_CROP_REGION,
		ANDROID_EDGE_MODE,
	};
	if (f.hasAf) {
		reqKeys.push_back(ANDROID_CONTROL_AF_MODE);
		reqKeys.push_back(ANDROID_CONTROL_AF_TRIGGER);
		reqKeys.push_back(ANDROID_CONTROL_AF_REGIONS);
	}
	if (f.evCompensation)
		reqKeys.push_back(ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION);
	if (f.antibanding)
		reqKeys.push_back(ANDROID_CONTROL_AE_ANTIBANDING_MODE);
	if (f.aeLock)
		reqKeys.push_back(ANDROID_CONTROL_AE_LOCK);
	if (f.awbLock) {
		reqKeys.push_back(ANDROID_CONTROL_AWB_LOCK);
		reqKeys.push_back(ANDROID_COLOR_CORRECTION_MODE);
		reqKeys.push_back(ANDROID_COLOR_CORRECTION_GAINS);
	}
	/* 手动对焦：AF_MODE_OFF 加对焦距离 */
	if (f.hasAf)
		reqKeys.push_back(ANDROID_LENS_FOCUS_DISTANCE);
	if (f.manualSensor) {
		for (int32_t k : { ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_SENSITIVITY,
				   ANDROID_SENSOR_FRAME_DURATION, ANDROID_BLACK_LEVEL_LOCK })
			reqKeys.push_back(k);
	}
	std::vector<int32_t> resKeys = reqKeys;
	for (int32_t k : { ANDROID_SENSOR_TIMESTAMP, ANDROID_REQUEST_PIPELINE_DEPTH,
			   ANDROID_SENSOR_FRAME_DURATION,
			   ANDROID_CONTROL_AE_STATE, ANDROID_CONTROL_AWB_STATE, ANDROID_FLASH_STATE,
			   ANDROID_SENSOR_EXPOSURE_TIME, ANDROID_SENSOR_SENSITIVITY,
			   ANDROID_CONTROL_AF_STATE, ANDROID_LENS_STATE,
			   ANDROID_LENS_APERTURE, ANDROID_LENS_FOCAL_LENGTH,
			   ANDROID_LENS_FILTER_DENSITY, ANDROID_LENS_OPTICAL_STABILIZATION_MODE,
			   ANDROID_LENS_FOCUS_RANGE })
		resKeys.push_back(k);
	if (f.manualSensor)
		resKeys.push_back(ANDROID_BLACK_LEVEL_LOCK);
	/* 声明了 COLOR_CORRECTION_MODE：结果里报实际的增益与色彩矩阵 */
	if (f.awbLock)
		resKeys.push_back(ANDROID_COLOR_CORRECTION_TRANSFORM);
	/* 定焦相机声明了 READ_SENSOR_SETTINGS 也要报对焦距离（恒为 0） */
	if ((f.hasAf || f.manualSensor) &&
	    std::find(resKeys.begin(), resKeys.end(), ANDROID_LENS_FOCUS_DISTANCE) ==
			       resKeys.end())
		resKeys.push_back(ANDROID_LENS_FOCUS_DISTANCE);
	/* 请求键会回显；每个结果键只留一次 */
	std::sort(resKeys.begin(), resKeys.end());
	resKeys.erase(std::unique(resKeys.begin(), resKeys.end()), resKeys.end());
	add_camera_metadata_entry(m, ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS, reqKeys.data(),
				  reqKeys.size());
	add_camera_metadata_entry(m, ANDROID_REQUEST_AVAILABLE_RESULT_KEYS, resKeys.data(),
				  resKeys.size());
	std::vector<int32_t> charKeys;
	for (size_t i = 0; i < get_camera_metadata_entry_count(m); i++) {
		camera_metadata_ro_entry_t e;
		if (get_camera_metadata_ro_entry(m, i, &e) == 0)
			charKeys.push_back(static_cast<int32_t>(e.tag));
	}
	charKeys.push_back(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
	add_camera_metadata_entry(m, ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS,
				  charKeys.data(), charKeys.size());

	std::vector<uint8_t> out = pack(m);
	free_camera_metadata(m);
	ALOGI("characteristics 构造完成：%zu 字节（%zu 个特性键、%zu 个请求键、%zu 个结果键）",
	      out.size(), charKeys.size(), reqKeys.size(), resKeys.size());
	return out;
}

namespace {
/* 请求里已有这个键就改，没有就加 —— 结果要回显请求键，但值以会话实际状态为准。 */
template <typename T>
void setOrAdd(camera_metadata_t *m, uint32_t tag, const T *v, size_t n)
{
	camera_metadata_entry_t e;
	if (find_camera_metadata_entry(m, tag, &e) == 0)
		update_camera_metadata_entry(m, e.index, v, n, nullptr);
	else
		add_camera_metadata_entry(m, tag, v, n);
}
} /* namespace */

std::vector<uint8_t> buildResult(const std::vector<uint8_t> &requestSettings,
				 int64_t timestampNs, uint8_t pipelineDepth,
				 const FrameResultFacts &fr)
{
	/*
	 * 以请求设置为底（框架要求结果里能回显请求的键），再补上结果专有的。
	 * ⚠️ allocate 时要留出扩容余量：add_camera_metadata_entry 在空间不够时
	 *    是【静默失败】的，不会报错，症状会变成"某个键莫名其妙不见了"。
	 */
	const camera_metadata_t *req =
		reinterpret_cast<const camera_metadata_t *>(requestSettings.data());
	size_t entries = 28, data = 1024;
	if (!requestSettings.empty()) {
		entries += get_camera_metadata_entry_count(req);
		data += get_camera_metadata_data_count(req);
	}
	camera_metadata_t *m = allocate_camera_metadata(entries, data);
	if (!m)
		return {};
	if (!requestSettings.empty())
		append_camera_metadata(m, req);

	add_camera_metadata_entry(m, ANDROID_SENSOR_TIMESTAMP, &timestampNs, 1);
	add_camera_metadata_entry(m, ANDROID_REQUEST_PIPELINE_DEPTH, &pipelineDepth, 1);

	/* 3A 状态：AE/AWB 由软件 ISP 的 IPA 在跑；AF 由本 HAL 自己爬山（见 AutoFocus.cpp）。
	 * fr.afState/lensState/focusDistance 由会话每帧填。 */
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_STATE, &fr.aeState, 1);
	setOrAdd(m, ANDROID_CONTROL_AE_MODE, &fr.aeMode, 1);
	setOrAdd(m, ANDROID_FLASH_MODE, &fr.flashMode, 1);
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_STATE, &fr.awbState, 1);
	add_camera_metadata_entry(m, ANDROID_FLASH_STATE, &fr.flashState, 1);
	if (fr.exposureNs > 0)
		setOrAdd(m, ANDROID_SENSOR_EXPOSURE_TIME, &fr.exposureNs, 1);
	if (fr.sensitivity > 0)
		setOrAdd(m, ANDROID_SENSOR_SENSITIVITY, &fr.sensitivity, 1);
	if (fr.frameDurationNs > 0)
		setOrAdd(m, ANDROID_SENSOR_FRAME_DURATION, &fr.frameDurationNs, 1);
	if (fr.cropRegion[2] > 0)
		setOrAdd(m, ANDROID_SCALER_CROP_REGION, fr.cropRegion, 4);
	/* 报实际做出的缩略图尺寸：随图像旋转（CaptureResult 文档） */
	if (fr.thumbnailSize[0] >= 0)
		setOrAdd(m, ANDROID_JPEG_THUMBNAIL_SIZE, fr.thumbnailSize, 2);

	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_STATE, &fr.afState, 1);
	add_camera_metadata_entry(m, ANDROID_LENS_STATE, &fr.lensState, 1);
	/* LENS_FOCUS_DISTANCE 与 AF_STATE 是同一个"对焦结果组"：
	 * 应用（CameraX 的 FocusMeteringControl）会在收到 FOCUSED_LOCKED 时读它。
	 * 定焦相机只在声明了 READ_SENSOR_SETTINGS 时报（恒为 0，见特性的
	 * LENS_INFO_MINIMUM_FOCUS_DISTANCE）。 */
	if (fr.hasAf || fr.manualSensor) {
		const float fd = fr.hasAf ? fr.focusDistance : 0.0f;
		setOrAdd(m, ANDROID_LENS_FOCUS_DISTANCE, &fd, 1);
	}

	/* 镜头的固定参数，READ_SENSOR_SETTINGS 要求每帧报；景深没有标定，按对焦距离报 */
	setOrAdd(m, ANDROID_LENS_APERTURE, &fr.lens.fNumber, 1);
	setOrAdd(m, ANDROID_LENS_FOCAL_LENGTH, &fr.lens.focalLengthMm, 1);
	const float filterDensity = 0.0f;
	setOrAdd(m, ANDROID_LENS_FILTER_DENSITY, &filterDensity, 1);
	const uint8_t ois = ANDROID_LENS_OPTICAL_STABILIZATION_MODE_OFF;
	setOrAdd(m, ANDROID_LENS_OPTICAL_STABILIZATION_MODE, &ois, 1);
	const float focusRange[2] = { fr.hasAf ? fr.focusDistance : 0.0f,
				      fr.hasAf ? fr.focusDistance : 0.0f };
	setOrAdd(m, ANDROID_LENS_FOCUS_RANGE, focusRange, 2);
	if (fr.hasCcGains)
		setOrAdd(m, ANDROID_COLOR_CORRECTION_GAINS, fr.ccGains.data(), 4);
	if (fr.hasCcTransform) {
		camera_metadata_rational_t t[9];
		for (int i = 0; i < 9; i++)
			t[i] = { static_cast<int32_t>(std::lround(fr.ccTransform[i] * 10000.0f)), 10000 };
		setOrAdd(m, ANDROID_COLOR_CORRECTION_TRANSFORM, t, 9);
	}
	/* 黑电平由 libcamera 的调优固定，本来就不变：回显请求，没有时报 OFF */
	if (fr.manualSensor) {
		camera_metadata_entry_t e;
		if (find_camera_metadata_entry(m, ANDROID_BLACK_LEVEL_LOCK, &e) != 0) {
			const uint8_t bll = ANDROID_BLACK_LEVEL_LOCK_OFF;
			add_camera_metadata_entry(m, ANDROID_BLACK_LEVEL_LOCK, &bll, 1);
		}
	}

	std::vector<uint8_t> out = pack(m);
	free_camera_metadata(m);
	return out;
}

bool streamCombinationSupported(const StreamConfiguration &cfg, const SensorFacts &f,
				std::string *why)
{
	auto no = [&](const std::string &r) {
		if (why)
			*why = r;
		return false;
	};
	if (cfg.streams.empty())
		return no("没有流");
	int processed = 0, stalling = 0;
	for (const auto &s : cfg.streams) {
		const std::string id = "流 " + std::to_string(s.id) + ": ";
		if (s.streamType != StreamType::OUTPUT)
			return no(id + "不支持输入流（没有 reprocess）");
		if (s.rotation != StreamRotation::ROTATION_0)
			return no(id + "不支持流旋转");
		if (s.width <= 0 || s.height <= 0 || s.width > f.fovW || s.height > f.fovH)
			return no(id + "尺寸 " + std::to_string(s.width) + "x" +
				  std::to_string(s.height) + " 超出有效阵列");
		switch (s.format) {
		case PixelFormat::BLOB:
			stalling++;
			break;
		case PixelFormat::IMPLEMENTATION_DEFINED:
		case PixelFormat::YCBCR_420_888:
			processed++;
			break;
		default:
			return no(id + "格式 " + std::to_string(static_cast<int>(s.format)) +
				  " 交付不了（只支持 IMPLEMENTATION_DEFINED / YCBCR_420_888 / BLOB）");
		}
	}
	if (processed > 2)
		return no("非停顿流 " + std::to_string(processed) + " 路，上限 2");
	if (stalling > 1)
		return no("JPEG 流 " + std::to_string(stalling) + " 路，上限 1");
	return true;
}

std::vector<uint8_t> stripTriggers(const std::vector<uint8_t> &settings)
{
	if (settings.empty())
		return {};
	const camera_metadata_t *src =
		reinterpret_cast<const camera_metadata_t *>(settings.data());
	std::vector<uint8_t> out(get_camera_metadata_size(src));
	camera_metadata_t *m = copy_camera_metadata(out.data(), out.size(), src);
	if (!m)
		return settings;   /* 拷不出来就原样用 —— 最坏是多触发一次，不会丢设置 */
	const uint8_t afIdle = ANDROID_CONTROL_AF_TRIGGER_IDLE;
	const uint8_t aeIdle = ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER_IDLE;
	camera_metadata_entry_t e;
	/* 同类型同长度的原地更新不需要额外空间，所以不会因容量失败。 */
	if (find_camera_metadata_entry(m, ANDROID_CONTROL_AF_TRIGGER, &e) == 0)
		update_camera_metadata_entry(m, e.index, &afIdle, 1, nullptr);
	if (find_camera_metadata_entry(m, ANDROID_CONTROL_AE_PRECAPTURE_TRIGGER, &e) == 0)
		update_camera_metadata_entry(m, e.index, &aeIdle, 1, nullptr);
	return out;
}

bool templateSupported(int templateId, const SensorFacts &f)
{
	const auto tmpl = static_cast<RequestTemplate>(templateId);

	/* MANUAL 随 MANUAL_SENSOR 能力提供 */
	return tmpl != RequestTemplate::ZERO_SHUTTER_LAG &&
	       (tmpl != RequestTemplate::MANUAL || f.manualSensor);
}

std::vector<uint8_t> buildDefaultRequest(int templateId, const SensorFacts &f)
{
	camera_metadata_t *m = allocate_camera_metadata(32, 2048);
	if (!m)
		return {};

	const auto tmplId = static_cast<RequestTemplate>(templateId);
	const bool manual = tmplId == RequestTemplate::MANUAL;
	const uint8_t controlMode = manual ? ANDROID_CONTROL_MODE_OFF : ANDROID_CONTROL_MODE_AUTO;
	add_camera_metadata_entry(m, ANDROID_CONTROL_MODE, &controlMode, 1);
	/*
	 * 声明了 MANUAL_SENSOR 时，每个模板都要带这几项手动传感器设置（CTS
	 * CameraDeviceTest 逐模板检查）；只有 MANUAL 模板关掉 AE 用它们，
	 * 其余模板里它们不起作用。
	 */
	if (f.manualSensor) {
		/* 30 fps 的帧里 10 ms、ISO 100，限在范围内 */
		const int64_t exposure = std::clamp<int64_t>(10000000, f.exposureMinNs, f.exposureMaxNs);
		add_camera_metadata_entry(m, ANDROID_SENSOR_EXPOSURE_TIME, &exposure, 1);
		const int32_t iso = std::clamp(100, f.isoMin, f.isoMax);
		add_camera_metadata_entry(m, ANDROID_SENSOR_SENSITIVITY, &iso, 1);
		const int64_t frameDuration = 33333333;
		add_camera_metadata_entry(m, ANDROID_SENSOR_FRAME_DURATION, &frameDuration, 1);
		const uint8_t blackLevelLock = ANDROID_BLACK_LEVEL_LOCK_OFF;
		add_camera_metadata_entry(m, ANDROID_BLACK_LEVEL_LOCK, &blackLevelLock, 1);
		if (f.hasAf) {
			const float focus = 0.0f;
			add_camera_metadata_entry(m, ANDROID_LENS_FOCUS_DISTANCE, &focus, 1);
		}
	}
	/* ★ 模板一律按 AIDL 的枚举名比较，不写数字：PR #6 的初版把 ZSL/MANUAL 的编号记错了
	 *   （RequestTemplate.aidl：PREVIEW=1 STILL_CAPTURE=2 VIDEO_RECORD=3 VIDEO_SNAPSHOT=4
	 *    ZERO_SHUTTER_LAG=5 MANUAL=6）。 */
	const auto tmpl = static_cast<RequestTemplate>(templateId);
	/*
	 * Every template carries the capture intent that matches it (CameraDevice
	 * template docs). Clients build their requests from these templates, and
	 * the camera service refuses to start a Camera1 (android.hardware.Camera)
	 * preview without it (Camera2-Parameters updateRequest: BAD_VALUE), e.g. in
	 * QQ.
	 */
	uint8_t intent = ANDROID_CONTROL_CAPTURE_INTENT_PREVIEW;
	switch (tmpl) {
	case RequestTemplate::STILL_CAPTURE:
		intent = ANDROID_CONTROL_CAPTURE_INTENT_STILL_CAPTURE;
		break;
	case RequestTemplate::VIDEO_RECORD:
		intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_RECORD;
		break;
	case RequestTemplate::VIDEO_SNAPSHOT:
		intent = ANDROID_CONTROL_CAPTURE_INTENT_VIDEO_SNAPSHOT;
		break;
	case RequestTemplate::MANUAL:
		intent = ANDROID_CONTROL_CAPTURE_INTENT_MANUAL;
		break;
	default:   /* PREVIEW；ZERO_SHUTTER_LAG 不提供 */
		break;
	}
	add_camera_metadata_entry(m, ANDROID_CONTROL_CAPTURE_INTENT, &intent, 1);
	/* 有闪光灯时，静态拍照模板默认 ON_AUTO_FLASH，与 CameraCharacteristics 文档对模板的
	 * 约定一致；其余模板不闪。 */
	const uint8_t aeMode = manual ? ANDROID_CONTROL_AE_MODE_OFF
			       : (!f.flashLed.empty() && tmpl == RequestTemplate::STILL_CAPTURE)
				       ? ANDROID_CONTROL_AE_MODE_ON_AUTO_FLASH
				       : ANDROID_CONTROL_AE_MODE_ON;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_MODE, &aeMode, 1);
	/* MANUAL 模板关掉 AWB：没给增益时沿用当时 AWB 的增益（Session::apply3aControls()） */
	const uint8_t awbMode = manual && f.awbLock ? ANDROID_CONTROL_AWB_MODE_OFF
						    : ANDROID_CONTROL_AWB_MODE_AUTO;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_MODE, &awbMode, 1);
	if (f.awbLock) {
		/* HAL 不应用 COLOR_CORRECTION_TRANSFORM，色彩矩阵总是调优的：各模板都报 FAST */
		const uint8_t ccMode = ANDROID_COLOR_CORRECTION_MODE_FAST;
		add_camera_metadata_entry(m, ANDROID_COLOR_CORRECTION_MODE, &ccMode, 1);
	}
	const int32_t evComp = 0;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_EXPOSURE_COMPENSATION, &evComp, 1);
	/* 有 AUTO 时模板都用 AUTO（CTS 的模板检查） */
	if (f.antibanding) {
		const uint8_t ab = ANDROID_CONTROL_AE_ANTIBANDING_MODE_AUTO;
		add_camera_metadata_entry(m, ANDROID_CONTROL_AE_ANTIBANDING_MODE, &ab, 1);
	}
	const uint8_t aeLock = ANDROID_CONTROL_AE_LOCK_OFF;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_LOCK, &aeLock, 1);
	const uint8_t awbLock = ANDROID_CONTROL_AWB_LOCK_OFF;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AWB_LOCK, &awbLock, 1);
	/*
	 * AF 模式按模板给（AOSP 对模板的约定）：
	 *   PREVIEW / STILL_CAPTURE / ZERO_SHUTTER_LAG → CONTINUOUS_PICTURE
	 *   VIDEO_RECORD / VIDEO_SNAPSHOT → CONTINUOUS_VIDEO（录像中抓拍不能让镜头去扫）
	 *   MANUAL → OFF；定焦相机一律 OFF
	 * ★ 为什么要给 CONTINUOUS 而不是 AUTO：应用（CameraX）默认只把模板设下来的
	 *   模式原样用；给 AUTO 的话要等应用发 AF_TRIGGER=START 才会动，而很多应用
	 *   在预览里根本不发。CONTINUOUS_PICTURE 才是"预览就该一直对焦"的语义。
	 */
	uint8_t afMode = ANDROID_CONTROL_AF_MODE_OFF;
	if (f.hasAf) {
		switch (tmpl) {
		case RequestTemplate::VIDEO_RECORD:
		case RequestTemplate::VIDEO_SNAPSHOT:
			afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_VIDEO;
			break;
		case RequestTemplate::MANUAL:
			afMode = ANDROID_CONTROL_AF_MODE_OFF;
			break;
		default:   /* PREVIEW / STILL_CAPTURE / ZERO_SHUTTER_LAG */
			afMode = ANDROID_CONTROL_AF_MODE_CONTINUOUS_PICTURE;
			break;
		}
	}
	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_MODE, &afMode, 1);
	/* 触发器必须显式给 IDLE：框架要求默认请求里出现这个键，且 START 只在
	 * 应用真的要单次对焦时才由它发。 */
	const uint8_t afTrigger = ANDROID_CONTROL_AF_TRIGGER_IDLE;
	add_camera_metadata_entry(m, ANDROID_CONTROL_AF_TRIGGER, &afTrigger, 1);
	/* 拍照用 HIGH_QUALITY（5×5，照片细节），其余用 FAST */
	const uint8_t edgeMode = tmpl == RequestTemplate::STILL_CAPTURE
					 ? ANDROID_EDGE_MODE_HIGH_QUALITY
					 : ANDROID_EDGE_MODE_FAST;
	add_camera_metadata_entry(m, ANDROID_EDGE_MODE, &edgeMode, 1);
	const uint8_t flashMode = ANDROID_FLASH_MODE_OFF;
	add_camera_metadata_entry(m, ANDROID_FLASH_MODE, &flashMode, 1);
	/* Recording templates get a fixed range, the others a variable one. */
	const bool video = tmpl == RequestTemplate::VIDEO_RECORD ||
			   tmpl == RequestTemplate::VIDEO_SNAPSHOT;
	const int32_t fps[] = { video ? 30 : 15, 30 };
	add_camera_metadata_entry(m, ANDROID_CONTROL_AE_TARGET_FPS_RANGE, fps, 2);
	const int32_t cropRegion[] = { 0, 0, f.fovW, f.fovH };
	add_camera_metadata_entry(m, ANDROID_SCALER_CROP_REGION, cropRegion, 4);
	const uint8_t jpegQuality = 90;
	add_camera_metadata_entry(m, ANDROID_JPEG_QUALITY, &jpegQuality, 1);
	const int32_t thumbSize[] = { 160, 120 };
	add_camera_metadata_entry(m, ANDROID_JPEG_THUMBNAIL_SIZE, thumbSize, 2);
	const uint8_t thumbQuality = 85;
	add_camera_metadata_entry(m, ANDROID_JPEG_THUMBNAIL_QUALITY, &thumbQuality, 1);

	std::vector<uint8_t> out = pack(m);
	free_camera_metadata(m);
	return out;
}

} /* namespace gaokun3 */
