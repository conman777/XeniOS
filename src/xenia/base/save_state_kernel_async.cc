/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/save_state_kernel_async.h"

#include <limits>
#include <utility>

namespace xe::save_state {
namespace {

KernelAsyncCloseResult ConvertAdmissionResult(AdmissionGateResult result) {
  switch (result) {
    case AdmissionGateResult::kReached:
      return KernelAsyncCloseResult::kReached;
    case AdmissionGateResult::kBusy:
      return KernelAsyncCloseResult::kBusy;
    case AdmissionGateResult::kTimedOut:
      return KernelAsyncCloseResult::kTimedOut;
    case AdmissionGateResult::kInvalidRequest:
      return KernelAsyncCloseResult::kInvalidRequest;
  }
  return KernelAsyncCloseResult::kAccountingError;
}

bool IsKnownWaitObject(KernelWaitObjectKind object_kind) {
  switch (object_kind) {
    case KernelWaitObjectKind::kEvent:
    case KernelWaitObjectKind::kMutant:
    case KernelWaitObjectKind::kNotifyListener:
    case KernelWaitObjectKind::kSemaphore:
    case KernelWaitObjectKind::kThread:
      return true;
    case KernelWaitObjectKind::kTimer:
    case KernelWaitObjectKind::kIoCompletion:
    case KernelWaitObjectKind::kUnknown:
      return false;
  }
  return false;
}

}  // namespace

bool KernelAsyncAdmissionBoundary::Operation::RecordEnqueued(
    size_t count) noexcept {
  return boundary_ && admission_ &&
         boundary_->RecordEnqueued(domain_, count);
}

bool KernelAsyncAdmissionBoundary::Operation::RecordDequeued(
    size_t count) noexcept {
  return boundary_ && admission_ &&
         boundary_->RecordDequeued(domain_, count);
}

void KernelAsyncAdmissionBoundary::Operation::Reset() noexcept {
  admission_.Reset();
  boundary_ = nullptr;
}

KernelAsyncAdmissionBoundary::Operation KernelAsyncAdmissionBoundary::Enter(
    KernelAsyncDomain domain) {
  const size_t index = DomainIndex(domain);
  if (index >= kKernelAsyncDomainCount) {
    return {};
  }
  return Operation(this, domain, gates_[index].Enter());
}

bool KernelAsyncAdmissionBoundary::RecordEnqueued(
    KernelAsyncDomain domain, size_t count) noexcept {
  const size_t index = DomainIndex(domain);
  if (index >= kKernelAsyncDomainCount || count == 0) {
    return count == 0;
  }

  size_t current = pending_items_[index].load(std::memory_order_acquire);
  while (true) {
    if (current > std::numeric_limits<size_t>::max() - count) {
      accounting_errors_[index].store(true, std::memory_order_release);
      return false;
    }
    if (pending_items_[index].compare_exchange_weak(
            current, current + count, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
}

bool KernelAsyncAdmissionBoundary::RecordDequeued(
    KernelAsyncDomain domain, size_t count) noexcept {
  const size_t index = DomainIndex(domain);
  if (index >= kKernelAsyncDomainCount || count == 0) {
    return count == 0;
  }

  size_t current = pending_items_[index].load(std::memory_order_acquire);
  while (true) {
    if (current < count) {
      accounting_errors_[index].store(true, std::memory_order_release);
      return false;
    }
    if (pending_items_[index].compare_exchange_weak(
            current, current - count, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
  }
}

KernelAsyncCloseResult KernelAsyncAdmissionBoundary::CloseAndWait(
    KernelAsyncDomain domain, uint64_t owner_id,
    std::chrono::steady_clock::time_point deadline,
    KernelPendingPolicy pending_policy) {
  const size_t index = DomainIndex(domain);
  if (index >= kKernelAsyncDomainCount) {
    return KernelAsyncCloseResult::kInvalidRequest;
  }

  const AdmissionGateResult admission_result =
      gates_[index].CloseAndWait(owner_id, deadline);
  if (admission_result != AdmissionGateResult::kReached) {
    return ConvertAdmissionResult(admission_result);
  }

  if (accounting_errors_[index].load(std::memory_order_acquire)) {
    gates_[index].Reopen(owner_id);
    return KernelAsyncCloseResult::kAccountingError;
  }
  if (pending_policy == KernelPendingPolicy::kRequireEmpty &&
      pending_items_[index].load(std::memory_order_acquire) != 0) {
    gates_[index].Reopen(owner_id);
    return KernelAsyncCloseResult::kUnsupportedPendingState;
  }
  return KernelAsyncCloseResult::kReached;
}

bool KernelAsyncAdmissionBoundary::Reopen(
    KernelAsyncDomain domain, uint64_t owner_id) noexcept {
  const size_t index = DomainIndex(domain);
  return index < kKernelAsyncDomainCount &&
         gates_[index].Reopen(owner_id);
}

KernelAsyncSnapshot KernelAsyncAdmissionBoundary::Snapshot(
    KernelAsyncDomain domain) const {
  const size_t index = DomainIndex(domain);
  if (index >= kKernelAsyncDomainCount) {
    return {};
  }
  return {gates_[index].Snapshot(),
          pending_items_[index].load(std::memory_order_acquire),
          accounting_errors_[index].load(std::memory_order_acquire)};
}

KernelWaitInventoryResult ValidateKernelWaitObjectInventory(
    const KernelWaitObjectKind* object_kinds, size_t object_count) {
  if (!object_kinds && object_count != 0) {
    return {false, 0, KernelWaitObjectKind::kUnknown};
  }
  for (size_t index = 0; index < object_count; ++index) {
    if (!IsKnownWaitObject(object_kinds[index])) {
      return {false, index, object_kinds[index]};
    }
  }
  return {true, SIZE_MAX, KernelWaitObjectKind::kUnknown};
}

}  // namespace xe::save_state
