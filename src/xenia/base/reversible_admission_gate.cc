/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/reversible_admission_gate.h"

namespace xe {

ReversibleAdmissionGate::Lease::~Lease() { Reset(); }

ReversibleAdmissionGate::Lease::Lease(Lease&& other) noexcept
    : gate_(other.gate_) {
  other.gate_ = nullptr;
}

ReversibleAdmissionGate::Lease& ReversibleAdmissionGate::Lease::operator=(
    Lease&& other) noexcept {
  if (this != &other) {
    Reset();
    gate_ = other.gate_;
    other.gate_ = nullptr;
  }
  return *this;
}

void ReversibleAdmissionGate::Lease::Reset() noexcept {
  if (gate_) {
    ReversibleAdmissionGate* gate = gate_;
    gate_ = nullptr;
    gate->Leave();
  }
}

ReversibleAdmissionGate::Lease ReversibleAdmissionGate::Enter() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this]() { return !closed_; });
  ++in_flight_;
  return Lease(this);
}

AdmissionGateResult ReversibleAdmissionGate::CloseAndWait(
    uint64_t owner_id, std::chrono::steady_clock::time_point deadline) {
  if (owner_id == 0) {
    return AdmissionGateResult::kInvalidRequest;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  if (closed_) {
    return AdmissionGateResult::kBusy;
  }
  if (std::chrono::steady_clock::now() >= deadline) {
    return AdmissionGateResult::kTimedOut;
  }

  closed_ = true;
  owner_id_ = owner_id;
  if (!condition_.wait_until(lock, deadline,
                             [this]() { return in_flight_ == 0; })) {
    closed_ = false;
    owner_id_ = 0;
    lock.unlock();
    condition_.notify_all();
    return AdmissionGateResult::kTimedOut;
  }
  return AdmissionGateResult::kReached;
}

bool ReversibleAdmissionGate::Reopen(uint64_t owner_id) noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!closed_ || owner_id == 0 || owner_id != owner_id_) {
    return false;
  }
  closed_ = false;
  owner_id_ = 0;
  lock.unlock();
  condition_.notify_all();
  return true;
}

AdmissionGateSnapshot ReversibleAdmissionGate::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return {closed_, owner_id_, in_flight_};
}

void ReversibleAdmissionGate::Leave() noexcept {
  std::unique_lock<std::mutex> lock(mutex_);
  if (in_flight_ == 0) {
    return;
  }
  --in_flight_;
  const bool drained = in_flight_ == 0;
  lock.unlock();
  if (drained) {
    condition_.notify_all();
  }
}

}  // namespace xe
