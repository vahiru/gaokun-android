#!/usr/bin/env python3
"""Host-only regression tests: real Follow.cpp + sandboxed role shell script."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEVICE = ROOT / 'device/huawei/gaokun3'
PROP = 'persist.vendor.gaokun3.usbrole.device_only'


class DeviceOnlyTests(unittest.TestCase):
    def test_follow_and_write_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d/'android').mkdir()
            (d/'android/log.h').write_text('#define ANDROID_LOG_INFO 1\ninline int __android_log_print(int, const char*, const char*, ...) { return 0; }\n')
            port = (DEVICE/'usb/Port.cpp').read_text()
            write_role = re.search(r'bool WriteRole\(const std::string& role\) \{.*?\n\}', port, re.S).group()
            harness = r'''
#include "Follow.h"
#include <cassert>
#include <map>
#include <vector>
using namespace aidl::android::hardware::usb;
namespace aidl::android::hardware::usb {
std::map<std::string, std::string> props;
std::vector<std::string> writes;
int pullups = 0;
std::string GetProp(const char* p) { return props[p]; }
bool SetProp(const char* p, const std::string& v) { props[p] = v; return true; }
std::string ReadAttr(const char*) { return "not attached"; }
void RestartPullup() { ++pullups; }
bool Inputs::Enumerated() const {
    return udc == "configured" || udc == "addressed" || udc == "default" || udc == "suspended";
}
bool WriteAttr(const char*, const char* value) { writes.emplace_back(value); return true; }
#define LOGW(...) ((void)0)
WRITE_ROLE
}
int main() {
    using C = RoleFollow::Clock;
    auto t = C::time_point{};
    Inputs in; in.partner = true; in.typec = true; in.role = "device"; in.powerRole = "source";
    // Missing/false property retains the source->host default behavior.
    RoleFollow normal; normal.Tick(in, t);
    assert(writes == std::vector<std::string>{"host"}); writes.clear();
    for (auto value : {"true", "1"}) {
        props[kDeviceOnly] = value;
        assert(DeviceOnly());
        assert(!WriteRole("host")); assert(writes.empty());
        RoleFollow f; f.Choose("host");
        f.Tick(in, t); f.Tick(in, t + std::chrono::seconds(20));
        assert(writes.empty());
        in.powerRole = "sink";
        f.Tick(in, t + std::chrono::seconds(40));
        assert(writes.empty()); // no idle host probing for a charger
        in.role = "host";
        f.Tick(in, t + std::chrono::seconds(41));
        assert(writes.back() == "device"); writes.clear();
        in.role = "device"; in.partner = false;
        int before = pullups;
        f.Tick(in, t + std::chrono::seconds(42));
        assert(pullups == before + 1); // detach still clears stale gadget state
        f.Tick(in, t + std::chrono::seconds(43));
        assert(pullups == before + 1);
        in.partner = true; in.powerRole = "source";
    }
    // Enabling while a Settings host request is pending discards it. Disabling
    // restores automatic decisions, not the stale Settings host choice.
    props[kDeviceOnly] = "false";
    RoleFollow toggled; toggled.Choose("host");
    props[kDeviceOnly] = "true"; toggled.Tick(in, t);
    props[kDeviceOnly] = "false"; in.powerRole = "sink";
    toggled.Tick(in, t + std::chrono::seconds(1)); assert(writes.empty());
    toggled.Tick(in, t + std::chrono::seconds(8));
    assert(writes == std::vector<std::string>{"host"}); writes.clear();
    props[kDeviceOnly] = "0"; assert(!DeviceOnly()); assert(WriteRole("host"));
    return 0;
}
'''.replace('WRITE_ROLE', write_role)
            (d/'test.cpp').write_text(harness)
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I'+str(d), '-I'+str(DEVICE/'usb'), str(d/'test.cpp'), str(DEVICE/'usb/Follow.cpp'), '-o', str(d/'test')], check=True)
            subprocess.run([str(d/'test')], check=True)

    def run_role(self, policy, want, role='device', attached=False, downstream=False, toggle=False, xhci=True):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            sys = d/'sys'
            rolepath = sys/'class/usb_role/a600000.usb-role-switch/role'
            udc = sys/'class/udc/a600000.usb/state'
            adapter = sys/'class/power_supply/gaokun-ec-adapter/online'
            for p, value in [(rolepath, role), (udc, 'configured' if attached else 'not attached'), (adapter, '1' if attached else '0'), (sys/'power/wake_lock', ''), (sys/'power/wake_unlock', '')]:
                p.parent.mkdir(parents=True, exist_ok=True); p.write_text(value+'\n')
            if xhci:
                (sys/'bus/platform/devices/a600000.usb/xhci-hcd.0/driver').mkdir(parents=True)
            if downstream:
                (sys/'bus/platform/devices/a600000.usb/xhci-hcd.0/usb1/1-1').mkdir(parents=True)
            props = d/'props'; props.mkdir()
            (props/PROP).write_text(policy)
            (props/'debug.tracing.screen_state').write_text('1')
            bin = d/'bin'; bin.mkdir()
            commands = {
                'getprop': '#!/bin/sh\ncat "$PROPS/$1" 2>/dev/null || true\n',
                'setprop': '#!/bin/sh\nprintf "%s" "$2" > "$PROPS/$1"\n',
                'log': '#!/bin/sh\nexit 0\n',
                'sleep': '#!/bin/sh\nif [ "$TOGGLE" = 1 ]; then echo true > "$PROPS/'+PROP+'"; fi\n',
            }
            for name, body in commands.items():
                p = bin/name; p.write_text(body); p.chmod(0o755)
            # Substitute only the sysfs root; execute all original control flow.
            script = (DEVICE/'bin/gaokun3-usbrole.sh').read_text().replace('/sys/', str(sys)+'/')
            path = d/'role.sh'; path.write_text(script)
            env = dict(os.environ, PATH=str(bin)+':'+os.environ['PATH'], PROPS=str(props), TOGGLE=str(int(toggle)))
            subprocess.run(['sh', str(path), want], env=env, check=True, timeout=5, capture_output=True)
            return rolepath.read_text().strip(), (sys/'power/wake_unlock').read_text().strip(), (sys/'power/wake_lock').read_text().strip()

    def test_policy_screen_off_and_downstream(self):
        for value in ['true', '1']:
            self.assertEqual(self.run_role(value, 'host')[:2], ('device', ''))
            self.assertEqual(self.run_role(value, 'device', role='host', downstream=True)[:2], ('device', ''))

    def test_original_default_and_restore(self):
        for value in ['', 'false', '0']:
            self.assertEqual(self.run_role(value, 'host')[:2], ('host', 'gaokun3_usbrole'))
            self.assertEqual(self.run_role(value, 'device', role='host', downstream=True)[:2], ('host', ''))

    def test_enable_while_waiting_for_unplug(self):
        self.assertEqual(self.run_role('false', 'host', attached=True, toggle=True)[:2], ('device', ''))

    def test_enable_during_host_settle(self):
        self.assertEqual(self.run_role('false', 'host', xhci=False, toggle=True)[:2], ('device', ''))


if __name__ == '__main__':
    unittest.main()
