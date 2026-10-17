/*
 * USB port HAL (android.hardware.usb IUsb V3) for the Huawei MateBook E Go (gaokun3).
 *
 * It reports port0 to UsbPortManager (UsbManager.getPorts(), `dumpsys usb`)
 * and decides port0's data role from what is attached (Follow.h). Without a
 * port HAL, getPorts() is empty.
 *
 * It also feeds UsbDeviceManager, which runs Settings' USB page ("USB
 * controlled by", "Use USB for") and MTP/PTP. The framework only creates it
 * when /sys/class/android_usb exists, and takes the gadget's connection state
 * from android0/state and its uevents, which Android kernels send from their
 * configfs gadget. patches/0096 adds that class with a writable state; this
 * HAL writes it (GadgetState), as the UDC state alone does not show an unplug.
 * On a kernel without the class, it sets vendor.gaokun3.usb.no_android_usb,
 * and init.gaokun3.usb.rc puts adb in sys.usb.config as before 0096.
 *
 * Only port0 is reported: the USB-C port on the dwc3 controller a600000.usb,
 * the only one that can be a USB device. The other USB-C port (a800000.usb) is
 * host-only, so Settings has no roles to offer for it, and UsbDeviceManager
 * keeps one port state (whichever port changed last), so reporting it would
 * make that state flip between the two ports.
 *
 * Status. With the Type-C class (UCSI up), the port is connected while a
 * partner is attached; the power role is UCSI's, the data role is the
 * controller's role switch. Without it, the rules of gaokun3-usbrole.sh
 * (host_attached, has_downstream) stand in:
 *   - Device role, connected: the EC reports an adapter online and a host has
 *     enumerated the UDC. The UDC state alone is not enough: it stays
 *     "configured" after the cable is pulled, while the adapter goes offline
 *     at once. Switching USB functions rebinds the gadget, so the enumeration
 *     drops for about 0.2 s while VBUS stays; such a drop is held for 2 s
 *     before it counts as a disconnect.
 *   - Host role, connected: a device is enumerated under the controller's xhci.
 *
 * Roles. With the Type-C class and vreg_l9d always on (patches/0086), this
 * HAL decides the data role (Follow.h); gaokun3-usbrole.sh still switches it
 * at screen off and on (etc/usbrole.rc). A role can also be chosen through
 * UsbManager: Settings' "USB controlled by", or `dumpsys usb set-port-roles
 * port0 source host` / `sink device`. The EC cannot swap the power role, so
 * the port reports that it can change mode, and the framework asks for a
 * mode: DFP for host, UFP for device. Otherwise the HAL reports status only,
 * and vendor.gaokun3.usbrole.adb_pause then changes only when
 * gaokun3-usbrole.sh switches the role at screen off and on, no longer
 * within seconds of any role change as with the script's old follow loop.
 * Power role swaps, USB data disabling, power limiting and port reset are
 * answered with NOT_SUPPORTED.
 *
 * Changes are pushed from uevents (role switch, UDC, USB devices, the xhci,
 * the Type-C class, the EC adapter) and from sysfs_notify on the UDC state,
 * which changes without a uevent while a host enumerates the tablet.
 *
 * Debug: `dumpsys android.hardware.usb.IUsb/default`, or run the binary with
 * --status as root; both print the inputs, the status they map to and the
 * gadget state. Changes and role decisions are logged under the tag
 * gaokun3-usb.
 */
#define LOG_TAG "gaokun3-usb"

#include <aidl/android/hardware/usb/BnUsb.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "Follow.h"
#include "Port.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aidl::android::hardware::usb {

namespace {

using ::ndk::ScopedAStatus;
using Clock = std::chrono::steady_clock;

// Re-read interval when the uevent socket cannot be opened.
constexpr int kFallbackPollMs = 2000;

// How long a device-role connection is kept while VBUS stays but the UDC is no
// longer enumerated (the gadget rebinding). The framework debounces USB_STATE
// disconnects, but not port status, so without this Settings flickers. Only
// without the Type-C class, which keeps the partner through a rebind.
constexpr auto kRebindHold = std::chrono::seconds(2);

// How long a role chosen in Settings may take to come up.
constexpr auto kSwitchTimeout = std::chrono::seconds(6);

PortStatus ToStatus(const Inputs& in, bool following) {
    PortStatus s;
    s.portName = kPortName;
    s.supportedModes = {PortMode::DRP};
    s.usbDataStatus = {UsbDataStatus::ENABLED};
    s.canChangeMode = following;
    bool host = in.role == "host";
    if (in.typec) {
        if (!in.partner || (!host && in.role != "device")) return s;
    } else if (host ? !in.downstream
                    // Like gaokun3-usbrole.sh, an unreadable adapter node counts as online.
                    : in.role != "device" || in.adapter == "0" || !in.Enumerated()) {
        return s;
    }
    s.currentMode = host ? PortMode::DFP : PortMode::UFP;
    s.currentDataRole = host ? PortDataRole::HOST : PortDataRole::DEVICE;
    bool source = in.powerRole.empty() ? host : in.powerRole == "source";
    s.currentPowerRole = source ? PortPowerRole::SOURCE : PortPowerRole::SINK;
    return s;
}

// The gadget's connection state as Android kernels report it from their
// configfs gadget: CONFIGURED once a host has configured it, CONNECTED while a
// host is attached, else DISCONNECTED. The UDC state stays "configured" after
// the cable is pulled, so a host only counts while something is attached: a
// partner when the Type-C class is up, else the EC's adapter online (as in
// ToStatus, an unreadable adapter node counts as online). The adapter is not
// used with the Type-C class: it reads 0 while we supply power. A suspended
// bus keeps the last state.
std::string GadgetState(const Inputs& in, const std::string& last) {
    bool attached = in.typec ? in.partner : in.adapter != "0";
    if (!attached || in.udc.empty() || in.udc == "not attached") return "DISCONNECTED";
    if (in.udc == "configured") return "CONFIGURED";
    if (in.udc == "suspended") return last.empty() ? "CONNECTED" : last;
    return "CONNECTED";
}

std::string Describe(const PortStatus& s) {
    if (s.currentMode == PortMode::NONE) return "not connected";
    return toString(s.currentMode) + ", data " + toString(s.currentDataRole) + ", power " +
           toString(s.currentPowerRole);
}

void Check(const ScopedAStatus& ret, const char* what) {
    if (!ret.isOk()) LOGE("%s: %s", what, ret.getDescription().c_str());
}

int MsUntil(Clock::time_point due, Clock::time_point now) {
    auto left = std::chrono::ceil<std::chrono::milliseconds>(due - now);
    return static_cast<int>(std::clamp<int64_t>(left.count(), 0, INT_MAX));
}

}  // namespace

class Usb : public BnUsb {
  public:
    Usb() : mWake(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {}

    // Starts the thread that pushes status changes and follows the partner;
    // it runs for the life of the process.
    void Start() {
        // Without the class the framework has no UsbDeviceManager to put adb
        // in sys.usb.config; init.gaokun3.usb.rc then does.
        if (!HasGadgetState()) {
            LOGW("no android_usb class (a kernel without patches/0096): %s=1", kNoAndroidUsb);
            SetProp(kNoAndroidUsb, "1");
        }
        std::thread([this] { Monitor(); }).detach();
    }

    // Lets the HAL decide the data role, once it is registered.
    void EnableFollow() {
        {
            std::lock_guard<std::mutex> lock(mLock);
            mRegistered = true;
        }
        Wake();
    }

    ScopedAStatus setCallback(const std::shared_ptr<IUsbCallback>& in_callback) override {
        std::lock_guard<std::mutex> lock(mLock);
        mCallback = in_callback;
        LOGI("callback %s", in_callback ? "registered" : "cleared");
        return ScopedAStatus::ok();
    }

    ScopedAStatus queryPortStatus(int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        Report(Evaluate(ReadInputs(), Clock::now()), true);
        if (mCallback) {
            Check(mCallback->notifyQueryPortStatus("all", Status::SUCCESS, in_transactionId),
                  "notifyQueryPortStatus");
        }
        return ScopedAStatus::ok();
    }

    // Settings' "USB controlled by" asks for a mode (DFP: host, UFP: device),
    // as the port can change mode but not its power role. DRP returns to the
    // automatic choice. The result is sent once the controller is up in the
    // new role (FinishSwitch).
    ScopedAStatus switchRole(const std::string& in_portName, const PortRole& in_role,
                             int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        std::optional<std::string> target;
        if (in_portName == kPortName && mFollowing && in_role.getTag() == PortRole::Tag::mode) {
            switch (in_role.get<PortRole::Tag::mode>()) {
                case PortMode::DFP: target = "host"; break;
                case PortMode::UFP: target = "device"; break;
                case PortMode::DRP: target = ""; break;
                default: break;
            }
        }
        Status status = Status::NOT_SUPPORTED;
        if (target) {
            if (!ReadInputs().partner) {
                status = Status::ERROR;
            } else {
                if (mSwitch) Finish(false, "superseded");
                mFollow.Choose(*target);
                Wake();
                if (!target->empty()) {
                    mSwitch = PendingSwitch{in_role, in_transactionId, *target,
                                            Clock::now() + kSwitchTimeout};
                    LOGI("switchRole(%s): switching to %s", in_role.toString().c_str(),
                         target->c_str());
                    return ScopedAStatus::ok();
                }
                status = Status::SUCCESS;
            }
        }
        LOGI("switchRole(%s, %s): %s", in_portName.c_str(), in_role.toString().c_str(),
             toString(status).c_str());
        if (mCallback) {
            Check(mCallback->notifyRoleSwitchStatus(in_portName, in_role, status,
                                                    in_transactionId),
                  "notifyRoleSwitchStatus");
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus enableContaminantPresenceDetection(const std::string& in_portName,
                                                     bool /*in_enable*/,
                                                     int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        if (mCallback) {
            Check(mCallback->notifyContaminantEnabledStatus(in_portName, false,
                                                            Status::NOT_SUPPORTED,
                                                            in_transactionId),
                  "notifyContaminantEnabledStatus");
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus enableUsbData(const std::string& in_portName, bool in_enable,
                                int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        // Lineage's setTrustRestrictUsb calls this on every state change.
        if (!mEnableUsbDataLogged) {
            LOGI("enableUsbData(%s, %d): not supported (logged once)", in_portName.c_str(),
                 in_enable);
            mEnableUsbDataLogged = true;
        }
        if (mCallback) {
            Check(mCallback->notifyEnableUsbDataStatus(in_portName, in_enable,
                                                       Status::NOT_SUPPORTED, in_transactionId),
                  "notifyEnableUsbDataStatus");
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus enableUsbDataWhileDocked(const std::string& in_portName,
                                           int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        if (mCallback) {
            Check(mCallback->notifyEnableUsbDataWhileDockedStatus(
                          in_portName, Status::NOT_SUPPORTED, in_transactionId),
                  "notifyEnableUsbDataWhileDockedStatus");
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus limitPowerTransfer(const std::string& in_portName, bool in_limit,
                                     int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        if (mCallback) {
            Check(mCallback->notifyLimitPowerTransferStatus(in_portName, in_limit,
                                                            Status::NOT_SUPPORTED,
                                                            in_transactionId),
                  "notifyLimitPowerTransferStatus");
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus resetUsbPort(const std::string& in_portName,
                               int64_t in_transactionId) override {
        std::lock_guard<std::mutex> lock(mLock);
        if (mCallback) {
            Check(mCallback->notifyResetUsbPortStatus(in_portName, Status::NOT_SUPPORTED,
                                                      in_transactionId),
                  "notifyResetUsbPortStatus");
        }
        return ScopedAStatus::ok();
    }

    binder_status_t dump(int fd, const char** /*args*/, uint32_t /*numArgs*/) override {
        Inputs in = ReadInputs();
        std::string out;
        {
            std::lock_guard<std::mutex> lock(mLock);
            out = std::string(kPortName) + ": " + Describe(mLast) +
                  (mHoldUntil ? " (held while the gadget rebinds)" : "") + "\n" + Describe(in) +
                  "\ndata role: " +
                  (mFollowing ? "decided here; " + mFollow.Describe()
                              : std::string("not decided here (needs the Type-C port and "
                                            "vreg_l9d always on)")) +
                  (mSwitch ? "; switching to " + mSwitch->target : "") +
                  "\ngadget state: " +
                  (mGadgetState.empty() ? std::string("not written") : mGadgetState) +
                  "\ncallback: " + (mCallback ? "registered" : "none") + "\n";
        }
        if (write(fd, out.data(), out.size()) < 0) return -errno;
        return STATUS_OK;
    }

  private:
    struct PendingSwitch {
        PortRole role;
        int64_t transactionId;
        std::string target;
        Clock::time_point deadline;
    };

    void Wake() {
        uint64_t one = 1;
        if (write(mWake, &one, sizeof(one)) < 0) LOGE("eventfd: %s", strerror(errno));
    }

    // The status to report: the inputs' status, except that without the
    // Type-C class a device-role connection is held for kRebindHold while
    // VBUS stays. Called with mLock held.
    PortStatus Evaluate(const Inputs& in, Clock::time_point now) {
        PortStatus s = ToStatus(in, mFollowing);
        bool rebinding = !in.typec && s.currentMode == PortMode::NONE &&
                         mLast.currentMode == PortMode::UFP && in.role == "device" &&
                         in.adapter != "0";
        if (!rebinding) {
            mHoldUntil.reset();
            return s;
        }
        if (!mHoldUntil) mHoldUntil = now + kRebindHold;
        if (now < *mHoldUntil) return mLast;
        mHoldUntil.reset();
        return s;
    }

    // Sends the status to the framework if it changed, or always when forced
    // (a query). Called with mLock held.
    void Report(const PortStatus& s, bool force) {
        if (!(s == mLast)) LOGI("%s: %s", kPortName, Describe(s).c_str());
        else if (!force) return;
        mLast = s;
        if (!mCallback) return;
        Check(mCallback->notifyPortStatusChange({s}, Status::SUCCESS), "notifyPortStatusChange");
    }

    // Starts deciding the data role once registered with the Type-C class up
    // and vreg_l9d always on, and stops if the Type-C port goes away.
    // Called with mLock held.
    void UpdateFollowing(const Inputs& in) {
        bool want = mRegistered && in.typec;
        if (want && !mL9d) {
            mL9d = L9dAlwaysOn();
            if (!*mL9d) LOGI("vreg_l9d is not always on: not deciding the data role");
        }
        want = want && mL9d.value_or(false);
        if (want == mFollowing) return;
        mFollowing = want;
        mFollow = RoleFollow();
        mFollowDue.reset();
        LOGI("%s", want ? "deciding the data role"
                        : "the Type-C port is gone: no longer deciding the data role");
    }

    // While the HAL does not decide the data role, does what RoleFollow does
    // when the partner detaches in the device role: restart the gadget's
    // pullup. dwc3 does not see VBUS go away, so the UDC state would keep the
    // last host's "configured", and the next partner, a charger too, would
    // look enumerated. Called with mLock held.
    void UpdatePartner(const Inputs& in) {
        bool detached = in.typec && mPartner && !in.partner;
        mPartner = in.typec && in.partner;
        if (!detached || mFollowing || in.role != "device") return;
        RestartPullup();
        LOGI("partner detached; restarted the gadget pullup (UDC %s)",
             ReadAttr(kUdcState).c_str());
    }

    // Mirrors the gadget's connection state into the android_usb class for
    // UsbDeviceManager. Called with mLock held.
    void UpdateGadgetState(const Inputs& in) {
        std::string s = GadgetState(in, mGadgetState);
        if (s == mGadgetState) return;
        if (!HasGadgetState()) {
            if (!mNoGadgetStateLogged) LOGW("no %s: the gadget state is not mirrored", kGadgetState);
            mNoGadgetStateLogged = true;
            return;
        }
        // Retried on every update: log only the first failure in a row.
        if (!WriteGadgetState(s, !mGadgetStateWriteFailed)) {
            mGadgetStateWriteFailed = true;
            return;
        }
        mGadgetStateWriteFailed = false;
        LOGI("gadget state %s (UDC %s, adapter %s)", s.c_str(),
             in.udc.empty() ? "absent" : in.udc.c_str(), in.adapter.empty() ? "?" : in.adapter.c_str());
        mGadgetState = s;
    }

    // Reports the pending switchRole once the controller is up in the new
    // role, or when it times out or no longer applies. Called with mLock held.
    void FinishSwitch(const Inputs& in, Clock::time_point now) {
        if (!mSwitch) return;
        const std::string& want = mSwitch->target;
        bool done = in.role == want && (want == "host" ? in.xhci : !in.udc.empty());
        if (done) Finish(true, "done");
        else if (!mFollowing || !in.partner) Finish(false, "the partner is gone");
        else if (now >= mSwitch->deadline) Finish(false, "timed out");
    }

    void Finish(bool ok, const char* why) {
        LOGI("switchRole to %s: %s", mSwitch->target.c_str(), why);
        if (mCallback) {
            Check(mCallback->notifyRoleSwitchStatus(kPortName, mSwitch->role,
                                                    ok ? Status::SUCCESS : Status::ERROR,
                                                    mSwitch->transactionId),
                  "notifyRoleSwitchStatus");
        }
        mSwitch.reset();
    }

    // Re-reads the port, lets RoleFollow act on it and reports the status.
    // Called with mLock held.
    void Update(Clock::time_point now) {
        Inputs in = ReadInputs();
        UpdateFollowing(in);
        mFollowDue = mFollowing ? mFollow.Tick(in, now) : std::nullopt;
        UpdatePartner(in);
        Report(Evaluate(in, now), false);
        UpdateGadgetState(in);
        FinishSwitch(in, now);
    }

    void Monitor() {
        int ep = epoll_create1(EPOLL_CLOEXEC);
        int uevents = OpenUeventSocket();
        if (uevents >= 0) {
            epoll_event ev = {.events = EPOLLIN, .data = {.fd = uevents}};
            epoll_ctl(ep, EPOLL_CTL_ADD, uevents, &ev);
        } else {
            LOGW("no uevents; re-reading every %d ms", kFallbackPollMs);
        }
        if (mWake >= 0) {
            epoll_event ev = {.events = EPOLLIN, .data = {.fd = mWake}};
            epoll_ctl(ep, EPOLL_CTL_ADD, mWake, &ev);
        }
        int udc = -1;
        for (bool check = true;;) {
            if (udc < 0 && (udc = WatchAttr(kUdcState)) >= 0) {
                epoll_event ev = {.events = EPOLLPRI, .data = {.fd = udc}};
                epoll_ctl(ep, EPOLL_CTL_ADD, udc, &ev);
                check = true;
            }
            int timeout = uevents >= 0 ? -1 : kFallbackPollMs;
            {
                std::lock_guard<std::mutex> lock(mLock);
                Clock::time_point now = Clock::now();
                if (check) Update(now);
                // Wake for the end of a rebind hold, RoleFollow's next
                // decision and a switchRole deadline.
                std::optional<Clock::time_point> switchDue;
                if (mSwitch) switchDue = mSwitch->deadline;
                for (const std::optional<Clock::time_point>& due :
                     {mHoldUntil, mFollowDue, switchDue}) {
                    if (!due) continue;
                    int ms = MsUntil(*due, now);
                    timeout = timeout < 0 ? ms : std::min(timeout, ms);
                }
            }
            check = false;
            epoll_event events[4];
            int n = epoll_wait(ep, events, 4, timeout);
            if (n < 0) {
                if (errno != EINTR) {
                    LOGE("epoll_wait: %s", strerror(errno));
                    sleep(1);
                }
                continue;
            }
            if (n == 0) check = true;
            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;
                if (fd == uevents) {
                    bool udcReplaced = false;
                    if (DrainUevents(uevents, &udcReplaced)) check = true;
                    if (udcReplaced && udc >= 0) {
                        // Removing the UDC does not wake its watched state
                        // attribute, and the new one is a new file: watch
                        // that one instead (at the top of the loop).
                        epoll_ctl(ep, EPOLL_CTL_DEL, udc, nullptr);
                        close(udc);
                        udc = -1;
                    }
                } else if (fd == udc) {
                    if (!RearmAttr(udc)) {
                        epoll_ctl(ep, EPOLL_CTL_DEL, udc, nullptr);
                        close(udc);
                        udc = -1;
                    }
                    check = true;
                } else if (fd == mWake) {
                    uint64_t count;
                    if (read(mWake, &count, sizeof(count)) < 0 && errno != EAGAIN) {
                        LOGE("eventfd: %s", strerror(errno));
                    }
                    check = true;
                }
            }
        }
    }

    const int mWake;
    std::mutex mLock;
    std::shared_ptr<IUsbCallback> mCallback;
    PortStatus mLast;
    std::optional<Clock::time_point> mHoldUntil;
    bool mRegistered = false;
    std::optional<bool> mL9d;
    bool mFollowing = false;
    bool mPartner = false;
    RoleFollow mFollow;
    std::optional<Clock::time_point> mFollowDue;
    std::optional<PendingSwitch> mSwitch;
    std::string mGadgetState;
    bool mNoGadgetStateLogged = false;
    bool mGadgetStateWriteFailed = false;
    bool mEnableUsbDataLogged = false;
};

}  // namespace aidl::android::hardware::usb

int main(int argc, char** argv) {
    using namespace ::aidl::android::hardware::usb;

    if (argc > 1 && !strcmp(argv[1], "--status")) {
        Inputs in = ReadInputs();
        std::string gadget = ReadAttr(kGadgetState);
        printf("%s: %s\n%s\nvreg_l9d always on: %s\ngadget state: %s (from these inputs: %s)\n",
               kPortName, Describe(ToStatus(in, false)).c_str(), Describe(in).c_str(),
               L9dAlwaysOn() ? "yes" : "no", gadget.empty() ? "no android_usb class" : gadget.c_str(),
               GadgetState(in, gadget).c_str());
        return 0;
    }

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Usb> usb = ::ndk::SharedRefBase::make<Usb>();
    usb->Start();

    const std::string instance = std::string(Usb::descriptor) + "/default";
    binder_exception_t err = AServiceManager_addService(usb->asBinder().get(), instance.c_str());
    if (err != EX_NONE) {
        LOGE("registering %s failed: %d", instance.c_str(), err);
        return 1;
    }
    LOGI("registered %s", instance.c_str());
    usb->EnableFollow();

    ABinderProcess_joinThreadPool();
    return 1;  // not reached
}
