/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_REVERSIBLE_ADMISSION_GATE_H_
#define XENIA_BASE_REVERSIBLE_ADMISSION_GATE_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace xe {

enum class AdmissionGateResult {
  kReached,
  kBusy,
  kTimedOut,
  kInvalidRequest,
};

struct AdmissionGateSnapshot {
  bool closed = false;
  uint64_t owner_id = 0;
  size_t in_flight = 0;
};

// Reversible admission barrier for host operations that may mutate one
// subsystem. Operations admitted before CloseAndWait retain a move-only lease
// and are allowed to finish. Later operations wait until Reopen. A timed-out
// close reopens automatically and leaves normal execution unchanged.
class ReversibleAdmissionGate {
 public:
  class Lease {
   public:
    Lease() = default;
    ~Lease();
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept;

    explicit operator bool() const { return gate_ != nullptr; }
    void Reset() noexcept;

   private:
    friend class ReversibleAdmissionGate;
    explicit Lease(ReversibleAdmissionGate* gate) : gate_(gate) {}

    ReversibleAdmissionGate* gate_ = nullptr;
  };

  ReversibleAdmissionGate() = default;
  ReversibleAdmissionGate(const ReversibleAdmissionGate&) = delete;
  ReversibleAdmissionGate& operator=(const ReversibleAdmissionGate&) = delete;

  Lease Enter();
  AdmissionGateResult CloseAndWait(
      uint64_t owner_id, std::chrono::steady_clock::time_point deadline);
  bool Reopen(uint64_t owner_id) noexcept;
  AdmissionGateSnapshot Snapshot() const;

 private:
  void Leave() noexcept;

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool closed_ = false;
  uint64_t owner_id_ = 0;
  size_t in_flight_ = 0;
};

}  // namespace xe

#endif  // XENIA_BASE_REVERSIBLE_ADMISSION_GATE_H_
