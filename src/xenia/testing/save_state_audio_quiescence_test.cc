/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_audio_quiescence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <utility>

namespace xe::save_state::test {
namespace {

bool WaitUntil(const std::function<bool()>& predicate) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

class TestAudioQuiescenceBackend final : public AudioQuiescenceBackend {
 public:
  explicit TestAudioQuiescenceBackend(AudioQuiescenceBoundary* boundary)
      : boundary_(boundary) {}

  AudioQuiescenceResult PrepareHostOutputBoundary(
      const AudioQuiescenceRequest& request,
      std::string* error_message) override {
    ++host_prepare_count;
    prepared_request_id = request.request_id;
    host_prepare_entered.store(true);
    const AdmissionGateSnapshot audio =
        boundary_->Snapshot(AudioAdmissionDomain::kAudioSystem);
    const AdmissionGateSnapshot xma =
        boundary_->Snapshot(AudioAdmissionDomain::kXma);
    const AdmissionGateSnapshot host =
        boundary_->Snapshot(AudioAdmissionDomain::kHostCompletion);
    const AdmissionGateSnapshot callback =
        boundary_->Snapshot(AudioAdmissionDomain::kGuestCallback);
    saw_host_prepare_order =
        audio.closed && xma.closed && !host.closed && !callback.closed &&
        audio.in_flight == 0 && xma.in_flight == 0;
    host_hold = true;
    if (host_prepare_delay != std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(host_prepare_delay);
    }
    if (host_result != AudioQuiescenceResult::kReached && error_message) {
      *error_message = "injected host output failure";
    }
    return host_result;
  }

  AudioQuiescenceResult PrepareAudioWorkerBoundary(
      const AudioQuiescenceRequest& request,
      std::string* error_message) override {
    ++worker_prepare_count;
    prepared_request_id = request.request_id;
    worker_prepare_entered.store(true);
    const AdmissionGateSnapshot audio =
        boundary_->Snapshot(AudioAdmissionDomain::kAudioSystem);
    const AdmissionGateSnapshot xma =
        boundary_->Snapshot(AudioAdmissionDomain::kXma);
    const AdmissionGateSnapshot host =
        boundary_->Snapshot(AudioAdmissionDomain::kHostCompletion);
    const AdmissionGateSnapshot callback =
        boundary_->Snapshot(AudioAdmissionDomain::kGuestCallback);
    saw_worker_prepare_order =
        audio.closed && xma.closed && host.closed && !callback.closed &&
        host.in_flight == 0;
    worker_hold = true;
    if (worker_prepare_delay != std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(worker_prepare_delay);
    }
    if (worker_result != AudioQuiescenceResult::kReached && error_message) {
      *error_message = "injected audio worker failure";
    }
    return worker_result;
  }

  void Abort(uint64_t request_id) noexcept override {
    ++abort_count;
    aborted_matching_request = request_id == prepared_request_id;
    for (size_t index = 0; index < kAudioAdmissionDomainCount; ++index) {
      closed_on_abort[index] =
          boundary_->Snapshot(kAudioAdmissionOrder[index]).closed;
    }
    host_hold = false;
    worker_hold = false;
  }

  AudioQuiescenceBoundary* boundary_;
  AudioQuiescenceResult host_result = AudioQuiescenceResult::kReached;
  AudioQuiescenceResult worker_result = AudioQuiescenceResult::kReached;
  std::chrono::milliseconds host_prepare_delay =
      std::chrono::milliseconds::zero();
  std::chrono::milliseconds worker_prepare_delay =
      std::chrono::milliseconds::zero();
  std::atomic<bool> host_prepare_entered{false};
  std::atomic<bool> worker_prepare_entered{false};
  std::array<bool, kAudioAdmissionDomainCount> closed_on_abort = {};
  uint64_t prepared_request_id = 0;
  size_t host_prepare_count = 0;
  size_t worker_prepare_count = 0;
  size_t abort_count = 0;
  bool saw_host_prepare_order = false;
  bool saw_worker_prepare_order = false;
  bool aborted_matching_request = false;
  bool host_hold = false;
  bool worker_hold = false;
};

bool AllDomainsOpen(const AudioQuiescenceBoundary& boundary) {
  for (AudioAdmissionDomain domain : kAudioAdmissionOrder) {
    if (boundary.Snapshot(domain).closed) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("Audio boundary drains each producer in dependency order",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  auto audio_submission = boundary.EnterAudioSystemOperation();
  auto xma_work = boundary.EnterXmaOperation();
  auto host_completion = boundary.EnterHostCompletion();
  auto guest_callback = boundary.EnterGuestCallback();

  HeldAudioQuiescence held;
  AudioQuiescenceAcquireResult acquire_result;
  std::thread closer([&]() {
    acquire_result = boundary.Acquire(
        151, std::chrono::seconds(1), &backend, &held);
  });

  REQUIRE(WaitUntil([&]() {
    return boundary.Snapshot(AudioAdmissionDomain::kAudioSystem).closed;
  }));
  audio_submission.Reset();
  REQUIRE(WaitUntil([&]() {
    return boundary.Snapshot(AudioAdmissionDomain::kXma).closed;
  }));
  xma_work.Reset();
  REQUIRE(WaitUntil([&]() { return backend.host_prepare_entered.load(); }));
  REQUIRE(WaitUntil([&]() {
    return boundary.Snapshot(AudioAdmissionDomain::kHostCompletion).closed;
  }));
  host_completion.Reset();
  REQUIRE(WaitUntil([&]() { return backend.worker_prepare_entered.load(); }));
  REQUIRE(WaitUntil([&]() {
    return boundary.Snapshot(AudioAdmissionDomain::kGuestCallback).closed;
  }));
  guest_callback.Reset();

  closer.join();
  REQUIRE(acquire_result.ok());
  REQUIRE(held);
  CHECK(backend.saw_host_prepare_order);
  CHECK(backend.saw_worker_prepare_order);
  CHECK(backend.host_hold);
  CHECK(backend.worker_hold);

  std::atomic<bool> later_started{false};
  std::atomic<bool> later_entered{false};
  std::thread later_submission([&]() {
    later_started.store(true);
    auto admission = boundary.EnterAudioSystemOperation();
    later_entered.store(true);
  });
  REQUIRE(WaitUntil([&]() { return later_started.load(); }));
  CHECK_FALSE(later_entered.load());

  held.Release();
  later_submission.join();
  CHECK(later_entered.load());
  CHECK_FALSE(backend.host_hold);
  CHECK_FALSE(backend.worker_hold);
  CHECK(backend.abort_count == 1);
  CHECK(backend.aborted_matching_request);
  CHECK(std::all_of(backend.closed_on_abort.begin(),
                    backend.closed_on_abort.end(),
                    [](bool closed) { return closed; }));
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("AudioSystem admission timeout reopens without backend activity",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  auto active_submission = boundary.EnterAudioSystemOperation();

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      157, std::chrono::milliseconds(2), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kTimedOut);
  CHECK(result.domain == AudioAdmissionDomain::kAudioSystem);
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 0);
  CHECK(backend.abort_count == 0);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("XMA admission timeout reopens earlier AudioSystem admission",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  auto active_xma_work = boundary.EnterXmaOperation();

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      159, std::chrono::milliseconds(2), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kTimedOut);
  CHECK(result.domain == AudioAdmissionDomain::kXma);
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 0);
  CHECK(backend.abort_count == 0);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("Unsupported host audio drain aborts partial hold and reopens",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  backend.host_result = AudioQuiescenceResult::kUnsupported;

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      163, std::chrono::seconds(1), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kUnsupported);
  CHECK(result.domain == AudioAdmissionDomain::kHostCompletion);
  CHECK(result.message == "injected host output failure");
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 1);
  CHECK(backend.worker_prepare_count == 0);
  CHECK(backend.abort_count == 1);
  CHECK_FALSE(backend.host_hold);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("Host completion timeout rolls back prepared host boundary",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  auto completion = boundary.EnterHostCompletion();

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      167, std::chrono::milliseconds(2), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kTimedOut);
  CHECK(result.domain == AudioAdmissionDomain::kHostCompletion);
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 1);
  CHECK(backend.worker_prepare_count == 0);
  CHECK(backend.abort_count == 1);
  CHECK_FALSE(backend.host_hold);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("Audio worker failure restores host completion admission",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  backend.worker_result = AudioQuiescenceResult::kBackendFailure;

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      173, std::chrono::seconds(1), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kBackendFailure);
  CHECK(result.domain == AudioAdmissionDomain::kGuestCallback);
  CHECK(result.message == "injected audio worker failure");
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 1);
  CHECK(backend.worker_prepare_count == 1);
  CHECK(backend.abort_count == 1);
  CHECK(backend.closed_on_abort[2]);
  CHECK_FALSE(backend.host_hold);
  CHECK_FALSE(backend.worker_hold);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("Guest callback timeout rolls back every audio boundary",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  auto callback = boundary.EnterGuestCallback();

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult result = boundary.Acquire(
      179, std::chrono::milliseconds(2), &backend, &held);

  CHECK(result.result == AudioQuiescenceResult::kTimedOut);
  CHECK(result.domain == AudioAdmissionDomain::kGuestCallback);
  CHECK_FALSE(held);
  CHECK(backend.host_prepare_count == 1);
  CHECK(backend.worker_prepare_count == 1);
  CHECK(backend.abort_count == 1);
  CHECK(backend.closed_on_abort[0]);
  CHECK(backend.closed_on_abort[1]);
  CHECK(backend.closed_on_abort[2]);
  CHECK_FALSE(backend.closed_on_abort[3]);
  CHECK(AllDomainsOpen(boundary));
}

TEST_CASE("Late audio backend success fails closed and preserves ownership",
          "[save_state][audio][quiescence]") {
  AudioQuiescenceBoundary boundary;
  TestAudioQuiescenceBackend backend(&boundary);
  backend.host_prepare_delay = std::chrono::milliseconds(20);

  HeldAudioQuiescence held;
  const AudioQuiescenceAcquireResult late = boundary.Acquire(
      181, std::chrono::milliseconds(5), &backend, &held);
  CHECK(late.result == AudioQuiescenceResult::kTimedOut);
  CHECK_FALSE(held);
  CHECK(backend.abort_count == 1);
  CHECK(AllDomainsOpen(boundary));

  backend.host_prepare_delay = std::chrono::milliseconds::zero();
  backend.worker_prepare_delay = std::chrono::milliseconds(20);
  const AudioQuiescenceAcquireResult late_worker = boundary.Acquire(
      187, std::chrono::milliseconds(5), &backend, &held);
  CHECK(late_worker.result == AudioQuiescenceResult::kTimedOut);
  CHECK(late_worker.domain == AudioAdmissionDomain::kGuestCallback);
  CHECK_FALSE(held);
  CHECK(backend.abort_count == 2);
  CHECK(AllDomainsOpen(boundary));

  backend.worker_prepare_delay = std::chrono::milliseconds::zero();
  REQUIRE(boundary
              .Acquire(191, std::chrono::seconds(1), &backend, &held)
              .ok());
  HeldAudioQuiescence conflicting;
  const AudioQuiescenceAcquireResult busy = boundary.Acquire(
      193, std::chrono::seconds(1), &backend, &conflicting);
  CHECK(busy.result == AudioQuiescenceResult::kBusy);
  CHECK_FALSE(conflicting);

  HeldAudioQuiescence moved(std::move(held));
  CHECK_FALSE(held);
  REQUIRE(moved);
  moved.Release();
  CHECK(AllDomainsOpen(boundary));

  HeldAudioQuiescence invalid;
  CHECK(boundary
            .Acquire(0, std::chrono::seconds(1), &backend, &invalid)
            .result == AudioQuiescenceResult::kInvalidRequest);
  CHECK(boundary
            .Acquire(197, kMaximumAudioQuiescenceWait +
                              std::chrono::milliseconds(1),
                     &backend, &invalid)
            .result == AudioQuiescenceResult::kInvalidRequest);
}

}  // namespace xe::save_state::test
