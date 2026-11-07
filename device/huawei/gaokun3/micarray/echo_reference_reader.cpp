#include "echo_reference_reader.h"

#include <dlfcn.h>
#include <sys/system_properties.h>

#include <algorithm>

namespace gaokun::micarray {

namespace {

using gaokun::echoref::EchoReference;

}  // namespace

bool EchoReferenceReader::attach(std::string* why) {
    if (mRing != nullptr) return true;
    // The HAL loads speakerfx from this path (device.mk) the first time a speaker stream opens
    // and keeps it (patches/android/hardware-interfaces/0009).  Loading it here, from the same namespace, gets that same
    // instance, before or after the HAL does; the handle is kept, so it stays for good.
    char path[PROP_VALUE_MAX] = {0};
    if (__system_property_get("ro.vendor.audio.primary.playback_hook", path) <= 0) {
        *why = "no ro.vendor.audio.primary.playback_hook";
        return false;
    }
    void* lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
        const char* e = dlerror();
        *why = e ? e : path;
        return false;
    }
    using Get = EchoReference* (*)();
    auto get = reinterpret_cast<Get>(dlsym(lib, "playback_hook_echo_reference"));
    if (get == nullptr || (mRing = get()) == nullptr) {
        *why = std::string(path) + " has no echo reference";
        dlclose(lib);
        return false;
    }
    return true;
}

size_t EchoReferenceReader::pull(float* out, size_t maxFrames) {
    if (mRing == nullptr) return 0;
    const uint64_t written = mRing->written.load(std::memory_order_acquire);
    if (!mStarted) {
        mCursor = written;
        mStarted = true;
    } else if (written - mCursor > kMaxBacklog) {
        mCursor = written - kRestart;
        ++mSkips;
    }
    const size_t n = static_cast<size_t>(std::min<uint64_t>(maxFrames, written - mCursor));
    for (size_t i = 0; i < n; ++i) out[i] = mRing->mono[(mCursor + i) % EchoReference::kFrames];
    mCursor += n;
    mFrames += n;
    return n;
}

}  // namespace gaokun::micarray
