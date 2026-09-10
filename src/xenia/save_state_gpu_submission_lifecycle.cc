/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_gpu_submission_lifecycle.h"

#include <thread>
#include <utility>

namespace xe::save_state {
namespace {

GpuSubmissionLifecycleOperationResult InvalidSnapshot(
    const char* message) {
  return {GpuSubmissionLifecycleResult::kUnsupported, message};
}

bool DeadlineIsValid(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  return deadline > now &&
         deadline - now <= kMaximumGpuSubmissionLifecycleWait;
}

}  // namespace

HeldGpuSubmissionLifecycle::~HeldGpuSubmissionLifecycle() { Reset(); }

HeldGpuSubmissionLifecycle::HeldGpuSubmissionLifecycle(
    HeldGpuSubmissionLifecycle&& other) noexcept {
  MoveFrom(std::move(other));
}

HeldGpuSubmissionLifecycle& HeldGpuSubmissionLifecycle::operator=(
    HeldGpuSubmissionLifecycle&& other) noexcept {
  if (this != &other) {
    Reset();
    MoveFrom(std::move(other));
  }
  return *this;
}

void HeldGpuSubmissionLifecycle::MoveFrom(
    HeldGpuSubmissionLifecycle&& other) noexcept {
  backend_ = other.backend_;
  request_ = other.request_;
  snapshot_ = other.snapshot_;
  state_ = other.state_;
  owner_id_ = other.owner_id_;
  target_submission_ = other.target_submission_;
  other.backend_ = nullptr;
  other.request_ = {};
  other.snapshot_ = {};
  other.state_ = HeldGpuSubmissionLifecycleState::kEmpty;
  other.owner_id_ = 0;
  other.target_submission_ = 0;
}

GpuSubmissionLifecycleOperationResult
HeldGpuSubmissionLifecycle::CommitAndWait() {
  if (!backend_ || owner_id_ == 0 ||
      state_ == HeldGpuSubmissionLifecycleState::kEmpty ||
      state_ == HeldGpuSubmissionLifecycleState::kIndeterminate) {
    return {GpuSubmissionLifecycleResult::kInvalidRequest,
            "GPU submission lifecycle hold cannot be committed."};
  }
  if (state_ == HeldGpuSubmissionLifecycleState::kCompleted) {
    return {GpuSubmissionLifecycleResult::kCompleted, {}};
  }

  if (state_ == HeldGpuSubmissionLifecycleState::kPrepared) {
    if (std::chrono::steady_clock::now() >= request_.deadline) {
      return {GpuSubmissionLifecycleResult::kTimedOut,
              "GPU submission deadline elapsed before commit; no mutation "
              "was attempted."};
    }
    uint64_t submitted_submission = 0;
    std::string error_message;
    const GpuSubmissionCommitResult commit_result = backend_->CloseAndSubmit(
        request_, snapshot_, &submitted_submission, &error_message);
    if (commit_result == GpuSubmissionCommitResult::kFailedBeforeMutation) {
      if (error_message.empty()) {
        error_message =
            "GPU submission close failed before any mutation.";
      }
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              std::move(error_message)};
    }
    if (commit_result ==
        GpuSubmissionCommitResult::kIndeterminateAfterMutation) {
      state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
      if (error_message.empty()) {
        error_message =
            "GPU submission close may have mutated backend state.";
      }
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              std::move(error_message)};
    }
    if (commit_result != GpuSubmissionCommitResult::kSubmitted) {
      state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "GPU backend returned an unknown commit result."};
    }
    if (submitted_submission != snapshot_.current_submission ||
        submitted_submission == 0) {
      state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "GPU backend returned an invalid committed submission index."};
    }
    target_submission_ = submitted_submission;
    state_ = HeldGpuSubmissionLifecycleState::kSubmittedPending;
  }

  return WaitUntil(request_.deadline);
}

GpuSubmissionLifecycleOperationResult HeldGpuSubmissionLifecycle::WaitUntil(
    std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (!backend_ || owner_id_ == 0 ||
      state_ == HeldGpuSubmissionLifecycleState::kPrepared ||
      state_ == HeldGpuSubmissionLifecycleState::kEmpty ||
      state_ == HeldGpuSubmissionLifecycleState::kIndeterminate ||
      target_submission_ == 0) {
    return {GpuSubmissionLifecycleResult::kInvalidRequest,
            "GPU completion wait request is invalid."};
  }
  if (state_ == HeldGpuSubmissionLifecycleState::kCompleted) {
    return {GpuSubmissionLifecycleResult::kCompleted, {}};
  }
  if (deadline > now &&
      deadline - now > kMaximumGpuSubmissionLifecycleWait) {
    return {GpuSubmissionLifecycleResult::kInvalidRequest,
            "GPU completion wait request is invalid."};
  }
  if (deadline <= now) {
    return {GpuSubmissionLifecycleResult::kTimedOut,
            "GPU submission did not complete before the deadline; "
            "admission remains closed."};
  }

  for (;;) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return {GpuSubmissionLifecycleResult::kTimedOut,
              "GPU submission did not complete before the deadline; "
              "admission remains closed."};
    }
    uint64_t completed_submission = 0;
    std::string error_message;
    const GpuSubmissionPollResult poll_result = backend_->PollCompletion(
        owner_id_, target_submission_, &completed_submission, &error_message);
    if (poll_result == GpuSubmissionPollResult::kCompleted) {
      if (completed_submission < target_submission_) {
        state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
        return {GpuSubmissionLifecycleResult::kBackendFailure,
                "GPU backend reported completion below the target index."};
      }
      state_ = HeldGpuSubmissionLifecycleState::kCompleted;
      return {GpuSubmissionLifecycleResult::kCompleted, {}};
    }
    if (poll_result == GpuSubmissionPollResult::kBackendFailure) {
      state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
      if (error_message.empty()) {
        error_message = "GPU completion polling failed.";
      }
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              std::move(error_message)};
    }
    if (poll_result != GpuSubmissionPollResult::kPending) {
      state_ = HeldGpuSubmissionLifecycleState::kIndeterminate;
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "GPU backend returned an unknown completion poll result."};
    }
    std::this_thread::yield();
  }
}

bool HeldGpuSubmissionLifecycle::ReleaseCompleted() noexcept {
  if (!backend_ || owner_id_ == 0 ||
      state_ != HeldGpuSubmissionLifecycleState::kCompleted) {
    return false;
  }
  if (!backend_->ReleaseCompletedAndReopen(owner_id_)) {
    return false;
  }
  backend_ = nullptr;
  request_ = {};
  snapshot_ = {};
  state_ = HeldGpuSubmissionLifecycleState::kEmpty;
  owner_id_ = 0;
  target_submission_ = 0;
  return true;
}

void HeldGpuSubmissionLifecycle::Reset() noexcept {
  if (backend_ && owner_id_ != 0) {
    if (state_ == HeldGpuSubmissionLifecycleState::kPrepared) {
      if (!backend_->AbortPreparedAndReopen(owner_id_)) {
        backend_->RetainFailClosed(owner_id_);
      }
    } else if (state_ == HeldGpuSubmissionLifecycleState::kCompleted) {
      if (!backend_->ReleaseCompletedAndReopen(owner_id_)) {
        backend_->RetainFailClosed(owner_id_);
      }
    } else if (state_ == HeldGpuSubmissionLifecycleState::kSubmittedPending ||
               state_ == HeldGpuSubmissionLifecycleState::kIndeterminate) {
      backend_->RetainFailClosed(owner_id_);
    }
  }
  backend_ = nullptr;
  request_ = {};
  snapshot_ = {};
  state_ = HeldGpuSubmissionLifecycleState::kEmpty;
  owner_id_ = 0;
  target_submission_ = 0;
}

GpuSubmissionLifecycleOperationResult
GpuSubmissionLifecycleCoordinator::Prepare(
    const GpuSubmissionLifecycleRequest& request,
    GpuSubmissionLifecycleBackend* backend,
    HeldGpuSubmissionLifecycle* output) {
  if (request.owner_id == 0 || !DeadlineIsValid(request.deadline) || !backend ||
      !output || *output) {
    return {GpuSubmissionLifecycleResult::kInvalidRequest,
            "GPU submission lifecycle request is invalid."};
  }

  GpuSubmissionLifecycleSnapshot snapshot;
  std::string error_message;
  const GpuSubmissionLifecycleResult prepare_result =
      backend->PrepareAndInspect(request, &snapshot, &error_message);
  if (prepare_result != GpuSubmissionLifecycleResult::kPrepared) {
    if (!backend->AbortPreparedAndReopen(request.owner_id)) {
      backend->RetainFailClosed(request.owner_id);
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "GPU submission preflight could not reopen admission."};
    }
    if (error_message.empty()) {
      error_message = "GPU submission preflight was not prepared.";
    }
    return {prepare_result, std::move(error_message)};
  }
  if (std::chrono::steady_clock::now() >= request.deadline) {
    if (!backend->AbortPreparedAndReopen(request.owner_id)) {
      backend->RetainFailClosed(request.owner_id);
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "Late GPU preflight could not reopen admission."};
    }
    return {GpuSubmissionLifecycleResult::kTimedOut,
            "GPU submission preflight exceeded its deadline."};
  }

  GpuSubmissionLifecycleOperationResult validation_result;
  if (!snapshot.draw_ingress_closed) {
    validation_result =
        InvalidSnapshot("GPU draw ingress is not closed.");
  } else if (!snapshot.on_parked_worker_control_lane) {
    validation_result =
        InvalidSnapshot("GPU submission preflight is not on its worker lane.");
  } else if (!snapshot.independent_submission_producers_closed) {
    validation_result = InvalidSnapshot(
        "Independent GPU submission producers remain open.");
  } else if (!snapshot.completion_poll_nonblocking) {
    validation_result =
        InvalidSnapshot("GPU completion polling is not bounded.");
  } else if (snapshot.device_lost) {
    validation_result =
        InvalidSnapshot("The Vulkan device is already lost.");
  } else if (snapshot.current_submission == 0 ||
             snapshot.completed_submission >= snapshot.current_submission) {
    validation_result =
        InvalidSnapshot("GPU submission indices are inconsistent.");
  } else if (snapshot.sparse_bind_pending) {
    validation_result = InvalidSnapshot(
        "Pending sparse binds cannot be committed failure-atomically.");
  } else if (snapshot.queue_wait_semaphores_pending) {
    validation_result = InvalidSnapshot(
        "GPU submission has wait semaphores from a prior queue mutation.");
  } else if (snapshot.submission_open &&
             (!snapshot.active_command_buffer_valid ||
              !snapshot.close_resources_preallocated ||
              !snapshot.close_submit_failure_atomic)) {
    validation_result = InvalidSnapshot(
        "Open GPU submission cannot be closed failure-atomically.");
  }
  if (validation_result.result ==
      GpuSubmissionLifecycleResult::kUnsupported) {
    if (!backend->AbortPreparedAndReopen(request.owner_id)) {
      backend->RetainFailClosed(request.owner_id);
      return {GpuSubmissionLifecycleResult::kBackendFailure,
              "Rejected GPU preflight could not reopen admission."};
    }
    return validation_result;
  }

  HeldGpuSubmissionLifecycle held;
  held.backend_ = backend;
  held.request_ = request;
  held.snapshot_ = snapshot;
  held.owner_id_ = request.owner_id;
  if (snapshot.submission_open) {
    held.state_ = HeldGpuSubmissionLifecycleState::kPrepared;
  } else {
    held.target_submission_ = snapshot.current_submission - 1;
    held.state_ =
        snapshot.completed_submission >= held.target_submission_
            ? HeldGpuSubmissionLifecycleState::kCompleted
            : HeldGpuSubmissionLifecycleState::kSubmittedPending;
  }
  *output = std::move(held);
  return {GpuSubmissionLifecycleResult::kPrepared, {}};
}

}  // namespace xe::save_state
