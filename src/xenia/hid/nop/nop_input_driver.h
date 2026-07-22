/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
#define XENIA_HID_NOP_NOP_INPUT_DRIVER_H_

#include <atomic>
#include <cstdint>

#include "xenia/hid/input_driver.h"
#include "xenia/ui/window_listener.h"

namespace xe {
namespace hid {
namespace nop {

#if XE_PLATFORM_ANDROID
void SetAndroidPhysicalGamepadState(uint16_t buttons, float left_x,
                                    float left_y, float right_x, float right_y,
                                    float left_trigger, float right_trigger);
void ResetAndroidPhysicalGamepadState();
#endif

class NopInputDriver final : public InputDriver,
                             public xe::ui::WindowInputListener {
 public:
  explicit NopInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~NopInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  virtual InputType GetInputType() const override;

  void OnTouchEvent(xe::ui::TouchEvent& e) override;
  void OnMouseDown(xe::ui::MouseEvent& e) override;
  void OnMouseUp(xe::ui::MouseEvent& e) override;

 private:
  uint16_t GetButtonsForPoll(uint32_t poll_index);
  void ApplyAndroidPointer(float x, float y, bool is_down);
  uint16_t GetAndroidButtonsForPoll();
  void PulseAndroidButtons(uint16_t buttons);

  uint32_t packet_number_ = 0;
  uint32_t poll_count_ = 0;
  uint16_t last_buttons_ = 0;
  uint8_t last_left_trigger_ = 0;
  uint8_t last_right_trigger_ = 0;
  int16_t last_thumb_lx_ = 0;
  int16_t last_thumb_ly_ = 0;
  int16_t last_thumb_rx_ = 0;
  int16_t last_thumb_ry_ = 0;

  std::atomic<uint32_t> android_active_pointer_id_{
      xe::ui::TouchEvent::kPointerIDNone};
  std::atomic<uint16_t> android_held_buttons_{0};
  std::atomic<uint16_t> android_pulse_buttons_{0};
  std::atomic<uint32_t> android_pulse_polls_{0};
  std::atomic<int16_t> android_thumb_lx_{0};
  std::atomic<int16_t> android_thumb_ly_{0};
  uint32_t android_debug_input_poll_ = 0;
};

}  // namespace nop
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_NOP_NOP_INPUT_DRIVER_H_
