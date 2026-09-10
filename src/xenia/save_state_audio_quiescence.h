/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_AUDIO_QUIESCENCE_H_
#define XENIA_SAVE_STATE_AUDIO_QUIESCENCE_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "xenia/base/reversible_admission_gate.h"

namespace xe::save_state {

constexpr std::chrono::milliseconds kMaximumAudioQuiescenceWait{5000};

enum class AudioAdmissionDomain {
  kAudioSystem,
  kXma,
  kHostCompletion,
  kGuestCallback,
};

constexpr size_t kAudioAdmissionDomainCount = 4;
constexpr std::array<AudioAdmissionDomain, kAudioAdmissionDomainCount>
    kAudioAdmissionOrder = {
        AudioAdmissionDomain::kAudioSystem,
        AudioAdmissionDomain::kXma,
        AudioAdmissionDomain::kHostCompletion,
        AudioAdmissionDomain::kGuestCallback,
};
static_assert(static_cast<size_t>(AudioAdmissionDomain::kGuestCallback) + 1 ==
              kAudioAdmissionDomainCount);

enum class AudioQuiescenceResult {
  kReached,
  kBusy,
  kTimedOut,
  kUnsupported,
  kBackendFailure,
  kInvalidRequest,
};

struct AudioQuiescenceRequest {
  uint64_t request_id = 0;
  std::chrono::steady_clock::time_point deadline;
};

// Future live adapters own the control lanes for host playback and the
// AudioSystem worker. PrepareHostOutputBoundary runs while host completion and
// guest callback admission are still open. It must reversibly stop new host
// playback consumption and drain already queued buffers and their completion
// callbacks before the shared deadline. It must return kUnsupported rather
// than flushing, dropping, or approximating opaque host buffers.
//
// PrepareAudioWorkerBoundary runs after host completion admission is closed
// and drained. It must drain pending client semaphore tokens and guest audio
// callbacks, then park the AudioSystem worker without an unbounded wait.
//
// Abort is an idempotent, non-blocking control-lane operation. It may be called
// after either preparation attempt and must not enter any admission gate,
// submit audio, release a client semaphore, or invoke guest code.
class AudioQuiescenceBackend {
 public:
  virtual ~AudioQuiescenceBackend() = default;

  virtual AudioQuiescenceResult PrepareHostOutputBoundary(
      const AudioQuiescenceRequest& request,
      std::string* error_message) = 0;
  virtual AudioQuiescenceResult PrepareAudioWorkerBoundary(
      const AudioQuiescenceRequest& request,
      std::string* error_message) = 0;
  virtual void Abort(uint64_t request_id) noexcept = 0;
};

struct AudioQuiescenceAcquireResult {
  AudioQuiescenceResult result = AudioQuiescenceResult::kInvalidRequest;
  AudioAdmissionDomain domain = AudioAdmissionDomain::kAudioSystem;
  std::string message;

  bool ok() const { return result == AudioQuiescenceResult::kReached; }
};

class AudioQuiescenceBoundary;

// Sole owner of a successfully prepared audio boundary. Release first aborts
// backend holds, then reopens guest callbacks, host completions, XMA, and
// AudioSystem admission in reverse dependency order.
class HeldAudioQuiescence {
 public:
  HeldAudioQuiescence() = default;
  ~HeldAudioQuiescence();
  HeldAudioQuiescence(const HeldAudioQuiescence&) = delete;
  HeldAudioQuiescence& operator=(const HeldAudioQuiescence&) = delete;
  HeldAudioQuiescence(HeldAudioQuiescence&& other) noexcept;
  HeldAudioQuiescence& operator=(HeldAudioQuiescence&& other) noexcept;

  explicit operator bool() const { return boundary_ != nullptr; }
  uint64_t request_id() const { return request_id_; }

  void Release() noexcept;

 private:
  friend class AudioQuiescenceBoundary;

  AudioQuiescenceBoundary* boundary_ = nullptr;
  AudioQuiescenceBackend* backend_ = nullptr;
  uint64_t request_id_ = 0;
};

// Unregistered admission/drain foundation. Live methods intentionally do not
// enter these gates yet.
class AudioQuiescenceBoundary {
 public:
  ReversibleAdmissionGate::Lease EnterAudioSystemOperation() {
    return gates_[DomainIndex(AudioAdmissionDomain::kAudioSystem)].Enter();
  }
  ReversibleAdmissionGate::Lease EnterXmaOperation() {
    return gates_[DomainIndex(AudioAdmissionDomain::kXma)].Enter();
  }
  ReversibleAdmissionGate::Lease EnterHostCompletion() {
    return gates_[DomainIndex(AudioAdmissionDomain::kHostCompletion)].Enter();
  }
  ReversibleAdmissionGate::Lease EnterGuestCallback() {
    return gates_[DomainIndex(AudioAdmissionDomain::kGuestCallback)].Enter();
  }

  AudioQuiescenceAcquireResult Acquire(
      uint64_t request_id, std::chrono::milliseconds timeout,
      AudioQuiescenceBackend* backend, HeldAudioQuiescence* output);

  AdmissionGateSnapshot Snapshot(AudioAdmissionDomain domain) const {
    return gates_[DomainIndex(domain)].Snapshot();
  }

 private:
  friend class HeldAudioQuiescence;

  static constexpr size_t DomainIndex(AudioAdmissionDomain domain) {
    return static_cast<size_t>(domain);
  }

  void ReopenPrepared(size_t prepared_count, uint64_t request_id) noexcept;

  std::array<ReversibleAdmissionGate, kAudioAdmissionDomainCount> gates_;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_AUDIO_QUIESCENCE_H_
