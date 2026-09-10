/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_RUNTIME_H_
#define XENIA_SAVE_STATE_RUNTIME_H_

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

#include "xenia/save_state_phase2.h"

namespace xe::save_state {

struct GuestThreadBinding {
  uint32_t thread_id = 0;
  void* context = nullptr;
};

enum class LiveBoundaryError {
  kNone,
  kBusy,
  kNoParticipants,
  kInvalidTimeout,
  kBarrierRejected,
  kTimedOut,
};

class LiveGuestRuntime;

// Keeps both the cooperative barrier and the thread enrollment set frozen.
// Context pointers in bindings() are stable only for this object's lifetime.
class HeldGuestBoundary {
 public:
  HeldGuestBoundary() = default;
  ~HeldGuestBoundary();
  HeldGuestBoundary(const HeldGuestBoundary&) = delete;
  HeldGuestBoundary& operator=(const HeldGuestBoundary&) = delete;
  HeldGuestBoundary(HeldGuestBoundary&& other) noexcept;
  HeldGuestBoundary& operator=(HeldGuestBoundary&& other) noexcept;

  explicit operator bool() const { return runtime_ != nullptr; }
  uint64_t generation() const { return generation_; }
  const std::vector<GuestThreadBinding>& bindings() const { return bindings_; }
  void Reset();

 private:
  friend class LiveGuestRuntime;
  HeldGuestBoundary(LiveGuestRuntime* runtime, uint64_t generation,
                    std::vector<GuestThreadBinding> bindings);

  LiveGuestRuntime* runtime_ = nullptr;
  uint64_t generation_ = 0;
  std::vector<GuestThreadBinding> bindings_;
};

// Owns live guest-thread enrollment and coordinates a stable cooperative
// boundary. It never suspends host threads. Register and Unregister wait while
// a successful boundary is held so that captured PPCContext pointers remain
// valid.
class LiveGuestRuntime {
 public:
  LiveGuestRuntime() = default;
  LiveGuestRuntime(const LiveGuestRuntime&) = delete;
  LiveGuestRuntime& operator=(const LiveGuestRuntime&) = delete;

  bool RegisterThread(uint32_t thread_id, void* context);
  void* UnregisterThread(uint32_t thread_id);

  HeldGuestBoundary AcquireBoundary(std::chrono::milliseconds timeout,
                                    LiveBoundaryError* error);
  BarrierPollResult Poll(uint32_t thread_id, uint64_t generation) {
    return barrier_.Poll(thread_id, generation);
  }

  uint64_t requested_generation() const {
    return barrier_.requested_generation();
  }
  BarrierStatus barrier_status() const { return barrier_.status(); }
  const std::atomic<uint64_t>* requested_generation_address() const {
    return barrier_.requested_generation_address();
  }
  size_t registered_thread_count() const;

 private:
  friend class HeldGuestBoundary;
  void ReleaseBoundary(uint64_t generation);
  void UnfreezeEnrollment();

  mutable std::mutex registry_mutex_;
  std::condition_variable registry_condition_;
  bool enrollment_frozen_ = false;
  std::vector<GuestThreadBinding> bindings_;
  CooperativeGuestBarrier barrier_;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_RUNTIME_H_
