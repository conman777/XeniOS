/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/reversible_admission_gate.h"
#include "xenia/save_state_subsystem_quiescence.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace xe::save_state::test {
namespace {

const char* SubsystemName(QuiescentSubsystem subsystem) {
  switch (subsystem) {
    case QuiescentSubsystem::kKernelDispatchAndTimers:
      return "kernel";
    case QuiescentSubsystem::kVfsAndIo:
      return "vfs";
    case QuiescentSubsystem::kVulkanCommandProcessor:
      return "vulkan";
    case QuiescentSubsystem::kAudio:
      return "audio";
    case QuiescentSubsystem::kInput:
      return "input";
  }
  return "unknown";
}

class TestQuiescenceParticipant final
    : public SubsystemQuiescenceParticipant {
 public:
  TestQuiescenceParticipant(QuiescentSubsystem subsystem,
                            std::vector<std::string>* events)
      : subsystem_(subsystem), events_(events) {}

  QuiescentSubsystem subsystem() const override { return subsystem_; }

  SubsystemQuiescenceResult Prepare(
      const SubsystemQuiescenceRequest& request,
      std::string* error_message) override {
    ++prepare_count;
    prepared_request_id = request.request_id;
    prepared_operation = request.operation;
    events_->push_back(std::string(SubsystemName(subsystem_)) + ".prepare");
    if (prepare_delay != std::chrono::milliseconds::zero()) {
      std::this_thread::sleep_for(prepare_delay);
    }
    if (prepare_result != SubsystemQuiescenceResult::kReached) {
      if (error_message) {
        *error_message = "injected prepare failure";
      }
      // This models the participant contract: failed Prepare has already
      // undone any partial work and is not owned by the coordinator.
      active = false;
      return prepare_result;
    }
    active = true;
    return SubsystemQuiescenceResult::kReached;
  }

  void Abort(uint64_t request_id) noexcept override {
    ++abort_count;
    aborted_matching_request = request_id == prepared_request_id;
    events_->push_back(std::string(SubsystemName(subsystem_)) + ".abort");
    active = false;
  }

  QuiescentSubsystem subsystem_;
  std::vector<std::string>* events_;
  SubsystemQuiescenceResult prepare_result =
      SubsystemQuiescenceResult::kReached;
  std::chrono::milliseconds prepare_delay = std::chrono::milliseconds::zero();
  uint64_t prepared_request_id = 0;
  Operation prepared_operation = Operation::kSave;
  size_t prepare_count = 0;
  size_t abort_count = 0;
  bool active = false;
  bool aborted_matching_request = false;
};

struct TestParticipants {
  explicit TestParticipants(std::vector<std::string>* events)
      : kernel(QuiescentSubsystem::kKernelDispatchAndTimers, events),
        vfs(QuiescentSubsystem::kVfsAndIo, events),
        vulkan(QuiescentSubsystem::kVulkanCommandProcessor, events),
        audio(QuiescentSubsystem::kAudio, events),
        input(QuiescentSubsystem::kInput, events),
        ordered{&kernel, &vfs, &vulkan, &audio, &input} {}

  TestQuiescenceParticipant kernel;
  TestQuiescenceParticipant vfs;
  TestQuiescenceParticipant vulkan;
  TestQuiescenceParticipant audio;
  TestQuiescenceParticipant input;
  std::array<SubsystemQuiescenceParticipant*,
             kRequiredQuiescentSubsystemCount>
      ordered;
};

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

}  // namespace

TEST_CASE("Admission gate drains accepted work and blocks later host work",
          "[save_state][subsystem_quiescence][admission]") {
  ReversibleAdmissionGate gate;
  ReversibleAdmissionGate::Lease active_poll = gate.Enter();
  REQUIRE(active_poll);
  CHECK(gate.Snapshot().in_flight == 1);

  std::atomic<AdmissionGateResult> close_result{
      AdmissionGateResult::kInvalidRequest};
  std::thread closer([&]() {
    close_result.store(gate.CloseAndWait(
        71, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
  });
  REQUIRE(WaitUntil([&]() { return gate.Snapshot().closed; }));

  std::atomic<bool> vibration_started{false};
  std::atomic<bool> vibration_entered{false};
  std::thread pending_vibration([&]() {
    vibration_started.store(true);
    auto admission = gate.Enter();
    vibration_entered.store(true);
  });
  REQUIRE(WaitUntil([&]() { return vibration_started.load(); }));
  CHECK_FALSE(vibration_entered.load());
  CHECK(gate.Snapshot().in_flight == 1);

  active_poll.Reset();
  closer.join();
  CHECK(close_result.load() == AdmissionGateResult::kReached);
  CHECK(gate.Snapshot().closed);
  CHECK(gate.Snapshot().in_flight == 0);
  CHECK_FALSE(vibration_entered.load());

  REQUIRE(gate.Reopen(71));
  pending_vibration.join();
  CHECK(vibration_entered.load());
  const AdmissionGateSnapshot open_snapshot = gate.Snapshot();
  CHECK_FALSE(open_snapshot.closed);
  CHECK(open_snapshot.owner_id == 0);
  CHECK(open_snapshot.in_flight == 0);
}

TEST_CASE("Admission timeout rolls back without stranding fake callbacks",
          "[save_state][subsystem_quiescence][admission]") {
  ReversibleAdmissionGate gate;
  auto callback = gate.Enter();
  ReversibleAdmissionGate::Lease moved_callback(std::move(callback));
  CHECK_FALSE(callback);
  REQUIRE(moved_callback);

  const AdmissionGateResult result = gate.CloseAndWait(
      73, std::chrono::steady_clock::now() + std::chrono::milliseconds(2));

  CHECK(result == AdmissionGateResult::kTimedOut);
  const AdmissionGateSnapshot snapshot = gate.Snapshot();
  CHECK_FALSE(snapshot.closed);
  CHECK(snapshot.owner_id == 0);
  CHECK(snapshot.in_flight == 1);
  moved_callback.Reset();

  auto next_timer_callback = gate.Enter();
  REQUIRE(next_timer_callback);
  next_timer_callback.Reset();
  CHECK(gate.Snapshot().in_flight == 0);
}

TEST_CASE("Admission ownership is failure-closed on conflicts",
          "[save_state][subsystem_quiescence][admission]") {
  ReversibleAdmissionGate gate;
  CHECK(gate.CloseAndWait(
            79, std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
        AdmissionGateResult::kReached);
  CHECK(gate.CloseAndWait(
            83, std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
        AdmissionGateResult::kBusy);
  CHECK_FALSE(gate.Reopen(83));
  CHECK(gate.Snapshot().closed);
  CHECK(gate.Snapshot().owner_id == 79);
  CHECK(gate.Reopen(79));

  CHECK(gate.CloseAndWait(
            0, std::chrono::steady_clock::now() + std::chrono::seconds(1)) ==
        AdmissionGateResult::kInvalidRequest);
  CHECK(gate.CloseAndWait(
            89, std::chrono::steady_clock::now()) ==
        AdmissionGateResult::kTimedOut);
  CHECK_FALSE(gate.Snapshot().closed);
}

TEST_CASE("VFS write completion drains before ordered closure",
          "[save_state][subsystem_quiescence][vfs]") {
  ReversibleAdmissionGate kernel_completion_gate;
  ReversibleAdmissionGate vfs_write_gate;
  std::mutex events_mutex;
  std::vector<std::string> events;
  const auto record = [&](const char* event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    events.emplace_back(event);
  };
  const auto event_index = [&](const char* event) {
    std::lock_guard<std::mutex> lock(events_mutex);
    const auto it = std::find(events.cbegin(), events.cend(), event);
    REQUIRE(it != events.cend());
    return size_t(it - events.cbegin());
  };

  std::atomic<bool> backend_write_finished{false};
  std::atomic<bool> allow_completion{false};
  std::thread admitted_write([&]() {
    auto kernel_completion = kernel_completion_gate.Enter();
    auto vfs_write = vfs_write_gate.Enter();
    record("backend.write");
    backend_write_finished.store(true);
    while (!allow_completion.load()) {
      std::this_thread::yield();
    }
    record("completion.port");
    record("completion.status");
    record("completion.apc");
    record("completion.event");
    record("vfs.release");
    vfs_write.Reset();
    record("kernel.release");
    kernel_completion.Reset();
  });
  REQUIRE(WaitUntil([&]() { return backend_write_finished.load(); }));

  const auto shared_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  std::atomic<AdmissionGateResult> kernel_close_result{
      AdmissionGateResult::kInvalidRequest};
  std::atomic<AdmissionGateResult> vfs_close_result{
      AdmissionGateResult::kInvalidRequest};
  std::thread closer([&]() {
    kernel_close_result.store(
        kernel_completion_gate.CloseAndWait(101, shared_deadline));
    record("kernel.closed");
    if (kernel_close_result.load() == AdmissionGateResult::kReached) {
      vfs_close_result.store(vfs_write_gate.CloseAndWait(101, shared_deadline));
      record("vfs.closed");
    }
  });
  REQUIRE(WaitUntil(
      [&]() { return kernel_completion_gate.Snapshot().closed; }));

  std::atomic<bool> later_write_started{false};
  std::atomic<bool> later_write_admitted{false};
  std::thread later_write([&]() {
    later_write_started.store(true);
    auto kernel_completion = kernel_completion_gate.Enter();
    auto vfs_write = vfs_write_gate.Enter();
    later_write_admitted.store(true);
    record("later.backend.write");
  });
  REQUIRE(WaitUntil([&]() { return later_write_started.load(); }));
  CHECK_FALSE(later_write_admitted.load());

  allow_completion.store(true);
  admitted_write.join();
  closer.join();
  CHECK(kernel_close_result.load() == AdmissionGateResult::kReached);
  CHECK(vfs_close_result.load() == AdmissionGateResult::kReached);
  CHECK_FALSE(later_write_admitted.load());
  CHECK(event_index("backend.write") < event_index("completion.port"));
  CHECK(event_index("completion.port") < event_index("completion.status"));
  CHECK(event_index("completion.status") < event_index("completion.apc"));
  CHECK(event_index("completion.apc") < event_index("completion.event"));
  CHECK(event_index("completion.event") < event_index("kernel.closed"));
  CHECK(event_index("vfs.release") < event_index("kernel.closed"));
  CHECK(event_index("kernel.release") < event_index("kernel.closed"));

  REQUIRE(vfs_write_gate.Reopen(101));
  CHECK_FALSE(later_write_admitted.load());
  REQUIRE(kernel_completion_gate.Reopen(101));
  later_write.join();
  CHECK(later_write_admitted.load());
  CHECK(event_index("vfs.closed") < event_index("later.backend.write"));
}

TEST_CASE("VFS write close timeout reopens without stranding writers",
          "[save_state][subsystem_quiescence][vfs]") {
  ReversibleAdmissionGate vfs_write_gate;
  auto admitted_write = vfs_write_gate.Enter();

  CHECK(vfs_write_gate.CloseAndWait(
            103,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2)) ==
        AdmissionGateResult::kTimedOut);
  const AdmissionGateSnapshot rolled_back = vfs_write_gate.Snapshot();
  CHECK_FALSE(rolled_back.closed);
  CHECK(rolled_back.owner_id == 0);
  CHECK(rolled_back.in_flight == 1);

  auto write_after_rollback = vfs_write_gate.Enter();
  CHECK(vfs_write_gate.Snapshot().in_flight == 2);
  write_after_rollback.Reset();
  admitted_write.Reset();
  CHECK(vfs_write_gate.Snapshot().in_flight == 0);
}

TEST_CASE("Subsystem quiescence owns fixed order and releases in reverse",
          "[save_state][subsystem_quiescence]") {
  std::vector<std::string> events;
  TestParticipants participants(&events);
  HeldSubsystemQuiescence held;
  const SubsystemQuiescenceAcquireResult result =
      SubsystemQuiescenceCoordinator().Acquire(
          Operation::kRestore, 17, std::chrono::seconds(1),
          participants.ordered, &held);

  REQUIRE(result.ok());
  REQUIRE(held);
  CHECK(held.request_id() == 17);
  CHECK(held.prepared_count() == kRequiredQuiescentSubsystemCount);
  for (SubsystemQuiescenceParticipant* participant : participants.ordered) {
    auto* test_participant =
        static_cast<TestQuiescenceParticipant*>(participant);
    CHECK(test_participant->active);
    CHECK(test_participant->prepared_operation == Operation::kRestore);
  }

  held.Release();
  const std::vector<std::string> expected = {
      "kernel.prepare", "vfs.prepare",   "vulkan.prepare",
      "audio.prepare",  "input.prepare", "input.abort",
      "audio.abort",    "vulkan.abort",  "vfs.abort",
      "kernel.abort"};
  CHECK(events == expected);
  for (SubsystemQuiescenceParticipant* participant : participants.ordered) {
    auto* test_participant =
        static_cast<TestQuiescenceParticipant*>(participant);
    CHECK_FALSE(test_participant->active);
    CHECK(test_participant->aborted_matching_request);
  }
}

TEST_CASE("Every subsystem prepare failure rolls back only prior ownership",
          "[save_state][subsystem_quiescence]") {
  for (size_t failed_index = 0;
       failed_index < kRequiredQuiescentSubsystemCount; ++failed_index) {
    DYNAMIC_SECTION("failure at participant " << failed_index) {
      std::vector<std::string> events;
      TestParticipants participants(&events);
      auto* failed = static_cast<TestQuiescenceParticipant*>(
          participants.ordered[failed_index]);
      failed->prepare_result = SubsystemQuiescenceResult::kUnsupported;
      HeldSubsystemQuiescence held;

      const SubsystemQuiescenceAcquireResult result =
          SubsystemQuiescenceCoordinator().Acquire(
              Operation::kSave, 23, std::chrono::seconds(1),
              participants.ordered, &held);

      CHECK(result.result == SubsystemQuiescenceResult::kUnsupported);
      CHECK(result.participant_index == failed_index);
      CHECK(result.subsystem == kSubsystemQuiescenceOrder[failed_index]);
      CHECK_FALSE(held);
      CHECK(failed->prepare_count == 1);
      CHECK(failed->abort_count == 0);
      CHECK_FALSE(failed->active);
      for (size_t index = 0; index < participants.ordered.size(); ++index) {
        auto* participant = static_cast<TestQuiescenceParticipant*>(
            participants.ordered[index]);
        CHECK(participant->prepare_count ==
              (index <= failed_index ? size_t(1) : size_t(0)));
        CHECK(participant->abort_count ==
              (index < failed_index ? size_t(1) : size_t(0)));
        CHECK_FALSE(participant->active);
      }
    }
  }
}

TEST_CASE("Invalid subsystem ownership configurations perform no work",
          "[save_state][subsystem_quiescence]") {
  std::vector<std::string> events;
  TestParticipants participants(&events);
  HeldSubsystemQuiescence held;

  auto invalid = participants.ordered;
  std::swap(invalid[1], invalid[2]);
  SubsystemQuiescenceAcquireResult result =
      SubsystemQuiescenceCoordinator().Acquire(
          Operation::kSave, 31, std::chrono::seconds(1), invalid, &held);
  CHECK(result.result ==
        SubsystemQuiescenceResult::kInvalidConfiguration);
  CHECK(events.empty());

  invalid = participants.ordered;
  invalid[2] = invalid[1];
  result = SubsystemQuiescenceCoordinator().Acquire(
      Operation::kSave, 32, std::chrono::seconds(1), invalid, &held);
  CHECK(result.result ==
        SubsystemQuiescenceResult::kInvalidConfiguration);
  CHECK(events.empty());

  invalid = participants.ordered;
  invalid[3] = nullptr;
  result = SubsystemQuiescenceCoordinator().Acquire(
      Operation::kSave, 33, std::chrono::seconds(1), invalid, &held);
  CHECK(result.result ==
        SubsystemQuiescenceResult::kInvalidConfiguration);
  CHECK(events.empty());
}

TEST_CASE("Late participant success is aborted and reported as timeout",
          "[save_state][subsystem_quiescence]") {
  std::vector<std::string> events;
  TestParticipants participants(&events);
  participants.kernel.prepare_delay = std::chrono::milliseconds(5);
  HeldSubsystemQuiescence held;

  const SubsystemQuiescenceAcquireResult result =
      SubsystemQuiescenceCoordinator().Acquire(
          Operation::kRestore, 41, std::chrono::milliseconds(1),
          participants.ordered, &held);

  CHECK(result.result == SubsystemQuiescenceResult::kTimedOut);
  CHECK(result.participant_index == 0);
  CHECK_FALSE(held);
  CHECK(participants.kernel.prepare_count == 1);
  CHECK(participants.kernel.abort_count == 1);
  CHECK(participants.kernel.aborted_matching_request);
  CHECK_FALSE(participants.kernel.active);
  CHECK(participants.vfs.prepare_count == 0);
  const std::vector<std::string> expected = {"kernel.prepare",
                                             "kernel.abort"};
  CHECK(events == expected);
}

TEST_CASE("Moved subsystem quiescence retains sole release ownership",
          "[save_state][subsystem_quiescence]") {
  std::vector<std::string> events;
  TestParticipants participants(&events);

  {
    HeldSubsystemQuiescence source;
    REQUIRE(SubsystemQuiescenceCoordinator()
                .Acquire(Operation::kSave, 53, std::chrono::seconds(1),
                         participants.ordered, &source)
                .ok());
    HeldSubsystemQuiescence destination(std::move(source));
    CHECK_FALSE(source);
    REQUIRE(destination);
    CHECK(destination.request_id() == 53);
  }

  for (SubsystemQuiescenceParticipant* participant : participants.ordered) {
    const auto* test_participant =
        static_cast<const TestQuiescenceParticipant*>(participant);
    CHECK(test_participant->abort_count == 1);
    CHECK_FALSE(test_participant->active);
  }
  const std::vector<std::string> expected = {
      "kernel.prepare", "vfs.prepare",   "vulkan.prepare",
      "audio.prepare",  "input.prepare", "input.abort",
      "audio.abort",    "vulkan.abort",  "vfs.abort",
      "kernel.abort"};
  CHECK(events == expected);
}

}  // namespace xe::save_state::test
