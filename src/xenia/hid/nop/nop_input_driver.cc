/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/nop/nop_input_driver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/hid/hid_flags.h"

DECLARE_path(storage_root);

namespace xe {
namespace hid {
namespace nop {

#if XE_PLATFORM_ANDROID
namespace {

constexpr uint32_t kAndroidTapPulsePolls = 45;
constexpr float kAndroidLeftZone = 0.42f;
constexpr float kAndroidRightZone = 0.62f;
constexpr float kAndroidCenterStartMinX = 0.35f;
constexpr float kAndroidCenterStartMaxX = 0.65f;
constexpr float kAndroidStartMinY = 0.72f;
constexpr float kAndroidDeadZone = 0.18f;
constexpr float kAndroidDpadThreshold = 0.55f;
constexpr uint32_t kAndroidDebugInputFilePollInterval = 20;
constexpr uint8_t kAndroidDebugLeftTrigger = 1;
constexpr uint8_t kAndroidDebugRightTrigger = 2;

std::atomic<uint8_t> android_debug_trigger_bits{0};
std::atomic<uint32_t> android_debug_trigger_polls{0};

constexpr const char* kAndroidDebugInputPaths[] = {
    "/sdcard/Android/data/jp.xenios.emulator.github.debug/files/"
    "android_input.txt",
    "/sdcard/Android/data/jp.xenios.emulator.github/files/android_input.txt",
};

int16_t AxisFromUnit(float value) {
  value = std::clamp(value, -1.0f, 1.0f);
  if (std::abs(value) < kAndroidDeadZone) {
    return 0;
  }
  return static_cast<int16_t>(value * 32767.0f);
}

std::string UppercaseAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(
        std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

uint16_t ButtonForAndroidDebugInputToken(const std::string& token) {
  if (token == "A") {
    return X_INPUT_GAMEPAD_A;
  }
  if (token == "B") {
    return X_INPUT_GAMEPAD_B;
  }
  if (token == "X") {
    return X_INPUT_GAMEPAD_X;
  }
  if (token == "Y") {
    return X_INPUT_GAMEPAD_Y;
  }
  if (token == "START" || token == "MENU") {
    return X_INPUT_GAMEPAD_START;
  }
  if (token == "BACK" || token == "SELECT") {
    return X_INPUT_GAMEPAD_BACK;
  }
  if (token == "UP") {
    return X_INPUT_GAMEPAD_DPAD_UP;
  }
  if (token == "DOWN") {
    return X_INPUT_GAMEPAD_DPAD_DOWN;
  }
  if (token == "LEFT") {
    return X_INPUT_GAMEPAD_DPAD_LEFT;
  }
  if (token == "RIGHT") {
    return X_INPUT_GAMEPAD_DPAD_RIGHT;
  }
  if (token == "LB" || token == "L1") {
    return X_INPUT_GAMEPAD_LEFT_SHOULDER;
  }
  if (token == "RB" || token == "R1") {
    return X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  }
  return 0;
}

uint16_t ReadAndroidDebugInputFileAtPath(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    return 0;
  }

  std::stringstream buffer;
  buffer << input.rdbuf();
  input.close();
  std::error_code remove_error;
  std::filesystem::remove(path, remove_error);

  const std::string contents = buffer.str();
  std::istringstream tokens(contents);
  std::string token;
  uint16_t buttons = 0;
  uint8_t trigger_bits = 0;
  while (tokens >> token) {
    const std::string upper_token = UppercaseAscii(token);
    buttons |= ButtonForAndroidDebugInputToken(upper_token);
    if (upper_token == "LT" || upper_token == "L2") {
      trigger_bits |= kAndroidDebugLeftTrigger;
    } else if (upper_token == "RT" || upper_token == "R2") {
      trigger_bits |= kAndroidDebugRightTrigger;
    }
  }
  const std::string log_contents =
      contents.size() <= 128 ? contents : contents.substr(0, 128);
  if (buttons || trigger_bits) {
    if (trigger_bits) {
      android_debug_trigger_bits.fetch_or(trigger_bits);
      android_debug_trigger_polls.store(kAndroidTapPulsePolls);
    }
    XELOGI("Android debug input command '{}' -> buttons=0x{:04X} triggers=0x{:02X}",
           log_contents, buttons, trigger_bits);
    return buttons;
  }
  XELOGW("Android debug input command '{}' did not map to any inputs",
         log_contents);
  return 0;
}

uint16_t ReadAndroidDebugInputFile() {
  if (!cvars::storage_root.empty()) {
    const uint16_t storage_root_buttons = ReadAndroidDebugInputFileAtPath(
        cvars::storage_root / "android_input.txt");
    if (storage_root_buttons) {
      return storage_root_buttons;
    }
  }
  for (const char* path : kAndroidDebugInputPaths) {
    const uint16_t buttons = ReadAndroidDebugInputFileAtPath(path);
    if (buttons) {
      return buttons;
    }
  }
  return 0;
}

}  // namespace
#endif  // XE_PLATFORM_ANDROID

NopInputDriver::NopInputDriver(xe::ui::Window* window, size_t window_z_order)
    : InputDriver(window, window_z_order) {
#if XE_PLATFORM_ANDROID
  if (window) {
    window->AddInputListener(this, window_z_order);
  }
#endif
}

NopInputDriver::~NopInputDriver() {
#if XE_PLATFORM_ANDROID
  if (window()) {
    window()->RemoveInputListener(this);
  }
#endif
}

X_STATUS NopInputDriver::Setup() { return X_STATUS_SUCCESS; }

uint16_t NopInputDriver::GetButtonsForPoll(uint32_t poll_index) {
#if XE_PLATFORM_ANDROID
  (void)poll_index;
  return GetAndroidButtonsForPoll();
#else
  (void)poll_index;
  return 0;
#endif
}

void NopInputDriver::PulseAndroidButtons(uint16_t buttons) {
#if XE_PLATFORM_ANDROID
  if (!buttons) {
    return;
  }
  android_pulse_buttons_.fetch_or(buttons);
  android_pulse_polls_.store(kAndroidTapPulsePolls);
#else
  (void)buttons;
#endif
}

uint16_t NopInputDriver::GetAndroidButtonsForPoll() {
#if XE_PLATFORM_ANDROID
  if ((android_debug_input_poll_++ % kAndroidDebugInputFilePollInterval) == 0) {
    PulseAndroidButtons(ReadAndroidDebugInputFile());
  }

  uint16_t buttons = android_held_buttons_.load();
  if (android_pulse_polls_.load()) {
    buttons |= android_pulse_buttons_.load();
    if (android_pulse_polls_.fetch_sub(1) == 1) {
      android_pulse_buttons_.store(0);
    }
  }
  return buttons;
#else
  return 0;
#endif
}

void NopInputDriver::ApplyAndroidPointer(float x, float y, bool is_down) {
#if XE_PLATFORM_ANDROID
  uint32_t width = window() ? window()->GetActualPhysicalWidth() : 0;
  uint32_t height = window() ? window()->GetActualPhysicalHeight() : 0;
  if (!width || !height) {
    width = window() ? window()->GetDesiredLogicalWidth() : 0;
    height = window() ? window()->GetDesiredLogicalHeight() : 0;
  }

  if (!is_down || !width || !height) {
    android_held_buttons_.store(0);
    android_thumb_lx_.store(0);
    android_thumb_ly_.store(0);
    return;
  }

  x = std::clamp(x, 0.0f, static_cast<float>(width));
  y = std::clamp(y, 0.0f, static_cast<float>(height));
  const float nx = x / static_cast<float>(width);
  const float ny = y / static_cast<float>(height);

  uint16_t held_buttons = 0;
  uint16_t pulse_buttons = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;

  if (nx < kAndroidLeftZone) {
    const float radius = static_cast<float>(std::min(width, height)) * 0.24f;
    const float dx = std::clamp(
        (x - static_cast<float>(width) * 0.22f) / radius, -1.0f, 1.0f);
    const float dy = std::clamp(
        (static_cast<float>(height) * 0.62f - y) / radius, -1.0f, 1.0f);

    thumb_lx = AxisFromUnit(dx);
    thumb_ly = AxisFromUnit(dy);
    if (dx <= -kAndroidDpadThreshold) {
      held_buttons |= X_INPUT_GAMEPAD_DPAD_LEFT;
    } else if (dx >= kAndroidDpadThreshold) {
      held_buttons |= X_INPUT_GAMEPAD_DPAD_RIGHT;
    }
    if (dy <= -kAndroidDpadThreshold) {
      held_buttons |= X_INPUT_GAMEPAD_DPAD_DOWN;
    } else if (dy >= kAndroidDpadThreshold) {
      held_buttons |= X_INPUT_GAMEPAD_DPAD_UP;
    }
  } else if (nx >= kAndroidRightZone) {
    if (ny < 0.45f) {
      held_buttons |= X_INPUT_GAMEPAD_B;
      pulse_buttons |= X_INPUT_GAMEPAD_B;
    } else {
      held_buttons |= X_INPUT_GAMEPAD_A;
      pulse_buttons |= X_INPUT_GAMEPAD_A;
    }
  } else if (ny >= kAndroidStartMinY || (nx >= kAndroidCenterStartMinX &&
                                         nx <= kAndroidCenterStartMaxX)) {
    pulse_buttons |= X_INPUT_GAMEPAD_START;
  }

  android_held_buttons_.store(held_buttons);
  android_thumb_lx_.store(thumb_lx);
  android_thumb_ly_.store(thumb_ly);
  PulseAndroidButtons(pulse_buttons);
#else
  (void)x;
  (void)y;
  (void)is_down;
#endif
}

void NopInputDriver::OnTouchEvent(xe::ui::TouchEvent& e) {
#if XE_PLATFORM_ANDROID
  const uint32_t pointer_id = e.pointer_id();
  switch (e.action()) {
    case xe::ui::TouchEvent::Action::kDown: {
      uint32_t no_pointer = xe::ui::TouchEvent::kPointerIDNone;
      if (android_active_pointer_id_.compare_exchange_strong(no_pointer,
                                                             pointer_id) ||
          android_active_pointer_id_.load() == pointer_id) {
        ApplyAndroidPointer(e.x(), e.y(), true);
        e.set_handled(true);
      }
      break;
    }
    case xe::ui::TouchEvent::Action::kMove:
      if (android_active_pointer_id_.load() == pointer_id) {
        ApplyAndroidPointer(e.x(), e.y(), true);
        e.set_handled(true);
      }
      break;
    case xe::ui::TouchEvent::Action::kUp:
    case xe::ui::TouchEvent::Action::kCancel:
      if (android_active_pointer_id_.load() == pointer_id) {
        ApplyAndroidPointer(e.x(), e.y(), false);
        android_active_pointer_id_.store(xe::ui::TouchEvent::kPointerIDNone);
        e.set_handled(true);
      }
      break;
  }
#else
  (void)e;
#endif
}

void NopInputDriver::OnMouseDown(xe::ui::MouseEvent& e) {
#if XE_PLATFORM_ANDROID
  if (e.button() == xe::ui::MouseEvent::Button::kLeft) {
    ApplyAndroidPointer(static_cast<float>(e.x()), static_cast<float>(e.y()),
                        true);
    e.set_handled(true);
  }
#else
  (void)e;
#endif
}

void NopInputDriver::OnMouseUp(xe::ui::MouseEvent& e) {
#if XE_PLATFORM_ANDROID
  if (e.button() == xe::ui::MouseEvent::Button::kLeft) {
    ApplyAndroidPointer(static_cast<float>(e.x()), static_cast<float>(e.y()),
                        false);
    e.set_handled(true);
  }
#else
  (void)e;
#endif
}

X_RESULT NopInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                         X_INPUT_CAPABILITIES* out_caps) {
  (void)flags;
  if (user_index >= XUserMaxUserCount || !out_caps) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = XINPUT_DEVTYPE_GAMEPAD;
  out_caps->sub_type = XINPUT_DEVSUBTYPE_GAMEPAD;
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xF7FF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFFu;
  out_caps->vibration.right_motor_speed = 0xFFFFu;
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetState(uint32_t user_index,
                                  X_INPUT_STATE* out_state) {
  if (user_index >= XUserMaxUserCount || !out_state) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

  std::memset(out_state, 0, sizeof(*out_state));
  const uint16_t buttons = GetButtonsForPoll(poll_count_++);
  uint8_t trigger_bits = 0;
#if XE_PLATFORM_ANDROID
  if (android_debug_trigger_polls.load()) {
    trigger_bits = android_debug_trigger_bits.load();
    if (android_debug_trigger_polls.fetch_sub(1) == 1) {
      android_debug_trigger_bits.store(0);
    }
  }
#endif
  const int16_t thumb_lx = android_thumb_lx_.load();
  const int16_t thumb_ly = android_thumb_ly_.load();
  if (buttons != last_buttons_ || trigger_bits != last_trigger_bits_ ||
      thumb_lx != last_thumb_lx_ || thumb_ly != last_thumb_ly_) {
    ++packet_number_;
    last_buttons_ = buttons;
    last_trigger_bits_ = trigger_bits;
    last_thumb_lx_ = thumb_lx;
    last_thumb_ly_ = thumb_ly;
  }
  out_state->packet_number = packet_number_;
  out_state->gamepad.buttons = buttons;
  out_state->gamepad.left_trigger =
      trigger_bits & kAndroidDebugLeftTrigger ? 0xFF : 0;
  out_state->gamepad.right_trigger =
      trigger_bits & kAndroidDebugRightTrigger ? 0xFF : 0;
  out_state->gamepad.thumb_lx = thumb_lx;
  out_state->gamepad.thumb_ly = thumb_ly;
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::SetState(uint32_t user_index,
                                  X_INPUT_VIBRATION* vibration) {
  if (user_index >= XUserMaxUserCount || !vibration) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  if (user_index != 0) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT NopInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                      X_INPUT_KEYSTROKE* out_keystroke) {
  const bool any_user = user_index == XUserIndexAny;
  (void)flags;
  if ((!any_user && user_index != 0) || !out_keystroke) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  std::memset(out_keystroke, 0, sizeof(*out_keystroke));
  return X_ERROR_EMPTY;
}

InputType NopInputDriver::GetInputType() const { return InputType::Controller; }

}  // namespace nop
}  // namespace hid
}  // namespace xe
