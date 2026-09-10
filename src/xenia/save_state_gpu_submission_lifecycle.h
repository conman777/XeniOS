/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_GPU_SUBMISSION_LIFECYCLE_H_
#define XENIA_SAVE_STATE_GPU_SUBMISSION_LIFECYCLE_H_

#include <chrono>
#include <cstdint>
#include <string>

namespace xe::save_state {

constexpr std::chrono::milliseconds kMaximumGpuSubmissionLifecycleWait{5000};

enum class GpuSubmissionLifecycleResult {
  kPrepared,
  kCompleted,
  kPending,
  kBusy,
  kTimedOut,
  kUnsupported,
  kBackendFailure,
  kInvalidRequest,
};

enum class GpuSubmissionCommitResult {
  kSubmitted,
  kFailedBeforeMutation,
  kIndeterminateAfterMutation,
};

enum class GpuSubmissionPollResult {
  kPending,
  kCompleted,
  kBackendFailure,
};

struct GpuSubmissionLifecycleRequest {
  uint64_t owner_id = 0;
  std::chrono::steady_clock::time_point deadline;
};

// Read-only state captured on the command-processor worker after draw/command
// ingress has closed. Capability booleans are proof obligations, not hints.
// A live adapter must return false unless the corresponding behavior is
// actually enforced for every path.
struct GpuSubmissionLifecycleSnapshot {
  bool draw_ingress_closed = false;
  bool on_parked_worker_control_lane = false;
  bool independent_submission_producers_closed = false;
  bool close_submit_failure_atomic = false;
  bool completion_poll_nonblocking = false;
  bool device_lost = false;
  bool submission_open = false;
  bool active_command_buffer_valid = false;
  bool close_resources_preallocated = false;
  bool sparse_bind_pending = false;
  bool queue_wait_semaphores_pending = false;
  uint64_t current_submission = 0;
  uint64_t completed_submission = 0;
};

// Independent control contract for a future Vulkan adapter.
//
// PrepareAndInspect may establish only reversible logical ownership and return
// a read-only snapshot. It must not end render passes, flush subsystem state,
// record command buffers, bind sparse memory, submit queue work, or wait.
//
// CloseAndSubmit may be called only if the snapshot explicitly proves failure
// atomicity. It must not wait and must honor request.deadline.
// kFailedBeforeMutation guarantees that no Vulkan or semantic state changed.
// Once mutation may have happened, the only honest failure is
// kIndeterminateAfterMutation, which permanently follows the fail-closed path.
//
// PollCompletion must be non-blocking. AbortPreparedAndReopen is used only
// before mutation. RetainFailClosed must keep all admission closed.
class GpuSubmissionLifecycleBackend {
 public:
  virtual ~GpuSubmissionLifecycleBackend() = default;

  virtual GpuSubmissionLifecycleResult PrepareAndInspect(
      const GpuSubmissionLifecycleRequest& request,
      GpuSubmissionLifecycleSnapshot* snapshot,
      std::string* error_message) = 0;
  virtual bool AbortPreparedAndReopen(uint64_t owner_id) noexcept = 0;
  virtual GpuSubmissionCommitResult CloseAndSubmit(
      const GpuSubmissionLifecycleRequest& request,
      const GpuSubmissionLifecycleSnapshot& expected_snapshot,
      uint64_t* submitted_submission, std::string* error_message) = 0;
  virtual GpuSubmissionPollResult PollCompletion(
      uint64_t owner_id, uint64_t target_submission,
      uint64_t* completed_submission, std::string* error_message) = 0;
  virtual bool ReleaseCompletedAndReopen(uint64_t owner_id) noexcept = 0;
  virtual void RetainFailClosed(uint64_t owner_id) noexcept = 0;
};

enum class HeldGpuSubmissionLifecycleState {
  kEmpty,
  kPrepared,
  kSubmittedPending,
  kCompleted,
  kIndeterminate,
};

struct GpuSubmissionLifecycleOperationResult {
  GpuSubmissionLifecycleResult result =
      GpuSubmissionLifecycleResult::kInvalidRequest;
  std::string message;

  bool ok() const { return result == GpuSubmissionLifecycleResult::kCompleted; }
};

class HeldGpuSubmissionLifecycle {
 public:
  HeldGpuSubmissionLifecycle() = default;
  ~HeldGpuSubmissionLifecycle();
  HeldGpuSubmissionLifecycle(const HeldGpuSubmissionLifecycle&) = delete;
  HeldGpuSubmissionLifecycle& operator=(const HeldGpuSubmissionLifecycle&) =
      delete;
  HeldGpuSubmissionLifecycle(HeldGpuSubmissionLifecycle&& other) noexcept;
  HeldGpuSubmissionLifecycle& operator=(
      HeldGpuSubmissionLifecycle&& other) noexcept;

  explicit operator bool() const {
    return state_ != HeldGpuSubmissionLifecycleState::kEmpty;
  }
  HeldGpuSubmissionLifecycleState state() const { return state_; }
  uint64_t owner_id() const { return owner_id_; }
  uint64_t target_submission() const { return target_submission_; }
  const GpuSubmissionLifecycleSnapshot& snapshot() const { return snapshot_; }

  GpuSubmissionLifecycleOperationResult CommitAndWait();
  GpuSubmissionLifecycleOperationResult WaitUntil(
      std::chrono::steady_clock::time_point deadline);
  bool ReleaseCompleted() noexcept;
  void Reset() noexcept;

 private:
  friend class GpuSubmissionLifecycleCoordinator;

  void MoveFrom(HeldGpuSubmissionLifecycle&& other) noexcept;

  GpuSubmissionLifecycleBackend* backend_ = nullptr;
  GpuSubmissionLifecycleRequest request_;
  GpuSubmissionLifecycleSnapshot snapshot_;
  HeldGpuSubmissionLifecycleState state_ =
      HeldGpuSubmissionLifecycleState::kEmpty;
  uint64_t owner_id_ = 0;
  uint64_t target_submission_ = 0;
};

class GpuSubmissionLifecycleCoordinator {
 public:
  GpuSubmissionLifecycleOperationResult Prepare(
      const GpuSubmissionLifecycleRequest& request,
      GpuSubmissionLifecycleBackend* backend,
      HeldGpuSubmissionLifecycle* output);
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_GPU_SUBMISSION_LIFECYCLE_H_
