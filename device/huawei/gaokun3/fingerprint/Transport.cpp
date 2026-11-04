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
#include <ctime>
#include <unistd.h>
namespace gaokun3 {
using namespace std::chrono_literals;
// The Goodix fixed pattern follows temperature: retake the no-finger frame
// this often while no finger is present, and after every lift-off.
constexpr int64_t kBaseRefreshMs = 60000;
// Try again this soon after a rejected candidate (a finger close to the key),
// and after a session's first frame, which nothing could be checked against.
constexpr int64_t kBaseRetryMs = 5000;
// Candidates rejected this many times over this long mean the base itself is
// off (drift), not the candidates: take the latest one.
constexpr unsigned kBaseMaxRejects = 3;
constexpr int64_t kBaseRejectSpanMs = 10000;

static int64_t boottimeMs() {
    timespec t{};
    clock_gettime(CLOCK_BOOTTIME,&t);
    return int64_t(t.tv_sec)*1000 + t.tv_nsec/1000000;
}
static void samples(const std::vector<uint8_t>& raw, std::vector<uint16_t>* out) {
    out->resize(raw.size()/2);
    for (size_t i = 0; i < out->size(); ++i) (*out)[i] = raw[2*i] | raw[2*i+1] << 8;
}

Transport::~Transport() { stop(); if (fd_ >= 0) close(fd_); }
const SensorProfile* Transport::detect() {
    int chip = open("/dev/gpiochip4", O_RDONLY | O_CLOEXEC);
    if (chip < 0) { PLOG(ERROR) << "Fingerprint strap GPIO chip unavailable"; return nullptr; }
    gpiochip_info info{};
    gpio_v2_line_request req{};
    bool ok = ioctl(chip,GPIO_GET_CHIPINFO_IOCTL,&info) == 0 &&
              !strcmp(info.label,"f100000.pinctrl") && info.lines == 230;
    // Straps are read as inputs, bias left as the firmware set it.
    req.offsets[0] = 61; req.offsets[1] = 62; req.num_lines = 2;
    strcpy(req.consumer,"gaokun3-fingerprint-strap");
    req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
    if (ok) ok = ioctl(chip,GPIO_V2_GET_LINE_IOCTL,&req) == 0;
    close(chip);
    gpio_v2_line_values values{}; values.mask = 3;
    if (ok) {
        ok = ioctl(req.fd,GPIO_V2_LINE_GET_VALUES_IOCTL,&values) == 0;
        close(req.fd);
    }
    if (!ok) { PLOG(ERROR) << "Fingerprint strap read failed"; return nullptr; }
    const bool g61 = values.bits & 1, g62 = values.bits & 2;
    const SensorProfile* profile = profileFromStraps(g61,g62);
    LOG(INFO) << "Fingerprint straps GPIO61/62 " << g61 << "/" << g62 << ": "
              << (profile ? profile->name : "no sensor");
    return profile;
}
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
    // FocalTech is the driver's default; kernels without SET_SENSOR stay usable.
    if (profile_->sensor == Sensor::kGoodix) {
        uint32_t sensor = GAOKUN3_FPCMD_SENSOR_GOODIX;
        if (ioctl(fd_,GAOKUN3_FPCMD_IOC_SET_SENSOR,&sensor) < 0) {
            if (errno == ENOTTY) LOG(ERROR) << "Fingerprint kernel driver has no Goodix support";
            else PLOG(ERROR) << "Fingerprint sensor selection failed";
            stop(); return false;
        }
    }
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_INIT) < 0) {
        PLOG(ERROR) << "Fingerprint TA init failed"; stop(); return false;
    }
    clearBase();
    return true;
}
void Transport::clearBase() {
    base_.clear(); baseMs_ = 0; fingerSeen_ = false; baseRejects_ = 0; firstRejectMs_ = 0;
}
bool Transport::stop() {
    if (poisoned_) return false;
    if (loaded_ && ioctl(fd_,GAOKUN3_FPCMD_IOC_UNLOAD) < 0) {
        poisoned_ = true; return false;
    }
    loaded_ = false;
    clearBase();
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
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_GET_FINGER_STATUS,value)) return false;
    if (profile_->sensor != Sensor::kGoodix) return true;
    // Goodix reports physical presence: take the no-finger frame while clear.
    const bool lifted = fingerSeen_ && !*value;
    fingerSeen_ = *value;
    if (!*value && (base_.empty() || lifted || boottimeMs() - baseMs_ >= kBaseRefreshMs))
        return refreshBase();
    return true;
}
bool Transport::refreshBase() {
    std::vector<uint8_t> raw(profile_->captureBytes);
    std::vector<uint16_t> candidate;
    if (!captureRaw(raw.data(),raw.size())) return false;
    samples(raw,&candidate);
    const int64_t now = boottimeMs();
    const bool first = base_.empty();
    bool accept = goodixBaseUsable(candidate.data(),first ? nullptr : base_.data());
    if (!accept && goodixBaseUsable(candidate.data(),nullptr)) {
        if (!baseRejects_++) firstRejectMs_ = now;
        if (baseRejects_ >= kBaseMaxRejects && now - firstRejectMs_ >= kBaseRejectSpanMs) {
            LOG(INFO) << "Fingerprint no-finger frame rejected " << baseRejects_
                      << " times; replacing the base";
            accept = true;
        }
    }
    if (accept) {
        base_ = std::move(candidate);
        baseRejects_ = 0;
        baseMs_ = first ? now - kBaseRefreshMs + kBaseRetryMs : now;
    } else {
        LOG(INFO) << "Fingerprint no-finger frame rejected; keeping the previous one";
        baseMs_ = now - kBaseRefreshMs + kBaseRetryMs;
    }
    return true;
}
bool Transport::captureRaw(uint8_t* data, size_t bytes) {
    gaokun3_fpcmd_capture req{};
    req.image_bytes = bytes; req.user_ptr = reinterpret_cast<uintptr_t>(data);
    if (!loaded_) { errno = ENODEV; return false; }
    if (bytes != profile_->captureBytes) { errno = EINVAL; return false; }
    if (ioctl(fd_,GAOKUN3_FPCMD_IOC_CAPTURE,&req) < 0) {
        PLOG(ERROR) << "Fingerprint image transport failed"; return false;
    }
    return true;
}
bool Transport::capture(Gaokun3Frame* frame, bool* usable) {
    std::vector<uint8_t> raw(profile_->captureBytes);
    if (!captureRaw(raw.data(),raw.size())) return false;
    if (profile_->sensor == Sensor::kGoodix) {
        // Without a no-finger frame yet (finger on the key since start), the
        // frame is unusable; the next lift-off provides one.
        std::vector<uint16_t> px;
        samples(raw,&px);
        *usable = decodeGoodix(px.data(),base_.empty() ? nullptr : base_.data(),frame);
    } else {
        *usable = decode(raw.data(),frame);
    }
    return true;
}
}
