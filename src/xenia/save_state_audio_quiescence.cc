/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_audio_quiescence.h"

#include <utility>

namespace xe::save_state {
namespace {

const char* DomainName(AudioAdmissionDomain domain) {
  switch (domain) {
    case AudioAdmissionDomain::kAudioSystem:
      return "AudioSystem";
    case AudioAdmissionDomain::kXma:
      return "XMA";
    case AudioAdmissionDomain::kHostCompletion:
      return "host audio completion";
    case AudioAdmissionDomain::kGuestCallback:
      return "guest audio callback";
  }
  return "unknown audio";
}

AudioQuiescenceResult ConvertAdmissionResult(AdmissionGateResult result) {
  switch (result) {
    case AdmissionGateResult::kReached:
      return AudioQuiescenceResult::kReached;
    case AdmissionGateResult::kBusy:
      return AudioQuiescenceResult::kBusy;
    case AdmissionGateResult::kTimedOut:
      return AudioQuiescenceResult::kTimedOut;
    case AdmissionGateResult::kInvalidRequest:
      return AudioQuiescenceResult::kInvalidRequest;
  }
  return AudioQuiescenceResult::kBackendFailure;
}

}  // namespace

HeldAudioQuiescence::~HeldAudioQuiescence() { Release(); }

HeldAudioQuiescence::HeldAudioQuiescence(
    HeldAudioQuiescence&& other) noexcept
    : boundary_(other.boundary_),
      backend_(other.backend_),
      request_id_(other.request_id_) {
  other.boundary_ = nullptr;
  other.backend_ = nullptr;
  other.request_id_ = 0;
}

HeldAudioQuiescence& HeldAudioQuiescence::operator=(
    HeldAudioQuiescence&& other) noexcept {
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

void HeldAudioQuiescence::Release() noexcept {
  AudioQuiescenceBoundary* boundary = boundary_;
  AudioQuiescenceBackend* backend = backend_;
  const uint64_t request_id = request_id_;
  boundary_ = nullptr;
  backend_ = nullptr;
  request_id_ = 0;
  if (!boundary || !backend || request_id == 0) {
    return;
  }
  backend->Abort(request_id);
  boundary->ReopenPrepared(kAudioAdmissionDomainCount, request_id);
}

void AudioQuiescenceBoundary::ReopenPrepared(
    size_t prepared_count, uint64_t request_id) noexcept {
  while (prepared_count) {
    --prepared_count;
    const AudioAdmissionDomain domain = kAudioAdmissionOrder[prepared_count];
    gates_[DomainIndex(domain)].Reopen(request_id);
  }
}

AudioQuiescenceAcquireResult AudioQuiescenceBoundary::Acquire(
    uint64_t request_id, std::chrono::milliseconds timeout,
    AudioQuiescenceBackend* backend, HeldAudioQuiescence* output) {
  if (request_id == 0 || timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumAudioQuiescenceWait || !backend || !output || *output) {
    return {AudioQuiescenceResult::kInvalidRequest,
            AudioAdmissionDomain::kAudioSystem,
            "Audio quiescence request is invalid or its output is occupied."};
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const AudioQuiescenceRequest request{request_id, deadline};
  size_t prepared_count = 0;
  bool backend_attempted = false;

  auto close_domain = [&](AudioAdmissionDomain domain)
      -> AudioQuiescenceAcquireResult {
    const AdmissionGateResult gate_result =
        gates_[DomainIndex(domain)].CloseAndWait(request_id, deadline);
    if (gate_result == AdmissionGateResult::kReached) {
      ++prepared_count;
      return {AudioQuiescenceResult::kReached, domain, {}};
    }
    return {ConvertAdmissionResult(gate_result), domain,
            std::string(DomainName(domain)) +
                " admission did not reach a drained boundary."};
  };

  auto rollback = [&]() noexcept {
    if (backend_attempted) {
      backend->Abort(request_id);
    }
    ReopenPrepared(prepared_count, request_id);
  };

  for (size_t index = 0; index < 2; ++index) {
    AudioQuiescenceAcquireResult close_result =
        close_domain(kAudioAdmissionOrder[index]);
    if (!close_result.ok()) {
      rollback();
      return close_result;
    }
  }

  std::string error_message;
  backend_attempted = true;
  AudioQuiescenceResult backend_result =
      backend->PrepareHostOutputBoundary(request, &error_message);
  if (backend_result != AudioQuiescenceResult::kReached) {
    rollback();
    if (error_message.empty()) {
      error_message = "Host audio output did not reach a safe boundary.";
    }
    return {backend_result, AudioAdmissionDomain::kHostCompletion,
            std::move(error_message)};
  }
  if (std::chrono::steady_clock::now() > deadline) {
    rollback();
    return {AudioQuiescenceResult::kTimedOut,
            AudioAdmissionDomain::kHostCompletion,
            "Host audio output reported quiescence after its deadline."};
  }

  AudioQuiescenceAcquireResult host_completion_result =
      close_domain(AudioAdmissionDomain::kHostCompletion);
  if (!host_completion_result.ok()) {
    rollback();
    return host_completion_result;
  }

  error_message.clear();
  backend_result =
      backend->PrepareAudioWorkerBoundary(request, &error_message);
  if (backend_result != AudioQuiescenceResult::kReached) {
    rollback();
    if (error_message.empty()) {
      error_message =
          "AudioSystem worker did not reach a safe callback boundary.";
    }
    return {backend_result, AudioAdmissionDomain::kGuestCallback,
            std::move(error_message)};
  }
  if (std::chrono::steady_clock::now() > deadline) {
    rollback();
    return {AudioQuiescenceResult::kTimedOut,
            AudioAdmissionDomain::kGuestCallback,
            "AudioSystem worker reported quiescence after its deadline."};
  }

  AudioQuiescenceAcquireResult callback_result =
      close_domain(AudioAdmissionDomain::kGuestCallback);
  if (!callback_result.ok()) {
    rollback();
    return callback_result;
  }

  HeldAudioQuiescence held;
  held.boundary_ = this;
  held.backend_ = backend;
  held.request_id_ = request_id;
  *output = std::move(held);
  return {AudioQuiescenceResult::kReached,
          AudioAdmissionDomain::kGuestCallback, {}};
}

}  // namespace xe::save_state
