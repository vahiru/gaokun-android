/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <aidl/android/hardware/camera/device/BnCameraDevice.h>
#include <libcamera/libcamera.h>

#include "Metadata.h"

namespace gaokun3 {

class Provider;

class Device : public aidl::android::hardware::camera::device::BnCameraDevice {
public:
	/* provider 只用来把手电筒状态推给框架；Provider 持有所有 Device，寿命更长。 */
	Device(std::shared_ptr<libcamera::Camera> cam, std::string name, Provider *provider);
	~Device() override;

	/* 从 libcamera 抽出传感器事实并预先构造 characteristics。 */
	bool init();

	::ndk::ScopedAStatus getCameraCharacteristics(
		aidl::android::hardware::camera::device::CameraMetadata *out) override;
	::ndk::ScopedAStatus getPhysicalCameraCharacteristics(
		const std::string &in_physicalCameraId,
		aidl::android::hardware::camera::device::CameraMetadata *out) override;
	::ndk::ScopedAStatus getResourceCost(
		aidl::android::hardware::camera::common::CameraResourceCost *out) override;
	::ndk::ScopedAStatus isStreamCombinationSupported(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_streams,
		bool *out) override;
	::ndk::ScopedAStatus open(
		const std::shared_ptr<aidl::android::hardware::camera::device::
					      ICameraDeviceCallback> &in_callback,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraDeviceSession>
			*out) override;
	::ndk::ScopedAStatus openInjectionSession(
		const std::shared_ptr<aidl::android::hardware::camera::device::
					      ICameraDeviceCallback> &in_callback,
		std::shared_ptr<aidl::android::hardware::camera::device::ICameraInjectionSession>
			*out) override;
	::ndk::ScopedAStatus setTorchMode(bool in_on) override;
	::ndk::ScopedAStatus turnOnTorchWithStrengthLevel(int32_t in_torchStrength) override;
	::ndk::ScopedAStatus getTorchStrengthLevel(int32_t *out) override;
	::ndk::ScopedAStatus constructDefaultRequestSettings(
		aidl::android::hardware::camera::device::RequestTemplate in_type,
		aidl::android::hardware::camera::device::CameraMetadata *out) override;
	::ndk::ScopedAStatus isStreamCombinationWithSettingsSupported(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_streams,
		bool *out) override;
	::ndk::ScopedAStatus getSessionCharacteristics(
		const aidl::android::hardware::camera::device::StreamConfiguration &in_sessionConfig,
		aidl::android::hardware::camera::device::CameraMetadata *out) override;

	const SensorFacts &facts() const { return facts_; }
	std::shared_ptr<libcamera::Camera> camera() const { return cam_; }

private:
	/* 填写 facts_.minFrameDurationsNs 和 3A 控制的标志，见 Device.cpp。 */
	void probeCamera();
	/* 探测时相机被占用：最多再试这么多次，间隔 kAcquireRetryUs（共约 1 s） */
	static constexpr int kAcquireRetries = 5;
	static constexpr unsigned int kAcquireRetryUs = 200000;
	void probe3aControls();
	/* 每种探测过的配置都接受的曝光、增益与帧长范围 */
	struct ManualLimits {
		int64_t expMinUs = 0, expMaxUs = INT64_MAX;
		double gainMin = 0.0, gainMax = 1e9;
		int64_t frameMaxUs = INT64_MAX;
		bool seen = false;
	};
	void accumulateLimits(ManualLimits &l);
	/* 会话关闭时由 Session 回调：相机不再占用，手电筒重新可用。 */
	void onSessionClosed();
	/* 写 LED 的 brightness（torch 档 = max_brightness）。没有闪光灯时返回 false。 */
	bool setLed(bool on);

	std::mutex mutex_;
	std::shared_ptr<libcamera::Camera> cam_;
	std::string name_;
	Provider *provider_ = nullptr;
	bool inUse_ = false;
	SensorFacts facts_;
	std::vector<uint8_t> characteristics_;
};

} /* namespace gaokun3 */
