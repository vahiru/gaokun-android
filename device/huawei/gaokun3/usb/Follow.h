/*
 * Port0's data role, decided from what is attached. These are the rules of
 * the follow loop gaokun3-usbrole.sh used to run (case file #118 §6-8), run
 * on events instead of a 2 s poll.
 *
 * The EC's UCSI gives the partner's presence, the power direction and PD, but
 * not its data role: it reports partner type UFP for everything. So UCSI
 * leaves usb_0's role switch alone (patches/kernel/ec-usb/0009), and the role comes from
 * electrical facts.
 *   - We supply power: the partner can only be a device (a USB drive, a
 *     keyboard, an unpowered hub). Switch to host at once.
 *   - We take power: a computer, a powered dock or hub, or a charger. That
 *     does not tell the role: a hub with PD pass-through powers us and needs
 *     us as its host. Stay in the current role while it shows a data
 *     connection (enumerated by a host as a device; a device under the xhci
 *     as a host); after 6 s without one, try the other role. With neither
 *     working (a charger), settle on host until the partner detaches.
 *   - The partner detaches: in the device role, restart the gadget's pullup
 *     so the UDC state does not keep the last host's "configured"; in the
 *     host role, go back to device, the role a computer plugged in next
 *     needs.
 * Two properties tell Parts what the user should know (UsbPortNotifier):
 *   - vendor.gaokun3.usbrole.reversed: we supply a partner without PD and
 *     have no data connection with it for 6 s. That is a computer that came
 *     up as the sink; UCSI can only swap the power role with PD, so the user
 *     has to replug. A power-only gadget (a USB fan) looks the same, which is
 *     why the notice asks for a replug only "if this is a computer".
 *   - vendor.gaokun3.usbrole.broken: the host role does not come up.
 * vendor.gaokun3.usbrole.adb_pause follows the role: in the host role there
 * is no UDC, and adbd stops retrying it (etc/usbrole.rc).
 *
 * A role chosen in Settings is kept until the partner detaches, and restored
 * if something else changes it (gaokun3-usbrole.sh at screen off and on).
 *
 * Only for a DT with vreg_l9d always on: then the controller may suspend in
 * either role, so roles change regardless of the screen, and no wakelock is
 * needed around role changes.
 */
#pragma once

#include <chrono>
#include <optional>
#include <string>

#include "Port.h"

namespace aidl::android::hardware::usb {

class RoleFollow {
  public:
    using Clock = std::chrono::steady_clock;

    // Acts on the current inputs. Returns when to run again if nothing else
    // happens by then.
    std::optional<Clock::time_point> Tick(const Inputs& in, Clock::time_point now);

    // A role chosen in Settings, "host" or "device", kept until the partner
    // detaches; "" returns to the automatic choice.
    void Choose(const std::string& role);

    std::string Describe() const;

  private:
    void Detached(const Inputs& in, Clock::time_point now);
    void Switch(const std::string& from, const std::string& to, const char* why,
                Clock::time_point now);
    void SyncAdbPause(const std::string& role);
    void SetFlag(const char* prop, bool on, const char* why);

    bool mAttached = false;
    // The role last written, until the controller shows it: it changes role
    // asynchronously, and inputs read before that still show the old one.
    std::string mSwitching;
    Clock::time_point mSwitchingSince;
    // Taking power: since when the current role has had no data connection,
    // which roles were tried, and whether neither worked.
    std::optional<Clock::time_point> mIdleSince;
    unsigned mTried = 0;
    bool mSettled = false;
    // Supplying power: since when the partner has had no PD and no data.
    std::optional<Clock::time_point> mSourceIdleSince;
    // Settings' choice, whether it has been written, and whether the role
    // has matched it since (if something changes it later, it is restored).
    std::string mChosen;
    bool mChosenWritten = false;
    bool mChosenSeen = false;
};

}  // namespace aidl::android::hardware::usb
