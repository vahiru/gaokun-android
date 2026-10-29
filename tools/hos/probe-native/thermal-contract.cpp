// Read-only IThermal V3 contract probe. Never registers callbacks or changes policy.
#include <aidl/android/hardware/thermal/IThermal.h>
#include <android/binder_manager.h>
#include <android/binder_status.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

using aidl::android::hardware::thermal::IThermal;
using aidl::android::hardware::thermal::Temperature;
using aidl::android::hardware::thermal::TemperatureThreshold;
using aidl::android::hardware::thermal::TemperatureType;

static int failures = 0;
static void Check(bool pass, const std::string& label) {
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", label.c_str());
    if (!pass) ++failures;
}
static void Status(const char* method, const ndk::ScopedAStatus& status) {
    std::printf("%s status=%d exception=%d message=%s\n", method,
                static_cast<int>(status.getStatus()),
                static_cast<int>(status.getExceptionCode()),
                status.getDescription().c_str());
}
static bool Sane(float c) {
    // Negative Celsius is valid; reject NaN, infinity and implausible readings.
    return std::isfinite(c) && c >= -40.0f && c <= 150.0f;
}
static void PrintThresholds(const char* label, const std::vector<float>& values) {
    std::printf("  %s=[", label);
    for (size_t i = 0; i < values.size(); ++i)
        std::printf("%s%.3f", i ? "," : "", static_cast<double>(values[i]));
    std::printf("]\n");
}
static bool ValidThresholds(const std::vector<float>& values) {
    if (values.size() != 7) return false;
    for (float value : values)
        if (!std::isnan(value) && !Sane(value)) return false;
    return true;
}

int main() {
    const std::string instance = std::string(IThermal::descriptor) + "/default";
    // checkService does not request a lazy service start and does not wait forever.
    auto thermal = IThermal::fromBinder(ndk::SpAIBinder(
            AServiceManager_checkService(instance.c_str())));
    if (!thermal) {
        Check(false, "thermal service already running and accessible");
        return 1;
    }
    int32_t version = 0;
    auto versionStatus = thermal->getInterfaceVersion(&version);
    Status("getInterfaceVersion", versionStatus);
    std::printf("interface_version=%d\n", version);
    Check(versionStatus.isOk() && version >= 3, "interface supports V3");

    std::vector<Temperature> temperatures;
    auto tempStatus = thermal->getTemperaturesWithType(TemperatureType::SKIN, &temperatures);
    Status("getTemperaturesWithType(SKIN)", tempStatus);
    Check(tempStatus.isOk() && !temperatures.empty(), "real SKIN readings available");
    std::set<std::string> names;
    for (const auto& t : temperatures) {
        std::printf("temperature name=%s type=%d value_C=%.3f severity=%d\n",
                    t.name.c_str(), static_cast<int>(t.type), static_cast<double>(t.value),
                    static_cast<int>(t.throttlingStatus));
        const int severity = static_cast<int>(t.throttlingStatus);
        Check(t.type == TemperatureType::SKIN && !t.name.empty() &&
              names.insert(t.name).second && Sane(t.value) && severity >= 0 && severity <= 6,
              "valid unique SKIN reading: " + t.name);
    }

    std::vector<TemperatureThreshold> thresholds;
    auto thresholdStatus = thermal->getTemperatureThresholdsWithType(
            TemperatureType::SKIN, &thresholds);
    Status("getTemperatureThresholdsWithType(SKIN)", thresholdStatus);
    Check(thresholdStatus.isOk() && !thresholds.empty(), "SKIN thresholds available");
    std::set<std::string> thresholdNames;
    for (const auto& t : thresholds) {
        std::printf("threshold name=%s type=%d\n", t.name.c_str(), static_cast<int>(t.type));
        PrintThresholds("hot_C", t.hotThrottlingThresholds);
        PrintThresholds("cold_C", t.coldThrottlingThresholds);
        Check(t.type == TemperatureType::SKIN && !t.name.empty() &&
              thresholdNames.insert(t.name).second &&
              ValidThresholds(t.hotThrottlingThresholds) &&
              ValidThresholds(t.coldThrottlingThresholds),
              "valid unique SKIN threshold arrays: " + t.name);
    }
    for (const auto& name : names)
        Check(thresholdNames.count(name) != 0, "threshold matches reading: " + name);

    for (int32_t seconds : {int32_t{-1}, std::numeric_limits<int32_t>::min(),
                            int32_t{0}, int32_t{60}}) {
        float c = std::numeric_limits<float>::quiet_NaN();
        auto status = thermal->forecastSkinTemperature(seconds, &c);
        Status("forecastSkinTemperature", status);
        std::printf("forecast seconds=%d value_C=%.3f\n", seconds, static_cast<double>(c));
        if (seconds < 0) {
            Check(status.getExceptionCode() == EX_ILLEGAL_ARGUMENT,
                  "negative forecast rejected: " + std::to_string(seconds));
        } else {
            Check(status.isOk() && Sane(c),
                  "forecast finite and within -40..150 C: " + std::to_string(seconds));
        }
    }
    std::printf("RESULT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
