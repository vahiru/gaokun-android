// SPDX-License-Identifier: Apache-2.0
#include "Transport.h"
#include "gaokun3_fpcmd.h"
#include <android-base/logging.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/gpio.h>
#include <sys/ioctl.h>
#include <thread>
#include <chrono>
#include <unistd.h>
namespace gaokun3 {
using namespace std::chrono_literals;
Transport::~Transport() { stop(); if (fd_ >= 0) close(fd_); }
bool Transport::reset() {
    int chip = open("/dev/gpiochip4", O_RDONLY | O_CLOEXEC);
    if (chip < 0) return false;
    gpiochip_info info{};
    gpio_v2_line_request req{};
    bool ok = ioctl(chip,GPIO_GET_CHIPINFO_IOCTL,&info) == 0 &&
              !strcmp(info.label,"f100000.pinctrl") && info.lines == 230;
    req.offsets[0] = 185; req.num_lines = 1;
    strcpy(req.consumer,"gaokun3-fingerprint");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 1;
    req.config.attrs[0].mask = 1;
    if (ok) ok = ioctl(chip,GPIO_V2_GET_LINE_IOCTL,&req) == 0;
    close(chip);
    if (!ok) return false;
    gpioFd_ = req.fd;
    gpio_v2_line_values readback{}; readback.mask = 1;
    if (ioctl(gpioFd_,GPIO_V2_LINE_GET_VALUES_IOCTL,&readback) ||
        (readback.bits & 1) != 1) return false;
    std::this_thread::sleep_for(10ms);
    gpio_v2_line_values value{}; value.mask = 1;
    ok = ioctl(gpioFd_,GPIO_V2_LINE_SET_VALUES_IOCTL,&value) == 0 &&
         ioctl(gpioFd_,GPIO_V2_LINE_GET_VALUES_IOCTL,&readback) == 0 &&
         (readback.bits & 1) == 0;
    // The verified Windows/reset-probe sequence holds output-low with pull-up
    // throughout the secure operation, matching the initialization-only probe.
    std::this_thread::sleep_for(400ms);
    return ok;
}
bool Transport::start() {
    if (poisoned_) return false;
    if (loaded_) return true;
    if (fd_ < 0) fd_ = open("/dev/gaokun3_fpcmd",O_RDWR | O_CLOEXEC);
    if (fd_ < 0) { PLOG(ERROR) << "Fingerprint device open failed"; return false; }
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_LOAD) < 0) {
        PLOG(ERROR) << "Fingerprint TA load failed"; return false;
    }
    loaded_ = true;
    if (!reset()) { LOG(ERROR) << "Fingerprint reset failed"; stop(); return false; }
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_INIT) < 0) {
        PLOG(ERROR) << "Fingerprint TA init failed"; stop(); return false;
    }
    return true;
}
bool Transport::stop() {
    if (poisoned_) return false;
    if (loaded_ && ioctl(fd_,GAOKUN3_FPCMD_IOC_UNLOAD) < 0) {
        poisoned_ = true; return false;
    }
    loaded_ = false;
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    if (gpioFd_ >= 0) {
        gpio_v2_line_config restore{};
        restore.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
        if (ioctl(gpioFd_,GPIO_V2_LINE_SET_CONFIG_IOCTL,&restore)) {
            PLOG(ERROR) << "Fingerprint GPIO restore failed; retaining ownership";
            poisoned_ = true; return false;
        }
        close(gpioFd_); gpioFd_ = -1;
    }
    return true;
}
bool Transport::status(uint8_t* value) {
    if (!loaded_) { errno = ENODEV; return false; }
    return ioctl(fd_,GAOKUN3_FPCMD_IOC_GET_FINGER_STATUS,value) == 0;
}
bool Transport::captureEncoded(std::array<uint8_t,kImageBytes>& raw) {
    gaokun3_fpcmd_capture req{};
    req.image_bytes = raw.size(); req.user_ptr = reinterpret_cast<uintptr_t>(raw.data());
    if (!loaded_) { errno = ENODEV; return false; }
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_CAPTURE,&req) < 0) {
        PLOG(ERROR) << "Fingerprint image transport failed"; return false;
    }
    return true;
}
bool Transport::capture(Gaokun3Frame* frame, bool* usable) {
    std::array<uint8_t,kImageBytes> raw{};
    if (!captureEncoded(raw)) return false;
    *usable = decode(raw.data(),frame);
    return true;
}
}
