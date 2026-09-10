/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_gpu_quiescence.h"

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

class TestGpuQuiescenceBackend final : public GpuQuiescenceBackend {
 public:
  explicit TestGpuQuiescenceBackend(GpuQuiescenceBoundary* boundary)
      : boundary_(boundary) {}

  GpuQuiescenceResult PrepareAndDrain(
      const GpuQuiescenceRequest& request,
      std::string* error_message) override {
    ++prepare_count;
    prepared_request_id = request.request_id;
    saw_future_deadline =
        request.deadline > std::chrono::steady_clock::now();
    const AdmissionGateSnapshot snapshot = boundary_->Snapshot();
    saw_closed_drained_admission =
        snapshot.closed && snapshot.owner_id == request.request_id &&
        snapshot.in_flight == 0;
    // Model a backend that may establish a partial worker hold even if its
    // later drain step fails. The coordinator must call Abort on every
    // attempted prepare.
    active = true;
    if (prepare_delay != std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(prepare_delay);
    }
    if (prepare_result != GpuQuiescenceResult::kReached) {
      if (error_message) {
        *error_message = "injected GPU drain failure";
      }
      return prepare_result;
    }
    return GpuQuiescenceResult::kReached;
  }

  void Abort(uint64_t request_id) noexcept override {
    ++abort_count;
    aborted_matching_request = request_id == prepared_request_id;
    saw_closed_admission_on_abort = boundary_->Snapshot().closed;
    active = false;
  }

  GpuQuiescenceBoundary* boundary_;
  GpuQuiescenceResult prepare_result = GpuQuiescenceResult::kReached;
  std::chrono::milliseconds prepare_delay = std::chrono::milliseconds::zero();
  uint64_t prepared_request_id = 0;
  size_t prepare_count = 0;
  size_t abort_count = 0;
  bool saw_future_deadline = false;
  bool saw_closed_drained_admission = false;
  bool saw_closed_admission_on_abort = false;
  bool aborted_matching_request = false;
  bool active = false;
};

}  // namespace

TEST_CASE("GPU boundary drains admitted producers and blocks later work",
          "[save_state][gpu][quiescence]") {
  GpuQuiescenceBoundary boundary;
  TestGpuQuiescenceBackend backend(&boundary);
  auto accepted_submission = boundary.EnterProducer();
  REQUIRE(accepted_submission);

  HeldGpuQuiescence held;
  GpuQuiescenceAcquireResult acquire_result;
  std::thread closer([&]() {
    acquire_result = boundary.Acquire(
        101, std::chrono::seconds(1), &backend, &held);
  });
  REQUIRE(WaitUntil([&]() { return boundary.Snapshot().closed; }));

  std::atomic<bool> later_started{false};
  std::atomic<bool> later_entered{false};
  std::thread later_producer([&]() {
    later_started.store(true);
    auto admission = boundary.EnterProducer();
    later_entered.store(true);
  });
  REQUIRE(WaitUntil([&]() { return later_started.load(); }));
  CHECK_FALSE(later_entered.load());

  accepted_submission.Reset();
  closer.join();
  REQUIRE(acquire_result.ok());
  REQUIRE(held);
  CHECK(held.request_id() == 101);
  CHECK(backend.saw_future_deadline);
  CHECK(backend.saw_closed_drained_admission);
  CHECK(backend.active);
  CHECK_FALSE(later_entered.load());

  held.Release();
  later_producer.join();
  CHECK_FALSE(backend.active);
  CHECK(backend.abort_count == 1);
  CHECK(backend.aborted_matching_request);
  CHECK(backend.saw_closed_admission_on_abort);
  CHECK(later_entered.load());
  CHECK_FALSE(boundary.Snapshot().closed);
}

TEST_CASE("GPU admission timeout rolls back without invoking backend drain",
          "[save_state][gpu][quiescence]") {
  GpuQuiescenceBoundary boundary;
  TestGpuQuiescenceBackend backend(&boundary);
  auto active_readback = boundary.EnterProducer();

  HeldGpuQuiescence held;
  const GpuQuiescenceAcquireResult result = boundary.Acquire(
      103, std::chrono::milliseconds(2), &backend, &held);

  CHECK(result.result == GpuQuiescenceResult::kTimedOut);
  CHECK_FALSE(held);
  CHECK(backend.prepare_count == 0);
  CHECK(backend.abort_count == 0);
  CHECK_FALSE(boundary.Snapshot().closed);
  active_readback.Reset();
  auto later_submission = boundary.EnterProducer();
  REQUIRE(later_submission);
}

TEST_CASE("GPU backend failure aborts partial holds and reopens admission",
          "[save_state][gpu][quiescence]") {
  GpuQuiescenceBoundary boundary;
  TestGpuQuiescenceBackend backend(&boundary);
  backend.prepare_result = GpuQuiescenceResult::kUnsupported;

  HeldGpuQuiescence held;
  const GpuQuiescenceAcquireResult result = boundary.Acquire(
      107, std::chrono::seconds(1), &backend, &held);

  CHECK(result.result == GpuQuiescenceResult::kUnsupported);
  CHECK(result.message == "injected GPU drain failure");
  CHECK_FALSE(held);
  CHECK(backend.prepare_count == 1);
  CHECK(backend.abort_count == 1);
  CHECK(backend.aborted_matching_request);
  CHECK(backend.saw_closed_admission_on_abort);
  CHECK_FALSE(backend.active);
  CHECK_FALSE(boundary.Snapshot().closed);
  auto later_present = boundary.EnterProducer();
  REQUIRE(later_present);
}

TEST_CASE("GPU backend late success is rolled back as a timeout",
          "[save_state][gpu][quiescence]") {
  GpuQuiescenceBoundary boundary;
  TestGpuQuiescenceBackend backend(&boundary);
  backend.prepare_delay = std::chrono::milliseconds(20);

  HeldGpuQuiescence held;
  const GpuQuiescenceAcquireResult result = boundary.Acquire(
      109, std::chrono::milliseconds(5), &backend, &held);

  CHECK(result.result == GpuQuiescenceResult::kTimedOut);
  CHECK_FALSE(held);
  CHECK(backend.prepare_count == 1);
  CHECK(backend.abort_count == 1);
  CHECK(backend.saw_closed_admission_on_abort);
  CHECK_FALSE(backend.active);
  CHECK_FALSE(boundary.Snapshot().closed);
}

TEST_CASE("GPU boundary preserves exclusive ownership and move-only release",
          "[save_state][gpu][quiescence]") {
  GpuQuiescenceBoundary boundary;
  TestGpuQuiescenceBackend backend(&boundary);

  HeldGpuQuiescence first;
  REQUIRE(boundary
              .Acquire(113, std::chrono::seconds(1), &backend, &first)
              .ok());
  HeldGpuQuiescence conflicting;
  const GpuQuiescenceAcquireResult busy = boundary.Acquire(
      127, std::chrono::seconds(1), &backend, &conflicting);
  CHECK(busy.result == GpuQuiescenceResult::kBusy);
  CHECK_FALSE(conflicting);
  CHECK(backend.prepare_count == 1);

  HeldGpuQuiescence moved(std::move(first));
  CHECK_FALSE(first);
  REQUIRE(moved);
  moved.Release();
  CHECK(backend.abort_count == 1);
  CHECK_FALSE(boundary.Snapshot().closed);

  HeldGpuQuiescence invalid;
  CHECK(boundary
            .Acquire(0, std::chrono::seconds(1), &backend, &invalid)
            .result == GpuQuiescenceResult::kInvalidRequest);
  CHECK(boundary
            .Acquire(131, kMaximumGpuQuiescenceWait +
                              std::chrono::milliseconds(1),
                     &backend, &invalid)
            .result == GpuQuiescenceResult::kInvalidRequest);
}

TEST_CASE("GPU command lanes drain PM4 work and block later producers",
          "[save_state][gpu][command_admission]") {
  GpuCommandAdmissionLanes lanes;
  auto pending_pm4 = lanes.EnterPm4Producer();
  REQUIRE(pending_pm4);
  CHECK(lanes.Snapshot().pm4_producer.in_flight == 1);

  GpuCommandAdmissionCloseResult close_result;
  std::thread closer([&]() {
    close_result = lanes.CloseAndWait(
        137, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  });
  REQUIRE(WaitUntil([&]() {
    return lanes.Snapshot().pm4_producer.closed;
  }));

  std::atomic<bool> later_started{false};
  std::atomic<bool> later_entered{false};
  std::thread later_pm4([&]() {
    later_started.store(true);
    auto admission = lanes.EnterPm4Producer();
    later_entered.store(true);
  });
  REQUIRE(WaitUntil([&]() { return later_started.load(); }));
  CHECK_FALSE(later_entered.load());

  pending_pm4.Reset();
  closer.join();
  REQUIRE(close_result.ok());
  const GpuCommandAdmissionSnapshot closed = lanes.Snapshot();
  CHECK(closed.pm4_producer.closed);
  CHECK(closed.pm4_producer.in_flight == 0);
  CHECK(closed.pending_callback.closed);

  std::atomic<bool> callback_entered{false};
  std::thread later_callback([&]() {
    auto admission = lanes.EnterPendingCallback();
    callback_entered.store(true);
  });
  std::this_thread::yield();
  CHECK_FALSE(callback_entered.load());
  REQUIRE(lanes.Reopen(137));
  later_pm4.join();
  later_callback.join();
  CHECK(later_entered.load());
  CHECK(callback_entered.load());
}

TEST_CASE("GPU command callback lifetime participates through execution",
          "[save_state][gpu][command_admission]") {
  GpuCommandAdmissionLanes lanes;
  auto queued_callback = lanes.EnterPendingCallback();
  REQUIRE(queued_callback);
  CHECK(lanes.Snapshot().pending_callback.in_flight == 1);

  GpuCommandAdmissionCloseResult close_result;
  std::thread closer([&]() {
    close_result = lanes.CloseAndWait(
        139, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  });
  REQUIRE(WaitUntil([&]() {
    return lanes.Snapshot().pending_callback.closed;
  }));

  // Models the worker popping and completing the queued callback.
  queued_callback.Reset();
  closer.join();
  REQUIRE(close_result.ok());
  REQUIRE(lanes.Reopen(139));
}

TEST_CASE("GPU command callback timeout reopens both ingress lanes",
          "[save_state][gpu][command_admission]") {
  GpuCommandAdmissionLanes lanes;
  auto active_callback = lanes.EnterPendingCallback();

  const GpuCommandAdmissionCloseResult result = lanes.CloseAndWait(
      149, std::chrono::steady_clock::now() + std::chrono::milliseconds(2));
  CHECK(result.result == AdmissionGateResult::kTimedOut);
  CHECK(result.lane == GpuCommandAdmissionLane::kPendingCallback);
  const GpuCommandAdmissionSnapshot reopened = lanes.Snapshot();
  CHECK_FALSE(reopened.pm4_producer.closed);
  CHECK_FALSE(reopened.pending_callback.closed);

  active_callback.Reset();
  auto later_pm4 = lanes.EnterPm4Producer();
  auto later_callback = lanes.EnterPendingCallback();
  REQUIRE(later_pm4);
  REQUIRE(later_callback);
}

TEST_CASE("GPU command lane ownership rejects stale reopen",
          "[save_state][gpu][command_admission]") {
  GpuCommandAdmissionLanes lanes;
  const GpuCommandAdmissionCloseResult reached = lanes.CloseAndWait(
      151, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  REQUIRE(reached.ok());
  CHECK_FALSE(lanes.Reopen(153));
  CHECK(lanes.Snapshot().pm4_producer.closed);
  CHECK(lanes.Snapshot().pending_callback.closed);
  REQUIRE(lanes.Reopen(151));

  const GpuCommandAdmissionCloseResult invalid = lanes.CloseAndWait(
      0, std::chrono::steady_clock::now() + std::chrono::seconds(1));
  CHECK(invalid.result == AdmissionGateResult::kInvalidRequest);
  CHECK(invalid.lane == GpuCommandAdmissionLane::kPm4Producer);
}

TEST_CASE("GPU command worker parks after ingress drains and reopens in order",
          "[save_state][gpu][command_worker]") {
  GpuCommandAdmissionLanes lanes;
  std::atomic<bool> worker_running{true};
  std::atomic<bool> worker_started{false};
  std::atomic<size_t> wake_count{0};
  std::thread worker([&]() {
    worker_started.store(true);
    while (worker_running.load()) {
      lanes.PollWorkerPark();
      std::this_thread::yield();
    }
  });
  REQUIRE(WaitUntil([&]() { return worker_started.load(); }));

  const GpuCommandWorkerParkAcquireResult parked =
      lanes.ParkWorkerAndWait(
          157, std::chrono::steady_clock::now() + std::chrono::seconds(1),
          [&]() { ++wake_count; });
  REQUIRE(parked.ok());
  CHECK(wake_count.load() == 1);
  const GpuCommandAdmissionSnapshot held = lanes.Snapshot();
  CHECK(held.pm4_producer.closed);
  CHECK(held.pending_callback.closed);
  CHECK(held.worker_park_requested);
  CHECK(held.worker_parked);
  CHECK(held.worker_owner_id == 157);

  CHECK_FALSE(lanes.ReleaseWorkerAndReopen(
      159, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  CHECK(lanes.Snapshot().worker_parked);
  CHECK_FALSE(lanes.ReleaseWorkerAndReopen(
      157, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)));
  CHECK(lanes.Snapshot().worker_parked);
  CHECK(lanes.Snapshot().pm4_producer.closed);
  REQUIRE(lanes.ReleaseWorkerAndReopen(
      157, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  worker_running.store(false);
  worker.join();

  const GpuCommandAdmissionSnapshot reopened = lanes.Snapshot();
  CHECK_FALSE(reopened.pm4_producer.closed);
  CHECK_FALSE(reopened.pending_callback.closed);
  CHECK_FALSE(reopened.worker_park_requested);
  CHECK_FALSE(reopened.worker_parked);
  CHECK(reopened.worker_owner_id == 0);
  auto later_pm4 = lanes.EnterPm4Producer();
  auto later_callback = lanes.EnterPendingCallback();
  REQUIRE(later_pm4);
  REQUIRE(later_callback);
}

TEST_CASE("GPU command worker park waits for queued callback ownership",
          "[save_state][gpu][command_worker]") {
  GpuCommandAdmissionLanes lanes;
  auto queued_callback = lanes.EnterPendingCallback();
  REQUIRE(queued_callback);
  std::atomic<bool> worker_running{true};
  std::atomic<bool> callback_executed{false};
  std::thread worker(
      [&, callback = std::move(queued_callback)]() mutable {
        if (WaitUntil([&]() {
              return lanes.Snapshot().pending_callback.closed;
            })) {
          callback.Reset();
          callback_executed.store(true);
        }
        while (worker_running.load()) {
          lanes.PollWorkerPark();
          std::this_thread::yield();
        }
      });

  const GpuCommandWorkerParkAcquireResult parked =
      lanes.ParkWorkerAndWait(
          163, std::chrono::steady_clock::now() + std::chrono::seconds(1),
          []() {});
  REQUIRE(parked.ok());
  CHECK(callback_executed.load());
  CHECK(lanes.Snapshot().pending_callback.in_flight == 0);
  REQUIRE(lanes.ReleaseWorkerAndReopen(
      163, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  worker_running.store(false);
  worker.join();
}

TEST_CASE("GPU command worker acknowledgement timeout reopens ingress",
          "[save_state][gpu][command_worker]") {
  GpuCommandAdmissionLanes lanes;
  std::atomic<size_t> wake_count{0};
  const GpuCommandWorkerParkAcquireResult result =
      lanes.ParkWorkerAndWait(
          167,
          std::chrono::steady_clock::now() + std::chrono::milliseconds(2),
          [&]() { ++wake_count; });

  CHECK(result.result == GpuCommandWorkerParkResult::kTimedOut);
  CHECK(result.failure_lane == GpuCommandAdmissionLane::kNone);
  CHECK(wake_count.load() == 1);
  const GpuCommandAdmissionSnapshot reopened = lanes.Snapshot();
  CHECK_FALSE(reopened.pm4_producer.closed);
  CHECK_FALSE(reopened.pending_callback.closed);
  CHECK_FALSE(reopened.worker_park_requested);
  CHECK_FALSE(reopened.worker_parked);
  auto later_pm4 = lanes.EnterPm4Producer();
  REQUIRE(later_pm4);
}

TEST_CASE("GPU active PM4 timeout never requests a worker park",
          "[save_state][gpu][command_worker]") {
  GpuCommandAdmissionLanes lanes;
  auto active_pm4 = lanes.EnterPm4Producer();
  std::atomic<size_t> wake_count{0};

  const GpuCommandWorkerParkAcquireResult result =
      lanes.ParkWorkerAndWait(
          173,
          std::chrono::steady_clock::now() + std::chrono::milliseconds(2),
          [&]() { ++wake_count; });

  CHECK(result.result == GpuCommandWorkerParkResult::kTimedOut);
  CHECK(result.failure_lane == GpuCommandAdmissionLane::kPm4Producer);
  CHECK(wake_count.load() == 0);
  const GpuCommandAdmissionSnapshot reopened = lanes.Snapshot();
  CHECK_FALSE(reopened.pm4_producer.closed);
  CHECK_FALSE(reopened.pending_callback.closed);
  CHECK_FALSE(reopened.worker_park_requested);
  active_pm4.Reset();
}

}  // namespace xe::save_state::test
