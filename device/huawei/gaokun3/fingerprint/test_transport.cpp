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
#include <ctime>
#include <vector>
#include <algorithm>
static bool owned, loaded, high, failInit, failUnload, failReadback, failCapture;
static bool acceptBase = true;
static int64_t nowMs = 1000000;
static bool sensorGoodix;
static unsigned decodeCalls, goodixDecodeCalls, captures, statusFinger;
static std::vector<bool> goodixWithBase;
static unsigned strapBits, strapHeld;
static std::vector<std::string> events;
static int mockOpen(const char* path, int) {
    if (!strcmp(path,"/dev/gpiochip4")) return 101;
    assert(!strcmp(path,"/dev/gaokun3_fpcmd")); return 100;
}
static int mockClose(int fd) {
    if (fd == 102) { assert(!loaded); owned=false; events.push_back("release"); }
    if (fd == 103) { assert(strapHeld); strapHeld=0; }
    return 0;
}
static int mockIoctl(int fd, unsigned long command, void* arg=nullptr) {
    if (fd == 100) {
        if (command == GAOKUN3_FPCMD_IOC_CAPTURE) {
            auto* request=static_cast<gaokun3_fpcmd_capture*>(arg);
            const auto& p=sensorGoodix ? gaokun3::kGoodix : gaokun3::kFocalTech;
            assert(loaded && request->image_bytes==p.captureBytes && !request->reserved);
            if (failCapture) {errno=EIO;return -1;}
            memset(reinterpret_cast<void*>(request->user_ptr),0x42,request->image_bytes);
            ++captures; events.push_back("capture");
            return 0;
        }
        if (command == GAOKUN3_FPCMD_IOC_SET_SENSOR) {
            assert(loaded && owned && !high);
            assert(*static_cast<uint32_t*>(arg)==GAOKUN3_FPCMD_SENSOR_GOODIX);
            sensorGoodix=true; events.push_back("goodix"); return 0;
        }
        if (command == GAOKUN3_FPCMD_IOC_GET_FINGER_STATUS) {
            *static_cast<uint8_t*>(arg)=statusFinger; return 0;
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
    if (command == GPIO_V2_GET_LINE_IOCTL &&
        static_cast<gpio_v2_line_request*>(arg)->offsets[0]==61) {
        // Straps: both lines, input only, no bias or output attributes.
        auto* r=static_cast<gpio_v2_line_request*>(arg);
        assert(r->num_lines==2 && r->offsets[1]==62 && !strapHeld);
        assert(r->config.flags==GPIO_V2_LINE_FLAG_INPUT && !r->config.num_attrs);
        r->fd=103; strapHeld=1; return 0;
    }
    if (fd == 103 && command == GPIO_V2_LINE_GET_VALUES_IOCTL) {
        auto* v=static_cast<gpio_v2_line_values*>(arg);
        assert(v->mask==3 && strapHeld); v->bits=strapBits; return 0;
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
static int mockClockGettime(clockid_t, timespec* t) {
    t->tv_sec = nowMs / 1000; t->tv_nsec = nowMs % 1000 * 1000000; return 0;
}
#define open mockOpen
#define close mockClose
#define ioctl mockIoctl
#define clock_gettime mockClockGettime
#include "Transport.cpp"
#undef clock_gettime
#undef ioctl
#undef close
#undef open
namespace gaokun3 {
bool decode(const uint8_t*, Gaokun3Frame*) { ++decodeCalls;return false; }
bool decodeGoodix(const uint16_t* raw, const uint16_t* base, Gaokun3Frame*) {
    assert(raw[0]==0x4242); ++goodixDecodeCalls; goodixWithBase.push_back(base); return base;
}
bool goodixBaseUsable(const uint16_t* candidate, const uint16_t* current) {
    return candidate[0]==0x4242 && (!current || acceptBase);
}
const SensorProfile* profileFromStraps(bool g61, bool g62) {
    return g61 && !g62 ? &kGoodix : !g61 && g62 ? &kFocalTech : nullptr;
}
}
static void reset() {
    owned=loaded=high=failInit=failUnload=failReadback=failCapture=sensorGoodix=false;
    acceptBase=true;
    decodeCalls=goodixDecodeCalls=captures=statusFinger=0;events.clear();goodixWithBase.clear();
}
int main() {
    // GPIO61 is bit 0, GPIO62 bit 1; the lines are released after reading.
    for (auto [bits,want] : {std::pair{2u,&gaokun3::kFocalTech},{1u,&gaokun3::kGoodix},
                             {0u,nullptr},{3u,nullptr}}) {
        strapBits=bits; assert(gaokun3::Transport::detect()==want); assert(!strapHeld);
    }
    reset();
    { gaokun3::Transport t(gaokun3::kFocalTech); assert(t.start()); assert(owned && loaded);
      assert(t.stop()); assert(!owned && !loaded); }
    assert((events==std::vector<std::string>{"load","acquire-high","low","init","unload","restore","release"}));
    reset();
    { gaokun3::Transport t(gaokun3::kFocalTech);
      std::array<uint8_t,gaokun3::kFocalTech.captureBytes> raw{};
      assert(!t.captureRaw(raw.data(),raw.size()) && errno==ENODEV);
      assert(t.start());assert(t.captureRaw(raw.data(),raw.size()));
      assert(raw.front()==0x42 && raw.back()==0x42 && decodeCalls==0);
      assert(!t.captureRaw(raw.data(),raw.size()-2) && errno==EINVAL);
      Gaokun3Frame frame{};bool usable=true;
      uint8_t finger=1; assert(t.status(&finger) && !finger && captures==1); // no base capture
      assert(t.capture(&frame,&usable) && !usable && decodeCalls==1);
      failCapture=true;assert(!t.captureRaw(raw.data(),raw.size()) && errno==EIO);
      assert(decodeCalls==1 && !goodixDecodeCalls);assert(t.stop()); }
    assert(std::find(events.begin(),events.end(),"goodix")==events.end());
    reset();
    // Goodix: SET_SENSOR before INIT; the no-finger frame is taken while the
    // status reads 0 and used for the next capture.
    { gaokun3::Transport t(gaokun3::kGoodix); assert(t.start());
      assert((events==std::vector<std::string>{"load","acquire-high","low","goodix","init"}));
      Gaokun3Frame frame{};bool usable=false;
      statusFinger=1; assert(t.capture(&frame,&usable) && !usable);   // finger since start
      uint8_t finger=0; assert(t.status(&finger) && finger && captures==1);
      statusFinger=0; assert(t.status(&finger) && !finger && captures==2);
      assert(t.status(&finger) && captures==2);                        // fresh base kept
      assert(t.capture(&frame,&usable) && usable && captures==3);
      assert((goodixWithBase==std::vector<bool>{false,true}));
      // The first frame of a session is retaken soon; later ones last 60 s.
      nowMs += 5000; assert(t.status(&finger) && captures==4);
      nowMs += 5000; assert(t.status(&finger) && captures==4);
      // A lift-off does not retake it (the finger's trace would be in it).
      statusFinger=1; assert(t.status(&finger) && finger && captures==4);
      statusFinger=0; assert(t.status(&finger) && !finger && captures==4);
      nowMs += 55000; assert(t.status(&finger) && captures==5);
      // Rejected candidates are retried every 5 s; the third, 10 s after the
      // first, replaces the base anyway.
      acceptBase=false;
      nowMs += 60000; assert(t.status(&finger) && captures==6);
      nowMs += 4000; assert(t.status(&finger) && captures==6);
      nowMs += 1000; assert(t.status(&finger) && captures==7);
      nowMs += 5000; assert(t.status(&finger) && captures==8);
      nowMs += 5000; assert(t.status(&finger) && captures==8);   // accepted
      assert(t.stop()); }
    // A new session starts without a base.
    { gaokun3::Transport t(gaokun3::kGoodix); assert(t.start());
      Gaokun3Frame frame{};bool usable=true;
      assert(t.capture(&frame,&usable) && !usable); assert(t.stop()); }
    reset(); failInit=true;
    { gaokun3::Transport t(gaokun3::kFocalTech); assert(!t.start()); assert(!owned && !loaded); }
    reset(); failReadback=true;
    { gaokun3::Transport t(gaokun3::kFocalTech); assert(!t.start()); assert(!owned && !loaded); }
    reset();
    { gaokun3::Transport t(gaokun3::kFocalTech); assert(t.start()); failUnload=true;
      assert(!t.stop()); assert(owned && loaded);
      const auto count=events.size(); assert(!t.start()); assert(events.size()==count); }
    assert(owned && loaded); // Failed shutdown never restores/releases reset.
    std::cout << "PASS: encoded capture/decode boundary, Goodix selection and no-finger frame, held pull-up reset, readback, init failure cleanup, shutdown failure retention\n";
}
