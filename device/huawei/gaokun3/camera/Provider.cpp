/* SPDX-License-Identifier: Apache-2.0 */
#include "Provider.h"
#include "Device.h"

#include <algorithm>

#include <aidl/android/hardware/camera/common/CameraDeviceStatus.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>
#include <aidl/android/hardware/camera/common/VendorTagSection.h>
#include <aidl/android/hardware/camera/device/ICameraDevice.h>
#include <aidl/android/hardware/camera/provider/ICameraProviderCallback.h>
#include <log/log.h>

#include <libcamera/property_ids.h>

using ::aidl::android::hardware::camera::common::Status;
using ::aidl::android::hardware::camera::common::TorchModeStatus;
using ::aidl::android::hardware::camera::common::VendorTagSection;
using ::aidl::android::hardware::camera::device::ICameraDevice;
using ::aidl::android::hardware::camera::provider::CameraIdAndStreamCombination;
using ::aidl::android::hardware::camera::provider::ConcurrentCameraIdCombination;
using ::aidl::android::hardware::camera::provider::ICameraProviderCallback;

namespace gaokun3 {

namespace {
ndk::ScopedAStatus err(Status s)
{
	return ndk::ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(s));
}
} /* namespace */

Provider::Provider() = default;

Provider::~Provider()
{
	if (cm_)
		cm_->stop();
}

bool Provider::init()
{
	cm_ = std::make_unique<libcamera::CameraManager>();
	int ret = cm_->start();
	if (ret) {
		ALOGE("libcamera CameraManager::start 失败: %d", ret);
		return false;
	}

	auto cams = cm_->cameras();
	ALOGI("libcamera 发现 %zu 个相机", cams.size());
	/*
	 * ★ 编号要【确定】：libcamera 的枚举顺序随 media 设备出现的先后而变，2026-09-14 同一台机器
	 *   两次开机后摄一次是 internal/1、一次是 internal/0。Android 的约定是 ID 0 = 后摄
	 *   （很多应用直接 open("0") 当后摄），而且框架按 ID 记每个相机的设置。
	 *   这里按 Location 排：Back 在前，其余按 libcamera id 字符串排，与开机顺序无关。
	 */
	std::sort(cams.begin(), cams.end(),
		  [](const std::shared_ptr<libcamera::Camera> &a,
		     const std::shared_ptr<libcamera::Camera> &b) {
			  auto loc = [](const std::shared_ptr<libcamera::Camera> &c) {
				  auto l = c->properties().get(libcamera::properties::Location);
				  return l && *l == libcamera::properties::CameraLocationBack ? 0 : 1;
			  };
			  const int la = loc(a), lb = loc(b);
			  if (la != lb)
				  return la < lb;
			  return a->id() < b->id();
		  });
	int idx = 0;
	for (const auto &c : cams) {
		std::string name = std::string("device@") + kDeviceVersion +
				   "/internal/" + std::to_string(idx);
		cameras_[name] = c;
		ALOGI("  %s  ←  %s", name.c_str(), c->id().c_str());
		idx++;
	}
	/*
	 * 在注册服务之前建好每个相机的 Device：Device::init() 要按每种画面尺寸
	 * 配置一遍 libcamera、读最短帧长。两个相机共用一个流水线，
	 * 同一时间只能 acquire 一个；注册之后再探测，可能撞上另一个相机的会话或
	 * 别的 libcamera 进程，探测失败的相机就一直只声明 30 fps。这样也不用在
	 * binder 调用里持着 mutex_ 做十来次 configure。失败的留到
	 * getCameraDeviceInterface() 再试。
	 */
	for (const auto &kv : cameras_) {
		auto dev = ndk::SharedRefBase::make<Device>(kv.second, kv.first, this);
		if (dev->init())
			devices_[kv.first] = dev;
		else
			ALOGW("Device init failed for %s, will retry on first use", kv.first.c_str());
	}
	/* ⚠️ 一个相机都没有时【不要】报成功：provider 注册上去但列表为空，
	 *    框架只会显示"没有相机"，而真正的失败原因（camss 没 probe /
	 *    缺 DTB）就被埋掉了。宁可让服务起不来，日志里看得见。 */
	return !cameras_.empty();
}

ndk::ScopedAStatus Provider::setCallback(
	const std::shared_ptr<ICameraProviderCallback> &callback)
{
	std::lock_guard<std::mutex> lk(mutex_);
	cb_ = callback;
	if (cb_) {
		/* 内置相机是常在的，注册回调时立刻把状态推过去。 */
		for (const auto &kv : cameras_)
			cb_->cameraDeviceStatusChange(
				kv.first,
				aidl::android::hardware::camera::common::CameraDeviceStatus::PRESENT);
	}
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getVendorTags(std::vector<VendorTagSection> *out)
{
	out->clear();      /* 没有厂商私有 tag */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getCameraIdList(std::vector<std::string> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);
	out->clear();
	for (const auto &kv : cameras_)
		out->push_back(kv.first);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getCameraDeviceInterface(
	const std::string &name, std::shared_ptr<ICameraDevice> *out)
{
	std::lock_guard<std::mutex> lk(mutex_);

	auto it = devices_.find(name);
	if (it != devices_.end()) {
		*out = it->second;
		return ndk::ScopedAStatus::ok();
	}

	auto cit = cameras_.find(name);
	if (cit == cameras_.end()) {
		ALOGE("getCameraDeviceInterface: 未知设备名 %s", name.c_str());
		return err(Status::ILLEGAL_ARGUMENT);
	}

	auto dev = ndk::SharedRefBase::make<Device>(cit->second, name, this);
	if (!dev->init()) {
		ALOGE("Device::init 失败: %s", name.c_str());
		return err(Status::INTERNAL_ERROR);
	}
	devices_[name] = dev;
	*out = dev;
	return ndk::ScopedAStatus::ok();
}

void Provider::notifyTorch(const std::string &deviceName, TorchModeStatus status)
{
	std::shared_ptr<ICameraProviderCallback> cb;
	{
		std::lock_guard<std::mutex> lk(mutex_);
		cb = cb_;
	}
	if (cb)
		cb->torchModeStatusChange(deviceName, status);
}

ndk::ScopedAStatus Provider::notifyDeviceStateChange(int64_t state)
{
	/* 折叠/展开之类的设备状态，本机无关。 */
	ALOGD("notifyDeviceStateChange(%lld) —— 忽略", (long long)state);
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::getConcurrentCameraIds(
	std::vector<ConcurrentCameraIdCombination> *out)
{
	out->clear();      /* 只有一个相机，谈不上并发组合 */
	return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Provider::isConcurrentStreamCombinationSupported(
	const std::vector<CameraIdAndStreamCombination> &, bool *out)
{
	*out = false;
	return ndk::ScopedAStatus::ok();
}

} /* namespace gaokun3 */
