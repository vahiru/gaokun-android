/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sensors-impl/Sensor.h"

#include <log/log.h>
#include "utils/SystemClock.h"

#include <cmath>
#include <sched.h>

#include "SscHub.h"

using ::ndk::ScopedAStatus;

namespace {
// SSC 的 accuracy 是 0..3（0 = 不可信），映射到 AIDL 的 SensorStatus。
::aidl::android::hardware::sensors::SensorStatus SscAccuracyToStatus(int a) {
    using ::aidl::android::hardware::sensors::SensorStatus;
    switch (a) {
        case 3: return SensorStatus::ACCURACY_HIGH;
        case 2: return SensorStatus::ACCURACY_MEDIUM;
        case 1: return SensorStatus::ACCURACY_LOW;
        default: return SensorStatus::UNRELIABLE;
    }
}
}  // namespace

namespace aidl {
namespace android {
namespace hardware {
namespace sensors {

static constexpr int32_t kDefaultMaxDelayUs = 10 * 1000 * 1000;

Sensor::Sensor(ISensorsEventCallback* callback)
    : mIsEnabled(false),
      mSamplingPeriodNs(0),
      mLastSampleTimeNs(0),
      mCallback(callback),
      mMode(OperationMode::NORMAL) {
    mRunThread = std::thread(startThread, this);
}

Sensor::~Sensor() {
    std::unique_lock<std::mutex> lock(mRunMutex);
    mStopThread = true;
    mIsEnabled = false;
    mWaitCV.notify_all();
    lock.release();
    mRunThread.join();
}

const SensorInfo& Sensor::getSensorInfo() const {
    return mSensorInfo;
}

void Sensor::batch(int64_t samplingPeriodNs) {
    if (samplingPeriodNs < mSensorInfo.minDelayUs * 1000LL) {
        samplingPeriodNs = mSensorInfo.minDelayUs * 1000LL;
    } else if (samplingPeriodNs > mSensorInfo.maxDelayUs * 1000LL) {
        samplingPeriodNs = mSensorInfo.maxDelayUs * 1000LL;
    }

    if (mSamplingPeriodNs != samplingPeriodNs) {
        mSamplingPeriodNs = samplingPeriodNs;
        // Wake up the 'run' thread to check if a new event should be generated now
        mWaitCV.notify_all();
    }
}

void Sensor::activate(bool enable) {
    if (mIsEnabled != enable) {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mIsEnabled = enable;
        // gaokun3：让 SscHub 知道这一路在 SSC 上该不该开着 —— 没有订阅者时
        // 停用，SLPI 才能睡（v1.0 计划 PWR-3）。放在锁里，保证 SscHub 最后
        // 看到的与 mIsEnabled 的最终值一致；锁序 mRunMutex → SscHub::m_ 与
        // run() 里读样本时相同，SscHub 自己从不碰 mRunMutex。
        if (mSensorInfo.type == SensorType::ACCELEROMETER) {
            gaokun3::SscHub::Get().SetAccelWanted(enable);
        } else if (mSensorInfo.type == SensorType::GYROSCOPE) {
            gaokun3::SscHub::Get().SetGyroWanted(enable);
        } else if (mSensorInfo.type == SensorType::LIGHT) {
            gaokun3::SscHub::Get().SetLightWanted(enable);
        }
        mWaitCV.notify_all();
    }
}

ScopedAStatus Sensor::flush() {
    // Only generate a flush complete event if the sensor is enabled and if the sensor is not a
    // one-shot sensor.
    if (!mIsEnabled ||
        (mSensorInfo.flags & static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ONE_SHOT_MODE))) {
        return ScopedAStatus::fromServiceSpecificError(
                static_cast<int32_t>(BnSensors::ERROR_BAD_VALUE));
    }

    // Note: If a sensor supports batching, write all of the currently batched events for the sensor
    // to the Event FMQ prior to writing the flush complete event.
    Event ev;
    ev.sensorHandle = mSensorInfo.sensorHandle;
    ev.sensorType = SensorType::META_DATA;
    EventPayload::MetaData meta = {
            .what = MetaDataEventType::META_DATA_FLUSH_COMPLETE,
    };
    ev.payload.set<EventPayload::Tag::meta>(meta);
    std::vector<Event> evs{ev};
    mCallback->postEvents(evs, isWakeUpSensor());

    return ScopedAStatus::ok();
}

void Sensor::startThread(Sensor* sensor) {
    sensor->run();
}

void Sensor::run() {
    std::unique_lock<std::mutex> runLock(mRunMutex);
    constexpr int64_t kNanosecondsInSeconds = 1000 * 1000 * 1000;

    struct sched_param params;
    // Set the thread to the lowest real-time priority.
    params.sched_priority = 1;
    if (sched_setscheduler(/*pid=*/0, SCHED_FIFO, &params)) {
        ALOGE("Unable to set SCHED_FIFO: error %s", strerror(errno));
    }
    while (!mStopThread) {
        if (!mIsEnabled || mMode == OperationMode::DATA_INJECTION) {
            mWaitCV.wait(runLock, [&] {
                return ((mIsEnabled && mMode == OperationMode::NORMAL) || mStopThread);
            });
        } else {
            timespec curTime;
            clock_gettime(CLOCK_BOOTTIME, &curTime);
            int64_t now = (curTime.tv_sec * kNanosecondsInSeconds) + curTime.tv_nsec;
            int64_t nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;

            if (now >= nextSampleTime) {
                mLastSampleTimeNs = now;
                nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;
                mCallback->postEvents(readEvents(), isWakeUpSensor());
            }

            mWaitCV.wait_for(runLock, std::chrono::nanoseconds(nextSampleTime - now));
        }
    }
}

bool Sensor::isWakeUpSensor() {
    return mSensorInfo.flags & static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
}

std::vector<Event> Sensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    memset(&event.payload, 0, sizeof(event.payload));
    readEventPayload(event.payload);
    events.push_back(event);
    return events;
}

void Sensor::setOperationMode(OperationMode mode) {
    if (mMode != mode) {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mMode = mode;
        mWaitCV.notify_all();
    }
}

bool Sensor::supportsDataInjection() const {
    return mSensorInfo.flags & static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
}

ScopedAStatus Sensor::injectEvent(const Event& event) {
    if (event.sensorType == SensorType::ADDITIONAL_INFO) {
        return ScopedAStatus::ok();
        // When in OperationMode::NORMAL, SensorType::ADDITIONAL_INFO is used to push operation
        // environment data into the device.
    }

    if (!supportsDataInjection()) {
        return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    if (mMode == OperationMode::DATA_INJECTION) {
        mCallback->postEvents(std::vector<Event>{event}, isWakeUpSensor());
        return ScopedAStatus::ok();
    }

    return ScopedAStatus::fromServiceSpecificError(
            static_cast<int32_t>(BnSensors::ERROR_BAD_VALUE));
}

OnChangeSensor::OnChangeSensor(ISensorsEventCallback* callback)
    : Sensor(callback), mPreviousEventSet(false) {}

void OnChangeSensor::activate(bool enable) {
    Sensor::activate(enable);
    if (!enable) {
        mPreviousEventSet = false;
    }
}

std::vector<Event> OnChangeSensor::readEvents() {
    std::vector<Event> events = Sensor::readEvents();
    std::vector<Event> outputEvents;

    for (auto iter = events.begin(); iter != events.end(); ++iter) {
        Event ev = *iter;
        if (!mPreviousEventSet ||
            memcmp(&mPreviousEvent.payload, &ev.payload, sizeof(ev.payload)) != 0) {
            outputEvents.push_back(ev);
            mPreviousEvent = ev;
            mPreviousEventSet = true;
        }
    }
    return outputEvents;
}

AccelSensor::AccelSensor(int32_t sensorHandle, ISensorsEventCallback* callback) : Sensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "SH3001 Accelerometer";
    mSensorInfo.vendor = "Senodia (via Qualcomm SSC)";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::ACCELEROMETER;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 78.4f;  // +/- 8g
    mSensorInfo.resolution = 1.52e-5;
    mSensorInfo.power = 0.001f;          // mA
    // ★ v1.0 HW-12（2026-10-05）：10 ms → 20 ms，与 SSC 实际的 50 Hz 一致（SscHub.cpp 的 kRateHz）。
    //   原来宣称 100 Hz，订阅方按 100 Hz 拿到的是同一个 SSC 样本复读两次（实机 dumpsys sensorservice：
    //   陀螺仪相邻两条数值成对相同、间隔 10 ms）。CDD 对加速度计 / 陀螺仪的下限是 50 Hz，20 ms 正好满足。
    //   长期做法（透传 SSC 速率与报告时间戳）见 v1.0-plan HW-12，未做。
    mSensorInfo.minDelayUs = 20 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
};

void AccelSensor::readEventPayload(EventPayload& payload) {
    // 真数据来自 SLPI 上的 sh3001，经 SscHub 缓存（见 SscHub.h 的线程模型）。
    // 数据还没到时报 UNRELIABLE 而不是编一个 9.8 出来 —— 那会让上层以为
    // 机器平放着。
    //
    // 坐标轴：SSC上报芯片自身的坐标轴（安装矩阵全为0，见README.md），即面板竖屏的坐标轴。
    // Android要求按屏幕自然方向的坐标轴上报，而自然方向是横屏，即面板旋转90°（设备树中面板的
    // rotation，见device.mk）。接键盘横放时芯片的+x朝上、+y朝左，所以x = -y、y = x。
    const gaokun3::Sample s = gaokun3::SscHub::Get().Accel();
    EventPayload::Vec3 vec3 = {
            .x = -s.v[1],
            .y = s.v[0],
            .z = s.v[2],
            .status = s.valid ? SscAccuracyToStatus(s.accuracy)
                              : SensorStatus::UNRELIABLE,
    };
    payload.set<EventPayload::Tag::vec3>(vec3);
}

PressureSensor::PressureSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Pressure Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::PRESSURE;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 1100.0f;       // hPa
    mSensorInfo.resolution = 0.005f;      // hPa
    mSensorInfo.power = 0.001f;           // mA
    mSensorInfo.minDelayUs = 100 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = 0;
};

void PressureSensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(1013.25f);
}

MagnetometerSensor::MagnetometerSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Magnetic Field Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::MAGNETIC_FIELD;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 1300.0f;
    mSensorInfo.resolution = 0.01f;
    mSensorInfo.power = 0.001f;          // mA
    mSensorInfo.minDelayUs = 20 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
};

void MagnetometerSensor::readEventPayload(EventPayload& payload) {
    EventPayload::Vec3 vec3 = {
            .x = 100.0,
            .y = 0,
            .z = 50.0,
            .status = SensorStatus::ACCURACY_HIGH,
    };
    payload.set<EventPayload::Tag::vec3>(vec3);
}

LightSensor::LightSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    // The streaming tcs3701 ambient light sensor on the SLPI (see SscHub).
    mSensorInfo.name = "TCS3701 Ambient Light";
    mSensorInfo.vendor = "ams (via Qualcomm SSC)";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::LIGHT;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 60000.0f;      // lux
    mSensorInfo.resolution = 0.036f;      // SSC attribute 7 (resolutions)
    mSensorInfo.power = 0.09f;            // mA, SSC attribute 9 (active current, uA)
    mSensorInfo.minDelayUs = 200 * 1000;  // microseconds, SSC streams it at 5 Hz
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
};

void LightSensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(gaokun3::SscHub::Get().Light().v[0]);
}

std::vector<Event> LightSensor::readEvents() {
    // Report nothing until the SSC has delivered a sample: an on-change sensor
    // has no UNRELIABLE value to send, and a made-up 0 lux would dim the screen.
    if (!gaokun3::SscHub::Get().Light().valid) return {};
    return OnChangeSensor::readEvents();
}

ProximitySensor::ProximitySensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Proximity Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::PROXIMITY;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 5.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.power = 0.012f;           // mA
    mSensorInfo.minDelayUs = 200 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE |
                                              SensorInfo::SENSOR_FLAG_BITS_WAKE_UP);
};

void ProximitySensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(2.5f);
}

GyroSensor::GyroSensor(int32_t sensorHandle, ISensorsEventCallback* callback) : Sensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "SH3001 Gyroscope";
    mSensorInfo.vendor = "Senodia (via Qualcomm SSC)";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::GYROSCOPE;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 1000.0f * M_PI / 180.0f;
    mSensorInfo.resolution = 1000.0f * M_PI / (180.0f * 32768.0f);
    mSensorInfo.power = 0.001f;
    // ★ v1.0 HW-12：同加速度计，20 ms = SSC 实际的 50 Hz（见 AccelSensor 那段）。
    mSensorInfo.minDelayUs = 20 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
};

void GyroSensor::readEventPayload(EventPayload& payload) {
    // 与加速度计同一颗 sh3001（6 轴），单位 rad/s。
    // 同一颗芯片，坐标轴的换算与AccelSensor::readEventPayload()相同。
    const gaokun3::Sample s = gaokun3::SscHub::Get().Gyro();
    EventPayload::Vec3 vec3 = {
            .x = -s.v[1],
            .y = s.v[0],
            .z = s.v[2],
            .status = s.valid ? SscAccuracyToStatus(s.accuracy)
                              : SensorStatus::UNRELIABLE,
    };
    payload.set<EventPayload::Tag::vec3>(vec3);
}

AmbientTempSensor::AmbientTempSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Ambient Temp Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::AMBIENT_TEMPERATURE;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 80.0f;
    mSensorInfo.resolution = 0.01f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelayUs = 40 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
};

void AmbientTempSensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(40.0f);
}

RelativeHumiditySensor::RelativeHumiditySensor(int32_t sensorHandle,
                                               ISensorsEventCallback* callback)
    : OnChangeSensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Relative Humidity Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::RELATIVE_HUMIDITY;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 100.0f;
    mSensorInfo.resolution = 0.1f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelayUs = 40 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE);
}

void RelativeHumiditySensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(50.0f);
}

HingeAngleSensor::HingeAngleSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(callback) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.name = "Hinge Angle Sensor";
    mSensorInfo.vendor = "Vendor String";
    mSensorInfo.version = 1;
    mSensorInfo.type = SensorType::HINGE_ANGLE;
    mSensorInfo.typeAsString = "";
    mSensorInfo.maxRange = 360.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelayUs = 40 * 1000;  // microseconds
    mSensorInfo.maxDelayUs = kDefaultMaxDelayUs;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = static_cast<uint32_t>(SensorInfo::SENSOR_FLAG_BITS_ON_CHANGE_MODE |
                                              SensorInfo::SENSOR_FLAG_BITS_WAKE_UP |
                                              SensorInfo::SENSOR_FLAG_BITS_DATA_INJECTION);
}

void HingeAngleSensor::readEventPayload(EventPayload& payload) {
    payload.set<EventPayload::Tag::scalar>(180.0f);
}

}  // namespace sensors
}  // namespace hardware
}  // namespace android
}  // namespace aidl
