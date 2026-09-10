/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_SAVE_STATE_KERNEL_ASYNC_H_
#define XENIA_BASE_SAVE_STATE_KERNEL_ASYNC_H_

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "xenia/base/reversible_admission_gate.h"

namespace xe::save_state {

enum class KernelAsyncDomain {
  kApc,
  kDpc,
  kNotification,
  kWait,
  kTimer,
};

constexpr size_t kKernelAsyncDomainCount = 5;
static_assert(static_cast<size_t>(KernelAsyncDomain::kTimer) + 1 ==
              kKernelAsyncDomainCount);

enum class KernelAsyncCloseResult {
  kReached,
  kBusy,
  kTimedOut,
  kUnsupportedPendingState,
  kAccountingError,
  kInvalidRequest,
};

enum class KernelPendingPolicy {
  kAllowStablePending,
  kRequireEmpty,
};

struct KernelAsyncSnapshot {
  AdmissionGateSnapshot admission;
  size_t pending_items = 0;
  bool accounting_error = false;
};

class KernelAsyncAdmissionBoundary {
 public:
  class Operation {
   public:
    Operation() = default;
    ~Operation() = default;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    Operation(Operation&&) noexcept = default;
    Operation& operator=(Operation&&) noexcept = default;

    explicit operator bool() const { return bool(admission_); }

    bool RecordEnqueued(size_t count = 1) noexcept;
    bool RecordDequeued(size_t count = 1) noexcept;
    void Reset() noexcept;

   private:
    friend class KernelAsyncAdmissionBoundary;

    Operation(KernelAsyncAdmissionBoundary* boundary, KernelAsyncDomain domain,
              ReversibleAdmissionGate::Lease admission)
        : boundary_(boundary),
          domain_(domain),
          admission_(std::move(admission)) {}

    KernelAsyncAdmissionBoundary* boundary_ = nullptr;
    KernelAsyncDomain domain_ = KernelAsyncDomain::kApc;
    ReversibleAdmissionGate::Lease admission_;
  };

  Operation Enter(KernelAsyncDomain domain);

  KernelAsyncCloseResult CloseAndWait(
      KernelAsyncDomain domain, uint64_t owner_id,
      std::chrono::steady_clock::time_point deadline,
      KernelPendingPolicy pending_policy);
  bool Reopen(KernelAsyncDomain domain, uint64_t owner_id) noexcept;
  KernelAsyncSnapshot Snapshot(KernelAsyncDomain domain) const;

 private:
  static constexpr size_t DomainIndex(KernelAsyncDomain domain) {
    return static_cast<size_t>(domain);
  }

  bool RecordEnqueued(KernelAsyncDomain domain, size_t count) noexcept;
  bool RecordDequeued(KernelAsyncDomain domain, size_t count) noexcept;

  std::array<ReversibleAdmissionGate, kKernelAsyncDomainCount> gates_;
  std::array<std::atomic<size_t>, kKernelAsyncDomainCount> pending_items_ = {};
  std::array<std::atomic<bool>, kKernelAsyncDomainCount> accounting_errors_ =
      {};
};

enum class KernelWaitObjectKind {
  kEvent,
  kMutant,
  kNotifyListener,
  kSemaphore,
  kThread,
  kTimer,
  kIoCompletion,
  kUnknown,
};

struct KernelWaitInventoryResult {
  bool supported = false;
  size_t object_index = SIZE_MAX;
  KernelWaitObjectKind object_kind = KernelWaitObjectKind::kUnknown;
};

// This only validates that a waitable object has a central, inspectable host
// representation. It does not claim that the object's state is serialized.
KernelWaitInventoryResult ValidateKernelWaitObjectInventory(
    const KernelWaitObjectKind* object_kinds, size_t object_count);

}  // namespace xe::save_state

#endif  // XENIA_BASE_SAVE_STATE_KERNEL_ASYNC_H_
