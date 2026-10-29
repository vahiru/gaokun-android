"""Compile the actual HAL forecast method with a fake sensor and Binder status."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ForecastContractTest(unittest.TestCase):
    def test_actual_method_contract(self):
        compiler = shutil.which('clang++') or shutil.which('g++')
        self.assertIsNotNone(compiler, 'C++ compiler is required for the contract test')
        source = (ROOT / 'device/huawei/gaokun3/thermal/Thermal.cpp').read_text()
        start = source.index('    ndk::ScopedAStatus forecastSkinTemperature(')
        end = source.index('\n  private:', start)
        method = source[start:end].replace(' override', '')
        harness = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
constexpr int EX_ILLEGAL_ARGUMENT = -3;
constexpr int EX_UNSUPPORTED_OPERATION = -7;
namespace ndk {
struct ScopedAStatus {
    int exception;
    std::string message;
    static ScopedAStatus fromExceptionCodeWithMessage(int code, const char* text) {
        return {code, text};
    }
    static ScopedAStatus ok() { return {0, ""}; }
};
}
struct Probe {
    float sensor = 27.5f;
    int reads = 0;
    float SkinTempC() { ++reads; return sensor; }
METHOD
};
int main() {
    for (int32_t seconds : {std::numeric_limits<int32_t>::min(), int32_t(-1)}) {
        Probe p; float out = 123.0f;
        auto status = p.forecastSkinTemperature(seconds, &out);
        assert(status.exception == EX_ILLEGAL_ARGUMENT);
        assert(!status.message.empty());
        assert(p.reads == 0 && out == 123.0f);
    }
    for (int32_t seconds : {int32_t(0), int32_t(1), int32_t(60)}) {
        Probe p; float out = 123.0f;
        auto status = p.forecastSkinTemperature(seconds, &out);
        assert(status.exception == 0 && p.reads == 1 && out == 27.5f);
        p.sensor = NAN; p.reads = 0; out = 123.0f;
        status = p.forecastSkinTemperature(seconds, &out);
        assert(status.exception == EX_UNSUPPORTED_OPERATION);
        assert(!status.message.empty());
        assert(p.reads == 1 && out == 123.0f);
    }
}
'''.replace('METHOD', method)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            cpp = path / 'test.cpp'; exe = path / 'test'
            cpp.write_text(harness)
            subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', str(cpp), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
