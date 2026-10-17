/*
 * Port0's state as the kernel shows it, and the writes the HAL makes to it.
 */
#pragma once

#include <string>

namespace aidl::android::hardware::usb {

constexpr char kPortName[] = "port0";
constexpr char kRoleSwitch[] = "/sys/class/usb_role/a600000.usb-role-switch/role";
constexpr char kUdcState[] = "/sys/class/udc/a600000.usb/state";
constexpr char kAdapterName[] = "gaokun-ec-adapter";

// The gadget's connection state for Android's UsbDeviceManager, in the
// android_usb class that patches/0096 adds.
constexpr char kGadgetState[] = "/sys/class/android_usb/android0/state";

// Set to 1 when that class is missing: init.gaokun3.usb.rc then puts adb in
// sys.usb.config itself, as there is no UsbDeviceManager to do it.
constexpr char kNoAndroidUsb[] = "vendor.gaokun3.usb.no_android_usb";

struct Inputs {
    std::string role;     // usb_role: "device", "host" or "none"
    std::string udc;      // UDC state; "" while the controller is a host
    std::string adapter;  // EC adapter online: "1" or "0"; "" if unreadable
    bool downstream = false;  // a device enumerated under the controller's xhci
    bool xhci = false;        // the controller's xhci bound to its driver

    // From the Type-C class, which UCSI fills in. typec is false while port0
    // is not registered (UCSI not up), and the fields below are then unset.
    bool typec = false;
    bool partner = false;
    std::string powerRole;     // "source" or "sink"; "" if unknown
    std::string orientation;   // "normal", "reverse" or "unknown"
    bool partnerNoPd = false;  // no PD with the partner, and not an accessory

    // A host has enumerated the UDC. The state stays at its last value after
    // the cable is pulled, until the gadget's pullup is restarted.
    bool Enumerated() const;
};

Inputs ReadInputs();
std::string Describe(const Inputs& in);

// First line of a sysfs attribute without trailing whitespace; "" if unreadable.
std::string ReadAttr(const char* path);

// Writes "host" or "device" to the role switch. The controller changes role
// asynchronously; the role switch, UDC and xhci uevents follow.
bool WriteRole(const std::string& role);

// Disconnects and reconnects the gadget's pullup, which resets the UDC state
// to "not attached" when no host is there.
void RestartPullup();

// Whether kGadgetState exists, and writes "DISCONNECTED", "CONNECTED" or
// "CONFIGURED" to it; the class sends UsbDeviceManager a uevent on a change.
// logError = false leaves a failed write unlogged.
bool HasGadgetState();
bool WriteGadgetState(const std::string& state, bool logError = true);

// Whether the DT keeps vreg_l9d on (regulator-always-on, patches/0086).
// Without it the controller must not be suspended in the device role (case
// file #52). gaokun3-usbrole.sh guards that at screen off; this HAL, which
// changes the role at any time, then leaves the role alone.
bool L9dAlwaysOn();

std::string GetProp(const char* name);
bool SetProp(const char* name, const std::string& value);

int OpenUeventSocket();
// Drains the socket; true if any uevent that can change port0 arrived, or
// some were lost. Sets *udcReplaced when the UDC was added or removed (or
// uevents were lost): a watched UDC state attribute is then gone.
bool DrainUevents(int fd, bool* udcReplaced);

// Opens a sysfs attribute for epoll (EPOLLPRI on sysfs_notify) and arms it.
int WatchAttr(const char* path);
// Re-arms a watched attribute; false once its device is gone.
bool RearmAttr(int fd);

}  // namespace aidl::android::hardware::usb
