/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <array>
#include <atomic>
#include <bitset>
#include <memory>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/base/reversible_admission_gate.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/skylander/skylander_portal.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, uint32_t flags,
                    X_INPUT_STATE* out_state);
  // GetState variant for UI that bypasses the input blocker
  X_RESULT GetStateForUI(uint32_t user_index, uint32_t flags,
                         X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

  // Block/unblock input to the game (for UI dialogs)
  void AddUIInputBlocker();
  void RemoveUIInputBlocker();

  bool GetVibrationCvar();
  void ToggleVibration();

  X_STATUS ReadSkylanderPortal(std::vector<uint8_t>& data);
  X_STATUS WriteSkylanderPortal(std::vector<uint8_t>& data);

  AdmissionGateResult CloseSaveStateInputAdmission(
      uint64_t owner_id, std::chrono::steady_clock::time_point deadline) {
    return save_state_admission_gate_.CloseAndWait(owner_id, deadline);
  }
  bool ReopenSaveStateInputAdmission(uint64_t owner_id) noexcept {
    return save_state_admission_gate_.Reopen(owner_id);
  }
  AdmissionGateSnapshot GetSaveStateInputAdmissionState() const {
    return save_state_admission_gate_.Snapshot();
  }

  const std::bitset<XUserMaxUserCount> GetConnectedSlots() const {
    return connected_slots;
  }

  uint32_t GetLastUsedSlot() const { return last_used_slot; }

  std::unique_lock<xe_unlikely_mutex> lock();

 private:
  typedef std::pair<uint16_t, uint16_t> joystick_value;

  const std::string controller_slot_state_change_message[2] = {
      "Controller disconnected from slot {}.",
      "New controller connected to slot {}."};

  void UpdateUsedSlot(InputDriver* driver, uint8_t slot, bool connected);
  void AdjustDeadzoneLevels(const uint8_t slot, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(X_INPUT_VIBRATION* vibration);
  X_RESULT GetStateForUIImpl(uint32_t user_index, uint32_t flags,
                             X_INPUT_STATE* out_state);
  X_RESULT SetStateImpl(uint32_t user_index,
                        X_INPUT_VIBRATION* vibration);

  std::vector<InputDriver*> FilterDrivers(uint32_t flags);

  xe::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<SkylanderPortal> skylander_portal_;

  std::bitset<XUserMaxUserCount> connected_slots = {};
  std::array<std::pair<joystick_value, joystick_value>, XUserMaxUserCount>
      controllers_max_joystick_value = {};
  uint32_t last_used_slot = 0;

  xe_unlikely_mutex lock_;

  // Reference count for UI elements blocking game input
  std::atomic<int> ui_input_blockers_{0};

  // Buttons that should be masked from game input until released (per slot).
  // This prevents button presses used to close UI dialogs from being
  // seen by the game immediately after the dialog closes.
  std::array<uint16_t, XUserMaxUserCount> consumed_buttons_{};

  // Tracks every InputSystem-owned driver poll, portal access, connection
  // update, keystroke dequeue, and vibration write. It is not registered as a
  // complete input save-state participant because driver-internal background
  // state has not yet joined this boundary.
  ReversibleAdmissionGate save_state_admission_gate_;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
