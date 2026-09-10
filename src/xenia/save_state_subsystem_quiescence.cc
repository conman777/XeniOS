/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_subsystem_quiescence.h"

#include <algorithm>
#include <utility>

namespace xe::save_state {
namespace {

void AbortPrepared(
    const std::array<SubsystemQuiescenceParticipant*,
                     kRequiredQuiescentSubsystemCount>& participants,
    size_t prepared_count, uint64_t request_id) noexcept {
  while (prepared_count) {
    --prepared_count;
    participants[prepared_count]->Abort(request_id);
  }
}

SubsystemQuiescenceAcquireResult Failure(
    SubsystemQuiescenceResult result, size_t participant_index,
    QuiescentSubsystem subsystem, std::string message) {
  return {result, participant_index, subsystem, std::move(message)};
}

}  // namespace

HeldSubsystemQuiescence::~HeldSubsystemQuiescence() { Release(); }

HeldSubsystemQuiescence::HeldSubsystemQuiescence(
    HeldSubsystemQuiescence&& other) noexcept
    : request_id_(other.request_id_),
      participants_(other.participants_),
      prepared_count_(other.prepared_count_) {
  other.request_id_ = 0;
  other.participants_.fill(nullptr);
  other.prepared_count_ = 0;
}

HeldSubsystemQuiescence& HeldSubsystemQuiescence::operator=(
    HeldSubsystemQuiescence&& other) noexcept {
  if (this != &other) {
    Release();
    request_id_ = other.request_id_;
    participants_ = other.participants_;
    prepared_count_ = other.prepared_count_;
    other.request_id_ = 0;
    other.participants_.fill(nullptr);
    other.prepared_count_ = 0;
  }
  return *this;
}

void HeldSubsystemQuiescence::Release() noexcept {
  AbortPrepared(participants_, prepared_count_, request_id_);
  request_id_ = 0;
  participants_.fill(nullptr);
  prepared_count_ = 0;
}

SubsystemQuiescenceAcquireResult SubsystemQuiescenceCoordinator::Acquire(
    Operation operation, uint64_t request_id,
    std::chrono::milliseconds timeout,
    const std::array<SubsystemQuiescenceParticipant*,
                     kRequiredQuiescentSubsystemCount>& participants,
    HeldSubsystemQuiescence* output) const {
  if (!output || *output || request_id == 0 ||
      timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumSubsystemQuiescenceWait) {
    return Failure(SubsystemQuiescenceResult::kInvalidConfiguration, SIZE_MAX,
                   QuiescentSubsystem::kKernelDispatchAndTimers,
                   "Subsystem quiescence request is invalid.");
  }
  for (size_t index = 0; index < participants.size(); ++index) {
    if (!participants[index] ||
        participants[index]->subsystem() !=
            kSubsystemQuiescenceOrder[index] ||
        std::find(participants.begin(), participants.begin() + index,
                  participants[index]) != participants.begin() + index) {
      return Failure(SubsystemQuiescenceResult::kInvalidConfiguration, index,
                     kSubsystemQuiescenceOrder[index],
                     "Subsystem quiescence participants are missing, "
                     "duplicated, or out of dependency order.");
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const SubsystemQuiescenceRequest request = {operation, request_id, deadline};
  size_t prepared_count = 0;
  for (size_t index = 0; index < participants.size(); ++index) {
    if (std::chrono::steady_clock::now() >= deadline) {
      AbortPrepared(participants, prepared_count, request_id);
      return Failure(SubsystemQuiescenceResult::kTimedOut, index,
                     kSubsystemQuiescenceOrder[index],
                     "Subsystem quiescence deadline expired.");
    }

    std::string error_message;
    const SubsystemQuiescenceResult result =
        participants[index]->Prepare(request, &error_message);
    if (result != SubsystemQuiescenceResult::kReached) {
      AbortPrepared(participants, prepared_count, request_id);
      return Failure(result, index, kSubsystemQuiescenceOrder[index],
                     std::move(error_message));
    }
    ++prepared_count;

    // A participant must honor the shared deadline. If it incorrectly reports
    // success after expiry, include it in reverse abort and fail closed.
    if (std::chrono::steady_clock::now() > deadline) {
      AbortPrepared(participants, prepared_count, request_id);
      return Failure(SubsystemQuiescenceResult::kTimedOut, index,
                     kSubsystemQuiescenceOrder[index],
                     "Subsystem reported quiescence after its deadline.");
    }
  }

  HeldSubsystemQuiescence built;
  built.request_id_ = request_id;
  built.participants_ = participants;
  built.prepared_count_ = prepared_count;
  *output = std::move(built);
  return {SubsystemQuiescenceResult::kReached, SIZE_MAX,
          QuiescentSubsystem::kInput, {}};
}

}  // namespace xe::save_state
