/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_TRANSACTION_H_
#define XENIA_SAVE_STATE_TRANSACTION_H_

#include <cstddef>
#include <string>
#include <vector>

#include "xenia/save_state_phase2.h"

namespace xe::save_state {

constexpr size_t kMaximumTransactionParticipantCount = 16;

enum class TransactionStage {
  kNone,
  kPreparing,
  kCommitting,
  kRollingBack,
  kSucceeded,
};

struct TransactionResult {
  StateProviderResult result = StateProviderResult::kOk;
  TransactionStage failed_stage = TransactionStage::kNone;
  size_t participant_index = SIZE_MAX;
  bool rollback_succeeded = true;
  std::string message;

  bool ok() const { return result == StateProviderResult::kOk; }
};

// Prepare may acquire reversible resources or create detached staging, but
// must not make an irreversible live-state change. Commit may mutate live
// state only if Rollback can still restore the state from before Prepare.
// Finalize must be infallible and may only discard rollback data or release a
// reversible resource after every participant has committed.
class TransactionParticipant {
 public:
  virtual ~TransactionParticipant() = default;

  virtual StateProviderResult Prepare(std::string* error_message) = 0;
  virtual StateProviderResult Commit(std::string* error_message) = 0;
  virtual StateProviderResult Rollback() noexcept = 0;
  virtual void Finalize() noexcept = 0;
};

// Participants prepare in registration order, then commit, rollback, and
// finalize in reverse order. Register a clock-freeze participant before memory
// so the clock remains frozen until memory has committed or rolled back.
class SubsystemTransactionCoordinator {
 public:
  TransactionResult Execute(
      const std::vector<TransactionParticipant*>& participants) const;
};

class FrozenClockAccess {
 public:
  virtual ~FrozenClockAccess() = default;

  virtual StateProviderResult Freeze(ClockSnapshot* original,
                                     std::string* error_message) = 0;
  virtual StateProviderResult Restore(const ClockSnapshot& snapshot,
                                      std::string* error_message) = 0;
  virtual void Unfreeze() noexcept = 0;
};

class FrozenClockRestoreParticipant final : public TransactionParticipant {
 public:
  FrozenClockRestoreParticipant(FrozenClockAccess& access,
                                const ClockSnapshot& target);

  StateProviderResult Prepare(std::string* error_message) override;
  StateProviderResult Commit(std::string* error_message) override;
  StateProviderResult Rollback() noexcept override;
  void Finalize() noexcept override;

 private:
  FrozenClockAccess& access_;
  const ClockSnapshot& target_;
  ClockSnapshot original_;
  bool prepared_ = false;
};

class PlannedMemoryRestoreAccess {
 public:
  virtual ~PlannedMemoryRestoreAccess() = default;

  // CaptureInventory and Stage must not mutate live guest memory. A successful
  // Stage owns all rollback data needed by Commit until Finalize.
  virtual StateProviderResult CaptureInventory(
      MemoryAllocationInventory* inventory, std::string* error_message) = 0;
  virtual StateProviderResult Stage(const MemoryRestorePlan& plan,
                                    const MemorySnapshot& target,
                                    std::string* error_message) = 0;
  virtual StateProviderResult Commit(std::string* error_message) = 0;
  virtual StateProviderResult Rollback() noexcept = 0;
  virtual void Finalize() noexcept = 0;
};

class PlannedMemoryRestoreParticipant final : public TransactionParticipant {
 public:
  PlannedMemoryRestoreParticipant(PlannedMemoryRestoreAccess& access,
                                  const MemorySnapshot& target);

  StateProviderResult Prepare(std::string* error_message) override;
  StateProviderResult Commit(std::string* error_message) override;
  StateProviderResult Rollback() noexcept override;
  void Finalize() noexcept override;

  const MemoryRestorePlan& plan() const { return plan_; }

 private:
  PlannedMemoryRestoreAccess& access_;
  const MemorySnapshot& target_;
  MemoryRestorePlan plan_;
  bool prepared_ = false;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_TRANSACTION_H_
