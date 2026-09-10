/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_gpu_quiescence.h"

#include <utility>

namespace xe::save_state {
namespace {

GpuQuiescenceAcquireResult AdmissionFailure(AdmissionGateResult result) {
  switch (result) {
    case AdmissionGateResult::kBusy:
      return {GpuQuiescenceResult::kBusy,
              "GPU producer admission is owned by another request."};
    case AdmissionGateResult::kTimedOut:
      return {GpuQuiescenceResult::kTimedOut,
              "GPU producer admission did not drain before the deadline."};
    case AdmissionGateResult::kInvalidRequest:
      return {GpuQuiescenceResult::kInvalidRequest,
              "GPU producer admission rejected the request identity."};
    case AdmissionGateResult::kReached:
      break;
  }
  return {GpuQuiescenceResult::kBackendFailure,
          "GPU producer admission returned an unknown result."};
}

}  // namespace

GpuCommandAdmissionCloseResult GpuCommandAdmissionLanes::CloseAndWait(
    uint64_t owner_id,
    std::chrono::steady_clock::time_point deadline) {
  std::unique_lock<std::mutex> ownership_lock(ownership_mutex_,
                                               std::try_to_lock);
  if (!ownership_lock.owns_lock()) {
    return {AdmissionGateResult::kBusy, GpuCommandAdmissionLane::kNone};
  }
  return CloseAndWaitLocked(owner_id, deadline);
}

GpuCommandAdmissionCloseResult
GpuCommandAdmissionLanes::CloseAndWaitLocked(
    uint64_t owner_id,
    std::chrono::steady_clock::time_point deadline) {
  const AdmissionGateResult pm4_result =
      pm4_producer_.CloseAndWait(owner_id, deadline);
  if (pm4_result != AdmissionGateResult::kReached) {
    return {pm4_result, GpuCommandAdmissionLane::kPm4Producer};
  }

  const AdmissionGateResult callback_result =
      pending_callback_.CloseAndWait(owner_id, deadline);
  if (callback_result != AdmissionGateResult::kReached) {
    pm4_producer_.Reopen(owner_id);
    return {callback_result, GpuCommandAdmissionLane::kPendingCallback};
  }
  return {AdmissionGateResult::kReached, GpuCommandAdmissionLane::kNone};
}

GpuCommandWorkerParkAcquireResult
GpuCommandAdmissionLanes::ParkWorkerAndWait(
    uint64_t owner_id,
    std::chrono::steady_clock::time_point deadline,
    const std::function<void()>& wake_worker) {
  if (!wake_worker) {
    return {GpuCommandWorkerParkResult::kInvalidRequest,
            GpuCommandAdmissionLane::kNone};
  }
  std::unique_lock<std::mutex> ownership_lock(ownership_mutex_,
                                               std::try_to_lock);
  if (!ownership_lock.owns_lock()) {
    return {GpuCommandWorkerParkResult::kBusy,
            GpuCommandAdmissionLane::kNone};
  }
  const GpuCommandAdmissionCloseResult close_result =
      CloseAndWaitLocked(owner_id, deadline);
  if (!close_result.ok()) {
    GpuCommandWorkerParkResult result =
        GpuCommandWorkerParkResult::kInvalidRequest;
    switch (close_result.result) {
      case AdmissionGateResult::kReached:
        break;
      case AdmissionGateResult::kBusy:
        result = GpuCommandWorkerParkResult::kBusy;
        break;
      case AdmissionGateResult::kTimedOut:
        result = GpuCommandWorkerParkResult::kTimedOut;
        break;
      case AdmissionGateResult::kInvalidRequest:
        result = GpuCommandWorkerParkResult::kInvalidRequest;
        break;
    }
    return {result, close_result.lane};
  }

  std::unique_lock<std::mutex> worker_lock(worker_park_mutex_);
  if (worker_park_requested_) {
    worker_lock.unlock();
    ReopenLocked(owner_id);
    return {GpuCommandWorkerParkResult::kBusy,
            GpuCommandAdmissionLane::kNone};
  }
  worker_park_owner_id_ = owner_id;
  worker_park_requested_ = true;
  worker_parked_ = false;
  worker_lock.unlock();

  wake_worker();

  worker_lock.lock();
  const bool acknowledged = worker_park_condition_.wait_until(
      worker_lock, deadline,
      [this]() { return worker_parked_ || !worker_park_requested_; });
  const bool reached_before_deadline =
      acknowledged && worker_parked_ &&
      std::chrono::steady_clock::now() <= deadline;
  if (!reached_before_deadline) {
    worker_park_requested_ = false;
    worker_park_owner_id_ = 0;
    worker_park_condition_.notify_all();
    worker_lock.unlock();
    ReopenLocked(owner_id);
    return {acknowledged ? GpuCommandWorkerParkResult::kBusy
                         : GpuCommandWorkerParkResult::kTimedOut,
            GpuCommandAdmissionLane::kNone};
  }
  return {GpuCommandWorkerParkResult::kParked,
          GpuCommandAdmissionLane::kNone};
}

bool GpuCommandAdmissionLanes::Reopen(uint64_t owner_id) noexcept {
  std::lock_guard<std::mutex> ownership_lock(ownership_mutex_);
  return ReopenLocked(owner_id);
}

bool GpuCommandAdmissionLanes::ReopenLocked(uint64_t owner_id) noexcept {
  const GpuCommandAdmissionSnapshot snapshot = Snapshot();
  if (owner_id == 0 || !snapshot.pm4_producer.closed ||
      !snapshot.pending_callback.closed ||
      snapshot.pm4_producer.owner_id != owner_id ||
      snapshot.pending_callback.owner_id != owner_id ||
      snapshot.worker_owner_id != 0 || snapshot.worker_park_requested ||
      snapshot.worker_parked) {
    return false;
  }
  const bool callback_reopened = pending_callback_.Reopen(owner_id);
  const bool pm4_reopened = pm4_producer_.Reopen(owner_id);
  return callback_reopened && pm4_reopened;
}

bool GpuCommandAdmissionLanes::ReleaseWorkerAndReopen(
    uint64_t owner_id,
    std::chrono::steady_clock::time_point deadline) noexcept {
  if (owner_id == 0 || deadline <= std::chrono::steady_clock::now()) {
    return false;
  }
  std::lock_guard<std::mutex> ownership_lock(ownership_mutex_);
  const GpuCommandAdmissionSnapshot snapshot = Snapshot();
  if (owner_id == 0 || !snapshot.pm4_producer.closed ||
      !snapshot.pending_callback.closed ||
      snapshot.pm4_producer.owner_id != owner_id ||
      snapshot.pending_callback.owner_id != owner_id ||
      snapshot.worker_owner_id != owner_id ||
      (snapshot.worker_park_requested && !snapshot.worker_parked)) {
    return false;
  }

  std::unique_lock<std::mutex> worker_lock(worker_park_mutex_);
  if (worker_park_owner_id_ != owner_id ||
      (worker_park_requested_ && !worker_parked_)) {
    return false;
  }
  if (worker_park_requested_) {
    worker_park_requested_ = false;
    worker_park_condition_.notify_all();
  }
  if (!worker_park_condition_.wait_until(
          worker_lock, deadline,
          [this]() { return !worker_parked_; })) {
    // Keep both ingress lanes closed and preserve the owner so release can be
    // retried. Reopening before the worker confirms exit would race new work.
    return false;
  }
  if (worker_park_owner_id_ != owner_id) {
    return false;
  }
  worker_park_owner_id_ = 0;
  worker_lock.unlock();
  return ReopenLocked(owner_id);
}

bool GpuCommandAdmissionLanes::PollWorkerPark() {
  std::unique_lock<std::mutex> worker_lock(worker_park_mutex_);
  if (!worker_park_requested_) {
    return false;
  }
  worker_parked_ = true;
  worker_park_condition_.notify_all();
  worker_park_condition_.wait(
      worker_lock, [this]() { return !worker_park_requested_; });
  worker_parked_ = false;
  worker_park_condition_.notify_all();
  return true;
}

bool GpuCommandAdmissionLanes::WorkerParkRequested() const {
  std::lock_guard<std::mutex> worker_lock(worker_park_mutex_);
  return worker_park_requested_;
}

void GpuCommandAdmissionLanes::CancelWorkerParkForShutdown() noexcept {
  std::lock_guard<std::mutex> worker_lock(worker_park_mutex_);
  worker_park_requested_ = false;
  worker_park_owner_id_ = 0;
  worker_park_condition_.notify_all();
}

GpuCommandAdmissionSnapshot GpuCommandAdmissionLanes::Snapshot() const {
  GpuCommandAdmissionSnapshot snapshot = {
      pm4_producer_.Snapshot(), pending_callback_.Snapshot()};
  std::lock_guard<std::mutex> worker_lock(worker_park_mutex_);
  snapshot.worker_owner_id = worker_park_owner_id_;
  snapshot.worker_park_requested = worker_park_requested_;
  snapshot.worker_parked = worker_parked_;
  return snapshot;
}

HeldGpuQuiescence::~HeldGpuQuiescence() { Release(); }

HeldGpuQuiescence::HeldGpuQuiescence(HeldGpuQuiescence&& other) noexcept
    : boundary_(other.boundary_),
      backend_(other.backend_),
      request_id_(other.request_id_) {
  other.boundary_ = nullptr;
  other.backend_ = nullptr;
  other.request_id_ = 0;
}

HeldGpuQuiescence& HeldGpuQuiescence::operator=(
    HeldGpuQuiescence&& other) noexcept {
  if (this != &other) {
    Release();
    boundary_ = other.boundary_;
    backend_ = other.backend_;
    request_id_ = other.request_id_;
    other.boundary_ = nullptr;
    other.backend_ = nullptr;
    other.request_id_ = 0;
  }
  return *this;
}

void HeldGpuQuiescence::Release() noexcept {
  GpuQuiescenceBoundary* boundary = boundary_;
  GpuQuiescenceBackend* backend = backend_;
  const uint64_t request_id = request_id_;
  boundary_ = nullptr;
  backend_ = nullptr;
  request_id_ = 0;
  if (!boundary || !backend || request_id == 0) {
    return;
  }
  backend->Abort(request_id);
  boundary->producer_admission_.Reopen(request_id);
}

GpuQuiescenceAcquireResult GpuQuiescenceBoundary::Acquire(
    uint64_t request_id, std::chrono::milliseconds timeout,
    GpuQuiescenceBackend* backend, HeldGpuQuiescence* output) {
  if (request_id == 0 || timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumGpuQuiescenceWait || !backend || !output || *output) {
    return {GpuQuiescenceResult::kInvalidRequest,
            "GPU quiescence request is invalid or its output is occupied."};
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const AdmissionGateResult admission_result =
      producer_admission_.CloseAndWait(request_id, deadline);
  if (admission_result != AdmissionGateResult::kReached) {
    return AdmissionFailure(admission_result);
  }

  const GpuQuiescenceRequest request{request_id, deadline};
  std::string error_message;
  const GpuQuiescenceResult backend_result =
      backend->PrepareAndDrain(request, &error_message);
  if (backend_result != GpuQuiescenceResult::kReached) {
    backend->Abort(request_id);
    producer_admission_.Reopen(request_id);
    if (error_message.empty()) {
      error_message = "GPU backend did not reach a quiescent boundary.";
    }
    return {backend_result, std::move(error_message)};
  }

  // A backend reporting success after the shared deadline is unsafe. Include
  // it in rollback because it may have established a logical worker hold.
  if (std::chrono::steady_clock::now() > deadline) {
    backend->Abort(request_id);
    producer_admission_.Reopen(request_id);
    return {GpuQuiescenceResult::kTimedOut,
            "GPU backend reported quiescence after its deadline."};
  }

  HeldGpuQuiescence held;
  held.boundary_ = this;
  held.backend_ = backend;
  held.request_id_ = request_id;
  *output = std::move(held);
  return {GpuQuiescenceResult::kReached, {}};
}

}  // namespace xe::save_state
