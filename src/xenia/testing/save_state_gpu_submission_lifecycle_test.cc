/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_gpu_submission_lifecycle.h"

#include <chrono>
#include <string>

namespace xe::save_state::test {
namespace {

class TestGpuSubmissionLifecycleBackend final
    : public GpuSubmissionLifecycleBackend {
 public:
  TestGpuSubmissionLifecycleBackend() {
    snapshot.draw_ingress_closed = true;
    snapshot.on_parked_worker_control_lane = true;
    snapshot.independent_submission_producers_closed = true;
    snapshot.close_submit_failure_atomic = true;
    snapshot.completion_poll_nonblocking = true;
    snapshot.submission_open = true;
    snapshot.active_command_buffer_valid = true;
    snapshot.close_resources_preallocated = true;
    snapshot.current_submission = 11;
    snapshot.completed_submission = 9;
  }

  GpuSubmissionLifecycleResult PrepareAndInspect(
      const GpuSubmissionLifecycleRequest& request,
      GpuSubmissionLifecycleSnapshot* snapshot_out,
      std::string* error_message) override {
    ++prepare_count;
    prepared_owner = request.owner_id;
    control_held = true;
    if (prepare_result != GpuSubmissionLifecycleResult::kPrepared) {
      if (error_message) {
        *error_message = "injected preflight result";
      }
      return prepare_result;
    }
    *snapshot_out = snapshot;
    return GpuSubmissionLifecycleResult::kPrepared;
  }

  bool AbortPreparedAndReopen(uint64_t owner_id) noexcept override {
    ++abort_count;
    abort_owner_matched = owner_id == prepared_owner;
    if (abort_fails) {
      return false;
    }
    control_held = false;
    return true;
  }

  GpuSubmissionCommitResult CloseAndSubmit(
      const GpuSubmissionLifecycleRequest& request,
      const GpuSubmissionLifecycleSnapshot& expected_snapshot,
      uint64_t* submitted_submission,
      std::string* error_message) override {
    ++commit_count;
    commit_owner_matched = request.owner_id == prepared_owner;
    commit_snapshot_matched =
        expected_snapshot.current_submission == snapshot.current_submission &&
        expected_snapshot.completed_submission ==
            snapshot.completed_submission;
    if (commit_result != GpuSubmissionCommitResult::kSubmitted) {
      if (error_message) {
        *error_message = "injected submission result";
      }
      return commit_result;
    }
    mutated = true;
    *submitted_submission =
        invalid_submitted_index ? snapshot.current_submission + 1
                                : snapshot.current_submission;
    return GpuSubmissionCommitResult::kSubmitted;
  }

  GpuSubmissionPollResult PollCompletion(
      uint64_t owner_id, uint64_t target_submission,
      uint64_t* completed_submission,
      std::string* error_message) override {
    ++poll_count;
    poll_owner_matched = owner_id == prepared_owner;
    polled_target = target_submission;
    if (poll_result == GpuSubmissionPollResult::kBackendFailure) {
      if (error_message) {
        *error_message = "injected completion polling failure";
      }
      return poll_result;
    }
    if (poll_result == GpuSubmissionPollResult::kCompleted ||
        poll_count >= polls_until_completed) {
      *completed_submission =
          invalid_completed_index ? target_submission - 1 : target_submission;
      return GpuSubmissionPollResult::kCompleted;
    }
    return GpuSubmissionPollResult::kPending;
  }

  bool ReleaseCompletedAndReopen(uint64_t owner_id) noexcept override {
    ++release_count;
    release_owner_matched = owner_id == prepared_owner;
    if (release_fails) {
      return false;
    }
    control_held = false;
    return true;
  }

  void RetainFailClosed(uint64_t owner_id) noexcept override {
    ++fail_closed_count;
    fail_closed_owner_matched = owner_id == prepared_owner;
    // Deliberately retain control_held.
  }

  GpuSubmissionLifecycleSnapshot snapshot;
  GpuSubmissionLifecycleResult prepare_result =
      GpuSubmissionLifecycleResult::kPrepared;
  GpuSubmissionCommitResult commit_result =
      GpuSubmissionCommitResult::kSubmitted;
  GpuSubmissionPollResult poll_result = GpuSubmissionPollResult::kPending;
  size_t polls_until_completed = 1;
  uint64_t prepared_owner = 0;
  uint64_t polled_target = 0;
  size_t prepare_count = 0;
  size_t abort_count = 0;
  size_t commit_count = 0;
  size_t poll_count = 0;
  size_t release_count = 0;
  size_t fail_closed_count = 0;
  bool invalid_submitted_index = false;
  bool invalid_completed_index = false;
  bool abort_fails = false;
  bool release_fails = false;
  bool control_held = false;
  bool mutated = false;
  bool abort_owner_matched = false;
  bool commit_owner_matched = false;
  bool commit_snapshot_matched = false;
  bool poll_owner_matched = false;
  bool release_owner_matched = false;
  bool fail_closed_owner_matched = false;
};

GpuSubmissionLifecycleRequest FutureRequest(uint64_t owner_id) {
  return {owner_id,
          std::chrono::steady_clock::now() + std::chrono::seconds(1)};
}

}  // namespace

TEST_CASE("GPU submission lifecycle commits and acknowledges without reopening",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.polls_until_completed = 3;
  HeldGpuSubmissionLifecycle held;

  REQUIRE(coordinator.Prepare(FutureRequest(181), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);
  REQUIRE(held);
  CHECK(held.state() == HeldGpuSubmissionLifecycleState::kPrepared);
  CHECK(backend.control_held);

  const GpuSubmissionLifecycleOperationResult committed =
      held.CommitAndWait();
  REQUIRE(committed.ok());
  CHECK(held.state() == HeldGpuSubmissionLifecycleState::kCompleted);
  CHECK(held.target_submission() == 11);
  CHECK(backend.commit_count == 1);
  CHECK(backend.commit_owner_matched);
  CHECK(backend.commit_snapshot_matched);
  CHECK(backend.poll_count == 3);
  CHECK(backend.poll_owner_matched);
  CHECK(backend.polled_target == 11);
  CHECK(backend.control_held);
  REQUIRE(held.ReleaseCompleted());
  CHECK(backend.release_count == 1);
  CHECK(backend.release_owner_matched);
  CHECK_FALSE(backend.control_held);
}

TEST_CASE("GPU submission timeout stays sealed and can be acknowledged later",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.polls_until_completed = SIZE_MAX;
  HeldGpuSubmissionLifecycle held;
  GpuSubmissionLifecycleRequest request = {
      191, std::chrono::steady_clock::now() + std::chrono::milliseconds(2)};

  REQUIRE(coordinator.Prepare(request, &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);
  const GpuSubmissionLifecycleOperationResult timed_out =
      held.CommitAndWait();
  CHECK(timed_out.result == GpuSubmissionLifecycleResult::kTimedOut);
  CHECK(held.state() ==
        HeldGpuSubmissionLifecycleState::kSubmittedPending);
  CHECK(backend.mutated);
  CHECK(backend.control_held);
  CHECK(backend.abort_count == 0);
  CHECK(backend.release_count == 0);

  backend.polls_until_completed = backend.poll_count + 1;
  REQUIRE(held
              .WaitUntil(std::chrono::steady_clock::now() +
                         std::chrono::seconds(1))
              .ok());
  REQUIRE(held.ReleaseCompleted());
  CHECK_FALSE(backend.control_held);
}

TEST_CASE("GPU submission preflight rejects current non-atomic Vulkan shape",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.snapshot.close_submit_failure_atomic = false;
  backend.snapshot.sparse_bind_pending = true;
  HeldGpuSubmissionLifecycle held;

  const GpuSubmissionLifecycleOperationResult result =
      coordinator.Prepare(FutureRequest(193), &backend, &held);
  CHECK(result.result == GpuSubmissionLifecycleResult::kUnsupported);
  CHECK_FALSE(held);
  CHECK(backend.prepare_count == 1);
  CHECK(backend.commit_count == 0);
  CHECK(backend.abort_count == 1);
  CHECK(backend.abort_owner_matched);
  CHECK_FALSE(backend.control_held);
}

TEST_CASE("GPU pre-mutation submit failure aborts reversibly",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.commit_result =
      GpuSubmissionCommitResult::kFailedBeforeMutation;
  HeldGpuSubmissionLifecycle held;
  REQUIRE(coordinator.Prepare(FutureRequest(197), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);

  const GpuSubmissionLifecycleOperationResult result =
      held.CommitAndWait();
  CHECK(result.result == GpuSubmissionLifecycleResult::kBackendFailure);
  CHECK(held.state() == HeldGpuSubmissionLifecycleState::kPrepared);
  CHECK_FALSE(backend.mutated);
  CHECK(backend.control_held);
  held.Reset();
  CHECK(backend.abort_count == 1);
  CHECK(backend.fail_closed_count == 0);
  CHECK_FALSE(backend.control_held);
}

TEST_CASE("GPU indeterminate submit failure never reopens admission",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.commit_result =
      GpuSubmissionCommitResult::kIndeterminateAfterMutation;
  HeldGpuSubmissionLifecycle held;
  REQUIRE(coordinator.Prepare(FutureRequest(199), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);

  CHECK(held.CommitAndWait().result ==
        GpuSubmissionLifecycleResult::kBackendFailure);
  CHECK(held.state() ==
        HeldGpuSubmissionLifecycleState::kIndeterminate);
  held.Reset();
  CHECK(backend.abort_count == 0);
  CHECK(backend.release_count == 0);
  CHECK(backend.fail_closed_count == 1);
  CHECK(backend.fail_closed_owner_matched);
  CHECK(backend.control_held);
}

TEST_CASE("GPU lifecycle waits existing closed submission without committing",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.snapshot.submission_open = false;
  backend.snapshot.active_command_buffer_valid = false;
  backend.snapshot.current_submission = 17;
  backend.snapshot.completed_submission = 14;
  backend.polls_until_completed = 2;
  HeldGpuSubmissionLifecycle held;

  REQUIRE(coordinator.Prepare(FutureRequest(211), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);
  CHECK(held.state() ==
        HeldGpuSubmissionLifecycleState::kSubmittedPending);
  CHECK(held.target_submission() == 16);
  REQUIRE(held.CommitAndWait().ok());
  CHECK(backend.commit_count == 0);
  CHECK(backend.polled_target == 16);
  REQUIRE(held.ReleaseCompleted());
}

TEST_CASE("GPU lifecycle releases an already completed closed submission",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.snapshot.submission_open = false;
  backend.snapshot.active_command_buffer_valid = false;
  backend.snapshot.current_submission = 23;
  backend.snapshot.completed_submission = 22;
  HeldGpuSubmissionLifecycle held;

  REQUIRE(coordinator.Prepare(FutureRequest(223), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);
  CHECK(held.state() == HeldGpuSubmissionLifecycleState::kCompleted);
  CHECK(held.target_submission() == 22);
  REQUIRE(held.CommitAndWait().ok());
  CHECK(backend.commit_count == 0);
  CHECK(backend.poll_count == 0);
  REQUIRE(held.ReleaseCompleted());
}

TEST_CASE("GPU invalid completion index follows fail-closed path",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.invalid_completed_index = true;
  HeldGpuSubmissionLifecycle held;
  REQUIRE(coordinator.Prepare(FutureRequest(227), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);

  CHECK(held.CommitAndWait().result ==
        GpuSubmissionLifecycleResult::kBackendFailure);
  CHECK(held.state() ==
        HeldGpuSubmissionLifecycleState::kIndeterminate);
  held.Reset();
  CHECK(backend.fail_closed_count == 1);
  CHECK(backend.control_held);
}

TEST_CASE("GPU completed release failure preserves hold then fails closed",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.snapshot.submission_open = false;
  backend.snapshot.active_command_buffer_valid = false;
  backend.snapshot.current_submission = 31;
  backend.snapshot.completed_submission = 30;
  backend.release_fails = true;
  HeldGpuSubmissionLifecycle held;
  REQUIRE(coordinator.Prepare(FutureRequest(229), &backend, &held).result ==
          GpuSubmissionLifecycleResult::kPrepared);

  CHECK_FALSE(held.ReleaseCompleted());
  CHECK(held.state() == HeldGpuSubmissionLifecycleState::kCompleted);
  CHECK(backend.control_held);
  held.Reset();
  CHECK(backend.release_count == 2);
  CHECK(backend.fail_closed_count == 1);
  CHECK(backend.control_held);
}

TEST_CASE("GPU rejected preflight abort failure remains fail closed",
          "[save_state][gpu][submission_lifecycle]") {
  GpuSubmissionLifecycleCoordinator coordinator;
  TestGpuSubmissionLifecycleBackend backend;
  backend.snapshot.draw_ingress_closed = false;
  backend.abort_fails = true;
  HeldGpuSubmissionLifecycle held;

  const GpuSubmissionLifecycleOperationResult result =
      coordinator.Prepare(FutureRequest(233), &backend, &held);
  CHECK(result.result == GpuSubmissionLifecycleResult::kBackendFailure);
  CHECK_FALSE(held);
  CHECK(backend.abort_count == 1);
  CHECK(backend.fail_closed_count == 1);
  CHECK(backend.control_held);
}

}  // namespace xe::save_state::test
