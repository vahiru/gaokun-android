// SPDX-License-Identifier: Apache-2.0
// Standalone Linux host test. Redirect only this translation unit's syscalls.
#include "Core.h"
#include "gaokun3_fpcmd.h"
#include <cassert>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>
static bool owned, loaded, high, failInit, failUnload, failReadback, failCapture;
static unsigned decodeCalls;
static std::vector<std::string> events;
static int mockOpen(const char* path, int) {
    if (!strcmp(path,"/dev/gpiochip4")) return 101;
    assert(!strcmp(path,"/dev/gaokun3_fpcmd")); return 100;
}
static int mockClose(int fd) {
    if (fd == 102) { assert(!loaded); owned=false; events.push_back("release"); }
    return 0;
}
static int mockIoctl(int fd, unsigned long command, void* arg=nullptr) {
    if (fd == 100) {
        if (command == GAOKUN3_FPCMD_IOC_CAPTURE) {
            auto* request=static_cast<gaokun3_fpcmd_capture*>(arg);
            assert(loaded && request->image_bytes==gaokun3::kImageBytes && !request->reserved);
            if (failCapture) {errno=EIO;return -1;}
            memset(reinterpret_cast<void*>(request->user_ptr),0x42,request->image_bytes);
            return 0;
        }
        if (command == GAOKUN3_FPCMD_IOC_LOAD) {
            loaded=true; events.push_back("load"); return 0;
        }
        if (command == GAOKUN3_FPCMD_IOC_UNLOAD) {
            events.push_back("unload");
            if (failUnload) { errno=EIO; return -1; }
            loaded=false; return 0;
        }
        assert(command == GAOKUN3_FPCMD_IOC_INIT);
        assert(owned && !high && loaded); events.push_back("init");
        if (failInit) { errno=EPROTO; return -1; }
        return 0;
    }
    if (command == GPIO_GET_CHIPINFO_IOCTL) {
        auto* ci=static_cast<gpiochip_info*>(arg);
        strcpy(ci->label,"f100000.pinctrl"); ci->lines=230; return 0;
    }
    if (command == GPIO_V2_GET_LINE_IOCTL) {
        auto* r=static_cast<gpio_v2_line_request*>(arg);
        assert(r->offsets[0]==185 && r->num_lines==1 && !owned);
        assert(r->config.flags==(GPIO_V2_LINE_FLAG_OUTPUT|GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
        assert(r->config.attrs[0].attr.values==1);
        r->fd=102; owned=true; high=true; events.push_back("acquire-high"); return 0;
    }
    assert(fd==102 && owned);
    if (command == GPIO_V2_LINE_GET_VALUES_IOCTL) {
        if (failReadback) { errno=EIO; return -1; }
        static_cast<gpio_v2_line_values*>(arg)->bits=high; return 0;
    }
    if (command == GPIO_V2_LINE_SET_VALUES_IOCTL) {
        assert(high && !static_cast<gpio_v2_line_values*>(arg)->bits);
        high=false; events.push_back("low"); return 0;
    }
    assert(command == GPIO_V2_LINE_SET_CONFIG_IOCTL && !loaded);
    assert(static_cast<gpio_v2_line_config*>(arg)->flags==
           (GPIO_V2_LINE_FLAG_INPUT|GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN));
    events.push_back("restore"); return 0;
}
#define open mockOpen
#define close mockClose
#define ioctl mockIoctl
#include "Transport.cpp"
#undef ioctl
#undef close
#undef open
namespace gaokun3 { bool decode(const uint8_t*, Gaokun3Frame*) { ++decodeCalls;return false; } }
static void reset() {
    owned=loaded=high=failInit=failUnload=failReadback=failCapture=false;
    decodeCalls=0;events.clear();
}
int main() {
    reset();
    { gaokun3::Transport t; assert(t.start()); assert(owned && loaded);
      assert(t.stop()); assert(!owned && !loaded); }
    assert((events==std::vector<std::string>{"load","acquire-high","low","init","unload","restore","release"}));
    reset();
    { gaokun3::Transport t;std::array<uint8_t,gaokun3::kImageBytes> raw{};
      assert(!t.captureEncoded(raw) && errno==ENODEV);
      assert(t.start());assert(t.captureEncoded(raw));
      assert(raw.front()==0x42 && raw.back()==0x42 && decodeCalls==0);
      Gaokun3Frame frame{};bool usable=true;
      assert(t.capture(&frame,&usable) && !usable && decodeCalls==1);
      failCapture=true;assert(!t.captureEncoded(raw) && errno==EIO);
      assert(decodeCalls==1);assert(t.stop()); }
    reset(); failInit=true;
    { gaokun3::Transport t; assert(!t.start()); assert(!owned && !loaded); }
    reset(); failReadback=true;
    { gaokun3::Transport t; assert(!t.start()); assert(!owned && !loaded); }
    reset();
    { gaokun3::Transport t; assert(t.start()); failUnload=true;
      assert(!t.stop()); assert(owned && loaded);
      const auto count=events.size(); assert(!t.start()); assert(events.size()==count); }
    assert(owned && loaded); // Failed shutdown never restores/releases reset.
    std::cout << "PASS: encoded capture/decode boundary, held pull-up reset, readback, init failure cleanup, shutdown failure retention\n";
}
