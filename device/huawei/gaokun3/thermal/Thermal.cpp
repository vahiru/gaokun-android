/* gaokun3 Thermal HAL: real kernel sensors only.
 * CPU/GPU/PMIC retain the existing die-temperature policy. SKIN is available
 * only for skin-temp-thermal with a validated kernel trip policy. No CPU proxy.
 * An absent/invalid skin policy is unavailable, never fabricated as 0 C/NONE.
 */
#include <aidl/android/hardware/thermal/BnThermal.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <dirent.h>
#include "ThermalPolicy.h"
#include <map>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::thermal::impl::gaokun3 {

using ::android::base::ReadFileToString;
using ::android::base::Trim;

static constexpr float kCold[7] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN};

struct Zone {
    std::string path;   // /sys/class/thermal/thermal_zoneN
    std::string name;   // 上报给框架的名字（去掉 -thermal 后缀）
    TemperatureType type;
    gaokun_thermal::Thresholds hot;
};

struct Cdev {
    std::string path;
    std::string name;
    CoolingType type;
};

static bool ReadTrimmed(const std::string& p, std::string* out) {
    std::string s;
    if (!ReadFileToString(p, &s)) return false;
    *out = Trim(s);
    return true;
}

// 返回摄氏度；读不到或不合理返回 NAN（调用方据此跳过）
static float ReadTempC(const std::string& zone_path) {
    std::string s;
    if (!ReadTrimmed(zone_path + "/temp", &s)) return NAN;
    return gaokun_thermal::Celsius(s).value_or(NAN);
}

static std::optional<gaokun_thermal::Thresholds> ReadSkinThresholds(
        const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) return std::nullopt;
    std::vector<gaokun_thermal::Trip> trips;
    bool valid = true;
    while (dirent* e = readdir(d)) {
        std::string n(e->d_name);
        if (n.rfind("trip_point_", 0) != 0 || n.size() < 5 ||
            n.compare(n.size() - 5, 5, "_type") != 0) continue;
        std::string type, temp;
        if (!ReadTrimmed(path + "/" + n, &type) ||
            !ReadTrimmed(path + "/" + n.substr(0, n.size() - 5) + "_temp", &temp)) {
            valid = false;
            break;
        }
        trips.push_back({type, temp});
    }
    closedir(d);
    return valid ? gaokun_thermal::SkinThresholds(trips) : std::nullopt;
}

class Thermal : public BnThermal {
  public:
    Thermal() {
        poller_ = std::thread([this] { PollLoop(); });
        poller_.detach();
    }

    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out) override {
        *out = Snapshot(std::nullopt);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType t,
                                               std::vector<Temperature>* out) override {
        *out = Snapshot(t);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getTemperatureThresholds(std::vector<TemperatureThreshold>* out) override {
        *out = Thresholds(std::nullopt);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getTemperatureThresholdsWithType(
            TemperatureType t, std::vector<TemperatureThreshold>* out) override {
        *out = Thresholds(t);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out) override {
        *out = CdevSnapshot(std::nullopt);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getCoolingDevicesWithType(CoolingType t,
                                                 std::vector<CoolingDevice>* out) override {
        *out = CdevSnapshot(t);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& cb) override {
        return AddCb(cb, std::nullopt);
    }

    ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& cb, TemperatureType t) override {
        return AddCb(cb, t);
    }

    ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& cb) override {
        if (cb == nullptr) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "callback is null");
        }
        std::lock_guard<std::mutex> lk(cb_mutex_);
        auto it = std::remove_if(cbs_.begin(), cbs_.end(), [&](const CbEntry& e) {
            return e.cb->asBinder().get() == cb->asBinder().get();
        });
        if (it == cbs_.end()) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "callback was not registered");
        }
        cbs_.erase(it, cbs_.end());
        return ndk::ScopedAStatus::ok();
    }

    // 本机的 cooling device 由内核 thermal core 自己驱动，没有可上报的变更事件源。
    // 如实回答不支持，不假装注册成功。
    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>&, CoolingType) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>&) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    // ★ 没有预测模型，就返回当前值（平坦外推）。返回 UNSUPPORTED 会让
    //   getThermalHeadroom() 拿不到数，游戏那条自我降档的路就断了。
    ndk::ScopedAStatus forecastSkinTemperature(int32_t forecastSeconds, float* out) override {
        if (forecastSeconds < 0) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_ILLEGAL_ARGUMENT, "forecastSeconds must be nonnegative");
        }
        float c = SkinTempC();
        if (std::isnan(c)) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_UNSUPPORTED_OPERATION, "Physical skin temperature or trip policy unavailable");
        }
        *out = c;
        return ndk::ScopedAStatus::ok();
    }

  private:
    struct CbEntry {
        std::shared_ptr<IThermalChangedCallback> cb;
        std::optional<TemperatureType> filter;
    };

    struct Topology { std::vector<Zone> zones; std::vector<Cdev> cdevs; };

    // Local topology: binder callers and poller never share mutable zone indices.
    static Topology Discover() {
        Topology result;
        DIR* d = opendir("/sys/class/thermal");
        if (d == nullptr) {
            PLOG(ERROR) << "打不开 /sys/class/thermal";
            return result;
        }
        std::vector<std::string> zone_dirs, cdev_dirs;
        while (dirent* e = readdir(d)) {
            std::string n(e->d_name);
            if (n.rfind("thermal_zone", 0) == 0) {
                zone_dirs.push_back(n);
            } else if (n.rfind("cooling_device", 0) == 0) {
                cdev_dirs.push_back(n);
            }
        }
        closedir(d);
        std::sort(zone_dirs.begin(), zone_dirs.end());
        std::sort(cdev_dirs.begin(), cdev_dirs.end());

        const std::string suffix = "-thermal";
        for (const auto& n : zone_dirs) {
            std::string path = "/sys/class/thermal/" + n;
            std::string type;
            if (!ReadTrimmed(path + "/type", &type)) continue;
            Zone z{path, type, TemperatureType::UNKNOWN, gaokun_thermal::kDieHot};
            switch (gaokun_thermal::Classify(type)) {
                case gaokun_thermal::Sensor::Cpu: z.type = TemperatureType::CPU; break;
                case gaokun_thermal::Sensor::Gpu: z.type = TemperatureType::GPU; break;
                case gaokun_thermal::Sensor::Skin: {
                    auto hot = ReadSkinThresholds(path);
                    if (!hot) continue; // Unsupported policy is not a safe temperature.
                    z.type = TemperatureType::SKIN;
                    z.hot = *hot;
                    break;
                }
                case gaokun_thermal::Sensor::Unknown: break;
            }
            if (z.name.size() > suffix.size() &&
                z.name.compare(z.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                z.name = z.name.substr(0, z.name.size() - suffix.size());
            }
            result.zones.push_back(z);
        }

        for (const auto& n : cdev_dirs) {
            std::string path = "/sys/class/thermal/" + n;
            std::string type;
            if (!ReadTrimmed(path + "/type", &type)) continue;
            Cdev c{path, type, CoolingType::COMPONENT};
            if (type.find("cpufreq") != std::string::npos) {
                c.type = CoolingType::CPU;
            } else if (type.find("gpu") != std::string::npos) {
                c.type = CoolingType::GPU;
            }
            result.cdevs.push_back(c);
        }
        return result;
    }

    float SkinTempC() {
        float m = NAN;
        for (const auto& z : Discover().zones) {
            if (z.type != TemperatureType::SKIN) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            if (std::isnan(m) || c > m) m = c;
        }
        return m;
    }

    std::vector<Temperature> Snapshot(std::optional<TemperatureType> filter) {
        std::vector<Temperature> out;
        for (const auto& z : Discover().zones) {
            if (filter.has_value() && filter.value() != z.type) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            Temperature t;
            t.type = z.type;
            t.name = z.name;
            t.value = c;
            t.throttlingStatus = static_cast<ThrottlingSeverity>(gaokun_thermal::Severity(c, z.hot));
            out.push_back(t);
        }
        return out;
    }

    std::vector<TemperatureThreshold> Thresholds(std::optional<TemperatureType> filter) {
        std::vector<TemperatureThreshold> out;
        auto add = [&](const Zone& z) {
            auto t = z.type;
            const auto& n = z.name;
            if (filter.has_value() && filter.value() != t) return;
            TemperatureThreshold th;
            th.type = t;
            th.name = n;
            th.hotThrottlingThresholds.assign(z.hot.begin(), z.hot.end());
            th.coldThrottlingThresholds.assign(std::begin(kCold), std::end(kCold));
            out.push_back(th);
        };
        for (const auto& z : Discover().zones) add(z);
        return out;
    }

    std::vector<CoolingDevice> CdevSnapshot(std::optional<CoolingType> filter) {
        std::vector<CoolingDevice> out;
        for (const auto& c : Discover().cdevs) {
            if (filter.has_value() && filter.value() != c.type) continue;
            std::string s;
            int64_t v = 0;
            if (!ReadTrimmed(c.path + "/cur_state", &s)) continue;
            if (!::android::base::ParseInt(s, &v)) continue;
            CoolingDevice cd;
            cd.type = c.type;
            cd.name = c.name;
            cd.value = v;
            out.push_back(cd);
        }
        return out;
    }

    ndk::ScopedAStatus AddCb(const std::shared_ptr<IThermalChangedCallback>& cb,
                             std::optional<TemperatureType> filter) {
        if (cb == nullptr) {
            return ndk::ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                                    "callback is null");
        }
        std::lock_guard<std::mutex> lk(cb_mutex_);
        for (const auto& e : cbs_) {
            if (e.cb->asBinder().get() == cb->asBinder().get()) {
                return ndk::ScopedAStatus::fromExceptionCodeWithMessage(
                        EX_ILLEGAL_ARGUMENT, "callback already registered");
            }
        }
        cbs_.push_back(CbEntry{cb, filter});
        return ndk::ScopedAStatus::ok();
    }

    // 只在【等级变化】时回调，不是每次采样都喊 —— 否则 system_server 会被刷爆。
    void PollLoop() {
        using Key = std::pair<TemperatureType, std::string>;
        std::map<Key, ThrottlingSeverity> last;
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::vector<Temperature> temps = Snapshot(std::nullopt);
            std::map<Key, ThrottlingSeverity> current;
            for (const auto& t : temps) {
                Key key{t.type, t.name};
                current[key] = t.throttlingStatus;
                auto previous = last.find(key);
                if (previous != last.end() && previous->second == t.throttlingStatus) continue;
                LOG(INFO) << t.name << " 等级变为 " << static_cast<int>(t.throttlingStatus) << "（"
                          << t.value << " C）";
                std::vector<CbEntry> callbacks;
                {
                    std::lock_guard<std::mutex> lk(cb_mutex_);
                    callbacks = cbs_;
                }
                // Never hold the registration mutex across a remote Binder call.
                for (const auto& e : callbacks) {
                    if (e.filter.has_value() && e.filter.value() != t.type) continue;
                    e.cb->notifyThrottling(t);
                }
            }
            last = std::move(current); // Reappearance emits an initial callback.
        }
    }

    std::mutex cb_mutex_;
    std::vector<CbEntry> cbs_;
    std::thread poller_;
};

}  // namespace aidl::android::hardware::thermal::impl::gaokun3

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto svc = ndk::SharedRefBase::make<aidl::android::hardware::thermal::impl::gaokun3::Thermal>();
    const std::string name =
            std::string(aidl::android::hardware::thermal::BnThermal::descriptor) + "/default";
    binder_status_t st = AServiceManager_addService(svc->asBinder().get(), name.c_str());
    CHECK_EQ(st, STATUS_OK) << "注册 " << name << " 失败: " << st;
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // 不该走到这
}
