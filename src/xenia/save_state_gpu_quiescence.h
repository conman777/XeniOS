/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_GPU_QUIESCENCE_H_
#define XENIA_SAVE_STATE_GPU_QUIESCENCE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "xenia/base/reversible_admission_gate.h"

namespace xe::save_state {

constexpr std::chrono::milliseconds kMaximumGpuQuiescenceWait{5000};

enum class GpuQuiescenceResult {
  kReached,
  kBusy,
  kTimedOut,
  kUnsupported,
  kBackendFailure,
  kInvalidRequest,
};

struct GpuQuiescenceRequest {
  uint64_t request_id = 0;
  std::chrono::steady_clock::time_point deadline;
};

enum class GpuCommandAdmissionLane {
  kNone,
  kPm4Producer,
  kPendingCallback,
};

struct GpuCommandAdmissionCloseResult {
  AdmissionGateResult result = AdmissionGateResult::kInvalidRequest;
  GpuCommandAdmissionLane lane = GpuCommandAdmissionLane::kNone;

  bool ok() const { return result == AdmissionGateResult::kReached; }
};

struct GpuCommandAdmissionSnapshot {
  AdmissionGateSnapshot pm4_producer;
  AdmissionGateSnapshot pending_callback;
  uint64_t worker_owner_id = 0;
  bool worker_park_requested = false;
  bool worker_parked = false;
};

enum class GpuCommandWorkerParkResult {
  kParked,
  kBusy,
  kTimedOut,
  kInvalidRequest,
};

struct GpuCommandWorkerParkAcquireResult {
  GpuCommandWorkerParkResult result =
      GpuCommandWorkerParkResult::kInvalidRequest;
  GpuCommandAdmissionLane failure_lane =
      GpuCommandAdmissionLane::kNone;

  bool ok() const { return result == GpuCommandWorkerParkResult::kParked; }
};

// Actual CommandProcessor ingress foundation. PM4 producers retain their lease
// until the worker reaches the published write pointer. Pending callbacks
// retain theirs from enqueue through execution.
//
// CloseAndWait alone does not park the worker. ParkWorkerAndWait additionally
// parks it at the top of its CPU work loop, but still does not prove Vulkan,
// readback, or presenter completion. This remains separate from the
// unregistered GpuQuiescenceBoundary backend contract.
class GpuCommandAdmissionLanes {
 public:
  ReversibleAdmissionGate::Lease EnterPm4Producer() {
    return pm4_producer_.Enter();
  }
  ReversibleAdmissionGate::Lease EnterPendingCallback() {
    return pending_callback_.Enter();
  }

  GpuCommandAdmissionCloseResult CloseAndWait(
      uint64_t owner_id,
      std::chrono::steady_clock::time_point deadline);
  bool Reopen(uint64_t owner_id) noexcept;
  GpuCommandWorkerParkAcquireResult ParkWorkerAndWait(
      uint64_t owner_id,
      std::chrono::steady_clock::time_point deadline,
      // Must be a non-blocking, out-of-band wake. It must not enter either
      // admission lane or mutate command/backend state.
      const std::function<void()>& wake_worker);
  bool ReleaseWorkerAndReopen(
      uint64_t owner_id,
      std::chrono::steady_clock::time_point deadline) noexcept;

  // Called only by the command-processor worker at the top of its work loop.
  // The worker remains parked until the matching owner releases it; the
  // deadline applies to the requesting thread waiting for acknowledgement.
  bool PollWorkerPark();
  bool WorkerParkRequested() const;
  void CancelWorkerParkForShutdown() noexcept;

  GpuCommandAdmissionSnapshot Snapshot() const;

 private:
  GpuCommandAdmissionCloseResult CloseAndWaitLocked(
      uint64_t owner_id,
      std::chrono::steady_clock::time_point deadline);
  bool ReopenLocked(uint64_t owner_id) noexcept;

  std::mutex ownership_mutex_;
  ReversibleAdmissionGate pm4_producer_;
  ReversibleAdmissionGate pending_callback_;
  mutable std::mutex worker_park_mutex_;
  std::condition_variable worker_park_condition_;
  uint64_t worker_park_owner_id_ = 0;
  bool worker_park_requested_ = false;
  bool worker_parked_ = false;
};

// Future live implementations own command-processor dispatch and every
// backend-specific producer not covered by GpuQuiescenceBoundary admission.
// PrepareAndDrain must not return kReached until its worker is parked at a
// reversible safe boundary and all relevant GPU and presenter submissions are
// complete. It must honor request.deadline and must not use unbounded waits.
//
// Abort is an idempotent, non-blocking control-lane operation. It may be called
// after any PrepareAndDrain attempt, including a failed or late one, and must
// only release logical holds established for that request. It must not submit
// new GPU work or enter the producer admission gate.
class GpuQuiescenceBackend {
 public:
  virtual ~GpuQuiescenceBackend() = default;

  virtual GpuQuiescenceResult PrepareAndDrain(
      const GpuQuiescenceRequest& request,
      std::string* error_message) = 0;
  virtual void Abort(uint64_t request_id) noexcept = 0;
};

struct GpuQuiescenceAcquireResult {
  GpuQuiescenceResult result = GpuQuiescenceResult::kInvalidRequest;
  std::string message;

  bool ok() const { return result == GpuQuiescenceResult::kReached; }
};

class GpuQuiescenceBoundary;

// Sole owner of a successfully drained GPU boundary. Destruction releases the
// backend hold before reopening producer admission.
class HeldGpuQuiescence {
 public:
  HeldGpuQuiescence() = default;
  ~HeldGpuQuiescence();
  HeldGpuQuiescence(const HeldGpuQuiescence&) = delete;
  HeldGpuQuiescence& operator=(const HeldGpuQuiescence&) = delete;
  HeldGpuQuiescence(HeldGpuQuiescence&& other) noexcept;
  HeldGpuQuiescence& operator=(HeldGpuQuiescence&& other) noexcept;

  explicit operator bool() const { return boundary_ != nullptr; }
  uint64_t request_id() const { return request_id_; }

  void Release() noexcept;

 private:
  friend class GpuQuiescenceBoundary;

  GpuQuiescenceBoundary* boundary_ = nullptr;
  GpuQuiescenceBackend* backend_ = nullptr;
  uint64_t request_id_ = 0;
};

// Reversible admission/drain foundation for a future Vulkan participant.
// Every live producer integrated with this boundary must retain the returned
// lease until it can no longer enqueue command-processor, Vulkan, readback, or
// presentation work. The live emulator is intentionally not wired yet.
class GpuQuiescenceBoundary {
 public:
  ReversibleAdmissionGate::Lease EnterProducer() {
    return producer_admission_.Enter();
  }

  GpuQuiescenceAcquireResult Acquire(
      uint64_t request_id, std::chrono::milliseconds timeout,
      GpuQuiescenceBackend* backend, HeldGpuQuiescence* output);

  AdmissionGateSnapshot Snapshot() const {
    return producer_admission_.Snapshot();
  }

 private:
  friend class HeldGpuQuiescence;

  ReversibleAdmissionGate producer_admission_;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_GPU_QUIESCENCE_H_
