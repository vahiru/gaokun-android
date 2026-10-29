#!/usr/bin/env python3
"""Generate a separate AOSP module for an isolated HOS boot HAL and vendor tinymix.

Does not change the normal device tree. The HAL requires super_hos + misc_hos,
retains BCB reads/markBootSuccessful, rejects OTA mutators, and links no ESP/GK3 worker.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path


def replace_function(text, signature, body):
    start = text.index(signature)
    brace = text.index('{', start)
    depth, pos = 1, brace + 1
    while depth:
        depth += (text[pos] == '{') - (text[pos] == '}')
        pos += 1
    return text[:brace + 1] + '\n' + body + '\n' + text[pos - 1:]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source', type=Path, required=True, help='original boot_control directory')
    p.add_argument('--tinymix-source', type=Path, required=True)
    p.add_argument('--out', type=Path, required=True)
    a = p.parse_args()
    code = (a.source / 'BootControl.cpp').read_text()
    original = code
    code = code.replace('#include "EspSlot.h"', '''#include <android-base/properties.h>
#include <bootloader_message/bootloader_message.h>
#include <climits>
#include <cstdlib>
#include <cstring>''')
    code = replace_function(code, 'BootControl::BootControl()', '''    // Verify the block device before libboot_control can initialize or write its BCB.
    CHECK_EQ(::android::base::GetProperty("ro.boot.force_super_partition", ""), "super_hos");
    std::string error;
    const std::string misc = get_bootloader_message_blk_device(&error);
    char selected[PATH_MAX], expected[PATH_MAX], production[PATH_MAX];
    CHECK(realpath(misc.c_str(), selected)) << error;
    CHECK(realpath("/dev/block/by-name/misc_hos", expected));
    CHECK_EQ(strcmp(selected, expected), 0) << "refusing non-isolated misc";
    if (realpath("/dev/block/by-name/misc", production)) {
        CHECK_NE(strcmp(selected, production), 0) << "misc_hos aliases production misc";
    }
    CHECK(impl_.Init());''')
    code = replace_function(code, 'ScopedAStatus BootControl::markBootSuccessful()', '''    if (!impl_.MarkBootSuccessful()) {
        return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                                  "isolated BCB update failed");
    }
    return ScopedAStatus::ok();''')
    for signature, param in [('setActiveBootSlot(int32_t in_slot)', 'in_slot'),
                             ('setSlotAsUnbootable(int32_t in_slot)', 'in_slot'),
                             ('setSnapshotMergeStatus(MergeStatus in_status)', 'in_status')]:
        code = replace_function(code, 'ScopedAStatus BootControl::' + signature, f'''    (void){param};
    return ScopedAStatus::fromServiceSpecificErrorWithMessage(COMMAND_FAILED,
                                                              "isolated experiment: OTA mutation disabled");''')
    # Unused reverse converter is no longer needed once snapshot mutation is disabled.
    code = replace_function(code, 'static constexpr HIDLMergeStatus ToHIDLMergeStatus(MergeStatus status)', '    (void)status;\n    return HIDLMergeStatus::UNKNOWN;')
    start = code.index('static constexpr HIDLMergeStatus ToHIDLMergeStatus(')
    end = code.index('\n}\n', start) + 3
    code = code[:start] + code[end:]
    # The original header comment describes production ESP behavior, so replace it.
    license_lines = '\n'.join(line for line in original[:original.index('#include')].splitlines()
                              if 'Copyright ' in line or 'SPDX-License-Identifier:' in line)
    code = '/* Experimental HOS boot HAL. Derived from gaokun3/AOSP.\n' + license_lines + '\n */\n' + code[code.index('#include'):]
    main_code = (a.source / 'main.cpp').read_text()
    main_code = main_code.replace('#include "Gk3Boot.h"', '')
    main_code = main_code.replace('return gaokun3::MarkPoweroffMain();', 'return 0;')
    main_code = main_code.replace('    gaokun3::StartBootCompletedWorker();', '    // Isolated variant links no ESP or GK3 record worker.')
    if 'gaokun3::' in code or 'gaokun3::' in main_code:
        raise ValueError('unexpected remaining production boot helper reference')
    a.out.mkdir(parents=True, exist_ok=False)
    for name, content in [('BootControl.cpp', code), ('main.cpp', main_code),
                          ('BootControl.h', (a.source / 'BootControl.h').read_text()),
                          ('tinymix.c', a.tinymix_source.read_text())]:
        (a.out / name).write_text(content)
    (a.out / 'Android.bp').write_text('''// Local experiment only; not included in PRODUCT_PACKAGES.
cc_binary {
    name: "android.hardware.boot-service.gaokun3_hos",
    stem: "android.hardware.boot-service.gaokun3_hos",
    relative_install_path: "hw",
    vendor: true,
    defaults: ["libboot_control_defaults"],
    srcs: ["main.cpp", "BootControl.cpp"],
    stl: "c++_static",
    shared_libs: ["libbinder_ndk", "liblog"],
    static_libs: ["android.hardware.boot@1.1", "android.hardware.boot-V1-ndk",
                  "libbase", "libboot_control", "libbootloader_message"],
}
cc_binary {
    name: "tinymix_hos_vendor",
    stem: "tinymix_hos",
    vendor: true,
    srcs: ["tinymix.c"],
    static_libs: ["libtinyalsa"],
    cflags: ["-Werror", "-Wall"],
}
''')
    (a.out / 'review.diff').write_text(''.join(difflib.unified_diff(original.splitlines(True), code.splitlines(True),
                                                fromfile='original/BootControl.cpp', tofile='isolated/BootControl.cpp')))
    inputs = [a.source / name for name in ('BootControl.cpp', 'BootControl.h', 'main.cpp')] + [a.tinymix_source]
    (a.out / 'manifest.json').write_text(json.dumps({'inputs': {str(f): hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},
                            'ready_to_boot': False, 'requires': ['A17 module build', 'isolated fstab and misc_hos',
                                                                               'hardware validation']}, indent=2)+'\n')
    print(f'Generated isolated modules in {a.out}')


if __name__ == '__main__':
    main()
