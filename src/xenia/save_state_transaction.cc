/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_transaction.h"

#include <algorithm>
#include <utility>

namespace xe::save_state {
namespace {

bool RollbackPrepared(
    const std::vector<TransactionParticipant*>& participants,
    size_t prepared_count) noexcept {
  bool succeeded = true;
  while (prepared_count) {
    --prepared_count;
    if (participants[prepared_count]->Rollback() != StateProviderResult::kOk) {
      succeeded = false;
    }
  }
  return succeeded;
}

TransactionResult InvalidTransaction(std::string message) {
  return {StateProviderResult::kInvalidState, TransactionStage::kNone, SIZE_MAX,
          true, std::move(message)};
}

}  // namespace

TransactionResult SubsystemTransactionCoordinator::Execute(
    const std::vector<TransactionParticipant*>& participants) const {
  if (participants.empty() ||
      participants.size() > kMaximumTransactionParticipantCount ||
      std::any_of(participants.begin(), participants.end(),
                  [](const TransactionParticipant* participant) {
                    return participant == nullptr;
                  })) {
    return InvalidTransaction(
        "Save-state transaction participant list is invalid.");
  }
  for (size_t index = 0; index < participants.size(); ++index) {
    if (std::find(participants.begin(), participants.begin() + index,
                  participants[index]) != participants.begin() + index) {
      return InvalidTransaction(
          "Save-state transaction contains a duplicate participant.");
    }
  }

  size_t prepared_count = 0;
  for (size_t index = 0; index < participants.size(); ++index) {
    std::string error_message;
    const StateProviderResult result =
        participants[index]->Prepare(&error_message);
    if (result != StateProviderResult::kOk) {
      const bool rollback_succeeded =
          RollbackPrepared(participants, prepared_count);
      return {result, TransactionStage::kPreparing, index, rollback_succeeded,
              std::move(error_message)};
    }
    ++prepared_count;
  }

  for (size_t index = participants.size(); index > 0; --index) {
    const size_t participant_index = index - 1;
    std::string error_message;
    const StateProviderResult result =
        participants[participant_index]->Commit(&error_message);
    if (result != StateProviderResult::kOk) {
      const bool rollback_succeeded =
          RollbackPrepared(participants, prepared_count);
      return {result, TransactionStage::kCommitting, participant_index,
              rollback_succeeded, std::move(error_message)};
    }
  }

  for (size_t index = participants.size(); index > 0; --index) {
    participants[index - 1]->Finalize();
  }
  return {StateProviderResult::kOk, TransactionStage::kSucceeded, SIZE_MAX,
          true, {}};
}

FrozenClockRestoreParticipant::FrozenClockRestoreParticipant(
    FrozenClockAccess& access, const ClockSnapshot& target)
    : access_(access), target_(target) {}

StateProviderResult FrozenClockRestoreParticipant::Prepare(
    std::string* error_message) {
  if (prepared_) {
    if (error_message) {
      *error_message = "Guest clock transaction is already prepared.";
    }
    return StateProviderResult::kInvalidState;
  }
  const SnapshotCodecValidation validation = ValidateClockSnapshot(target_);
  if (!validation.ok()) {
    if (error_message) {
      *error_message = validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  const StateProviderResult result = access_.Freeze(&original_, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }
  const SnapshotCodecValidation original_validation =
      ValidateClockSnapshot(original_);
  if (!original_validation.ok()) {
    access_.Unfreeze();
    if (error_message) {
      *error_message = original_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  prepared_ = true;
  return StateProviderResult::kOk;
}

StateProviderResult FrozenClockRestoreParticipant::Commit(
    std::string* error_message) {
  if (!prepared_) {
    if (error_message) {
      *error_message = "Guest clock transaction is not prepared.";
    }
    return StateProviderResult::kInvalidState;
  }
  return access_.Restore(target_, error_message);
}

StateProviderResult FrozenClockRestoreParticipant::Rollback() noexcept {
  if (!prepared_) {
    return StateProviderResult::kOk;
  }
  const StateProviderResult result = access_.Restore(original_, nullptr);
  access_.Unfreeze();
  prepared_ = false;
  return result;
}

void FrozenClockRestoreParticipant::Finalize() noexcept {
  if (!prepared_) {
    return;
  }
  access_.Unfreeze();
  prepared_ = false;
}

PlannedMemoryRestoreParticipant::PlannedMemoryRestoreParticipant(
    PlannedMemoryRestoreAccess& access, const MemorySnapshot& target)
    : access_(access), target_(target) {}

StateProviderResult PlannedMemoryRestoreParticipant::Prepare(
    std::string* error_message) {
  if (prepared_) {
    if (error_message) {
      *error_message = "Memory transaction is already prepared.";
    }
    return StateProviderResult::kInvalidState;
  }
  MemoryAllocationInventory current;
  StateProviderResult result =
      access_.CaptureInventory(&current, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }
  MemoryRestorePlan built_plan;
  const SnapshotCodecValidation validation =
      BuildMemoryRestorePlan(current, target_, &built_plan);
  if (!validation.ok()) {
    if (error_message) {
      *error_message = validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  result = access_.Stage(built_plan, target_, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }
  plan_ = std::move(built_plan);
  prepared_ = true;
  return StateProviderResult::kOk;
}

StateProviderResult PlannedMemoryRestoreParticipant::Commit(
    std::string* error_message) {
  if (!prepared_) {
    if (error_message) {
      *error_message = "Memory transaction is not prepared.";
    }
    return StateProviderResult::kInvalidState;
  }
  return access_.Commit(error_message);
}

StateProviderResult PlannedMemoryRestoreParticipant::Rollback() noexcept {
  if (!prepared_) {
    return StateProviderResult::kOk;
  }
  const StateProviderResult result = access_.Rollback();
  prepared_ = false;
  plan_ = {};
  return result;
}

void PlannedMemoryRestoreParticipant::Finalize() noexcept {
  if (!prepared_) {
    return;
  }
  access_.Finalize();
  prepared_ = false;
  plan_ = {};
}

}  // namespace xe::save_state
