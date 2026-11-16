// SPDX-License-Identifier: Apache-2.0
#define GAOKUN3_FP_TEST
#include "Service.cpp"
#include <aidl/android/hardware/biometrics/fingerprint/BnSessionCallback.h>
#include <gtest/gtest.h>
#include "test_session_io.h"
#include <random>
#include <openssl/evp.h>

namespace aidl::android::hardware::biometrics::fingerprint {
class Callback final : public BnSessionCallback {
 public:
    std::atomic_int challenges{0}, errors{0}, closes{0}, interactions{0}, lastError{0}, successes{0}, rejections{0}, timedLockouts{0}, acquired{0};
    std::atomic<int64_t> lastChallenge{0};
    std::function<void()> errorHook, closeHook, rejectionHook;
    ScopedAStatus onChallengeGenerated(int64_t c) override {
        lastChallenge=c; ++challenges; return ScopedAStatus::ok();
    }
    ScopedAStatus onError(Error e, int32_t) override {
        lastError=static_cast<int>(e);
        if (errorHook) errorHook();
        ++errors; return ScopedAStatus::ok();
    }
    ScopedAStatus onSessionClosed() override {
        if (closeHook) closeHook();
        ++closes; return ScopedAStatus::ok();
    }
#define NOOP(name, args) ScopedAStatus name args override { return ScopedAStatus::ok(); }
    NOOP(onChallengeRevoked, (int64_t))
    ScopedAStatus onAcquired(AcquiredInfo, int32_t) override { ++acquired; return ScopedAStatus::ok(); }
    NOOP(onEnrollmentProgress, (int32_t, int32_t))
    ScopedAStatus onAuthenticationSucceeded(int32_t, const Hat&) override { ++successes; return ScopedAStatus::ok(); }
    ScopedAStatus onAuthenticationFailed() override {
        ++rejections;
        if (rejectionHook) rejectionHook();
        return ScopedAStatus::ok();
    }
    ScopedAStatus onLockoutTimed(int64_t) override { ++timedLockouts; return ScopedAStatus::ok(); }
    NOOP(onLockoutPermanent, ())
    NOOP(onLockoutCleared, ())
    ScopedAStatus onInteractionDetected() override { ++interactions; return ScopedAStatus::ok(); }
    NOOP(onEnrollmentsEnumerated, (const std::vector<int32_t>&))
    NOOP(onEnrollmentsRemoved, (const std::vector<int32_t>&))
    NOOP(onAuthenticatorIdRetrieved, (int64_t))
    NOOP(onAuthenticatorIdInvalidated, (int64_t))
#undef NOOP
};
static bool waitFor(const std::atomic_int& value, int ticks=500) {
    for (int i=0; i<ticks && !value.load(); ++i) std::this_thread::sleep_for(10ms);
    return value.load() != 0;
}
TEST(SessionLifecycle, ReplacesUnclosedSession) {
    auto hal = ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto oldCb = ndk::SharedRefBase::make<Callback>();
    auto newCb = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> old, current;
    ASSERT_TRUE(hal->createSession(0,0,oldCb,&old).isOk());
    ASSERT_TRUE(hal->createSession(0,0,newCb,&current).isOk());
    EXPECT_EQ(old->generateChallenge().getStatus(), STATUS_DEAD_OBJECT);
    ASSERT_TRUE(current->generateChallenge().isOk());
    EXPECT_TRUE(waitFor(newCb->challenges));
    EXPECT_EQ(oldCb->closes.load(),0);
    current->close();
    EXPECT_TRUE(waitFor(newCb->closes));
}
TEST(SessionLifecycle, HardwareErrorAllowsReentrantRetry) {
    auto hal = ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto oldCb = ndk::SharedRefBase::make<Callback>();
    auto newCb = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> old, current;
    ASSERT_TRUE(hal->createSession(0,0,oldCb,&old).isOk());
    oldCb->errorHook = [&] {
        EXPECT_TRUE(hal->createSession(0,0,newCb,&current).isOk());
        if (current) EXPECT_TRUE(current->generateChallenge().isOk());
    };
    std::static_pointer_cast<Session>(old)->reportHardwareUnavailableForTest();
    ASSERT_TRUE(waitFor(oldCb->errors));
    EXPECT_EQ(oldCb->lastError.load(),static_cast<int>(Error::HW_UNAVAILABLE));
    EXPECT_TRUE(waitFor(newCb->challenges));
    EXPECT_EQ(old->generateChallenge().getStatus(), STATUS_DEAD_OBJECT);
    EXPECT_EQ(oldCb->closes.load(),0);
    current->close();
    EXPECT_TRUE(waitFor(newCb->closes));
}
TEST(SessionLifecycle, CloseCallbackCanReplaceSession) {
    auto hal = ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto oldCb = ndk::SharedRefBase::make<Callback>();
    auto newCb = ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> old, current;
    ASSERT_TRUE(hal->createSession(0,0,oldCb,&old).isOk());
    oldCb->closeHook = [&] {
        EXPECT_TRUE(hal->createSession(0,0,newCb,&current).isOk());
        if (current) EXPECT_TRUE(current->generateChallenge().isOk());
    };
    old->close();
    ASSERT_TRUE(waitFor(oldCb->closes));
    EXPECT_TRUE(waitFor(newCb->challenges));
    current->close();
    EXPECT_TRUE(waitFor(newCb->closes));
}
}

namespace aidl::android::hardware::biometrics::fingerprint {
TEST(SessionLifecycle, CancellationRetainsInitializedTransportForNextEnrollment) {
    resetMockTransport();
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;
    ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::atomic_int calls{0}, done{0};
    std::static_pointer_cast<Session>(session)->setCaptureForTest([&](auto& transport,const auto& cancel) {
        EXPECT_TRUE(transport.start());
        const int call=++calls;
        if (call==1) { while (!cancel) std::this_thread::sleep_for(5ms); }
        else { EXPECT_EQ(mockLoads.load(),1); EXPECT_EQ(mockUnloads.load(),0); ++done; }
    });
    Signal initial, next;
    ASSERT_TRUE(session->enroll(Hat{},&initial).isOk());
    ASSERT_TRUE(waitFor(calls));
    initial->cancel();
    ASSERT_TRUE(session->enroll(Hat{},&next).isOk());
    ASSERT_TRUE(waitFor(done));
    EXPECT_EQ(mockLoads.load(),1);
    EXPECT_EQ(mockUnloads.load(),0);
    session->close();
    ASSERT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockUnloads.load(),1);
}
TEST(SessionLifecycle, ReplacementWaitsForPreviousTransportShutdown) {
    resetMockTransport();
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto oldCb=ndk::SharedRefBase::make<Callback>();
    auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> old, session;
    ASSERT_TRUE(hal->createSession(0,0,oldCb,&old).isOk());
    std::atomic_int entered{0}, done{0};
    std::static_pointer_cast<Session>(old)->setCaptureForTest([&](auto& transport,const auto& cancel) {
        EXPECT_TRUE(transport.start()); ++entered;
        while (!cancel) std::this_thread::sleep_for(5ms);
        std::this_thread::sleep_for(50ms);
    });
    Signal initial, next;
    ASSERT_TRUE(old->detectInteraction(&initial).isOk());
    ASSERT_TRUE(waitFor(entered));
    ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setCaptureForTest([&](auto& transport,const auto&) {
        EXPECT_EQ(mockUnloads.load(),1);
        EXPECT_TRUE(transport.start()); ++done;
    });
    ASSERT_TRUE(session->detectInteraction(&next).isOk());
    ASSERT_TRUE(waitFor(done));
    EXPECT_EQ(mockLoads.load(),2);
    session->close(); ASSERT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockUnloads.load(),2);
    EXPECT_EQ(oldCb->closes.load(),0);
}
}

namespace aidl::android::hardware::biometrics::fingerprint {
class TemporaryStore {
 public:
    TemporaryStore() { CHECK(mkdtemp(path_)); }
    ~TemporaryStore() { unlink((data()+"/gaokun3-templates.db").c_str()); rmdir(data().c_str()); rmdir(path_); }
    std::string data() const { return std::string(path_)+"/fpdata"; }
 private:
    char path_[64]="/data/local/tmp/gaokun3-fp-test.XXXXXX";
};
TEST(StatusPolling, TransientBadFramesDoNotTerminateDetection) {
    resetMockTransport();
    mockStatusSamples={0,-EPROTO,-EBADMSG,1};
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;
    ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;
    ASSERT_TRUE(session->detectInteraction(&signal).isOk());
    EXPECT_TRUE(waitFor(cb->interactions));
    EXPECT_EQ(cb->errors.load(),0);
    EXPECT_EQ(mockLoads.load(),1); // Retry does not reset/reload hardware.
    session->close();
    EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockUnloads.load(),1);
}
TEST(StatusPolling, PersistentBadFramesRetireSession) {
    resetMockTransport();
    mockStatusSamples=std::vector<int>(6,-EPROTO);
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;
    ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;
    ASSERT_TRUE(session->detectInteraction(&signal).isOk());
    EXPECT_TRUE(waitFor(cb->errors));
    std::static_pointer_cast<Session>(session)->waitDrained();
    EXPECT_EQ(cb->lastError.load(),static_cast<int>(Error::HW_UNAVAILABLE));
    EXPECT_EQ(mockStatusReads.load(),6);
    EXPECT_EQ(cb->interactions.load(),0);
    EXPECT_EQ(mockUnloads.load(),1);
    EXPECT_EQ(session->generateChallenge().getStatus(),STATUS_DEAD_OBJECT);
}
TEST(StatusPolling, TransportIoFailureIsNotRetried) {
    resetMockTransport();
    mockStatusSamples={-EIO,1};
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;
    ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;
    ASSERT_TRUE(session->detectInteraction(&signal).isOk());
    EXPECT_TRUE(waitFor(cb->errors));
    std::static_pointer_cast<Session>(session)->waitDrained();
    EXPECT_EQ(mockStatusReads.load(),1);
    EXPECT_EQ(cb->lastError.load(),static_cast<int>(Error::HW_UNAVAILABLE));
    EXPECT_EQ(cb->interactions.load(),0);
}
TEST(SessionLifecycle, ReplacementAcceptsChallengeOfRetiredSession) {
    // 传感器出错时HAL报HW_UNAVAILABLE并退役会话；框架新建会话后，设置拿原来的令牌重试录入。
    resetMockTransport();
    mockStatusSamples={-EIO,-EIO};
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);
    auto oldCb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> old;
    ASSERT_TRUE(hal->createSession(0,0,oldCb,&old).isOk());
    std::static_pointer_cast<Session>(old)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(old->generateChallenge().isOk());
    ASSERT_TRUE(waitFor(oldCb->challenges));
    Hat hat{};
    hat.challenge=oldCb->lastChallenge;
    hat.userId=1;
    hat.authenticatorType=keymaster::HardwareAuthenticatorType::PASSWORD;
    hat.mac.assign(32,0);
    Signal signal;
    ASSERT_TRUE(old->enroll(hat,&signal).isOk());
    ASSERT_TRUE(waitFor(oldCb->errors));
    std::static_pointer_cast<Session>(old)->waitDrained();
    EXPECT_EQ(oldCb->lastError.load(),static_cast<int>(Error::HW_UNAVAILABLE));
    // 新会话能识别旧会话生成的challenge，于是初始化传感器并读到同样的错误，
    // 而不是在开始时就以UNABLE_TO_PROCESS结束。
    auto newCb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> current;
    ASSERT_TRUE(hal->createSession(0,0,newCb,&current).isOk());
    std::static_pointer_cast<Session>(current)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(current->enroll(hat,&signal).isOk());
    ASSERT_TRUE(waitFor(newCb->errors));
    // 以UNABLE_TO_PROCESS结束的会话不会退役，先查错误码再等它关闭。
    ASSERT_EQ(newCb->lastError.load(),static_cast<int>(Error::HW_UNAVAILABLE));
    std::static_pointer_cast<Session>(current)->waitDrained();
    EXPECT_EQ(mockStatusReads.load(),2);
    // 另一个用户的会话不接受这个challenge，也不去碰传感器。
    auto otherCb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> other;
    ASSERT_TRUE(hal->createSession(0,10,otherCb,&other).isOk());
    std::static_pointer_cast<Session>(other)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(other->enroll(hat,&signal).isOk());
    ASSERT_TRUE(waitFor(otherCb->errors));
    EXPECT_EQ(otherCb->lastError.load(),static_cast<int>(Error::UNABLE_TO_PROCESS));
    EXPECT_EQ(mockLoads.load(),2);
    other->close();
    EXPECT_TRUE(waitFor(otherCb->closes));
}
}

namespace aidl::android::hardware::biometrics::fingerprint {
// Generated texture only; exercise AES decode and the real matcher without
// accessing a real fingerprint or bypassing the authentication decision.
static Gaokun3Frame encryptedSyntheticFrame(unsigned seed) {
    std::mt19937 random(seed);
    std::array<uint8_t,gaokun3::kFocalTech.captureBytes> plain{};
    for (size_t i=0;i<size_t(gaokun3::kFocalTech.width)*gaokun3::kFocalTech.height;++i) {
        unsigned value=200+(random()&255)*8;
        plain[2*i]=value&255;plain[2*i+1]=value>>8;
    }
    static const uint8_t key[]={0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
    static const uint8_t iv[]={0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
    auto* ctx=EVP_CIPHER_CTX_new();CHECK(ctx);int n=0,tail=0;
    std::array<uint8_t,gaokun3::kFocalTech.captureBytes+EVP_MAX_BLOCK_LENGTH> encrypted{};
    CHECK_EQ(EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key,iv),1);
    CHECK_EQ(EVP_CIPHER_CTX_set_padding(ctx,0),1);
    CHECK_EQ(EVP_EncryptUpdate(ctx,encrypted.data(),&n,plain.data(),plain.size()),1);
    CHECK_EQ(EVP_EncryptFinal_ex(ctx,encrypted.data()+n,&tail),1);
    EVP_CIPHER_CTX_free(ctx);CHECK_EQ(static_cast<size_t>(n+tail),gaokun3::kFocalTech.captureBytes);
    memcpy(mockCipher.data(),encrypted.data(),mockCipher.size());
    Gaokun3Frame decoded{};CHECK(gaokun3::decode(mockCipher.data(),&decoded));return decoded;
}
static void storeSyntheticEnrollment(const TemporaryStore& dir,const Gaokun3Frame& frame,uint32_t failures=0) {
    CHECK_EQ(mkdir(dir.data().c_str(),0700),0);
    gaokun3::Store store(dir.data()+"/gaokun3-templates.db",gaokun3::kFocalTech);
    gaokun3::Enrollment entry{};entry.id=7;entry.frames.assign(gaokun3::kStages,frame);
    store.enrollments.push_back(entry);store.failures=failures;store.authenticatorId=gaokun3::randomId();CHECK(store.save());
}
TEST(Authentication, AlreadyPresentMatchingContactIsProcessed) {
    resetMockTransport();mockStatusSamples={1};
    TemporaryStore dir;auto frame=encryptedSyntheticFrame(1);storeSyntheticEnrollment(dir,frame);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->authenticate(0,&signal).isOk());
    EXPECT_TRUE(waitFor(cb->successes));
    session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),1);EXPECT_EQ(cb->successes.load(),1);EXPECT_EQ(cb->rejections.load(),0);
}
TEST(Authentication, AlreadyPresentUnrelatedContactIsRejected) {
    resetMockTransport();mockStatusSamples={1,0};
    TemporaryStore dir;auto frame=encryptedSyntheticFrame(1);storeSyntheticEnrollment(dir,frame);
    encryptedSyntheticFrame(400);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->authenticate(0,&signal).isOk());
    EXPECT_TRUE(waitFor(cb->rejections));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),1);EXPECT_EQ(cb->successes.load(),0);EXPECT_EQ(cb->rejections.load(),1);
}
}

namespace aidl::android::hardware::biometrics::fingerprint {
TEST(Authentication, FrameGapDoesNotCountAnAttemptOrLockout) {
    resetMockTransport();mockStatusSamples={1,0,0,1};
    TemporaryStore dir;
    auto frame=encryptedSyntheticFrame(1);auto matching=mockCipher;
    storeSyntheticEnrollment(dir,frame,4);
    encryptedSyntheticFrame(400);mockCipherQueue={mockCipher,matching};
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->authenticate(0,&signal).isOk());
    EXPECT_TRUE(waitFor(cb->successes));
    session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),2);EXPECT_EQ(cb->successes.load(),1);
    EXPECT_EQ(cb->rejections.load(),0);EXPECT_EQ(cb->timedLockouts.load(),0);
    gaokun3::Store restored(dir.data()+"/gaokun3-templates.db",gaokun3::kFocalTech);
    ASSERT_TRUE(restored.load());EXPECT_EQ(restored.failures,0u);
}
TEST(Authentication, RejectedWindowDoesNotRequireReadyFlagToClear) {
    resetMockTransport();mockStatusSamples=std::vector<int>(80,1);
    TemporaryStore dir;
    auto frame=encryptedSyntheticFrame(1);auto matching=mockCipher;
    storeSyntheticEnrollment(dir,frame);
    encryptedSyntheticFrame(400);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    // Runs on the capture worker between reads: after the first unsuccessful
    // matching window, present the matching image while ready remains set.
    cb->rejectionHook=[matching] { mockCipher=matching; };
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->authenticate(0,&signal).isOk());
    EXPECT_TRUE(waitFor(cb->successes,1000));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(cb->successes.load(),1);EXPECT_EQ(cb->rejections.load(),1);
    EXPECT_EQ(cb->timedLockouts.load(),0);
    gaokun3::Store restored(dir.data()+"/gaokun3-templates.db",gaokun3::kFocalTech);
    ASSERT_TRUE(restored.load());EXPECT_EQ(restored.failures,0u);
}
}

namespace aidl::android::hardware::biometrics::fingerprint {
// FocalTech的状态位表示有一帧待读，读出后才复位。录入和检测在等待手指抬起时若状态位
// 仍为1，HAL须读出并丢弃这一帧，不能一直等待它变为0，否则此后的按压均无响应。
static bool waitAtLeast(const std::atomic_int& value, int count, int ticks=500) {
    for (int i=0; i<ticks && value.load()<count; ++i) std::this_thread::sleep_for(10ms);
    return value.load()>=count;
}
static Hat enrollmentHat(int64_t challenge) {
    Hat hat{};hat.challenge=challenge;hat.userId=1;
    hat.authenticatorType=keymaster::HardwareAuthenticatorType::PASSWORD;hat.mac.assign(32,0);
    return hat;
}
TEST(ReadyFlag, FramePendingAtEnrollmentStartIsDiscarded) {
    // 录入开始时手指已按在键上：第1次读状态时已有帧待读。这一帧读出后丢弃、不上报，
    // 第5次读状态时的新按压照常取图并上报。
    resetMockTransport();mockReadyModel=true;mockReadyAt={1,5};
    TemporaryStore dir;encryptedSyntheticFrame(1);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(session->generateChallenge().isOk());ASSERT_TRUE(waitFor(cb->challenges));
    Signal signal;ASSERT_TRUE(session->enroll(enrollmentHat(cb->lastChallenge),&signal).isOk());
    EXPECT_TRUE(waitFor(cb->acquired));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),2);EXPECT_EQ(cb->acquired.load(),1);
    EXPECT_EQ(cb->lastError.load(),static_cast<int>(Error::CANCELED));
}
TEST(ReadyFlag, FrameReadyAgainBeforeLiftIsDiscarded) {
    // 第2次读状态时按下并取图；手指尚未抬起，传感器在下一次读状态前又备好一帧，
    // 状态位始终不为0。这一帧读出后丢弃，第6次读状态时的下一次按压照常取图并上报。
    resetMockTransport();mockReadyModel=true;mockReadyAt={2,3,6};
    TemporaryStore dir;encryptedSyntheticFrame(1);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(session->generateChallenge().isOk());ASSERT_TRUE(waitFor(cb->challenges));
    Signal signal;ASSERT_TRUE(session->enroll(enrollmentHat(cb->lastChallenge),&signal).isOk());
    EXPECT_TRUE(waitAtLeast(cb->acquired,2));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),3);EXPECT_EQ(cb->acquired.load(),2);
    EXPECT_EQ(cb->lastError.load(),static_cast<int>(Error::CANCELED));
}
TEST(ReadyFlag, FramePendingAtDetectionStartIsDiscarded) {
    // 检测同样从等待手指抬起开始：开始时待读的帧读出后丢弃，第4次读状态时的按压才算交互。
    resetMockTransport();mockReadyModel=true;mockReadyAt={1,4};
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->detectInteraction(&signal).isOk());
    EXPECT_TRUE(waitFor(cb->interactions));
    session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),1);EXPECT_EQ(mockStatusReads.load(),4);
}
TEST(ReadyFlag, BadFrameWhileDiscardingIsRetried) {
    // 读出待读帧时遇到坏帧：这一帧本就丢弃，不中止录入，下次轮询时重试。
    resetMockTransport();mockReadyModel=true;mockReadyAt={1,5};mockCaptureErrors={EPROTO};
    TemporaryStore dir;encryptedSyntheticFrame(1);
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kFocalTech);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(session->generateChallenge().isOk());ASSERT_TRUE(waitFor(cb->challenges));
    Signal signal;ASSERT_TRUE(session->enroll(enrollmentHat(cb->lastChallenge),&signal).isOk());
    EXPECT_TRUE(waitFor(cb->acquired));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),3);EXPECT_EQ(cb->acquired.load(),1);
    EXPECT_EQ(cb->lastError.load(),static_cast<int>(Error::CANCELED));
}
TEST(ReadyFlag, GoodixFingerHeldIsNotCaptured) {
    // Goodix的状态位就是手指在位：录入开始时手指按着，抬起前不取图。
    resetMockTransport();mockStatusSamples={1,1,1,1};
    TemporaryStore dir;
    auto hal=ndk::SharedRefBase::make<Fingerprint>(&gaokun3::kGoodix);auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    ASSERT_TRUE(session->generateChallenge().isOk());ASSERT_TRUE(waitFor(cb->challenges));
    Signal signal;ASSERT_TRUE(session->enroll(enrollmentHat(cb->lastChallenge),&signal).isOk());
    // 第5、6次读到0时距最后一次按下不到2秒，也不会取无指底图。
    EXPECT_TRUE(waitAtLeast(mockStatusReads,6));
    signal->cancel();session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),0);EXPECT_EQ(cb->acquired.load(),0);
}
}
