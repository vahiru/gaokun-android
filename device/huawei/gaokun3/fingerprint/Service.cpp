// SPDX-License-Identifier: Apache-2.0
#include "Core.h"
#include "Transport.h"
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_ibinder.h>
#include <android-base/logging.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace aidl::android::hardware::biometrics::fingerprint {
using ndk::ScopedAStatus;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Hat = keymaster::HardwareAuthToken;
using Signal = std::shared_ptr<common::ICancellationSignal>;
static ScopedAStatus invalid() { return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT); }
// Framework clients catch RemoteException for a lost HAL/session. An AIDL
// IllegalStateException escapes FingerprintStartUserClient and kills system_server.
static ScopedAStatus unavailable() { return ScopedAStatus::fromStatus(STATUS_DEAD_OBJECT); }

class Cancellation final : public common::BnCancellationSignal {
  public:
    explicit Cancellation(std::shared_ptr<std::atomic_bool> flag) : flag_(std::move(flag)) {}
    ScopedAStatus cancel() override { *flag_ = true; return ScopedAStatus::ok(); }
  private:
    std::shared_ptr<std::atomic_bool> flag_;
};

class Session final : public BnSession {
  public:
    Session(const gaokun3::SensorProfile& profile, int32_t user,
            std::shared_ptr<ISessionCallback> cb,
            std::shared_ptr<gaokun3::Lockout> lockout,
            std::shared_ptr<std::mutex> hardwareMutex,
            std::shared_ptr<Session> predecessor = nullptr)
        : cb_(std::move(cb)),
          dir_("/data/vendor_de/"+std::to_string(user)+"/fpdata"),
          store_(dir_+"/gaokun3-templates.db",profile), transport_(profile),
          lockout_(std::move(lockout)),
          hardwareMutex_(std::move(hardwareMutex)), predecessor_(std::move(predecessor)),
          worker_([this] { loop(); }) {}
    ~Session() override {
        close();
        { std::lock_guard lock(mutex_); quit_ = true; }
        cv_.notify_all();
        worker_.join();
    }
    bool linkToDeath() {
        if (!AIBinder_isRemote(cb_->asBinder().get())) return true;
        death_ = AIBinder_DeathRecipient_new([](void*) { _exit(1); });
        return AIBinder_linkToDeath(cb_->asBinder().get(),death_,nullptr) == STATUS_OK;
    }
    bool closed() const { return drained_; }
    bool finished() const { return finished_; }
    void retire() { requestClose(false); }
    void waitDrained() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock,[this] { return drained_.load(); });
    }
#ifdef GAOKUN3_FP_TEST
    void setStoreDirectoryForTest(const std::string& dir) {
        dir_ = dir;
        store_ = gaokun3::Store(dir_+"/gaokun3-templates.db",store_.profile());
    }
    void setCaptureForTest(std::function<void(gaokun3::Transport&, const std::atomic_bool&)> hook) {
        captureForTest_ = std::move(hook);
    }
    void reportHardwareUnavailableForTest() {
        command([this] { error(Error::HW_UNAVAILABLE); });
    }
#endif
    ScopedAStatus generateChallenge() override {
        return command([this] {
            for (auto i = challenges_.begin(); i != challenges_.end();) {
                if (Clock::now()-i->second > 10min) i = challenges_.erase(i); else ++i;
            }
            int64_t c = gaokun3::randomId();
            if (!c || challenges_.size() >= 16) { error(Error::UNABLE_TO_PROCESS); return; }
            challenges_[c] = Clock::now();
            cb_->onChallengeGenerated(c);
        });
    }
    ScopedAStatus revokeChallenge(int64_t c) override {
        return command([this,c] {
            challenges_.erase(c); cb_->onChallengeRevoked(c);
        });
    }
    ScopedAStatus enroll(const Hat& hat, Signal* out) override {
        return operation(1,hat,out);
    }
    ScopedAStatus authenticate(int64_t operationId, Signal* out) override {
        // This host matcher has no HAT signing key; crypto authentication is unsupported.
        if (operationId != 0) return invalid();
        return operation(2,{},out);
    }
    ScopedAStatus detectInteraction(Signal* out) override { return operation(3,{},out); }
    ScopedAStatus enumerateEnrollments() override {
        return command([this] {
            if (!load()) return;
            std::vector<int32_t> ids;
            for (auto& e : store_.enrollments) ids.push_back(e.id);
            cb_->onEnrollmentsEnumerated(ids);
        });
    }
    ScopedAStatus removeEnrollments(const std::vector<int32_t>& ids) override {
        if (ids.size() > gaokun3::kMaxFingers ||
            std::any_of(ids.begin(),ids.end(),[](int32_t id){ return id <= 0; })) return invalid();
        return command([this,ids] {
            if (!load()) return;
            std::vector<int32_t> removed;
            auto& es = store_.enrollments;
            es.erase(std::remove_if(es.begin(),es.end(),[&](const auto& e) {
                if (std::find(ids.begin(),ids.end(),e.id) == ids.end()) return false;
                removed.push_back(e.id); return true;
            }),es.end());
            if (!removed.empty() && !save()) return;
            cb_->onEnrollmentsRemoved(removed);
        });
    }
    ScopedAStatus getAuthenticatorId() override {
        return command([this] { if (load()) cb_->onAuthenticatorIdRetrieved(store_.authenticatorId); });
    }
    ScopedAStatus invalidateAuthenticatorId() override {
        return command([this] {
            if (load() && save()) cb_->onAuthenticatorIdInvalidated(store_.authenticatorId);
        });
    }
    ScopedAStatus resetLockout(const Hat& hat) override {
        return command([this,hat] {
            // Development Class 1: trusted framework caller and structural
            // credential-token checks; cryptographic MAC verification is absent.
            if (!hat.userId || hat.mac.size() != 32 ||
                !(static_cast<int32_t>(hat.authenticatorType) & 1)) {
                error(Error::UNABLE_TO_PROCESS); return;
            }
            if (!load()) return;
            store_.failures = 0;
            if (!store_.save()) { error(Error::UNABLE_TO_PROCESS); return; }
            lockout_->reset(); clearLockoutTimer(); cb_->onLockoutCleared();
        });
    }
    ScopedAStatus close() override {
        requestClose(true);
        return ScopedAStatus::ok();
    }
    ScopedAStatus authenticateWithContext(int64_t id, const common::OperationContext&, Signal* o) override {
        return authenticate(id,o);
    }
    ScopedAStatus enrollWithContext(const Hat& h, const common::OperationContext&, Signal* o) override {
        return enroll(h,o);
    }
    ScopedAStatus detectInteractionWithContext(const common::OperationContext&, Signal* o) override {
        return detectInteraction(o);
    }
    ScopedAStatus onPointerDown(int32_t,int32_t,int32_t,float,float) override { return ScopedAStatus::ok(); }
    ScopedAStatus onPointerUp(int32_t) override { return ScopedAStatus::ok(); }
    ScopedAStatus onUiReady() override { return ScopedAStatus::ok(); }
    ScopedAStatus onPointerDownWithContext(const PointerContext&) override { return ScopedAStatus::ok(); }
    ScopedAStatus onPointerUpWithContext(const PointerContext&) override { return ScopedAStatus::ok(); }
    ScopedAStatus onPointerCancelWithContext(const PointerContext&) override { return ScopedAStatus::ok(); }
    ScopedAStatus onContextChanged(const common::OperationContext&) override { return ScopedAStatus::ok(); }
    ScopedAStatus setIgnoreDisplayTouches(bool) override { return ScopedAStatus::ok(); }
  private:
    void waitForPredecessor() {
        if (predecessor_) {
            predecessor_->waitDrained();
            predecessor_.reset();
        }
    }
    void requestClose(bool notify) {
        std::lock_guard lock(mutex_);
        if (!notify) silent_ = true;
        if (closed_.exchange(true)) return;
        if (cancel_) *cancel_ = true;
        queue_.push_back([this,notify] {
            lockoutDeadline_ = Clock::time_point::max();
            waitForPredecessor();
            {
                std::lock_guard hardwareLock(*hardwareMutex_);
                if (!transport_.stop()) LOG(ERROR) << "Fingerprint TA shutdown failed; transport disabled";
            }
            if (death_) {
                AIBinder_unlinkToDeath(cb_->asBinder().get(),death_,nullptr);
                AIBinder_DeathRecipient_delete(death_); death_ = nullptr;
            }
            { std::lock_guard lock(mutex_); drained_ = true; }
            cv_.notify_all();
            if (notify && !silent_) cb_->onSessionClosed();
            finished_ = true;
        });
        cv_.notify_all();
    }
    void error(Error e) {
        if (silent_) return;
        // AidlResponseHandler drops the framework session on HW_UNAVAILABLE.
        // Retire ours too, before a retry can arrive. No unsolicited
        // onSessionClosed should be delivered to the replacement's scheduler.
        if (e == Error::HW_UNAVAILABLE) retire();
        cb_->onError(e,0);
    }
    bool load() {
        if (mkdir(dir_.c_str(),0700) && errno != EEXIST) { error(Error::UNABLE_TO_PROCESS); return false; }
        struct stat st{};
        if (lstat(dir_.c_str(),&st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0700 || !store_.load()) {
            error(Error::UNABLE_TO_PROCESS); return false;
        }
        updateLockoutTimer();
        return true;
    }
    bool save() {
        store_.authenticatorId = store_.enrollments.empty() ? 0 : gaokun3::randomId();
        if (store_.enrollments.empty()) {
            store_.failures = 0; lockout_->reset(); clearLockoutTimer();
        }
        if ((!store_.enrollments.empty() && !store_.authenticatorId) || !store_.save()) {
            error(Error::UNABLE_TO_PROCESS); return false;
        }
        return true;
    }
    static int64_t monotonicMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now().time_since_epoch()).count();
    }
    void clearLockoutTimer() {
        lockoutDeadline_ = Clock::time_point::max();
        notifiedExpiryFailures_ = 0;
    }
    int64_t updateLockoutTimer() {
        const int64_t wait = lockout_->remaining(store_.failures,monotonicMs());
        if (wait > 0) {
            lockoutDeadline_ = Clock::now()+std::chrono::milliseconds(wait);
        } else if (wait == 0 && store_.failures && store_.failures < 20 &&
                   store_.failures % 5 == 0 && store_.failures != notifiedExpiryFailures_) {
            // Also restore notification after a session replacement at expiry.
            lockoutDeadline_ = Clock::now();
        } else {
            lockoutDeadline_ = Clock::time_point::max();
        }
        return wait;
    }
    void maybeClearTimedLockout() {
        if (lockoutDeadline_ == Clock::time_point::max() ||
            Clock::now() < lockoutDeadline_) return;
        lockoutDeadline_ = Clock::time_point::max();
        if (closed_) return;
        const int64_t wait = lockout_->remaining(store_.failures,monotonicMs());
        if (wait > 0) {
            lockoutDeadline_ = Clock::now()+std::chrono::milliseconds(wait);
        } else if (wait == 0) {
            notifiedExpiryFailures_ = store_.failures;
            cb_->onLockoutCleared();
        }
    }
    bool checkLockout() {
        int64_t wait = updateLockoutTimer();
        if (wait < 0) { cb_->onLockoutPermanent(); return true; }
        if (wait > 0) { cb_->onLockoutTimed(wait); return true; }
        return false;
    }
    bool failedAttempt() {
        ++store_.failures;
        if (!store_.save()) { error(Error::UNABLE_TO_PROCESS); return true; }
        cb_->onAuthenticationFailed();
        return checkLockout();
    }
    ScopedAStatus command(std::function<void()> fn) {
        std::lock_guard lock(mutex_);
        if (closed_ || queue_.size() >= 8) return unavailable();
        queue_.push_back([this,fn=std::move(fn)] {
            if (!closed_) fn();
        });
        cv_.notify_all(); return ScopedAStatus::ok();
    }
    ScopedAStatus operation(int kind, const Hat& hat, Signal* out) {
        auto flag = std::make_shared<std::atomic_bool>(false);
        std::lock_guard lock(mutex_);
        if (closed_ || queue_.size() >= 8) return unavailable();
        cancel_ = flag;
        *out = ndk::SharedRefBase::make<Cancellation>(flag);
        queue_.push_back([this,kind,hat,flag] {
            waitForPredecessor();
            // A replacement session waits for cancellation and shutdown of the
            // previous owner; it must not reset or probe hardware concurrently.
            std::lock_guard hardwareLock(*hardwareMutex_);
            capture(kind,hat,*flag);
            // FindSensor cancels its initial enrollment before Enrolling starts.
            // Keep this session's initialized TA/GPIO across that handoff, as
            // the vendor backend does across operations on an open device.
            // close()/HW_UNAVAILABLE retire the owner and shut it down.
        });
        cv_.notify_all(); return ScopedAStatus::ok();
    }
    void capture(int kind, const Hat& hat, const std::atomic_bool& cancel) {
        if (cancel || closed_) { error(Error::CANCELED); return; }
#ifdef GAOKUN3_FP_TEST
        if (captureForTest_) { captureForTest_(transport_,cancel); return; }
#endif
        if (!load()) return;
        if (kind == 1) {
            auto c = challenges_.find(hat.challenge);
            if (c == challenges_.end() || !hat.userId || hat.mac.size() != 32 ||
                !(static_cast<int32_t>(hat.authenticatorType) & 1) ||
                Clock::now()-c->second > 10min) { error(Error::UNABLE_TO_PROCESS); return; }
            // Only structural enrollment authorization is possible at Class 1.
            if (store_.enrollments.size() >= gaokun3::kMaxFingers) { error(Error::NO_SPACE); return; }
        }
        if (kind == 2 && store_.enrollments.empty()) { error(Error::UNABLE_TO_PROCESS); return; }
        if (kind == 2 && checkLockout()) return;
        if (!transport_.start()) { error(Error::HW_UNAVAILABLE); return; }
        gaokun3::Enrollment pending{};
        pending.id = 1;
        for (auto& e : store_.enrollments) {
            if (e.id == INT32_MAX) { error(Error::NO_SPACE); return; }
            pending.id = std::max(pending.id,e.id+1);
        }
        size_t stages = 0;
        // Authentication may start with the finger already on the power key.
        // The MCU flag means frame availability, not an observed finger-up.
        // Process its valid frame through the unchanged decode/match checks.
        bool latched = kind != 2;
        bool attempted = false;
        unsigned usableProbes = 0;
        unsigned statusErrors = 0;
        // Android keyguard authentication stays armed until canceled. The
        // desktop driver's 30-second verify timeout does not apply here.
        auto deadline = kind == 1 ? Clock::now() + 90s : Clock::time_point::max();
        auto pressStart = Clock::time_point{};
        while (!cancel && !closed_ && Clock::now() < deadline) {
            // Detection may stay active while authentication is locked out.
            maybeClearTimedLockout();
            // SRAM consumption clears frame-ready even while the finger stays
            // on the sensor. Group usable probes into one matching window;
            // a gap between frames is not a failed physical contact.
            if (kind == 2 && attempted && Clock::now()-pressStart >= 3s) {
                // Operation counts only; never emit images, features or scores.
                LOG(INFO) << "Fingerprint authentication window rejected; usable frames "
                          << usableProbes;
                if (failedAttempt()) return;
                attempted = false; pressStart = Clock::time_point{};
                usableProbes = 0;
                // Ready reports another frame, not a physical finger-up.
                // Continue processing a newly available frame after rejection
                // even if the flag never went low between matching windows.
                latched = false;
            }
            uint8_t down = 0;
            if (!transport_.status(&down)) {
                // A rejected status frame is not a finger-up event and must
                // not abort the whole session on the first transient burst.
                // Retry only framing/checksum errors, without resetting the
                // sensor or accepting an invalid frame as image data.
                if ((errno == EPROTO || errno == EBADMSG) && ++statusErrors <= 5) {
                    LOG(WARNING) << "Fingerprint status frame rejected; retry " << statusErrors;
                    std::this_thread::sleep_for(300ms);
                    continue;
                }
                error(Error::HW_UNAVAILABLE); return;
            }
            statusErrors = 0;
            if (!down) {
                latched = false;
            }
            else if (!latched) {
                if (kind == 3) { cb_->onInteractionDetected(); return; }
                Gaokun3Frame frame{}; bool usable = false;
                if (!transport_.capture(&frame,&usable)) { error(Error::HW_UNAVAILABLE); return; }
                if (!usable) { cb_->onAcquired(AcquiredInfo::INSUFFICIENT,0); }
                else if (kind == 1) {
                    bool duplicate = false;
                    for (size_t i = 0; i < stages; ++i)
                        if (!memcmp(&frame,&pending.frames[i],sizeof(frame))) duplicate = true;
                    if (!duplicate) {
                        pending.frames[stages++] = frame;
                        latched = true;
                        if (stages == gaokun3::kStages) {
                            store_.enrollments.push_back(pending);
                            if (!save()) return;
                        }
                        cb_->onAcquired(AcquiredInfo::GOOD,0);
                        LOG(INFO) << "Fingerprint enrollment frame accepted; remaining "
                                  << gaokun3::kStages-stages;
                        cb_->onEnrollmentProgress(pending.id,gaokun3::kStages-stages);
                        if (stages == gaokun3::kStages) return;
                    }
                } else if (kind == 2) {
                    attempted = true;
                    ++usableProbes;
                    if (pressStart == Clock::time_point{}) pressStart = Clock::now();
                    // Report the best scoring finger, not the first one over
                    // the threshold.
                    int32_t bestId = -1;
                    float bestScore = 0;
                    for (auto& e : store_.enrollments) {
                        if (cancel || closed_) break;
                        const float s = gaokun3::score(frame,e.frames);
                        if (s > bestScore) { bestScore = s; bestId = e.id; }
                    }
                    if (bestScore >= gaokun3::kMatchThreshold && !cancel && !closed_) {
                        store_.failures = 0;
                        if (!store_.save()) { error(Error::UNABLE_TO_PROCESS); return; }
                        lockout_->reset(); clearLockoutTimer();
                        cb_->onAcquired(AcquiredInfo::GOOD,0);
                        // Empty token: no fabricated MAC, SID or crypto authorization.
                        cb_->onAuthenticationSucceeded(bestId,Hat{}); return;
                    }
                }
            }
            std::this_thread::sleep_for(300ms);
        }
        error((cancel || closed_) ? Error::CANCELED : Error::TIMEOUT);
    }
    void loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock lock(mutex_);
                while (!quit_ && queue_.empty()) {
                    if (lockoutDeadline_ == Clock::time_point::max()) {
                        cv_.wait(lock,[this] { return quit_ || !queue_.empty(); });
                    } else {
                        cv_.wait_until(lock,lockoutDeadline_,[this] {
                            return quit_ || !queue_.empty();
                        });
                        if (Clock::now() >= lockoutDeadline_) break;
                    }
                }
                if (queue_.empty() && quit_) return;
                if (!queue_.empty()) { fn = std::move(queue_.front()); queue_.pop_front(); }
            }
            maybeClearTimedLockout();
            if (fn) fn();
        }
    }
    AIBinder_DeathRecipient* death_ = nullptr;
    std::shared_ptr<ISessionCallback> cb_;
    std::string dir_;
    gaokun3::Store store_;
    gaokun3::Transport transport_;
    std::shared_ptr<gaokun3::Lockout> lockout_;
    std::shared_ptr<std::mutex> hardwareMutex_;
    std::shared_ptr<Session> predecessor_;
    Clock::time_point lockoutDeadline_ = Clock::time_point::max();
    uint32_t notifiedExpiryFailures_ = 0;
#ifdef GAOKUN3_FP_TEST
    std::function<void(gaokun3::Transport&, const std::atomic_bool&)> captureForTest_;
#endif
    std::atomic_bool closed_ = false, drained_ = false, finished_ = false, silent_ = false;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::shared_ptr<std::atomic_bool> cancel_;
    bool quit_ = false;
    std::map<int64_t,Clock::time_point> challenges_;
    std::thread worker_;
};

class Fingerprint final : public BnFingerprint {
  public:
    // nullptr: the board straps report no sensor, so none is offered.
    explicit Fingerprint(const gaokun3::SensorProfile* profile) : profile_(profile) {}
    ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override {
        if (!profile_) { out->clear(); return ScopedAStatus::ok(); }
        SensorProps p{};
        p.commonProps.sensorId = 0;
        p.commonProps.sensorStrength = common::SensorStrength::CONVENIENCE;
        p.commonProps.maxEnrollmentsPerUser = gaokun3::kMaxFingers;
        p.sensorType = FingerprintSensorType::POWER_BUTTON;
        p.supportsDetectInteraction = true;
        // Where the power key sits, so the framework can point at it (side
        // fingerprint indicator, enrollment animation). Pixels of the built-in
        // display in its natural portrait orientation (1600 x 2560, 244.8 ppi,
        // 0.1038 mm per pixel), origin top-left. Held in landscape (ROTATION_90)
        // the key is on the left edge, its centre about 27 mm below the top of
        // the 184 mm chassis, i.e. 18 mm below the top of the active area past
        // the 9 mm bezel; the left edge is the natural top edge and the top
        // edge the natural right one. The key is about 16 mm long. Both sensor
        // variants sit in the same key. From Huawei's outline drawing, +/- 2 mm.
        // SystemUI's side-fps math takes y == 0 as "on the natural top edge",
        // x as the key's start along that edge and the radius as half its
        // length: the key spans [x, x + 2r], so x = centre (1428) - r.
        SensorLocation location{};
        location.sensorLocationX = 1600 - 172 - 78;
        location.sensorLocationY = 0;
        location.sensorRadius = 78;
        // The framework and Settings take "" as the default display, but
        // SystemUI's side fingerprint indicator looks the location up by the
        // display's unique ID only (SideFpsSensorInteractor, no "" fallback)
        // and stays hidden without it. "local:0" is the built-in panel's ID
        // (drm_hwcomposer physical display 0).
        SensorLocation builtIn = location;
        builtIn.display = "local:0";
        p.sensorLocations = {location, builtIn};
        *out = {p}; return ScopedAStatus::ok();
    }
    ScopedAStatus createSession(int32_t sensor, int32_t user,
                               const std::shared_ptr<ISessionCallback>& cb,
                               std::shared_ptr<ISession>* out) override {
        if (!profile_ || sensor != 0 || user < 0 || !cb) return invalid();
        std::lock_guard lock(mutex_);
        retired_.erase(std::remove_if(retired_.begin(),retired_.end(),
                      [](const auto& s) { return s->finished(); }),retired_.end());
        if (retired_.size() >= 8) return unavailable();
        // A synchronous onSessionClosed callback may re-enter createSession.
        // Keep its old worker alive until that callback has returned.
        auto predecessor = session_;
        if (session_) {
            session_->retire();
            retired_.push_back(session_);
        }
        // Death of system_server ends this process; init restarts it and the
        // exclusive kernel FD release shuts down the TA before another open.
        auto& lockout = lockouts_[user];
        if (!lockout) lockout = std::make_shared<gaokun3::Lockout>();
        session_ = ndk::SharedRefBase::make<Session>(*profile_,user,cb,lockout,hardwareMutex_,
                                                     std::move(predecessor));
        if (!session_->linkToDeath()) { session_->close(); return unavailable(); }
        *out = session_; return ScopedAStatus::ok();
    }
  private:
    const gaokun3::SensorProfile* const profile_;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<std::shared_ptr<Session>> retired_;
    std::map<int32_t,std::shared_ptr<gaokun3::Lockout>> lockouts_;
    std::shared_ptr<std::mutex> hardwareMutex_ = std::make_shared<std::mutex>();
};
}

#ifndef GAOKUN3_FP_TEST
int main(int argc, char** argv) {
    (void)argc;
    ::android::base::InitLogging(argv,::android::base::LogdLogger());
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    // The straps pick the sensor once; one image serves both variants.
    auto hal = ndk::SharedRefBase::make<aidl::android::hardware::biometrics::fingerprint::Fingerprint>(
            gaokun3::Transport::detect());
    const std::string name = std::string(hal->descriptor)+"/default";
    CHECK(AServiceManager_addService(hal->asBinder().get(),name.c_str()) == STATUS_OK);
    ABinderProcess_joinThreadPool();
    return 1;
}
#endif
