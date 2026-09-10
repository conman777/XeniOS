/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_runtime.h"

#include <algorithm>
#include <utility>

namespace xe::save_state {

HeldGuestBoundary::HeldGuestBoundary(
    LiveGuestRuntime* runtime, uint64_t generation,
    std::vector<GuestThreadBinding> bindings)
    : runtime_(runtime),
      generation_(generation),
      bindings_(std::move(bindings)) {}

HeldGuestBoundary::~HeldGuestBoundary() { Reset(); }

HeldGuestBoundary::HeldGuestBoundary(HeldGuestBoundary&& other) noexcept
    : runtime_(other.runtime_),
      generation_(other.generation_),
      bindings_(std::move(other.bindings_)) {
  other.runtime_ = nullptr;
  other.generation_ = 0;
}

HeldGuestBoundary& HeldGuestBoundary::operator=(
    HeldGuestBoundary&& other) noexcept {
  if (this != &other) {
    Reset();
    runtime_ = other.runtime_;
    generation_ = other.generation_;
    bindings_ = std::move(other.bindings_);
    other.runtime_ = nullptr;
    other.generation_ = 0;
  }
  return *this;
}

void HeldGuestBoundary::Reset() {
  if (!runtime_) {
    return;
  }
  LiveGuestRuntime* runtime = runtime_;
  const uint64_t generation = generation_;
  runtime_ = nullptr;
  generation_ = 0;
  bindings_.clear();
  runtime->ReleaseBoundary(generation);
}

bool LiveGuestRuntime::RegisterThread(uint32_t thread_id, void* context) {
  if (!thread_id || !context) {
    return false;
  }
  std::unique_lock<std::mutex> lock(registry_mutex_);
  registry_condition_.wait(lock,
                           [this]() { return !enrollment_frozen_; });
  if (bindings_.size() >= kMaximumGuestThreadCount ||
      std::any_of(bindings_.begin(), bindings_.end(),
                  [thread_id](const GuestThreadBinding& binding) {
                    return binding.thread_id == thread_id;
                  })) {
    return false;
  }
  bindings_.push_back({thread_id, context});
  std::sort(bindings_.begin(), bindings_.end(),
            [](const GuestThreadBinding& left,
               const GuestThreadBinding& right) {
              return left.thread_id < right.thread_id;
            });
  return true;
}

void* LiveGuestRuntime::UnregisterThread(uint32_t thread_id) {
  if (!thread_id) {
    return nullptr;
  }
  std::unique_lock<std::mutex> lock(registry_mutex_);
  while (enrollment_frozen_) {
    // A thread may enter a kernel wait or exit in the narrow interval after
    // enrollment was frozen but before it polls from generated guest code.
    // Join the active generation before allowing its context to disappear.
    const uint64_t generation = barrier_.requested_generation();
    lock.unlock();
    if (generation) {
      barrier_.Poll(thread_id, generation);
    }
    lock.lock();
    if (enrollment_frozen_) {
      registry_condition_.wait(lock);
    }
  }
  auto it = std::lower_bound(
      bindings_.begin(), bindings_.end(), thread_id,
      [](const GuestThreadBinding& binding, uint32_t id) {
        return binding.thread_id < id;
      });
  if (it == bindings_.end() || it->thread_id != thread_id) {
    return nullptr;
  }
  void* context = it->context;
  bindings_.erase(it);
  return context;
}

HeldGuestBoundary LiveGuestRuntime::AcquireBoundary(
    std::chrono::milliseconds timeout, LiveBoundaryError* error) {
  const auto set_error = [error](LiveBoundaryError value) {
    if (error) {
      *error = value;
    }
  };
  set_error(LiveBoundaryError::kNone);
  if (timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumGuestBarrierWait) {
    set_error(LiveBoundaryError::kInvalidTimeout);
    return {};
  }

  std::vector<GuestThreadBinding> bindings;
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    if (enrollment_frozen_) {
      set_error(LiveBoundaryError::kBusy);
      return {};
    }
    enrollment_frozen_ = true;
    bindings = bindings_;
  }

  // If every guest thread is currently inside a tracked kernel wait, freezing
  // enrollment is itself a complete boundary. Threads leaving their waits will
  // block in RegisterThread until this held boundary is released.
  if (bindings.empty()) {
    return HeldGuestBoundary(this, 0, {});
  }

  std::vector<uint32_t> participant_ids;
  participant_ids.reserve(bindings.size());
  for (const GuestThreadBinding& binding : bindings) {
    participant_ids.push_back(binding.thread_id);
  }
  BarrierBeginResult begin = barrier_.Begin(std::move(participant_ids));
  if (!begin.accepted()) {
    UnfreezeEnrollment();
    set_error(begin.error == BarrierError::kBusy
                  ? LiveBoundaryError::kBusy
                  : LiveBoundaryError::kBarrierRejected);
    return {};
  }

  const BarrierWaitResult wait = barrier_.WaitForAll(begin.generation, timeout);
  if (wait != BarrierWaitResult::kReached) {
    // WaitForAll rolls back the barrier on timeout. Cancel is harmless for
    // any other failure and guarantees that no participant remains parked.
    barrier_.Cancel(begin.generation);
    UnfreezeEnrollment();
    set_error(wait == BarrierWaitResult::kTimedOut
                  ? LiveBoundaryError::kTimedOut
                  : LiveBoundaryError::kBarrierRejected);
    return {};
  }
  return HeldGuestBoundary(this, begin.generation, std::move(bindings));
}

size_t LiveGuestRuntime::registered_thread_count() const {
  std::lock_guard<std::mutex> lock(registry_mutex_);
  return bindings_.size();
}

void LiveGuestRuntime::ReleaseBoundary(uint64_t generation) {
  if (!barrier_.Release(generation)) {
    barrier_.Cancel(generation);
  }
  UnfreezeEnrollment();
}

void LiveGuestRuntime::UnfreezeEnrollment() {
  {
    std::lock_guard<std::mutex> lock(registry_mutex_);
    enrollment_frozen_ = false;
  }
  registry_condition_.notify_all();
}

}  // namespace xe::save_state
