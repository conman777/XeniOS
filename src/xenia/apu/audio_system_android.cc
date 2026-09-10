/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#include "xenia/apu/audio_system_android.h"

#include <SLES/OpenSLES_Android.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/apu/conversion.h"
#include "xenia/base/logging.h"

namespace xe::apu {
namespace {

class AndroidAudioDriver final : public AudioDriver {
 public:
  AndroidAudioDriver(SLEngineItf engine, SLObjectItf mix,
                     xe::threading::Semaphore* semaphore, uint32_t frequency,
                     uint32_t channels, bool convert)
      : engine_(engine), mix_(mix), semaphore_(semaphore),
        frequency_(frequency), channels_(channels), convert_(convert) {}
  ~AndroidAudioDriver() override { Shutdown(); }

  bool Initialize() override {
    if (!engine_ || !mix_ || !semaphore_ ||
        (channels_ != 2 && channels_ != 6) ||
        (convert_ && channels_ != 6) || frequency_ < 8000 ||
        frequency_ > 192000) {
      return false;
    }
    samples_per_channel_ = kFrameSamplesMax / channels_;
    SLDataLocator_AndroidSimpleBufferQueue locator = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
        SLuint32(AudioSystem::kMaximumQueuedFrames)};
    SLDataFormat_PCM format = {
        SL_DATAFORMAT_PCM, 2, frequency_ * 1000,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
        SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&locator, &format};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, mix_};
    SLDataSink sink = {&output, nullptr};
    const SLInterfaceID interfaces[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean required[] = {SL_BOOLEAN_TRUE};
    if ((*engine_)->CreateAudioPlayer(engine_, &player_object_, &source, &sink,
                                      1, interfaces, required) != SL_RESULT_SUCCESS ||
        (*player_object_)->Realize(player_object_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*player_object_)->GetInterface(player_object_, SL_IID_PLAY, &play_) != SL_RESULT_SUCCESS ||
        (*player_object_)->GetInterface(player_object_, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                        &queue_) != SL_RESULT_SUCCESS ||
        (*queue_)->RegisterCallback(queue_, BufferConsumed, this) != SL_RESULT_SUCCESS ||
        (*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS) {
      XELOGE("Android audio player initialization failed");
      Shutdown();
      return false;
    }
    XELOGI("Android audio output opened: {} Hz stereo, {} samples per buffer",
           frequency_, samples_per_channel_);
    return true;
  }

  void SubmitFrame(float* samples) override {
    // AudioSystem serializes submissions. Its semaphore limits outstanding
    // buffers to kMaximumQueuedFrames, so a ring slot isn't reused before
    // Android has consumed it. No allocation or mutex is needed in callbacks.
    if (!queue_) {
      semaphore_->Release(1, nullptr);
      return;
    }
    std::array<float, kFrameSamplesMax> stereo;
    if (convert_) {
      conversion::sequential_6_BE_to_interleaved_2_LE(
          stereo.data(), samples, samples_per_channel_);
    } else if (channels_ == 2) {
      std::memcpy(stereo.data(), samples,
                  samples_per_channel_ * 2 * sizeof(float));
    } else {
      for (uint32_t i = 0; i < samples_per_channel_; ++i) {
        const float* frame = samples + i * 6;
        stereo[i * 2] = (frame[0] + frame[4] + frame[2] * 0.5f) * 0.4f;
        stereo[i * 2 + 1] = (frame[1] + frame[5] + frame[2] * 0.5f) * 0.4f;
      }
    }
    auto& output = buffers_[write_index_];
    const float gain = cvars::mute ? 0.0f : volume_.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < samples_per_channel_ * 2; ++i) {
      const float value = stereo[i] * gain;
      output[i] = std::isfinite(value)
                      ? int16_t(std::clamp(value, -1.0f, 1.0f) * 32767.0f)
                      : 0;
    }
    const SLresult result = (*queue_)->Enqueue(
        queue_, output.data(), samples_per_channel_ * 2 * sizeof(int16_t));
    if (result == SL_RESULT_SUCCESS) {
      write_index_ = (write_index_ + 1) % buffers_.size();
    } else {
      XELOGE("Android audio enqueue failed: {}", result);
      semaphore_->Release(1, nullptr);
    }
  }

  void Pause() override {
    if (play_) { (*play_)->SetPlayState(play_, SL_PLAYSTATE_PAUSED); }
  }
  void Resume() override {
    if (play_) { (*play_)->SetPlayState(play_, SL_PLAYSTATE_PLAYING); }
  }
  void SetVolume(float volume) override {
    volume_.store(std::isfinite(volume) ? std::clamp(volume, 0.0f, 1.0f) : 0.0f,
                  std::memory_order_relaxed);
  }
  void Shutdown() override {
    if (player_object_) {
      if (play_) { (*play_)->SetPlayState(play_, SL_PLAYSTATE_STOPPED); }
      // Destroy waits for callbacks before the semaphore or ring can be freed.
      (*player_object_)->Destroy(player_object_);
      player_object_ = nullptr;
      play_ = nullptr;
      queue_ = nullptr;
    }
  }

 private:
  static void BufferConsumed(SLAndroidSimpleBufferQueueItf, void* context) {
    static_cast<AndroidAudioDriver*>(context)->semaphore_->Release(1, nullptr);
  }
  SLEngineItf engine_;
  SLObjectItf mix_;
  xe::threading::Semaphore* semaphore_;
  uint32_t frequency_, channels_;
  bool convert_;
  uint32_t samples_per_channel_ = 0;
  std::atomic<float> volume_{1.0f};
  SLObjectItf player_object_ = nullptr;
  SLPlayItf play_ = nullptr;
  SLAndroidSimpleBufferQueueItf queue_ = nullptr;
  std::array<std::array<int16_t, kFrameSamplesMax>,
             AudioSystem::kMaximumQueuedFrames> buffers_{};
  size_t write_index_ = 0;
};
}  // namespace

std::unique_ptr<AudioSystem> AndroidAudioSystem::Create(cpu::Processor* processor) {
  return std::make_unique<AndroidAudioSystem>(processor);
}
AndroidAudioSystem::AndroidAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}
AndroidAudioSystem::~AndroidAudioSystem() { DestroyEngine(); }

X_STATUS AndroidAudioSystem::Setup(kernel::KernelState* kernel_state) {
  if (slCreateEngine(&engine_object_, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
      (*engine_object_)->Realize(engine_object_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
      (*engine_object_)->GetInterface(engine_object_, SL_IID_ENGINE, &engine_) != SL_RESULT_SUCCESS ||
      (*engine_)->CreateOutputMix(engine_, &output_mix_, 0, nullptr, nullptr) != SL_RESULT_SUCCESS ||
      (*output_mix_)->Realize(output_mix_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) {
    XELOGE("Android audio engine initialization failed");
    DestroyEngine();
    return X_STATUS_UNSUCCESSFUL;
  }
  return AudioSystem::Setup(kernel_state);
}

void AndroidAudioSystem::DestroyEngine() {
  if (output_mix_) {
    (*output_mix_)->Destroy(output_mix_);
    output_mix_ = nullptr;
  }
  if (engine_object_) {
    (*engine_object_)->Destroy(engine_object_);
    engine_object_ = nullptr;
    engine_ = nullptr;
  }
}
void AndroidAudioSystem::Shutdown() {
  AudioSystem::Shutdown();
  DestroyEngine();
}
X_STATUS AndroidAudioSystem::CreateDriver(size_t,
    xe::threading::Semaphore* semaphore, AudioDriver** out_driver) {
  auto driver = std::make_unique<AndroidAudioDriver>(
      engine_, output_mix_, semaphore, AudioDriver::kFrameFrequencyDefault,
      AudioDriver::kFrameChannelsDefault, true);
  if (!driver->Initialize()) { return X_STATUS_UNSUCCESSFUL; }
  *out_driver = driver.release();
  return X_STATUS_SUCCESS;
}
AudioDriver* AndroidAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
    uint32_t frequency, uint32_t channels, bool need_format_conversion) {
  return new AndroidAudioDriver(engine_, output_mix_, semaphore, frequency,
                                 channels, need_format_conversion);
}
void AndroidAudioSystem::DestroyDriver(AudioDriver* driver) {
  delete driver;
}
}  // namespace xe::apu
