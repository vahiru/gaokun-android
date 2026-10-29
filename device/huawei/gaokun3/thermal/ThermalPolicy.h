#pragma once
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gaokun_thermal {
using Thresholds = std::array<float, 7>;
inline constexpr Thresholds kDieHot = {NAN, 60, 70, 80, 90, 100, 105};
enum class Sensor { Unknown, Cpu, Gpu, Skin };
inline Sensor Classify(const std::string& type) {
    if (type == "skin-temp-thermal") return Sensor::Skin;
    if (type.rfind("cpu", 0) == 0 || type.rfind("cluster", 0) == 0) return Sensor::Cpu;
    if (type.rfind("gpu", 0) == 0) return Sensor::Gpu;
    return Sensor::Unknown;
}
// Input is trimmed sysfs text, strictly integral millidegrees Celsius.
inline std::optional<float> Celsius(const std::string& text) {
    int64_t value = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (r.ec != std::errc{} || r.ptr != text.data() + text.size() ||
        value < -40000 || value > 150000) return std::nullopt;
    return static_cast<float>(value) / 1000.0f;
}
struct Trip { std::string type; std::string temperature; };
inline std::optional<Thresholds> SkinThresholds(const std::vector<Trip>& trips) {
    std::vector<float> passive;
    std::optional<float> critical;
    for (const auto& trip : trips) {
        auto c = Celsius(trip.temperature);
        if (!c || *c <= 0) return std::nullopt;
        if (trip.type == "passive") passive.push_back(*c);
        else if (trip.type == "critical" && !critical) critical = *c;
        else return std::nullopt; // Unknown/duplicate policy requires explicit review.
    }
    if (passive.size() != 2 || !critical) return std::nullopt;
    std::sort(passive.begin(), passive.end());
    if (!(passive[0] < passive[1] && passive[1] < *critical)) return std::nullopt;
    return Thresholds{NAN, passive[0], NAN, passive[1], NAN, NAN, *critical};
}
inline int Severity(float c, const Thresholds& hot) {
    if (!std::isfinite(c)) return 0;
    for (int i = 6; i > 0; --i)
        if (std::isfinite(hot[i]) && c >= hot[i]) return i;
    return 0;
}
} // namespace gaokun_thermal
