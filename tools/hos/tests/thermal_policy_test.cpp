#include "../../../device/huawei/gaokun3/thermal/ThermalPolicy.h"
#include <cassert>
#include <limits>
using namespace gaokun_thermal;
int main() {
    assert(Classify("skin-temp-thermal") == Sensor::Skin);
    assert(Classify("skin-thermal") == Sensor::Unknown);
    assert(Classify("cpu0-thermal") == Sensor::Cpu);
    assert(Classify("gpu-thermal") == Sensor::Gpu);
    assert(Classify("pmc8280c-thermal") == Sensor::Unknown);
    assert(Celsius("26479") && std::abs(*Celsius("26479") - 26.479f) < .001f);
    for (auto s : {"", "NaN", "26.5", "1000garbage", "150001", "-40960",
                   "9223372036854775808"}) assert(!Celsius(s));
    assert(*Celsius("-40000") == -40 && *Celsius("150000") == 150);
    std::vector<Trip> trips{{"passive", "58000"}, {"critical", "73000"},
                            {"passive", "55000"}};
    auto hot = SkinThresholds(trips);
    assert(hot && (*hot)[1] == 55 && (*hot)[3] == 58 && (*hot)[6] == 73);
    assert(std::isnan((*hot)[2]) && std::isnan((*hot)[4]) && std::isnan((*hot)[5]));
    for (auto [c, expected] : std::vector<std::pair<float, int>>{
        {54.999f,0},{55,1},{57.999f,1},{58,3},{72.999f,3},{73,6},{100,6}})
        assert(Severity(c,*hot) == expected);
    assert(Severity(NAN,*hot) == 0);
    assert(Severity(73,kDieHot) == 2 && Severity(105,kDieHot) == 6);
    assert(!SkinThresholds({}));
    for (auto invalid : std::vector<std::vector<Trip>>{
        {{"passive","55000"},{"critical","73000"}},
        {{"passive","55000"},{"passive","55000"},{"critical","73000"}},
        {{"passive","55000"},{"passive","58000"},{"critical","57000"}},
        {{"passive","55000"},{"passive","58000"},{"critical","150001"}},
        {{"passive","55000"},{"passive","58000"},{"critical","73000"},{"critical","80000"}},
        {{"passive","55000"},{"passive","58000"},{"critical","73000"},{"active","40000"}}})
        assert(!SkinThresholds(invalid));
}
