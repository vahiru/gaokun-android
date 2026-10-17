#define LOG_TAG "gaokun3-usb"

#include "Port.h"

#include <android/log.h>
#include <sys/system_properties.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>

#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace aidl::android::hardware::usb {

namespace {

constexpr char kUdcSoftConnect[] = "/sys/class/udc/a600000.usb/soft_connect";
constexpr char kController[] = "/sys/bus/platform/devices/a600000.usb";
constexpr char kAdapterOnline[] = "/sys/class/power_supply/gaokun-ec-adapter/online";
constexpr char kTypecPort[] = "/sys/class/typec/port0";
constexpr char kTypecPartner[] = "/sys/class/typec/port0-partner";
constexpr char kPowerRole[] = "/sys/class/typec/port0/power_role";
constexpr char kOrientation[] = "/sys/class/typec/port0/orientation";
constexpr char kPowerOpMode[] = "/sys/class/typec/port0/power_operation_mode";
constexpr char kPartnerAccessory[] = "/sys/class/typec/port0-partner/accessory_mode";
constexpr char kPartnerPd[] = "/sys/class/typec/port0-partner/supports_usb_power_delivery";
constexpr char kRegulators[] = "/sys/class/regulator";

using Dir = std::unique_ptr<DIR, decltype(&closedir)>;

Dir OpenDir(const std::string& path) {
    return Dir(opendir(path.c_str()), closedir);
}

bool Exists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

bool WriteAttr(const char* path, const char* value, bool logError = true) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        if (logError) LOGE("open %s: %s", path, strerror(errno));
        return false;
    }
    ssize_t n = TEMP_FAILURE_RETRY(write(fd, value, strlen(value)));
    int err = errno;
    close(fd);
    if (n < 0) {
        if (logError) LOGE("write %s to %s: %s", value, path, strerror(err));
        return false;
    }
    return true;
}

// <bus>-<port>[.<port>...], e.g. "1-1" or "1-1.4": a device, not a root hub
// ("usb1") or an interface ("1-1:1.0").
bool IsUsbDeviceName(const char* n) {
    if (!isdigit(static_cast<unsigned char>(*n))) return false;
    while (isdigit(static_cast<unsigned char>(*n))) n++;
    if (*n++ != '-' || !*n) return false;
    for (; *n; n++) {
        if (!isdigit(static_cast<unsigned char>(*n)) && *n != '.') return false;
    }
    return true;
}

// Calls fn with the path of each xhci-hcd child of the controller (it only
// exists while the controller is a host) until fn returns true.
template <typename Fn>
bool AnyXhci(Fn fn) {
    Dir ctrl = OpenDir(kController);
    if (!ctrl) return false;
    while (dirent* x = readdir(ctrl.get())) {
        if (strncmp(x->d_name, "xhci-hcd.", strlen("xhci-hcd.")) == 0 &&
            fn(std::string(kController) + "/" + x->d_name)) {
            return true;
        }
    }
    return false;
}

bool HasDownstream() {
    return AnyXhci([](const std::string& xhci) {
        Dir xd = OpenDir(xhci);
        if (!xd) return false;
        while (dirent* h = readdir(xd.get())) {
            if (strncmp(h->d_name, "usb", 3) != 0 ||
                !isdigit(static_cast<unsigned char>(h->d_name[3]))) {
                continue;
            }
            Dir hub = OpenDir(xhci + "/" + h->d_name);
            if (!hub) continue;
            while (dirent* d = readdir(hub.get())) {
                if (IsUsbDeviceName(d->d_name)) return true;
            }
        }
        return false;
    });
}

bool XhciBound() {
    return AnyXhci([](const std::string& xhci) { return Exists(xhci + "/driver"); });
}

// The bracketed value of a Type-C role attribute, e.g. "source [sink]".
std::string Selected(const std::string& attr) {
    size_t open = attr.find('['), close = attr.find(']');
    if (open == std::string::npos || close == std::string::npos || close < open) return "";
    return attr.substr(open + 1, close - open - 1);
}

// Not an accessory, and the partner does not support PD or no PD contract is
// in place. Not PD revision 0.0: this EC reports 0.0 for partners with a
// contract too.
bool PartnerNoPd() {
    std::string acc = ReadAttr(kPartnerAccessory);
    if (!acc.empty() && acc != "none") return false;
    if (ReadAttr(kPartnerPd) == "no") return true;
    std::string mode = ReadAttr(kPowerOpMode);
    return !mode.empty() && mode != "usb_power_delivery";
}

// Whether a uevent can change port0: the role switch, the UDC, USB devices,
// the controller's xhci child (it binds after its root hubs appear), the
// Type-C class, or the EC adapter. Battery and other power supplies change
// often and are skipped.
bool IsRelevant(const char* msg, size_t len, bool* udcReplaced) {
    const char* end = msg + len;
    std::string subsystem, devpath, action;
    for (const char* p = msg; p < end; p += strlen(p) + 1) {
        if (!strncmp(p, "SUBSYSTEM=", 10)) subsystem = p + 10;
        else if (!strncmp(p, "DEVPATH=", 8)) devpath = p + 8;
        else if (!strncmp(p, "ACTION=", 7)) action = p + 7;
    }
    // The UDC is removed in the host role and added again as a new device in
    // the device role.
    if (subsystem == "udc" && (action == "add" || action == "remove")) *udcReplaced = true;
    if (subsystem == "usb_role" || subsystem == "udc" || subsystem == "usb" ||
        subsystem == "typec") {
        return true;
    }
    if (subsystem == "platform") return devpath.find("/a600000.usb/") != std::string::npos;
    if (subsystem == "power_supply") {
        size_t n = strlen(kAdapterName);
        return devpath.size() > n && devpath.compare(devpath.size() - n, n, kAdapterName) == 0 &&
               devpath[devpath.size() - n - 1] == '/';
    }
    return false;
}

}  // namespace

bool Inputs::Enumerated() const {
    return udc == "configured" || udc == "addressed" || udc == "default" || udc == "suspended";
}

std::string ReadAttr(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    char buf[128];
    ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf) - 1));
    close(fd);
    if (n <= 0) return "";
    std::string s(buf, n);
    s = s.substr(0, s.find('\n'));
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

Inputs ReadInputs() {
    Inputs in;
    in.role = ReadAttr(kRoleSwitch);
    in.udc = ReadAttr(kUdcState);
    in.adapter = ReadAttr(kAdapterOnline);
    in.downstream = HasDownstream();
    in.xhci = XhciBound();
    in.typec = Exists(kTypecPort);
    if (in.typec) {
        in.partner = Exists(kTypecPartner);
        in.powerRole = Selected(ReadAttr(kPowerRole));
        in.orientation = ReadAttr(kOrientation);
        in.partnerNoPd = in.partner && PartnerNoPd();
    }
    return in;
}

std::string Describe(const Inputs& in) {
    std::string s = "role=" + (in.role.empty() ? "?" : in.role) +
                    " udc=" + (in.udc.empty() ? "-" : in.udc) +
                    " adapter=" + (in.adapter.empty() ? "?" : in.adapter) +
                    " xhci=" + (in.xhci ? "bound" : "no") +
                    " downstream=" + (in.downstream ? "yes" : "no");
    if (!in.typec) return s + " typec=none";
    s += std::string(" partner=") + (in.partner ? "yes" : "no");
    if (in.partner) {
        s += " power=" + (in.powerRole.empty() ? "?" : in.powerRole) +
             " orientation=" + in.orientation + " pd=" + (in.partnerNoPd ? "no" : "yes");
    }
    return s;
}

bool WriteRole(const std::string& role) {
    return WriteAttr(kRoleSwitch, role.c_str());
}

void RestartPullup() {
    WriteAttr(kUdcSoftConnect, "disconnect");
    WriteAttr(kUdcSoftConnect, "connect");
}

bool HasGadgetState() {
    return Exists(kGadgetState);
}

bool WriteGadgetState(const std::string& state, bool logError) {
    return WriteAttr(kGadgetState, state.c_str(), logError);
}

bool L9dAlwaysOn() {
    Dir regs = OpenDir(kRegulators);
    if (!regs) return false;
    while (dirent* r = readdir(regs.get())) {
        if (strncmp(r->d_name, "regulator.", strlen("regulator.")) != 0) continue;
        std::string base = std::string(kRegulators) + "/" + r->d_name;
        if (ReadAttr((base + "/name").c_str()) != "vreg_l9d") continue;
        return Exists(base + "/of_node/regulator-always-on");
    }
    return false;
}

std::string GetProp(const char* name) {
    const prop_info* pi = __system_property_find(name);
    if (!pi) return "";
    std::string value;
    __system_property_read_callback(
            pi,
            [](void* cookie, const char*, const char* v, uint32_t) {
                *static_cast<std::string*>(cookie) = v;
            },
            &value);
    return value;
}

bool SetProp(const char* name, const std::string& value) {
    if (__system_property_set(name, value.c_str()) == 0) return true;
    LOGE("setting %s to %s failed", name, value.c_str());
    return false;
}

int OpenUeventSocket() {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        LOGE("uevent socket: %s", strerror(errno));
        return -1;
    }
    sockaddr_nl addr = {};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;  // kernel uevents
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOGE("uevent bind: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

bool DrainUevents(int fd, bool* udcReplaced) {
    bool relevant = false;
    char buf[8192];
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) {
                relevant = true;  // overflowed: re-read anyway
                *udcReplaced = true;
                continue;
            }
            break;  // EAGAIN: drained
        }
        buf[n] = '\0';
        if (IsRelevant(buf, n, udcReplaced)) relevant = true;
    }
    return relevant;
}

// sysfs_notify() raises EPOLLPRI after the value has been read once, so the
// attribute is read here to arm it.
int WatchAttr(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[64];
    if (read(fd, buf, sizeof(buf)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool RearmAttr(int fd) {
    char buf[64];
    return lseek(fd, 0, SEEK_SET) == 0 && read(fd, buf, sizeof(buf)) >= 0;
}

}  // namespace aidl::android::hardware::usb
