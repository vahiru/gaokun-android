// SPDX-License-Identifier: Apache-2.0
#include "Core.h"
#include "Transport.h"
#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnFingerprint.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/security/sharedsecret/ISharedSecret.h>
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
#include <future>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace aidl::android::hardware::biometrics::fingerprint {
using ndk::ScopedAStatus;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Hat = keymaster::HardwareAuthToken;
using Signal = std::shared_ptr<common::ICancellationSignal>;
// 取得令牌密钥；失败时返回false，调用方以UNABLE_TO_PROCESS结束这次操作。
using AuthKeySource = std::function<bool(gaokun3::AuthKey*)>;
static ScopedAStatus invalid() { return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT); }
// Framework clients catch RemoteException for a lost HAL/session. An AIDL
// IllegalStateException escapes FingerprintStartUserClient and kills system_server.
static ScopedAStatus unavailable() { return ScopedAStatus::fromStatus(STATUS_DEAD_OBJECT); }

static gaokun3::AuthToken fromHat(const Hat& hat) {
    gaokun3::AuthToken t;
    t.challenge = hat.challenge;
    t.userId = hat.userId;
    t.authenticatorId = hat.authenticatorId;
    t.authenticatorType = static_cast<int32_t>(hat.authenticatorType);
    t.timestampMs = hat.timestamp.milliSeconds;
    t.mac = hat.mac;
    return t;
}
static Hat toHat(const gaokun3::AuthToken& t) {
    Hat hat{};
    hat.challenge = t.challenge;
    hat.userId = t.userId;
    hat.authenticatorId = t.authenticatorId;
    hat.authenticatorType = static_cast<keymaster::HardwareAuthenticatorType>(t.authenticatorType);
    hat.timestamp.milliSeconds = t.timestampMs;
    hat.mac = t.mac;
    return hat;
}

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
            std::shared_ptr<gaokun3::Challenges> challenges,
            std::shared_ptr<std::mutex> hardwareMutex,
            AuthKeySource keys,
            std::shared_ptr<Session> predecessor = nullptr)
        : cb_(std::move(cb)),
          dir_("/data/vendor_de/"+std::to_string(user)+"/fpdata"),
          store_(dir_+"/gaokun3-templates.db",profile), transport_(profile),
          lockout_(std::move(lockout)), user_(user), challenges_(std::move(challenges)),
          hardwareMutex_(std::move(hardwareMutex)), keys_(std::move(keys)),
          predecessor_(std::move(predecessor)), worker_([this] { loop(); }) {}
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
            const int64_t c = challenges_->generate(user_,monotonicMs());
            if (!c) { error(Error::UNABLE_TO_PROCESS); return; }
            cb_->onChallengeGenerated(c);
        });
    }
    ScopedAStatus revokeChallenge(int64_t c) override {
        return command([this,c] {
            challenges_->revoke(user_,c); cb_->onChallengeRevoked(c);
        });
    }
    ScopedAStatus enroll(const Hat& hat, Signal* out) override {
        return operation(1,hat,0,out);
    }
    // operationId是Keystore操作的句柄（加密认证），锁屏为0；成功时作为令牌的challenge签发。
    ScopedAStatus authenticate(int64_t operationId, Signal* out) override {
        return operation(2,{},operationId,out);
    }
    ScopedAStatus detectInteraction(Signal* out) override { return operation(3,{},0,out); }
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
            // 删除手指不更换authenticatorId，删光时save()把它置0。
            if (!removed.empty() && !save()) return;
            cb_->onEnrollmentsRemoved(removed);
        });
    }
    ScopedAStatus getAuthenticatorId() override {
        return command([this] { if (load()) cb_->onAuthenticatorIdRetrieved(store_.authenticatorId); });
    }
    ScopedAStatus invalidateAuthenticatorId() override {
        return command([this] {
            if (!load()) return;
            // 框架在另一种生物识别录入新数据时调用：绑定“全部生物识别”的密钥随之失效。
            // 没有手指时保持0。
            if (!store_.enrollments.empty()) store_.authenticatorId = gaokun3::randomId();
            if (save()) cb_->onAuthenticatorIdInvalidated(store_.authenticatorId);
        });
    }
    ScopedAStatus resetLockout(const Hat& hat) override {
        return command([this,hat] {
            // 框架在用户确认PIN后传入Gatekeeper签发的令牌，须是几分钟内签发的
            // （ISession#resetLockout）。
            // 时间戳来自调用方，先比较再做减法，避免有符号溢出。
            const int64_t now = gaokun3::bootTimeMs(), issued = hat.timestamp.milliSeconds;
            if (issued > now || issued < now - kCredentialTokenMaxAgeMs) {
                LOG(ERROR) << "Fingerprint lockout reset rejected: credential token not issued in the last "
                           << kCredentialTokenMaxAgeMs / 60000 << " minutes";
                error(Error::UNABLE_TO_PROCESS); return;
            }
            if (!verifiedCredential(hat)) { error(Error::UNABLE_TO_PROCESS); return; }
            if (!load()) return;
            store_.failures = 0;
            store_.secureUserId = hat.userId;
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
    // authenticatorId由调用方按需更换（录入、invalidateAuthenticatorId），这里只在没有手指时置0。
    bool save() {
        if (store_.enrollments.empty()) {
            store_.authenticatorId = 0;
            store_.failures = 0; lockout_->reset(); clearLockoutTimer();
        }
        if ((!store_.enrollments.empty() && store_.authenticatorId <= 0) || !store_.save()) {
            error(Error::UNABLE_TO_PROCESS); return false;
        }
        return true;
    }
    // 令牌密钥；取不到时记录日志，由调用方结束操作。
    bool authKey(gaokun3::AuthKey* key) {
        if (keys_ && keys_(key)) return true;
        LOG(ERROR) << "Fingerprint auth token key unavailable";
        return false;
    }
    // Gatekeeper签发的PIN令牌：类型含密码，SID非零，MAC与令牌密钥相符。
    bool verifiedCredential(const Hat& hat) {
        if (!hat.userId || !(static_cast<int32_t>(hat.authenticatorType) & gaokun3::kAuthPassword)) {
            LOG(ERROR) << "Fingerprint credential token rejected: not a password token";
            return false;
        }
        gaokun3::AuthKey key;
        if (!authKey(&key)) return false;
        if (!gaokun3::verifyAuthToken(key,fromHat(hat))) {
            LOG(ERROR) << "Fingerprint credential token rejected: MAC mismatch";
            return false;
        }
        return true;
    }
    static constexpr int64_t kCredentialTokenMaxAgeMs = 10 * 60 * 1000;
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
            // A retired session may still be saving after an authentication;
            // reading or changing the database before it ends could undo that
            // save or race its temporary file.
            waitForPredecessor();
            if (!closed_) fn();
        });
        cv_.notify_all(); return ScopedAStatus::ok();
    }
    ScopedAStatus operation(int kind, const Hat& hat, int64_t operationId, Signal* out) {
        auto flag = std::make_shared<std::atomic_bool>(false);
        std::lock_guard lock(mutex_);
        if (closed_ || queue_.size() >= 8) return unavailable();
        cancel_ = flag;
        *out = ndk::SharedRefBase::make<Cancellation>(flag);
        queue_.push_back([this,kind,hat,operationId,flag] {
            waitForPredecessor();
            // A replacement session waits for cancellation and shutdown of the
            // previous owner; it must not reset or probe hardware concurrently.
            std::lock_guard hardwareLock(*hardwareMutex_);
            capture(kind,hat,operationId,*flag);
            // FindSensor cancels its initial enrollment before Enrolling starts.
            // Keep this session's initialized TA/GPIO across that handoff, as
            // the vendor backend does across operations on an open device.
            // close()/HW_UNAVAILABLE retire the owner and shut it down.
        });
        cv_.notify_all(); return ScopedAStatus::ok();
    }
    void capture(int kind, const Hat& hat, int64_t operationId, const std::atomic_bool& cancel) {
        if (cancel || closed_) { error(Error::CANCELED); return; }
#ifdef GAOKUN3_FP_TEST
        if (captureForTest_) { captureForTest_(transport_,cancel); return; }
#endif
        if (!load()) return;
        if (kind == 1) {
            // 录入须有本HAL生成、仍在有效期内的challenge，以及Gatekeeper为它签发的PIN令牌。
            if (!challenges_->valid(user_,hat.challenge,monotonicMs())) {
                LOG(ERROR) << "Fingerprint enrollment rejected: unknown or expired challenge";
                error(Error::UNABLE_TO_PROCESS); return;
            }
            if (!verifiedCredential(hat)) { error(Error::UNABLE_TO_PROCESS); return; }
            if (store_.enrollments.size() >= gaokun3::kMaxFingers) { error(Error::NO_SPACE); return; }
        }
        if (kind == 2 && store_.enrollments.empty()) { error(Error::UNABLE_TO_PROCESS); return; }
        if (kind == 2 && checkLockout()) return;
        // 认证成功时签发令牌：先取密钥，取不到就不开传感器。
        gaokun3::AuthKey key{};
        if (kind == 2 && !authKey(&key)) { error(Error::UNABLE_TO_PROCESS); return; }
        if (!transport_.start()) { error(Error::HW_UNAVAILABLE); return; }
        // Feature caches are built while the finger is still on its way.
        if (kind == 2) gaokun3::prepare(store_.enrollments);
        gaokun3::Enrollment pending{};
        pending.id = 1;
        for (auto& e : store_.enrollments) {
            if (e.id == INT32_MAX) { error(Error::NO_SPACE); return; }
            pending.id = std::max(pending.id,e.id+1);
        }
        // Authentication may start with the finger already on the power key.
        // The MCU flag means frame availability, not an observed finger-up.
        // Process its valid frame through the unchanged decode/match checks.
        bool latched = kind != 2;
        bool attempted = false;
        unsigned usableProbes = 0;
        unsigned statusErrors = 0;
        unsigned discardErrors = 0;
        // Every operation stays armed until canceled. Settings cancels
        // enrollment when the user leaves the screen; a HAL deadline only
        // discarded slow but progressing enrollments (frames lost to status
        // retries and quality rejects), and keyguard authentication must not
        // time out. The desktop driver's 30-second verify timeout does not
        // apply here either.
        auto pressStart = Clock::time_point{};
        while (!cancel && !closed_) {
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
                discardErrors = 0;
            }
            else if (latched) {
                // 录入和检测正在等待手指抬起。FocalTech有帧待读时状态位不会自行复位，
                // 不读出这一帧，状态位将始终为1，此后的按压均无响应。这一帧本就丢弃，
                // 读取时遇到坏帧不必中止操作，下次轮询时重试。
                if (transport_.discardPending()) discardErrors = 0;
                else if ((errno == EPROTO || errno == EBADMSG) && ++discardErrors <= 5)
                    LOG(WARNING) << "Fingerprint pending frame rejected; retry " << discardErrors;
                else { error(Error::HW_UNAVAILABLE); return; }
            }
            else {
                if (kind == 3) { cb_->onInteractionDetected(); return; }
                Gaokun3Frame frame{}; bool usable = false;
                if (!transport_.capture(&frame,&usable)) { error(Error::HW_UNAVAILABLE); return; }
                if (!usable) { cb_->onAcquired(AcquiredInfo::INSUFFICIENT,0); }
                else if (kind == 1) {
                    // One prompt per touch: a rejected frame waits for the lift.
                    const auto check = gaokun3::checkEnrollFrame(frame,pending.frames);
                    if (check == gaokun3::EnrollFrame::kPartial) {
                        cb_->onAcquired(AcquiredInfo::PARTIAL,0);
                        latched = true;
                    } else if (check == gaokun3::EnrollFrame::kRepeat) {
                        cb_->onAcquired(AcquiredInfo::IMMOBILE,0);
                        latched = true;
                    } else {
                        pending.frames.push_back(frame);
                        const size_t stages = pending.frames.size();
                        latched = true;
                        if (stages == gaokun3::kStages) {
                            store_.enrollments.push_back(pending);
                            // 录入新手指须更换authenticatorId，绑定指纹且随录入失效的密钥由此失效。
                            store_.authenticatorId = gaokun3::randomId();
                            store_.secureUserId = hat.userId;
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
                    // The finger that unlocked last goes first, its frames on
                    // every core, and the first finger over the threshold is
                    // the one: other fingers score under 20 against it in all
                    // collected data, far from kMatchThreshold.
                    const gaokun3::FrameFeatures probe(frame);
                    const unsigned threads = std::max(1u,std::min(8u,std::thread::hardware_concurrency()));
                    std::vector<size_t> order(store_.enrollments.size());
                    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
                    std::stable_sort(order.begin(),order.end(),[this](size_t a, size_t b) {
                        auto rank = [this](size_t i) {
                            auto it = std::find(recent_.begin(),recent_.end(),store_.enrollments[i].id);
                            return it == recent_.end() ? recent_.size() : size_t(it - recent_.begin());
                        };
                        return rank(a) < rank(b);
                    });
                    int32_t bestId = -1;
                    float bestScore = 0, bestFrame = 0;
                    for (size_t i : order) {
                        if (cancel || closed_) break;
                        float frameScore = 0;
                        const float s = gaokun3::score(probe,store_.enrollments[i],&frameScore,threads);
                        if (s > bestScore) { bestScore = s; bestId = store_.enrollments[i].id; bestFrame = frameScore; }
                        if (bestScore >= gaokun3::kMatchThreshold) break;
                    }
                    if (bestScore >= gaokun3::kMatchThreshold && !cancel && !closed_) {
                        // 令牌的userId是用户的SID：绑定“指纹或PIN”、不随录入失效的密钥只认它；
                        // authenticatorId对应随录入失效的密钥。SID尚未得知（升级后用户还没有
                        // 输入过PIN）时为0，前一类密钥要等用户输入一次PIN后才可用。
                        gaokun3::AuthToken token;
                        token.challenge = operationId;
                        token.userId = store_.secureUserId;
                        token.authenticatorId = store_.authenticatorId;
                        token.authenticatorType = gaokun3::kAuthFingerprint;
                        token.timestampMs = gaokun3::bootTimeMs();
                        if (!gaokun3::signAuthToken(key,&token)) {
                            LOG(ERROR) << "Fingerprint auth token signing failed";
                            error(Error::UNABLE_TO_PROCESS); return;
                        }
                        lockout_->reset(); clearLockoutTimer();
                        cb_->onAcquired(AcquiredInfo::GOOD,0);
                        cb_->onAuthenticationSucceeded(bestId,toHat(token));
                        recent_.erase(std::remove(recent_.begin(),recent_.end(),bestId),recent_.end());
                        recent_.insert(recent_.begin(),bestId);
                        // The unlock does not wait for learning or the disk. If
                        // this save fails, the failure count stays as stored:
                        // a lockout may come early, never late. Without a
                        // learned frame or failures to clear nothing changed.
                        bool changed = store_.failures != 0;
                        for (auto& e : store_.enrollments)
                            if (e.id == bestId && gaokun3::shouldLearn(probe,e,bestScore,bestFrame,threads)) {
                                gaokun3::learn(e,frame);
                                changed = true;
                            }
                        store_.failures = 0;
                        if (changed && !store_.save()) LOG(ERROR) << "Fingerprint database not saved after authentication";
                        return;
                    }
                }
            }
            std::this_thread::sleep_for(300ms);
        }
        error(Error::CANCELED);
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
    const int32_t user_;
    std::shared_ptr<gaokun3::Challenges> challenges_;
    std::shared_ptr<std::mutex> hardwareMutex_;
    AuthKeySource keys_;
    std::shared_ptr<Session> predecessor_;
    Clock::time_point lockoutDeadline_ = Clock::time_point::max();
    // Finger ids, the one that authenticated last first.
    std::vector<int32_t> recent_;
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
    std::thread worker_;
};

// 令牌密钥的来源：向本机全部ISharedSecret参与方（VINTF中声明的实例，与keystore2协商时的
// 名单相同）读取协商参数，推导出与协商结果相同的密钥。各方的参数在一次开机内不变，推导成功
// 后缓存到进程退出；失败（如参与方尚未启动）时下次操作重试。
class SharedSecretKeySource {
  public:
    bool get(gaokun3::AuthKey* key) {
        std::lock_guard lock(mutex_);
        if (!key_) key_ = derive();
        if (!key_) return false;
        *key = *key_;
        return true;
    }
  private:
    static std::optional<gaokun3::AuthKey> derive() {
        using security::sharedsecret::ISharedSecret;
        using security::sharedsecret::SharedSecretParameters;
        std::vector<std::string> instances;
        AServiceManager_forEachDeclaredInstance(ISharedSecret::descriptor,&instances,
            [](const char* instance, void* context) {
                static_cast<std::vector<std::string>*>(context)->push_back(instance);
            });
        if (instances.empty()) {
            LOG(ERROR) << "No ISharedSecret instance declared; auth token key unavailable";
            return std::nullopt;
        }
        std::vector<gaokun3::SharedSecretParams> params;
        for (const auto& instance : instances) {
            const std::string name = std::string(ISharedSecret::descriptor)+"/"+instance;
            auto participant = ISharedSecret::fromBinder(ndk::SpAIBinder(AServiceManager_checkService(name.c_str())));
            SharedSecretParameters p;
            if (!participant || !participant->getSharedSecretParameters(&p).isOk()) {
                LOG(ERROR) << "Shared secret parameters of " << name << " unavailable";
                return std::nullopt;
            }
            params.push_back({p.seed,p.nonce});
        }
        gaokun3::AuthKey key;
        if (!gaokun3::deriveAuthKey(std::move(params),&key)) {
            LOG(ERROR) << "Auth token key derivation failed";
            return std::nullopt;
        }
        LOG(INFO) << "Auth token key derived from " << instances.size() << " shared secret participants";
        return key;
    }
    std::mutex mutex_;
    std::optional<gaokun3::AuthKey> key_;
};

class Fingerprint final : public BnFingerprint {
  public:
    // nullptr: the board straps report no sensor, so none is offered.
    Fingerprint(const gaokun3::SensorProfile* profile, AuthKeySource keys)
        : profile_(profile), keys_(std::move(keys)) {}
    ScopedAStatus getSensorProps(std::vector<SensorProps>* out) override {
        if (!profile_) { out->clear(); return ScopedAStatus::ok(); }
        SensorProps p{};
        p.commonProps.sensorId = 0;
        // Class 3：签发的令牌用于Keystore，取舍见README“能力边界”。
        p.commonProps.sensorStrength = common::SensorStrength::STRONG;
        p.commonProps.maxEnrollmentsPerUser = gaokun3::kMaxFingers;
        p.sensorType = FingerprintSensorType::POWER_BUTTON;
        p.supportsDetectInteraction = true;
        // 电源键的位置，供框架指向它（侧边指纹提示、录入动画）。坐标是内屏在自然方向下的
        // 像素：自然方向为横屏（设备树中面板的rotation，见device.mk），2560x1600，244.8 ppi，
        // 每像素0.1038 mm，原点在左上角。接键盘横放时电源键在左边缘，中心距机身顶部约27 mm
        // （机身高184 mm），即距显示区域顶部约18 mm（边框9 mm），约172像素；按键长约16 mm。
        // 两种传感器都装在同一个按键里。数据来自华为的外形图，误差约2 mm。
        // SystemUI把y != 0的传感器当作竖直放置：y是按键沿边缘的起点，半径是长度的一半，按键占
        // [y, y + 2r]，所以y = 中心172 - r；x不使用，填0。位于左边缘由SystemUI的
        // config_sideFpsSensorOnLeftEdge声明（patches/android/frameworks-base/0004，
        // rro/Gaokun3SystemUIFingerprintOverlay），否则SystemUI按右边缘放置。
        SensorLocation location{};
        location.sensorLocationX = 0;
        location.sensorLocationY = 172 - 78;
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
        session_ = ndk::SharedRefBase::make<Session>(*profile_,user,cb,lockout,challenges_,hardwareMutex_,
                                                     keys_,std::move(predecessor));
        if (!session_->linkToDeath()) { session_->close(); return unavailable(); }
        *out = session_; return ScopedAStatus::ok();
    }
  private:
    const gaokun3::SensorProfile* const profile_;
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
    std::vector<std::shared_ptr<Session>> retired_;
    std::map<int32_t,std::shared_ptr<gaokun3::Lockout>> lockouts_;
    std::shared_ptr<gaokun3::Challenges> challenges_ = std::make_shared<gaokun3::Challenges>();
    std::shared_ptr<std::mutex> hardwareMutex_ = std::make_shared<std::mutex>();
    const AuthKeySource keys_;
};
}

#ifndef GAOKUN3_FP_TEST
int main(int argc, char** argv) {
    (void)argc;
    ::android::base::InitLogging(argv,::android::base::LogdLogger());
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    using namespace aidl::android::hardware::biometrics::fingerprint;
    auto keys = std::make_shared<SharedSecretKeySource>();
    // The straps pick the sensor once; one image serves both variants.
    auto hal = ndk::SharedRefBase::make<Fingerprint>(
            gaokun3::Transport::detect(),[keys](gaokun3::AuthKey* key) { return keys->get(key); });
    const std::string name = std::string(hal->descriptor)+"/default";
    CHECK(AServiceManager_addService(hal->asBinder().get(),name.c_str()) == STATUS_OK);
    ABinderProcess_joinThreadPool();
    return 1;
}
#endif
