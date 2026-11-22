/*
 * gaokun3的温控HAL：读取/sys/class/thermal，替代AOSP的mock。
 *
 * ⚠️ 阈值必须按本机重新定：AOSP的mock（android.hardware.thermal-service.example）报的
 *   skin/battery SHUTDOWN阈值只有36.0 °C，而ThermalManagerService一到SHUTDOWN就直接关机；
 *   本机温区空载就有36–37 °C。CPU/GPU等温区按裸片温度分级（kHot），SKIN的阈值见下。
 *
 * ★ SKIN：内核带有板载热敏电阻温区sys-therm1–4（patches/kernel/dts/0031）时，四路各作为一个
 *   SKIN温度上报，阈值以各自当前最低的被动降频点t为准：LIGHT=t-2、MODERATE=t（内核从这里开始
 *   降频）、SEVERE=t+4（降频已到最低保留性能附近）、CRITICAL=t+8、EMERGENCY=t+14、
 *   SHUTDOWN=95 °C（低于内核在这几路上的100 °C紧急关机点）。sys-therm3最低的降频点只限制GPU，
 *   它的等级因此跟随GPU降频点，CPU要到CRITICAL附近才开始受它限制。框架取所有SKIN中最高的等级
 *   作为整机状态，并逐路按各自的SEVERE计算温度余量。这几路都不是直接测得的外壳温度。
 *   等级升高立即生效，降低要等温度比当前等级的阈值低2 °C（与设备树里降频点的回差相同），
 *   否则在内核降频的平衡点附近会反复跳变。
 *   内核没有这些温区时，SKIN取所有CPU/GPU温区的最大值，按裸片温度分级。
 *
 * ★ 降频点档位：内核按本机Windows ACPI表的开机默认值设好sys-therm1–4的降频点。这里按性能模式
 *   与电源状态改写它们（数值见kSetA、kSetB与kPerf*）。降频本身仍由内核thermal core完成，
 *   这里只改降频点，并在改写后从内核读回实际值来计算SKIN阈值。
 *
 * ★ 接电源与用电池的CPU设置：Windows的处理器电源管理在接电源时给两类核心设较高的性能下限，用电池时
 *   降低下限，并限制后台线程与部分非焦点窗口的频率。本机由Power HAL（lineage-libperfmgr）按power/powerhint.json的AC_POWER模式调整，
 *   这里只按gaokun-ec-adapter的online通过Power HAL的扩展接口开关这个模式（见AcPowerHint）。
 *
 * ★ 内核的最后一道闸：8个cpu温区各有85 °C passive（patches/kernel/dts/0002）和110 °C critical。
 *   CPU/GPU的SHUTDOWN定在105，比内核critical低：先由Android有序关机，再轮到内核紧急断电。
 */
#include <aidl/android/hardware/thermal/BnThermal.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>
#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <dirent.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::thermal::impl::gaokun3 {

using ::aidl::google::hardware::power::extension::pixel::IPowerExt;
using ::android::base::ReadFileToString;
using ::android::base::Trim;

using Levels = std::array<float, 7>;

// 阈值顺序与ThrottlingSeverity一一对应：
//   NONE / LIGHT / MODERATE / SEVERE / CRITICAL / EMERGENCY / SHUTDOWN
// CPU/GPU等温区按裸片温度分级。
static constexpr Levels kHot = {NAN, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f, 105.0f};
static constexpr Levels kNone = {NAN, NAN, NAN, NAN, NAN, NAN, NAN};

// 读数合理性钳位：传感器读数异常时不能让它把机器关掉。
static constexpr float kSaneMin = -40.0f;
static constexpr float kSaneMax = 150.0f;

// ── 板载热敏电阻的降频点档位 ──────────────────────────────────────────────
// 数值来自本机Windows的ACPI表（°C，已取整）：
//   A = DSDT开机默认值（TZ31–34、TZ70），也是内核设备树里的值。未接电源时使用；Windows用电池时
//       的数值更保守（SSDT里EC事件0xC0的D9T8=0分支，如sys-therm2为44），这里没有采用。
//   B = SSDT里EC事件0xC0的D9T8=1、D9TD=0分支，按推测对应接电源。
//   C = SSDT里WMI方法THMS的性能模式，只改TZ31–33；sys-therm3的GPU降频点与sys-therm4仍随A/B。
struct TripSet {
    float th1;      // sys-therm1 trip 0：CPU + GPU
    float th2;      // sys-therm2 trip 0：CPU + GPU
    float th3_gpu;  // sys-therm3 trip 0：仅GPU
    float th3;      // sys-therm3 trip 1：CPU + GPU
    float th4;      // sys-therm4 trip 0：CPU + GPU
};
static constexpr TripSet kSetA = {54.0f, 58.0f, 58.0f, 65.0f, 64.0f};
static constexpr TripSet kSetB = {54.0f, 58.0f, 57.0f, 70.0f, 63.0f};
static constexpr float kPerfTh1 = 60.0f, kPerfTh2 = 64.0f, kPerfTh3 = 68.0f;

// 「设置 → 电池」里的性能模式开关写persist.sys.gaokun3.perf_mode，init（本目录的.rc）把它转成
// 这个vendor属性，因为vendor进程无法读取system_prop。
static constexpr char kPerfModeProp[] = "vendor.gaokun3.perf_mode";
// EC的电源适配器节点。无法读取时按未接电源处理（使用开机默认值A）。
static constexpr char kAdapterOnline[] = "/sys/class/power_supply/gaokun-ec-adapter/online";
// Power HAL及其扩展接口里接电源模式的名称，与power/powerhint.json一致。
static constexpr char kPowerHal[] = "android.hardware.power.IPower/default";
static constexpr char kAcPowerMode[] = "AC_POWER";
static constexpr const char* kBoardZones[] = {"sys-therm1-thermal", "sys-therm2-thermal",
                                              "sys-therm3-thermal", "sys-therm4-thermal"};
// 板载温区SKIN阈值的SHUTDOWN：低于内核在sys-therm1/2/4上的100 °C紧急关机点。
static constexpr float kSkinShutdown = 95.0f;
// 读回的降频点超出这个范围时视为无效，该温区不分级。
static constexpr float kTripMin = 30.0f, kTripMax = 90.0f;
// 降级回差，与设备树里被动降频点的hysteresis相同。
static constexpr float kHyst = 2.0f;

struct Zone {
    std::string path;   // /sys/class/thermal/thermal_zoneN
    std::string name;   // 上报给框架的名字（去掉-thermal后缀）
    TemperatureType type;
    bool feeds_skin;    // 是否参与回退模式下合成SKIN的取最大值
    bool board;         // 板载热敏电阻温区：作为SKIN上报，阈值随降频点变化
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

// 返回摄氏度；读不到或不合理返回NAN（调用方据此跳过）
static float ReadTempC(const std::string& zone_path) {
    std::string s;
    if (!ReadTrimmed(zone_path + "/temp", &s)) return NAN;
    int64_t milli = 0;
    if (!::android::base::ParseInt(s, &milli)) return NAN;
    float c = static_cast<float>(milli) / 1000.0f;
    if (c < kSaneMin || c > kSaneMax) {
        LOG(WARNING) << "ignoring implausible reading " << zone_path << " = " << c << " C";
        return NAN;
    }
    return c;
}

static ThrottlingSeverity SeverityFor(float c, const Levels& hot) {
    if (std::isnan(c)) return ThrottlingSeverity::NONE;
    for (int i = 6; i >= 1; --i) {
        if (!std::isnan(hot[i]) && c >= hot[i]) return static_cast<ThrottlingSeverity>(i);
    }
    return ThrottlingSeverity::NONE;
}

// 升级立即生效；降级要等温度比当前等级的阈值低kHyst以上。
static ThrottlingSeverity SeverityWithHyst(float c, const Levels& hot, ThrottlingSeverity prev) {
    const ThrottlingSeverity up = SeverityFor(c, hot);
    if (up >= prev) return up;
    return std::min(prev, SeverityFor(c + kHyst, hot));
}

static int ToMilli(float c) {
    return static_cast<int>(std::lround(c * 1000.0f));
}

// 温区当前最低的被动降频点（°C）；没有被动降频点或无法读取时返回NAN。
static float LowestPassiveTripC(const std::string& zone_path) {
    float low = NAN;
    for (int i = 0;; ++i) {
        const std::string base = zone_path + "/trip_point_" + std::to_string(i);
        std::string kind, temp;
        if (!ReadTrimmed(base + "_type", &kind)) break;
        int64_t milli = 0;
        if (kind != "passive" || !ReadTrimmed(base + "_temp", &temp) ||
            !::android::base::ParseInt(temp, &milli)) {
            continue;
        }
        const float c = static_cast<float>(milli) / 1000.0f;
        if (std::isnan(low) || c < low) low = c;
    }
    return low;
}

// 逐项比较两组阈值，NAN与NAN视为相同。
static bool SameLevels(const Levels& a, const Levels& b) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i]) != std::isnan(b[i])) return false;
        if (!std::isnan(a[i]) && a[i] != b[i]) return false;
    }
    return true;
}

static Levels BoardLevels(float t) {
    if (std::isnan(t) || t < kTripMin || t > kTripMax) return kNone;
    return {NAN, t - 2.0f, t, t + 4.0f, t + 8.0f, t + 14.0f, kSkinShutdown};
}

static bool AdapterOnline() {
    std::string online;
    return ReadTrimmed(kAdapterOnline, &online) && online == "1";
}

// 按电源状态开关Power HAL的AC_POWER模式。电源状态需连续两次读数一致才发送，启动后的第一次也是
// 如此。Power HAL重启后模式状态随之丢失，所以监听它的binder死亡，重新连上后按当前状态再发送一次。
// Power HAL明确没有扩展接口或不支持这个模式时（例如AOSP示例HAL），直到它下次重启前不再尝试；
// 调用本身失败时下一轮重试。连接时的几次同步调用没有超时，所以连接与发送都在单独的线程上进行：
// 轮询线程只交出读数，Power HAL无响应时，温控等级的上报与降频点档位的切换不受影响。工作线程忙时
// 只保留最近一次读数，所以等待Power HAL期间电源状态的变化要在它恢复后再确认两轮才发送。
class AcPowerHint {
  public:
    AcPowerHint() : death_(AIBinder_DeathRecipient_new(OnDied)) {
        // cookie非空而未设置onUnlinked回调时，libbinder_ndk每次链接都会记录警告。cookie指向本对象，
        // 本对象与进程同寿，回调无需释放任何资源。
        AIBinder_DeathRecipient_setOnUnlinked(death_.get(), [](void*) {});
        worker_ = std::thread([this] { Run(); });
        worker_.detach();
    }

    // 由轮询线程调用，不等待：只记下本次读到的电源状态，交给工作线程处理。
    void Post(bool adapter) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            reading_ = adapter;
            ++posted_;
        }
        cv_.notify_one();
    }

  private:
    void Run() {
        uint64_t handled = 0;
        while (true) {
            bool adapter;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [&] { return posted_ != handled; });
                adapter = reading_;
                handled = posted_;
            }
            Update(adapter);
        }
    }

    // 只在工作线程上调用，adapter为最近一次读到的电源状态。
    void Update(bool adapter) {
        const bool stable = last_read_.has_value() && *last_read_ == adapter;
        last_read_ = adapter;
        if (dead_.exchange(false)) {
            power_ = ndk::SpAIBinder();
            ext_ = nullptr;
            unsupported_ = false;
            failed_ = false;
            sent_.reset();
        }
        if (!stable || !Connect() || sent_ == adapter) return;
        ndk::ScopedAStatus st = ext_->setMode(kAcPowerMode, adapter);
        if (!st.isOk()) {
            // 多为Power HAL恰好退出；死亡通知随后到达，下一轮重新连接。
            LOG(ERROR) << "setMode(" << kAcPowerMode << ") failed: " << st.getDescription();
            return;
        }
        sent_ = adapter;
        LOG(INFO) << "power mode " << kAcPowerMode << (adapter ? " on" : " off");
    }

    // 在binder线程上调用。
    static void OnDied(void* cookie) { static_cast<AcPowerHint*>(cookie)->dead_ = true; }

    bool Connect() {
        if (ext_ != nullptr) return true;
        if (unsupported_) return false;
        // 不阻塞：Power HAL尚未注册时下一轮再试。
        ndk::SpAIBinder power(AServiceManager_checkService(kPowerHal));
        if (power.get() == nullptr) return false;
        // 同一个binder只链接一次死亡通知；调用失败后重试时不重复链接。
        if (power.get() != power_.get()) {
            if (AIBinder_linkToDeath(power.get(), death_.get(), this) != STATUS_OK) return false;
            power_ = power;
        }
        AIBinder* raw = nullptr;
        if (AIBinder_getExtension(power.get(), &raw) != STATUS_OK) return Failed("getExtension");
        if (raw == nullptr) return Unsupported();
        ndk::SpAIBinder ext_binder(raw);
        std::shared_ptr<IPowerExt> ext = IPowerExt::fromBinder(ext_binder);
        // fromBinder会向扩展接口的binder查询接口描述符；对方已退出时同样返回空，这时等死亡通知后重连。
        if (ext == nullptr) {
            return AIBinder_isAlive(ext_binder.get()) ? Unsupported() : Failed("fromBinder");
        }
        bool supported = false;
        if (!ext->isModeSupported(kAcPowerMode, &supported).isOk()) return Failed("isModeSupported");
        if (!supported) return Unsupported();
        ext_ = ext;
        failed_ = false;
        return true;
    }

    bool Unsupported() {
        LOG(WARNING) << kPowerHal << " does not support mode " << kAcPowerMode;
        unsupported_ = true;
        return false;
    }

    // 日志只记一次，直到连接成功或Power HAL重启。
    bool Failed(const char* call) {
        if (!failed_) LOG(ERROR) << call << " on " << kPowerHal << " failed; retrying";
        failed_ = true;
        return false;
    }

    ndk::ScopedAIBinder_DeathRecipient death_;
    std::atomic<bool> dead_{false};
    ndk::SpAIBinder power_;  // 持有引用，死亡通知才保持有效
    std::shared_ptr<IPowerExt> ext_;
    bool unsupported_ = false;
    bool failed_ = false;  // 已记过调用失败的日志
    std::optional<bool> last_read_;
    std::optional<bool> sent_;  // 已发送给当前Power HAL的状态
    // 轮询线程交给工作线程的读数
    std::mutex mutex_;
    std::condition_variable cv_;
    bool reading_ = false;
    uint64_t posted_ = 0;
    std::thread worker_;
};

class Thermal : public BnThermal {
  public:
    Thermal() {
        Discover();
        UpdateTripProfile(AdapterOnline());
        poller_ = std::thread([this] { PollLoop(); });
        poller_.detach();
    }

    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out) override {
        *out = Snapshot(std::nullopt, false);
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType t,
                                               std::vector<Temperature>* out) override {
        *out = Snapshot(t, false);
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

    // 本机的cooling device由内核thermal core自己驱动，没有可上报的变更事件源。
    // 如实回答不支持，不假装注册成功。
    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>&, CoolingType) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>&) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    // 没有预测模型。返回不支持时，ThermalManagerService按各路SKIN的历史读数自行外推，
    // 比返回当前值（平坦外推）更合理。
    ndk::ScopedAStatus forecastSkinTemperature(int32_t, float*) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

  private:
    struct CbEntry {
        std::shared_ptr<IThermalChangedCallback> cb;
        std::optional<TemperatureType> filter;
    };

    void Discover() {
        DIR* d = opendir("/sys/class/thermal");
        if (d == nullptr) {
            PLOG(ERROR) << "cannot open /sys/class/thermal";
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
            Zone z{path, type, TemperatureType::UNKNOWN, false, false};
            if (type.rfind("cpu", 0) == 0 || type.rfind("cluster", 0) == 0) {
                z.type = TemperatureType::CPU;
                z.feeds_skin = true;
            } else if (type.rfind("gpu", 0) == 0) {
                z.type = TemperatureType::GPU;
                z.feeds_skin = true;
            } else if (std::find(std::begin(kBoardZones), std::end(kBoardZones), type) !=
                       std::end(kBoardZones)) {
                z.board = true;
            }
            // mem/pmic/sys-therm7等如实报为UNKNOWN。
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

        // 四路板载温区齐全才把它们作为SKIN；缺任何一路就整体回退到CPU/GPU温区的最大值。
        int board = 0;
        for (const auto& z : zones_) board += z.board ? 1 : 0;
        board_skin_ = (board == static_cast<int>(std::size(kBoardZones)));
        for (auto& z : zones_) {
            if (!z.board) continue;
            if (board_skin_) {
                z.type = TemperatureType::SKIN;
            } else {
                z.board = false;
            }
        }
        hot_.assign(zones_.size(), kHot);
        status_.assign(zones_.size() + 1, ThrottlingSeverity::NONE);
        RefreshBoardLevels(false);
        LOG(INFO) << "found " << zones_.size() << " thermal zones, " << cdevs_.size()
                  << " cooling devices; skin source: "
                  << (board_skin_ ? "board thermistors" : "CPU/GPU die zones");
    }

    const Zone* FindZone(const std::string& type) const {
        for (const auto& z : zones_) {
            if (z.name + "-thermal" == type) return &z;
        }
        return nullptr;
    }

    // 只写类型为passive的降频点；与目标值相同就不写。verbose为false时失败不打日志。
    bool SetTrip(const std::string& zone_type, int trip, float c, bool verbose) {
        const Zone* z = FindZone(zone_type);
        if (z == nullptr) return false;
        const std::string base = z->path + "/trip_point_" + std::to_string(trip);
        std::string kind, cur;
        if (!ReadTrimmed(base + "_type", &kind)) {
            if (verbose) PLOG(ERROR) << "cannot read " << base << "_type";
            return false;
        }
        if (kind != "passive") {
            if (verbose) {
                LOG(ERROR) << zone_type << " trip " << trip << " is " << kind
                           << ", not passive; not changing it";
            }
            return false;
        }
        const std::string want = std::to_string(ToMilli(c));
        if (ReadTrimmed(base + "_temp", &cur) && cur == want) return true;
        if (!::android::base::WriteStringToFile(want, base + "_temp")) {
            if (verbose) PLOG(ERROR) << "cannot write " << base << "_temp";
            return false;
        }
        return true;
    }

    // 从内核读回各板载温区当前最低的被动降频点，更新它们的SKIN阈值；有变化且notify为true时通知框架。
    void RefreshBoardLevels(bool notify) {
        if (!board_skin_) return;
        std::vector<TemperatureThreshold> changed;
        {
            std::lock_guard<std::mutex> lk(hot_mutex_);
            for (size_t i = 0; i < zones_.size(); ++i) {
                if (!zones_[i].board) continue;
                const Levels lv = BoardLevels(LowestPassiveTripC(zones_[i].path));
                if (SameLevels(lv, hot_[i])) continue;
                hot_[i] = lv;
                changed.push_back(MakeThreshold(zones_[i], lv));
            }
        }
        if (!notify || changed.empty()) return;
        std::lock_guard<std::mutex> lk(cb_mutex_);
        for (const auto& th : changed) {
            for (const auto& e : cbs_) {
                if (e.filter.has_value() && e.filter.value() != TemperatureType::SKIN) continue;
                e.cb->notifyThresholdChanged(th);
            }
        }
    }

    // 按性能模式属性与电源状态选出档位；档位变化需连续两次读数一致才生效（首次除外），
    // 以免插头接触不良时反复改写降频点。写入后无论成败都从内核读回降频点更新SKIN阈值。
    // adapter为本轮读到的电源状态。
    void UpdateTripProfile(bool adapter) {
        if (!board_skin_) return;
        const bool perf = ::android::base::GetProperty(kPerfModeProp, "") == "performance";
        // 性能模式下sys-therm3的GPU降频点与sys-therm4仍随电源状态变化，所以档位键包含两者。
        const int key = (perf ? 2 : 0) + (adapter ? 1 : 0);
        if (key == profile_) {
            pending_ = -1;
            return;
        }
        if (profile_ >= 0 && key != pending_) {
            pending_ = key;
            return;
        }
        pending_ = -1;

        TripSet t = adapter ? kSetB : kSetA;
        if (perf) {
            t.th1 = kPerfTh1;
            t.th2 = kPerfTh2;
            t.th3 = kPerfTh3;
        }
        const bool verbose = (key != failed_);
        bool ok = SetTrip("sys-therm1-thermal", 0, t.th1, verbose);
        ok = SetTrip("sys-therm2-thermal", 0, t.th2, verbose) && ok;
        ok = SetTrip("sys-therm3-thermal", 0, t.th3_gpu, verbose) && ok;
        ok = SetTrip("sys-therm3-thermal", 1, t.th3, verbose) && ok;
        ok = SetTrip("sys-therm4-thermal", 0, t.th4, verbose) && ok;
        static constexpr const char* kNames[] = {"A (battery)", "B (adapter)",
                                                 "C (performance, battery)",
                                                 "C (performance, adapter)"};
        if (ok) {
            profile_ = key;
            failed_ = -1;
            LOG(INFO) << "trip profile " << kNames[key] << ": " << t.th1 << "/" << t.th2 << "/"
                      << t.th3_gpu << "/" << t.th3 << "/" << t.th4 << " C";
        } else {
            // 失败多为配置问题（DAC、SELinux、设备树），会一直存在。把profile_置为-1，下一轮
            // 不经确认就按当时的档位整套重写，以免部分写入后停在新旧混合的降频点上；日志只记一次。
            profile_ = -1;
            if (verbose) {
                failed_ = key;
                LOG(ERROR) << "trip profile " << kNames[key] << " not fully applied; retrying";
            }
        }
        RefreshBoardLevels(true);
    }

    static TemperatureThreshold MakeThreshold(const Zone& z, const Levels& hot) {
        TemperatureThreshold th;
        th.type = z.type;
        th.name = z.name;
        th.hotThrottlingThresholds.assign(hot.begin(), hot.end());
        th.coldThrottlingThresholds.assign(kNone.begin(), kNone.end());
        return th;
    }

    Levels ZoneLevels(size_t i) {
        std::lock_guard<std::mutex> lk(hot_mutex_);
        return hot_[i];
    }

    // 带回差的等级。只有轮询线程（update为true）推进记录的等级，binder调用只按它计算。
    ThrottlingSeverity ZoneSeverity(size_t slot, float c, const Levels& hot, bool update) {
        std::lock_guard<std::mutex> lk(hot_mutex_);
        const ThrottlingSeverity s = SeverityWithHyst(c, hot, status_[slot]);
        if (update) status_[slot] = s;
        return s;
    }

    // 回退模式下合成的SKIN：所有CPU/GPU温区的最大值。
    float FallbackSkinC() {
        float m = NAN;
        for (const auto& z : zones_) {
            if (!z.feeds_skin) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            if (std::isnan(m) || c > m) m = c;
        }
        return m;
    }

    std::vector<Temperature> Snapshot(std::optional<TemperatureType> filter, bool update) {
        std::vector<Temperature> out;
        for (size_t i = 0; i < zones_.size(); ++i) {
            const Zone& z = zones_[i];
            if (filter.has_value() && filter.value() != z.type) continue;
            float c = ReadTempC(z.path);
            if (std::isnan(c)) continue;
            Temperature t;
            t.type = z.type;
            t.name = z.name;
            t.value = c;
            t.throttlingStatus = ZoneSeverity(i, c, ZoneLevels(i), update);
            out.push_back(t);
        }
        if (!board_skin_ && (!filter.has_value() || filter.value() == TemperatureType::SKIN)) {
            float c = FallbackSkinC();
            if (!std::isnan(c)) {
                Temperature t;
                t.type = TemperatureType::SKIN;
                t.name = "skin";
                t.value = c;
                t.throttlingStatus = ZoneSeverity(zones_.size(), c, kHot, update);
                out.push_back(t);
            }
        }
        return out;
    }

    std::vector<TemperatureThreshold> Thresholds(std::optional<TemperatureType> filter) {
        std::vector<TemperatureThreshold> out;
        for (size_t i = 0; i < zones_.size(); ++i) {
            if (filter.has_value() && filter.value() != zones_[i].type) continue;
            out.push_back(MakeThreshold(zones_[i], ZoneLevels(i)));
        }
        if (!board_skin_ && (!filter.has_value() || filter.value() == TemperatureType::SKIN)) {
            out.push_back(MakeThreshold(Zone{"", "skin", TemperatureType::SKIN, false, false}, kHot));
        }
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

    // 只在等级变化时回调，不是每次采样都通知，否则system_server会被刷爆。
    void PollLoop() {
        std::vector<ThrottlingSeverity> last(zones_.size() + 1, ThrottlingSeverity::NONE);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const bool adapter = AdapterOnline();  // 每次读取都是一次EC事务，每轮只读一次
            UpdateTripProfile(adapter);
            ac_hint_.Post(adapter);
            std::vector<Temperature> temps = Snapshot(std::nullopt, true);
            for (const auto& t : temps) {
                size_t idx = zones_.size();  // 末位留给回退模式的合成SKIN
                for (size_t i = 0; i < zones_.size(); ++i) {
                    if (zones_[i].name == t.name) {
                        idx = i;
                        break;
                    }
                }
                if (t.throttlingStatus == last[idx]) continue;
                last[idx] = t.throttlingStatus;
                LOG(INFO) << t.name << " severity " << static_cast<int>(t.throttlingStatus) << " ("
                          << t.value << " C)";
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
    bool board_skin_ = false;
    int profile_ = -1;  // 已生效的档位键：0 = A，1 = B，2/3 = C（未接/接电源）；-1 = 尚未生效
    int pending_ = -1;  // 等待第二次读数确认的档位键
    int failed_ = -1;   // 上次写入失败且已记过日志的档位键
    std::mutex hot_mutex_;
    std::vector<Levels> hot_;  // 每个温区的分级阈值，板载温区随降频点变化
    std::vector<ThrottlingSeverity> status_;  // 每个温区（末位为回退模式的合成SKIN）当前的等级
    std::mutex cb_mutex_;
    std::vector<CbEntry> cbs_;
    std::thread poller_;
    AcPowerHint ac_hint_;
};

}  // namespace aidl::android::hardware::thermal::impl::gaokun3

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto svc = ndk::SharedRefBase::make<aidl::android::hardware::thermal::impl::gaokun3::Thermal>();
    const std::string name =
            std::string(aidl::android::hardware::thermal::BnThermal::descriptor) + "/default";
    binder_status_t st = AServiceManager_addService(svc->asBinder().get(), name.c_str());
    CHECK_EQ(st, STATUS_OK) << "failed to register " << name << ": " << st;
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
