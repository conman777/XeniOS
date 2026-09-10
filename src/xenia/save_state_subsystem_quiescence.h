/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_SUBSYSTEM_QUIESCENCE_H_
#define XENIA_SAVE_STATE_SUBSYSTEM_QUIESCENCE_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "xenia/save_state.h"

namespace xe::save_state {

enum class QuiescentSubsystem : uint32_t {
  kKernelDispatchAndTimers,
  kVfsAndIo,
  kVulkanCommandProcessor,
  kAudio,
  kInput,
};

constexpr size_t kRequiredQuiescentSubsystemCount = 5;
inline constexpr std::array<QuiescentSubsystem,
                            kRequiredQuiescentSubsystemCount>
    kSubsystemQuiescenceOrder = {
        QuiescentSubsystem::kKernelDispatchAndTimers,
        QuiescentSubsystem::kVfsAndIo,
        QuiescentSubsystem::kVulkanCommandProcessor,
        QuiescentSubsystem::kAudio,
        QuiescentSubsystem::kInput,
};
constexpr std::chrono::milliseconds kMaximumSubsystemQuiescenceWait =
    std::chrono::seconds(30);

enum class SubsystemQuiescenceResult {
  kReached,
  kBusy,
  kUnsupported,
  kTimedOut,
  kInvalidConfiguration,
  kFailed,
};

struct SubsystemQuiescenceRequest {
  Operation operation = Operation::kSave;
  uint64_t request_id = 0;
  std::chrono::steady_clock::time_point deadline;
};

// Prepare may only close a reversible admission gate, finish already accepted
// work, and retain ownership of the resulting stable boundary. It must honor
// request.deadline. A failed Prepare must synchronously undo its own partial
// work and leave the subsystem running exactly as before.
//
// Abort must be infallible, must not serialize or restore subsystem state, and
// must release only the matching request's boundary.
class SubsystemQuiescenceParticipant {
 public:
  virtual ~SubsystemQuiescenceParticipant() = default;

  virtual QuiescentSubsystem subsystem() const = 0;
  virtual SubsystemQuiescenceResult Prepare(
      const SubsystemQuiescenceRequest& request,
      std::string* error_message) = 0;
  virtual void Abort(uint64_t request_id) noexcept = 0;
};

struct SubsystemQuiescenceAcquireResult {
  SubsystemQuiescenceResult result =
      SubsystemQuiescenceResult::kInvalidConfiguration;
  size_t participant_index = SIZE_MAX;
  QuiescentSubsystem subsystem =
      QuiescentSubsystem::kKernelDispatchAndTimers;
  std::string message;

  bool ok() const { return result == SubsystemQuiescenceResult::kReached; }
};

// Sole owner of all successfully prepared subsystem boundaries. Participants
// are always aborted in reverse order, preserving the dependency hierarchy.
class HeldSubsystemQuiescence {
 public:
  HeldSubsystemQuiescence() = default;
  ~HeldSubsystemQuiescence();
  HeldSubsystemQuiescence(const HeldSubsystemQuiescence&) = delete;
  HeldSubsystemQuiescence& operator=(const HeldSubsystemQuiescence&) = delete;
  HeldSubsystemQuiescence(HeldSubsystemQuiescence&& other) noexcept;
  HeldSubsystemQuiescence& operator=(
      HeldSubsystemQuiescence&& other) noexcept;

  explicit operator bool() const {
    return prepared_count_ == kRequiredQuiescentSubsystemCount;
  }
  uint64_t request_id() const { return request_id_; }
  size_t prepared_count() const { return prepared_count_; }

  void Release() noexcept;

 private:
  friend class SubsystemQuiescenceCoordinator;

  uint64_t request_id_ = 0;
  std::array<SubsystemQuiescenceParticipant*,
             kRequiredQuiescentSubsystemCount>
      participants_ = {};
  size_t prepared_count_ = 0;
};

class SubsystemQuiescenceCoordinator {
 public:
  SubsystemQuiescenceAcquireResult Acquire(
      Operation operation, uint64_t request_id,
      std::chrono::milliseconds timeout,
      const std::array<SubsystemQuiescenceParticipant*,
                       kRequiredQuiescentSubsystemCount>& participants,
      HeldSubsystemQuiescence* output) const;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_SUBSYSTEM_QUIESCENCE_H_
