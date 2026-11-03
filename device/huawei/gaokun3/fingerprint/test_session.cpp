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
    std::atomic_int challenges{0}, errors{0}, closes{0}, interactions{0}, lastError{0}, successes{0}, rejections{0}, timedLockouts{0};
    std::function<void()> errorHook, closeHook, rejectionHook;
    ScopedAStatus onChallengeGenerated(int64_t) override { ++challenges; return ScopedAStatus::ok(); }
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
    NOOP(onAcquired, (AcquiredInfo, int32_t))
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
    auto hal = ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal = ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal = ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();
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
}

namespace aidl::android::hardware::biometrics::fingerprint {
// Generated texture only; exercise AES decode and the real matcher without
// accessing a real fingerprint or bypassing the authentication decision.
static Gaokun3Frame encryptedSyntheticFrame(unsigned seed) {
    std::mt19937 random(seed);
    std::array<uint8_t,gaokun3::kImageBytes> plain{};
    for (size_t i=0;i<GAOKUN3_PIXELS;++i) {
        unsigned value=200+(random()&255)*8;
        plain[2*i]=value&255;plain[2*i+1]=value>>8;
    }
    static const uint8_t key[]={0xeb,0x4a,0xa0,0x13,0x68,0xd8,0x46,0x47,0x0d,0x2b,0xae,0xe3,0x9d,0x02,0x0f,0xdf};
    static const uint8_t iv[]={0xd5,0xaa,0x36,0xa5,0x4b,0x7b,0x18,0x35,0xf2,0x37,0xd3,0xf0,0xc5,0x45,0x49,0xbb};
    auto* ctx=EVP_CIPHER_CTX_new();CHECK(ctx);int n=0,tail=0;
    std::array<uint8_t,gaokun3::kImageBytes+EVP_MAX_BLOCK_LENGTH> encrypted{};
    CHECK_EQ(EVP_EncryptInit_ex(ctx,EVP_aes_128_cbc(),nullptr,key,iv),1);
    CHECK_EQ(EVP_CIPHER_CTX_set_padding(ctx,0),1);
    CHECK_EQ(EVP_EncryptUpdate(ctx,encrypted.data(),&n,plain.data(),plain.size()),1);
    CHECK_EQ(EVP_EncryptFinal_ex(ctx,encrypted.data()+n,&tail),1);
    EVP_CIPHER_CTX_free(ctx);CHECK_EQ(static_cast<size_t>(n+tail),gaokun3::kImageBytes);
    memcpy(mockCipher.data(),encrypted.data(),mockCipher.size());
    Gaokun3Frame decoded{};CHECK(gaokun3::decode(mockCipher.data(),&decoded));return decoded;
}
static void storeSyntheticEnrollment(const TemporaryStore& dir,const Gaokun3Frame& frame,uint32_t failures=0) {
    CHECK_EQ(mkdir(dir.data().c_str(),0700),0);
    gaokun3::Store store(dir.data()+"/gaokun3-templates.db");
    gaokun3::Enrollment entry{};entry.id=7;entry.frames.fill(frame);
    store.enrollments.push_back(entry);store.failures=failures;store.authenticatorId=gaokun3::randomId();CHECK(store.save());
}
TEST(Authentication, AlreadyPresentMatchingContactIsProcessed) {
    resetMockTransport();mockStatusSamples={1};
    TemporaryStore dir;auto frame=encryptedSyntheticFrame(1);storeSyntheticEnrollment(dir,frame);
    auto hal=ndk::SharedRefBase::make<Fingerprint>();auto cb=ndk::SharedRefBase::make<Callback>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();auto cb=ndk::SharedRefBase::make<Callback>();
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
    auto hal=ndk::SharedRefBase::make<Fingerprint>();auto cb=ndk::SharedRefBase::make<Callback>();
    std::shared_ptr<ISession> session;ASSERT_TRUE(hal->createSession(0,0,cb,&session).isOk());
    std::static_pointer_cast<Session>(session)->setStoreDirectoryForTest(dir.data());
    Signal signal;ASSERT_TRUE(session->authenticate(0,&signal).isOk());
    EXPECT_TRUE(waitFor(cb->successes));
    session->close();EXPECT_TRUE(waitFor(cb->closes));
    EXPECT_EQ(mockCaptures.load(),2);EXPECT_EQ(cb->successes.load(),1);
    EXPECT_EQ(cb->rejections.load(),0);EXPECT_EQ(cb->timedLockouts.load(),0);
    gaokun3::Store restored(dir.data()+"/gaokun3-templates.db");
    ASSERT_TRUE(restored.load());EXPECT_EQ(restored.failures,0u);
}
TEST(Authentication, RejectedWindowDoesNotRequireReadyFlagToClear) {
    resetMockTransport();mockStatusSamples=std::vector<int>(80,1);
    TemporaryStore dir;
    auto frame=encryptedSyntheticFrame(1);auto matching=mockCipher;
    storeSyntheticEnrollment(dir,frame);
    encryptedSyntheticFrame(400);
    auto hal=ndk::SharedRefBase::make<Fingerprint>();auto cb=ndk::SharedRefBase::make<Callback>();
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
    gaokun3::Store restored(dir.data()+"/gaokun3-templates.db");
    ASSERT_TRUE(restored.load());EXPECT_EQ(restored.failures,0u);
}
}
