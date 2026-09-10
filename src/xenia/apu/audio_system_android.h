/**
 * Xenia : Xbox 360 Emulator Research Project
 * Released under the BSD license - see LICENSE in the root for more details.
 */
#ifndef XENIA_APU_AUDIO_SYSTEM_ANDROID_H_
#define XENIA_APU_AUDIO_SYSTEM_ANDROID_H_

#include <SLES/OpenSLES.h>

#include "xenia/apu/audio_system.h"

namespace xe::apu {

// OpenSL ES keeps audio available on the project's Android API 24 minimum.
class AndroidAudioSystem final : public AudioSystem {
 public:
  static bool IsAvailable() { return true; }
  static std::unique_ptr<AudioSystem> Create(cpu::Processor* processor);
  explicit AndroidAudioSystem(cpu::Processor* processor);
  ~AndroidAudioSystem() override;
  std::string name() const override { return "OpenSL ES"; }
  X_STATUS Setup(kernel::KernelState* kernel_state) override;
  void Shutdown() override;
  AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                            uint32_t frequency, uint32_t channels,
                            bool need_format_conversion) override;

 protected:
  X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;

 private:
  void DestroyEngine();
  SLObjectItf engine_object_ = nullptr;
  SLEngineItf engine_ = nullptr;
  SLObjectItf output_mix_ = nullptr;
};

}  // namespace xe::apu
#endif
