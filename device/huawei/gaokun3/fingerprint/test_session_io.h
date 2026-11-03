// SPDX-License-Identifier: Apache-2.0
// Linker wrappers for the session tests' fixed transport devices only.
#pragma once
#include "gaokun3_fpcmd.h"
#include <cassert>
#include <cstdarg>
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
static std::atomic_int mockLoads{0}, mockUnloads{0};
static std::array<uint8_t,gaokun3::kImageBytes> mockCipher{};
static std::atomic_int mockCaptures{0};
static std::vector<std::array<uint8_t,gaokun3::kImageBytes>> mockCipherQueue;
static size_t mockCipherCursor=0;
static std::vector<int> mockStatusSamples;
static size_t mockStatusCursor=0;
static std::atomic_int mockStatusReads{0};
static bool mockOwned=false, mockLoaded=false, mockHigh=false;
extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_close(int);
extern "C" int __real_ioctl(int, unsigned long, ...);
extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (!strcmp(path,"/dev/gaokun3_fpcmd")) return 100000;
    if (!strcmp(path,"/dev/gpiochip4")) return 100001;
    mode_t mode=0;
    if (flags & O_CREAT) { va_list ap; va_start(ap,flags); mode=va_arg(ap,int); va_end(ap); }
    return __real_open(path,flags,mode);
}
extern "C" int __wrap___open_2(const char* path, int flags) {
    return __wrap_open(path,flags);
}
extern "C" int __wrap_close(int fd) {
    if (fd==100000 || fd==100001) return 0;
    if (fd==100002) { assert(!mockLoaded); mockOwned=false; return 0; }
    return __real_close(fd);
}
extern "C" int __wrap_ioctl(int fd, unsigned long command, ...) {
    void* arg=nullptr;
    if (command!=GAOKUN3_FPCMD_IOC_LOAD && command!=GAOKUN3_FPCMD_IOC_INIT &&
        command!=GAOKUN3_FPCMD_IOC_UNLOAD) {
        va_list ap; va_start(ap,command); arg=va_arg(ap,void*); va_end(ap);
    }
    if (fd==100000) {
        if (command==GAOKUN3_FPCMD_IOC_LOAD) { assert(!mockLoaded); mockLoaded=true; ++mockLoads; return 0; }
        if (command==GAOKUN3_FPCMD_IOC_UNLOAD) { assert(mockLoaded); mockLoaded=false; ++mockUnloads; return 0; }
        if (command==GAOKUN3_FPCMD_IOC_CAPTURE) {
            auto* request=static_cast<gaokun3_fpcmd_capture*>(arg);
            assert(request->image_bytes==mockCipher.size() && request->reserved==0);
            const auto& cipher=mockCipherCursor<mockCipherQueue.size() ? mockCipherQueue[mockCipherCursor++] : mockCipher;
            memcpy(reinterpret_cast<void*>(request->user_ptr),cipher.data(),cipher.size());
            ++mockCaptures; return 0;
        }
        if (command==GAOKUN3_FPCMD_IOC_GET_FINGER_STATUS) {
            ++mockStatusReads;
            int sample=mockStatusCursor<mockStatusSamples.size() ? mockStatusSamples[mockStatusCursor++] : 0;
            if (sample<0) { errno=-sample; return -1; }
            *static_cast<uint8_t*>(arg)=sample; return 0;
        }
        assert(command==GAOKUN3_FPCMD_IOC_INIT && mockOwned && !mockHigh); return 0;
    }
    if (fd==100001) {
        if (command==GPIO_GET_CHIPINFO_IOCTL) {
            auto* p=static_cast<gpiochip_info*>(arg); strcpy(p->label,"f100000.pinctrl"); p->lines=230; return 0;
        }
        assert(command==GPIO_V2_GET_LINE_IOCTL && !mockOwned);
        auto* r=static_cast<gpio_v2_line_request*>(arg);
        assert(r->offsets[0]==185 && r->num_lines==1);
        assert(r->config.flags==(GPIO_V2_LINE_FLAG_OUTPUT|GPIO_V2_LINE_FLAG_BIAS_PULL_UP));
        r->fd=100002; mockOwned=mockHigh=true; return 0;
    }
    if (fd==100002) {
        assert(mockOwned);
        if (command==GPIO_V2_LINE_GET_VALUES_IOCTL) {
            static_cast<gpio_v2_line_values*>(arg)->bits=mockHigh; return 0;
        }
        if (command==GPIO_V2_LINE_SET_VALUES_IOCTL) { mockHigh=false; return 0; }
        assert(command==GPIO_V2_LINE_SET_CONFIG_IOCTL && !mockLoaded); return 0;
    }
    return __real_ioctl(fd,command,arg);
}
static void resetMockTransport() {
    assert(!mockOwned && !mockLoaded); mockLoads=mockUnloads=0;
    mockCipher.fill(0); mockCaptures=0; mockCipherQueue.clear(); mockCipherCursor=0;
    mockStatusSamples.clear(); mockStatusCursor=0; mockStatusReads=0;
}
