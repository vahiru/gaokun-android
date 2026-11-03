#include "echo_canceller.h"

#ifdef GAOKUN_MICARRAY_AEC
#include "modules/audio_processing/include/audio_processing.h"
#endif

namespace gaokun::micarray {

#ifdef GAOKUN_MICARRAY_AEC

namespace {

class Aec3 : public EchoCanceller {
  public:
    explicit Aec3(rtc::scoped_refptr<webrtc::AudioProcessing> apm) : mApm(std::move(apm)) {}

    void render(const float* frame) override {
        const float* in = frame;
        float* out = mRender;
        mApm->ProcessReverseStream(&in, mConfig, mConfig, &out);
    }

    void process(float* frame) override {
        float* io = frame;
        mApm->ProcessStream(&io, mConfig, mConfig, &io);
    }

    int delayMs() override {
        const webrtc::AudioProcessingStats stats = mApm->GetStatistics();
        return stats.delay_ms ? *stats.delay_ms : -1;
    }

  private:
    rtc::scoped_refptr<webrtc::AudioProcessing> mApm;
    const webrtc::StreamConfig mConfig{48000, 1};
    float mRender[kFrame] = {};  // ProcessReverseStream's output, not used
};

}  // namespace

std::unique_ptr<EchoCanceller> EchoCanceller::create(std::string* why) {
    rtc::scoped_refptr<webrtc::AudioProcessing> apm = webrtc::AudioProcessingBuilder().Create();
    if (!apm) {
        *why = "AudioProcessing not created";
        return nullptr;
    }
    // AEC3 (mobile_mode would be AECM) and nothing else: noise suppression is DeepFilterNet3's
    // job, and the level is the app's.  AEC3 always high-passes its input.
    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = true;
    config.echo_canceller.mobile_mode = false;
    apm->ApplyConfig(config);
    return std::make_unique<Aec3>(std::move(apm));
}

#else

std::unique_ptr<EchoCanceller> EchoCanceller::create(std::string* why) {
    *why = "built without GAOKUN_MICARRAY_AEC";
    return nullptr;
}

#endif

}  // namespace gaokun::micarray
