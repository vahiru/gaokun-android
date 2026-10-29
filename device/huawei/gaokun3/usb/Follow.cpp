#define LOG_TAG "gaokun3-usb"

#include "Follow.h"

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

namespace aidl::android::hardware::usb {

namespace {

// How long a role may go without a data connection before the other one is
// tried, and how long a partner we supply may go without PD and data before
// the user is told.
constexpr auto kNoData = std::chrono::seconds(6);

// How long to wait for the controller to show a role written to it before
// deciding again (it takes about 20 ms).
constexpr auto kSwitchSettle = std::chrono::seconds(1);

constexpr char kAdbPause[] = "vendor.gaokun3.usbrole.adb_pause";
constexpr char kReversed[] = "vendor.gaokun3.usbrole.reversed";
constexpr char kBroken[] = "vendor.gaokun3.usbrole.broken";

unsigned Bit(const std::string& role) {
    return role == "host" ? 1 : role == "device" ? 2 : 0;
}

const char* Other(const std::string& role) {
    return role == "host" ? "device" : "host";
}

bool KnownRole(const std::string& role) {
    return role == "host" || role == "device";
}

// A host has enumerated us, or we have enumerated a device.
bool HasData(const Inputs& in) {
    return in.role == "device" ? in.Enumerated() : in.role == "host" && in.downstream;
}

}  // namespace

std::optional<RoleFollow::Clock::time_point> RoleFollow::Tick(const Inputs& in,
                                                              Clock::time_point now) {
    const bool deviceOnly = DeviceOnly();
    if (deviceOnly != mDeviceOnly) {
        mDeviceOnly = deviceOnly;
        Choose("");  // Do not resurrect a pre-policy Settings host choice.
        mSwitching.clear();
    }
    SyncAdbPause(in.role);
    if (deviceOnly) {
        SetFlag(kReversed, false, "device-only policy");
        if (mAttached && !in.partner && in.role == "device") RestartPullup();
        mAttached = in.partner;
        if (in.role != "device") {
            // Retry a failed/asynchronous write without blocking the HAL
            // connection-state reporting.
            Switch(in.role, "device", "device-only policy", now);
            return now + kSwitchSettle;
        }
        mSwitching.clear();
        return std::nullopt;
    }
    if (in.role == "host" && in.xhci) SetFlag(kBroken, false, "the host role came up");

    if (!mSwitching.empty()) {
        Clock::time_point settled = mSwitchingSince + kSwitchSettle;
        if (in.role != mSwitching && now < settled) return settled;
        mSwitching.clear();
    }

    if (!in.partner) {
        if (mAttached) Detached(in, now);
        mAttached = false;
        mIdleSince.reset();
        mSourceIdleSince.reset();
        mTried = 0;
        mSettled = false;
        mChosen.clear();
        SetFlag(kReversed, false, "the partner detached");
        return std::nullopt;
    }
    if (!mAttached) {
        mAttached = true;
        LOGI("follow: partner attached (power %s, role %s)",
             in.powerRole.empty() ? "unknown" : in.powerRole.c_str(), in.role.c_str());
    }

    if (!mChosen.empty()) {
        if (in.role == mChosen) {
            mChosenSeen = true;
        } else if (!mChosenWritten || mChosenSeen) {
            Switch(in.role, mChosen,
                   mChosenSeen ? "restoring the role chosen in Settings" : "chosen in Settings",
                   now);
            mChosenWritten = true;
            mChosenSeen = false;
        }
        return std::nullopt;
    }

    if (in.powerRole != "sink") {
        // We supply power, or the direction is not known yet.
        mIdleSince.reset();
        if (in.powerRole == "source" && in.role == "device") {
            Switch(in.role, "host", "we supply power, so the partner is a device", now);
            mSourceIdleSince.reset();
            return std::nullopt;
        }
        bool broken = GetProp(kBroken) == "1";
        if (in.powerRole == "source" && in.partnerNoPd && !broken && KnownRole(in.role) &&
            !HasData(in)) {
            if (!mSourceIdleSince) mSourceIdleSince = now;
            Clock::time_point due = *mSourceIdleSince + kNoData;
            if (now < due) return due;
            if (in.role == "host" && !in.xhci) {
                SetFlag(kBroken, true, "the host role did not come up");
            } else {
                SetFlag(kReversed, true, "we supply a partner without PD that has no data");
            }
            return std::nullopt;
        }
        mSourceIdleSince.reset();
        SetFlag(kReversed, false, "the partner has PD or data");
        return std::nullopt;
    }

    // We take power.
    mSourceIdleSince.reset();
    SetFlag(kReversed, false, "we take power");
    if (mSettled || !KnownRole(in.role)) {
        mIdleSince.reset();
        return std::nullopt;
    }
    if (HasData(in)) {
        mIdleSince.reset();
        mTried = 0;
        return std::nullopt;
    }
    if (!mIdleSince) mIdleSince = now;
    Clock::time_point due = *mIdleSince + kNoData;
    if (now < due) return due;
    mIdleSince.reset();
    mTried |= Bit(in.role);
    std::string next = Other(in.role);
    if (mTried & Bit(next)) {
        // Tried both: a charger. Stay host until it detaches, as
        // gaokun3-usbrole.sh does.
        mSettled = true;
        if (in.role == "host") {
            LOGI("follow: no data connection in either role (a charger?); staying host");
            return std::nullopt;
        }
        next = "host";
    }
    Switch(in.role, next, "no data connection for 6 s", now);
    return std::nullopt;
}

void RoleFollow::Choose(const std::string& role) {
    mChosen = role == "host" && DeviceOnly() ? "" : role;
    mChosenWritten = false;
    mChosenSeen = false;
    mIdleSince.reset();
    mSourceIdleSince.reset();
    mTried = 0;
    mSettled = false;
}

std::string RoleFollow::Describe() const {
    if (DeviceOnly()) return "device-only policy";
    std::string s = mAttached ? "partner attached" : "no partner";
    if (!mChosen.empty()) s += ", role chosen in Settings: " + mChosen;
    if (mSettled) s += ", settled on host";
    if (mIdleSince) s += ", waiting for a data connection";
    if (mSourceIdleSince) s += ", waiting for PD or data";
    return s;
}

void RoleFollow::Detached(const Inputs& in, Clock::time_point now) {
    if (in.role == "device") {
        // dwc3 does not see VBUS go away: without this the UDC stays in the
        // state the last host left it in, and the next charger or dock would
        // look enumerated.
        RestartPullup();
        LOGI("follow: partner detached; restarted the gadget pullup (UDC %s)",
             ReadAttr(kUdcState).c_str());
    } else if (in.role == "host") {
        Switch(in.role, "device", "the partner detached", now);
    } else {
        LOGI("follow: partner detached");
    }
}

void RoleFollow::Switch(const std::string& from, const std::string& to, const char* why,
                        Clock::time_point now) {
    LOGI("follow: %s -> %s: %s", from.empty() ? "?" : from.c_str(), to.c_str(), why);
    if (!WriteRole(to)) return;
    mSwitching = to;
    mSwitchingSince = now;
}

// In the host role the UDC is gone, and adbd would retry binding its USB
// transport every second; adb_pause makes init pause it (etc/usbrole.rc).
void RoleFollow::SyncAdbPause(const std::string& role) {
    const char* want = role == "host" ? "1" : role == "device" ? "0" : nullptr;
    if (!want || GetProp(kAdbPause) == want) return;
    if (SetProp(kAdbPause, want)) LOGI("follow: role %s: %s=%s", role.c_str(), kAdbPause, want);
}

void RoleFollow::SetFlag(const char* prop, bool on, const char* why) {
    std::string cur = GetProp(prop);
    if (on ? cur == "1" : cur.empty() || cur == "0") return;
    if (SetProp(prop, on ? "1" : "0")) LOGI("follow: %s=%d: %s", prop, on, why);
}

}  // namespace aidl::android::hardware::usb
