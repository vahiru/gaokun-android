"""Compile the generated HAL wrapper on host and test its write-isolation behavior."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent


@unittest.skipUnless(shutil.which('clang++'), 'clang++ required')
class BootHalTests(unittest.TestCase):
    def test_guard_precedes_init_and_ota_mutators_never_reach_impl(self):
        with tempfile.TemporaryDirectory(prefix='hos-hal-test-') as directory:
            root = Path(directory)
            (root/'tiny.c').write_text('/* unused in host test */')
            subprocess.run(['python3', str(HERE/'prepare-boot-hal.py'), '--source',
                            str(HERE.parents[1]/'device/huawei/gaokun3/boot_control'),
                            '--tinymix-source', str(root/'tiny.c'), '--out', str(root/'module')], check=True, capture_output=True)
            files = {
                'host.h': r'''
#pragma once
#include <cstdlib>
#include <cstring>
#include <string>
#include <stdexcept>
namespace fake { inline std::string mode; inline int init=0, marked=0, mutated=0; }
inline char* hos_test_realpath(const char* path, char* result) {
    if (fake::mode == "missing") return nullptr;
    bool production = std::string(path) == "/dev/block/by-name/misc";
    bool wrong = fake::mode == "wrong" && std::string(path) == "/selected";
    strcpy(result, production || wrong || fake::mode == "alias" ? "/dev/block/p4" : "/dev/block/p9");
    return result;
}
#define realpath hos_test_realpath
''',
                'android-base/logging.h': r'''
#pragma once
#include <stdexcept>
struct Fatal {
    template<class T> Fatal& operator<<(const T&) { return *this; }
    ~Fatal() noexcept(false) { throw std::runtime_error("guard"); }
};
#define CHECK(x) if (x) ; else Fatal()
#define CHECK_EQ(a,b) CHECK((a)==(b))
#define CHECK_NE(a,b) CHECK((a)!=(b))
''',
                'android-base/properties.h': r'''
#pragma once
#include <string>
namespace android::base {
inline std::string GetProperty(const std::string&, const std::string&) {
    return fake::mode == "no-super" ? "" : "super_hos";
}}
''',
                'bootloader_message/bootloader_message.h': r'''
#pragma once
#include <string>
inline std::string get_bootloader_message_blk_device(std::string*) { return "/selected"; }
''',
                'libboot_control/libboot_control.h': r'''
#pragma once
namespace android::bootable {
class BootControl {
public:
    enum class MergeStatus { NONE, UNKNOWN, SNAPSHOTTED, MERGING, CANCELLED };
    bool Init() { ++fake::init; return true; }
    bool MarkBootSuccessful() { ++fake::marked; return true; }
    unsigned GetActiveBootSlot() { return 0; }
    unsigned GetCurrentSlot() { return 0; }
    unsigned GetNumberSlots() { return 2; }
    MergeStatus GetSnapshotMergeStatus() { return MergeStatus::NONE; }
    bool IsValidSlot(int s) { return s == 0 || s == 1; }
    const char* GetSuffix(int s) { return s ? "_b" : "_a"; }
    bool IsSlotBootable(int s) { return s == 0; }
    bool IsSlotMarkedSuccessful(int s) { return s == 0; }
    bool SetActiveBootSlot(int) { ++fake::mutated; return true; }
    bool SetSlotAsUnbootable(int) { ++fake::mutated; return true; }
    bool SetSnapshotMergeStatus(MergeStatus) { ++fake::mutated; return true; }
}; }
''',
                'aidl/android/hardware/boot/BnBootControl.h': r'''
#pragma once
#include <cstdint>
#include <string>
namespace ndk {
struct ScopedAStatus {
    bool good;
    static ScopedAStatus ok() { return {true}; }
    static ScopedAStatus fromServiceSpecificErrorWithMessage(int, const char*) { return {false}; }
    bool isOk() const { return good; }
}; }
namespace aidl::android::hardware::boot {
enum class MergeStatus { NONE, UNKNOWN, SNAPSHOTTED, MERGING, CANCELLED };
class BnBootControl {
public:
    static constexpr int COMMAND_FAILED=1, INVALID_SLOT=2;
    virtual ~BnBootControl() = default;
    virtual ndk::ScopedAStatus getActiveBootSlot(int32_t*) = 0;
    virtual ndk::ScopedAStatus getCurrentSlot(int32_t*) = 0;
    virtual ndk::ScopedAStatus getNumberSlots(int32_t*) = 0;
    virtual ndk::ScopedAStatus getSnapshotMergeStatus(MergeStatus*) = 0;
    virtual ndk::ScopedAStatus getSuffix(int32_t,std::string*) = 0;
    virtual ndk::ScopedAStatus isSlotBootable(int32_t,bool*) = 0;
    virtual ndk::ScopedAStatus isSlotMarkedSuccessful(int32_t,bool*) = 0;
    virtual ndk::ScopedAStatus markBootSuccessful() = 0;
    virtual ndk::ScopedAStatus setActiveBootSlot(int32_t) = 0;
    virtual ndk::ScopedAStatus setSlotAsUnbootable(int32_t) = 0;
    virtual ndk::ScopedAStatus setSnapshotMergeStatus(MergeStatus) = 0;
}; }
''',
                'test.cpp': r'''
#include "BootControl.h"
#include <cassert>
using namespace aidl::android::hardware::boot;
int main() {
    for (const char* mode : {"no-super", "wrong", "alias", "missing"}) {
        fake::mode = mode;
        bool refused=false;
        try { BootControl service; } catch (const std::runtime_error&) { refused=true; }
        assert(refused && fake::init==0 && fake::marked==0 && fake::mutated==0);
    }
    fake::mode="valid";
    BootControl service;
    assert(fake::init==1);
    assert(!service.setActiveBootSlot(1).isOk());
    assert(!service.setSlotAsUnbootable(0).isOk());
    assert(!service.setSnapshotMergeStatus(MergeStatus::MERGING).isOk());
    assert(fake::mutated==0);
    int32_t slots=0;
    assert(service.getNumberSlots(&slots).isOk() && slots==2);
    assert(service.markBootSuccessful().isOk() && fake::marked==1);
}
'''}
            for name, text in files.items():
                dest = root/name
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text(text)
            result = subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                     '-fsanitize=address,undefined', '-include', str(root/'host.h'),
                                     '-I'+str(root), '-I'+str(root/'module'),
                                     str(root/'module/BootControl.cpp'), str(root/'test.cpp'), '-o', str(root/'test')], capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            subprocess.run([str(root/'test')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
