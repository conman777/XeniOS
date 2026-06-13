/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/nop/nop_audio_system.h"

#include "xenia/apu/apu_flags.h"
#include "xenia/apu/audio_driver.h"
#include "xenia/base/assert.h"

namespace xe {
namespace apu {
namespace nop {

namespace {

class SilentAudioDriver final : public AudioDriver {
 public:
  explicit SilentAudioDriver(xe::threading::Semaphore* semaphore)
      : semaphore_(semaphore) {}

  bool Initialize() override { return true; }
  void Shutdown() override {}
  void SubmitFrame(float* samples) override {
    (void)samples;
    if (semaphore_) {
      semaphore_->Release(1, nullptr);
    }
  }
  void Pause() override {}
  void Resume() override {}
  void SetVolume(float volume) override { (void)volume; }

 private:
  xe::threading::Semaphore* semaphore_ = nullptr;
};

}  // namespace

std::unique_ptr<AudioSystem> NopAudioSystem::Create(cpu::Processor* processor) {
  return std::make_unique<NopAudioSystem>(processor);
}

NopAudioSystem::NopAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t index,
                                      xe::threading::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  (void)index;
  assert_not_null(out_driver);
  auto* driver = new SilentAudioDriver(semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

AudioDriver* NopAudioSystem::CreateDriver(xe::threading::Semaphore* semaphore,
                                          uint32_t frequency, uint32_t channels,
                                          bool need_format_conversion) {
  (void)frequency;
  (void)channels;
  (void)need_format_conversion;
  return new SilentAudioDriver(semaphore);
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) {
  if (!driver) {
    return;
  }
  driver->Shutdown();
  delete driver;
}

}  // namespace nop
}  // namespace apu
}  // namespace xe
