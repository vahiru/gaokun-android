/*
 * gaokun3 的温控 HAL —— 读 /sys/class/thermal，替掉 AOSP 那个 mock。
 *
 * ⚠️★★ 为什么这件事有危险，以及这里是怎么处理的：
 *   AOSP 的 mock（android.hardware.thermal-service.example）报的 skin/battery
 *   SHUTDOWN 阈值只有 36.0 °C，而 ThermalManagerService.shutdownIfNeeded()
 *   一到 SHUTDOWN 就直接 powerManager.shutdown()。
 *   本机温区【空载就是 36–37 °C】—— 也就是说，只要换成报真实温度的 HAL 而
 *   不同时改阈值，机器会开机几分钟就自动关机。本仓从 M4 起就记着这颗地雷。
 *   ⇒ 这里的阈值全部按【裸片温度】重新定，见下面 kHot。
 *
 * ★ 关于 SKIN：本机【没有】外壳热敏电阻。但框架的 getThermalHeadroom()
 *   （游戏用来自我降档的那个）只看 SKIN，不报的话它返回 NaN，等于没有。
 *   折中：SKIN 取所有 CPU/GPU 温区的最大值，并明确按裸片温度定阈值。
 *   这不是真的外壳温度，是一个诚实标注过的代理值。
 *
 * ★ 内核仍是最后一道闸：sc8280xp.dtsi 里 8 个 cpu 温区各有 85 °C passive
 *   （patches/kernel/dts/0002 加的 cooling-maps）和 110 °C critical。我们的 SHUTDOWN
 *   定在 105，比内核的 critical 低 —— 顺序是对的：先由 Android 有序关机，
 *   真到 110 才轮到内核紧急断电。
 */
#include <aidl/android/hardware/thermal/BnThermal.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <dirent.h>

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

// 阈值顺序与 ThrottlingSeverity 一一对应：
//   NONE / LIGHT / MODERATE / SEVERE / CRITICAL / EMERGENCY / SHUTDOWN
// ⚠️ 这是【裸片】温度，不是外壳温度。理由见文件头。
static constexpr float kHot[7] = {NAN, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f, 105.0f};
static constexpr float kCold[7] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN};

// ★ 读数合理性钳位：传感器抽风时不能让它把机器关掉。
static constexpr float kSaneMin = -40.0f;
static constexpr float kSaneMax = 150.0f;

struct Zone {
    std::string path;   // /sys/class/thermal/thermal_zoneN
    std::string name;   // 上报给框架的名字（去掉 -thermal 后缀）
    TemperatureType type;
    bool feeds_skin;    // 是否参与 SKIN 的取最大值
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
    int64_t milli = 0;
    if (!::android::base::ParseInt(s, &milli)) return NAN;
    float c = static_cast<float>(milli) / 1000.0f;
    if (c < kSaneMin || c > kSaneMax) {
        LOG(WARNING) << "忽略不合理读数 " << zone_path << " = " << c << " C";
        return NAN;
    }
    return c;
}

static ThrottlingSeverity SeverityFor(float c) {
    if (std::isnan(c)) return ThrottlingSeverity::NONE;
    for (int i = 6; i >= 1; --i) {
        if (!std::isnan(kHot[i]) && c >= kHot[i]) return static_cast<ThrottlingSeverity>(i);
    }
    return ThrottlingSeverity::NONE;
}

class Thermal : public BnThermal {
  public:
    Thermal() {
        Discover();
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
    ndk::ScopedAStatus forecastSkinTemperature(int32_t, float* out) override {
        float c = SkinTempC();
        if (std::isnan(c)) return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        *out = c;
        return ndk::ScopedAStatus::ok();
    }

  private:
    struct CbEntry {
        std::shared_ptr<IThermalChangedCallback> cb;
        std::optional<TemperatureType> filter;
    };

    void Discover() {
        DIR* d = opendir("/sys/class/thermal");
        if (d == nullptr) {
            PLOG(ERROR) << "打不开 /sys/class/thermal";
            return;
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
            Zone z{path, type, TemperatureType::UNKNOWN, false};
            if (type.rfind("cpu", 0) == 0 || type.rfind("cluster", 0) == 0) {
                z.type = TemperatureType::CPU;
                z.feeds_skin = true;
            } else if (type.rfind("gpu", 0) == 0) {
                z.type = TemperatureType::GPU;
                z.feeds_skin = true;
            }
            // mem/pmic 等如实报为 UNKNOWN，不参与 SKIN。
            if (z.name.size() > suffix.size() &&
                z.name.compare(z.name.size() - suffix.size(), suffix.size(), suffix) == 0) {
                z.name = z.name.substr(0, z.name.size() - suffix.size());
            }
            zones_.push_back(z);
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
            cdevs_.push_back(c);
        }
        LOG(INFO) << "发现 " << zones_.size() << " 个温区、" << cdevs_.size() << " 个冷却设备";
    }

    float SkinTempC() {
        float m = NAN;
        for (const auto& z : zones_) {
            if (!z.feeds_skin) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            if (std::isnan(m) || c > m) m = c;
        }
        return m;
    }

    std::vector<Temperature> Snapshot(std::optional<TemperatureType> filter) {
        std::vector<Temperature> out;
        for (const auto& z : zones_) {
            if (filter.has_value() && filter.value() != z.type) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            Temperature t;
            t.type = z.type;
            t.name = z.name;
            t.value = c;
            t.throttlingStatus = SeverityFor(c);
            out.push_back(t);
        }
        if (!filter.has_value() || filter.value() == TemperatureType::SKIN) {
            float c = SkinTempC();
            if (!std::isnan(c)) {
                Temperature t;
                t.type = TemperatureType::SKIN;
                t.name = "skin";
                t.value = c;
                t.throttlingStatus = SeverityFor(c);
                out.push_back(t);
            }
        }
        return out;
    }

    std::vector<TemperatureThreshold> Thresholds(std::optional<TemperatureType> filter) {
        std::vector<TemperatureThreshold> out;
        auto add = [&](TemperatureType t, const std::string& n) {
            if (filter.has_value() && filter.value() != t) return;
            TemperatureThreshold th;
            th.type = t;
            th.name = n;
            th.hotThrottlingThresholds.assign(std::begin(kHot), std::end(kHot));
            th.coldThrottlingThresholds.assign(std::begin(kCold), std::end(kCold));
            out.push_back(th);
        };
        for (const auto& z : zones_) add(z.type, z.name);
        add(TemperatureType::SKIN, "skin");
        return out;
    }

    std::vector<CoolingDevice> CdevSnapshot(std::optional<CoolingType> filter) {
        std::vector<CoolingDevice> out;
        for (const auto& c : cdevs_) {
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
        std::vector<ThrottlingSeverity> last(zones_.size() + 1, ThrottlingSeverity::NONE);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::vector<Temperature> temps = Snapshot(std::nullopt);
            for (const auto& t : temps) {
                size_t idx = zones_.size();  // 末位留给 skin
                for (size_t i = 0; i < zones_.size(); ++i) {
                    if (zones_[i].name == t.name) {
                        idx = i;
                        break;
                    }
                }
                if (t.throttlingStatus == last[idx]) continue;
                last[idx] = t.throttlingStatus;
                LOG(INFO) << t.name << " 等级变为 " << static_cast<int>(t.throttlingStatus) << "（"
                          << t.value << " C）";
                std::lock_guard<std::mutex> lk(cb_mutex_);
                for (const auto& e : cbs_) {
                    if (e.filter.has_value() && e.filter.value() != t.type) continue;
                    e.cb->notifyThrottling(t);
                }
            }
        }
    }

    std::vector<Zone> zones_;
    std::vector<Cdev> cdevs_;
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
