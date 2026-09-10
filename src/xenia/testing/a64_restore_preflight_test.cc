/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/a64/a64_restore_preflight.h"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <thread>

namespace xe::save_state::test {
namespace {

bool WaitForRequestedGeneration(LiveGuestRuntime* runtime,
                                uint64_t* generation) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  do {
    *generation = runtime->requested_generation();
    if (*generation) {
      return true;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return false;
}

CpuSnapshot MakeCpuSnapshot() {
  CpuSnapshot snapshot;
  snapshot.host_backend = HostBackend::kA64;
  CpuThreadSnapshot first;
  first.thread_id = 1;
  first.resume_pc = 0x82001000;
  first.gpr[1] = 0x70000000;
  first.stackpoints = {{0x70000000, 0x82002000, 0x40}};
  CpuThreadSnapshot second;
  second.thread_id = 2;
  second.resume_pc = 0x83001000;
  second.gpr[1] = 0x71000000;
  second.stackpoints = {{0x71000000, 0x83002000, 0x40}};
  snapshot.threads = {first, second};
  return snapshot;
}

struct TestCodeGenerationState {
  uint64_t generation = 7;
  bool frozen = false;
  bool fail_acquire = false;
  size_t acquire_calls = 0;
  size_t lookup_calls = 0;
  size_t invalidate_after_lookup = SIZE_MAX;
};

class TestFrozenCodeGeneration final : public A64FrozenCodeGeneration {
 public:
  explicit TestFrozenCodeGeneration(TestCodeGenerationState& state)
      : state_(state), acquired_generation_(state.generation) {}
  ~TestFrozenCodeGeneration() override { state_.frozen = false; }

  uint64_t generation() const override { return acquired_generation_; }

  StateProviderResult Validate(std::string* error_message) const override {
    if (!state_.frozen || state_.generation != acquired_generation_) {
      if (error_message) {
        *error_message = "injected stale code generation";
      }
      return StateProviderResult::kInvalidState;
    }
    return StateProviderResult::kOk;
  }

  StateProviderResult Lookup(uint32_t guest_pc, uintptr_t* host_entry,
                             uint32_t* host_stack_size,
                             std::string* error_message) override {
    if (Validate(error_message) != StateProviderResult::kOk) {
      return StateProviderResult::kInvalidState;
    }
    ++state_.lookup_calls;
    *host_entry = uintptr_t(0x10000000) + (guest_pc & 0x00FFFFFC);
    *host_stack_size = 0x40;
    if (state_.lookup_calls == state_.invalidate_after_lookup) {
      ++state_.generation;
    }
    return StateProviderResult::kOk;
  }

 private:
  TestCodeGenerationState& state_;
  uint64_t acquired_generation_;
};

class TestCodeGenerationControl final : public A64CodeGenerationControl {
 public:
  explicit TestCodeGenerationControl(TestCodeGenerationState& state)
      : state_(state) {}

  StateProviderResult AcquireFreeze(
      std::chrono::milliseconds timeout,
      std::unique_ptr<A64FrozenCodeGeneration>* output,
      std::string* error_message) override {
    ++state_.acquire_calls;
    if (timeout <= std::chrono::milliseconds::zero() || !output ||
        *output || state_.frozen || state_.fail_acquire) {
      if (error_message) {
        *error_message = "injected code-generation freeze failure";
      }
      return StateProviderResult::kFailed;
    }
    state_.frozen = true;
    *output = std::make_unique<TestFrozenCodeGeneration>(state_);
    return StateProviderResult::kOk;
  }

 private:
  TestCodeGenerationState& state_;
};

class TestDetachedPreparation final
    : public A64DetachedReentryPreparation {
 public:
  TestDetachedPreparation(uint32_t thread_id, size_t& active_count)
      : thread_id_(thread_id), active_count_(active_count) {
    ++active_count_;
  }
  ~TestDetachedPreparation() override { --active_count_; }
  uint32_t thread_id() const override { return thread_id_; }

 private:
  uint32_t thread_id_;
  size_t& active_count_;
};

class TestReentryDispatcher final : public A64ReentryDispatcher {
 public:
  StateProviderResult PrepareDetached(
      const A64PreparedThreadReentry& thread,
      std::unique_ptr<A64DetachedReentryPreparation>* output,
      std::string* error_message) override {
    calls.push_back(thread.thread_id);
    observed_contexts[thread.thread_id] = thread.live_context_identity;
    if (!output || *output || thread.thread_id == fail_thread_id ||
        thread.dispatch.segments.empty()) {
      if (error_message) {
        *error_message = "injected detached re-entry preparation failure";
      }
      return StateProviderResult::kFailed;
    }
    *output =
        std::make_unique<TestDetachedPreparation>(thread.thread_id,
                                                  active_preparations);
    return StateProviderResult::kOk;
  }

  uint32_t fail_thread_id = 0;
  size_t active_preparations = 0;
  std::vector<uint32_t> calls;
  std::map<uint32_t, const void*> observed_contexts;
};

}  // namespace

TEST_CASE("A64 restore preflight exclusively owns every parked guest thread",
          "[save_state][runtime][cpu][preflight]") {
  LiveGuestRuntime runtime;
  uint64_t first_context = UINT64_C(0x1111222233334444);
  uint64_t second_context = UINT64_C(0x5555666677778888);
  REQUIRE(runtime.RegisterThread(1, &first_context));
  REQUIRE(runtime.RegisterThread(2, &second_context));

  LiveBoundaryError boundary_error = LiveBoundaryError::kNone;
  auto acquire = std::async(std::launch::async, [&]() {
    return runtime.AcquireBoundary(std::chrono::seconds(2), &boundary_error);
  });
  uint64_t generation = 0;
  REQUIRE(WaitForRequestedGeneration(&runtime, &generation));
  std::array<BarrierPollResult, 2> poll_results = {};
  std::thread first_poll(
      [&]() { poll_results[0] = runtime.Poll(1, generation); });
  std::thread second_poll(
      [&]() { poll_results[1] = runtime.Poll(2, generation); });
  HeldGuestBoundary boundary = acquire.get();
  REQUIRE(boundary);

  TestCodeGenerationState code_state;
  TestCodeGenerationControl code_control(code_state);
  TestReentryDispatcher dispatcher;
  A64RestorePreflight preflight;
  std::string error;
  REQUIRE(PrepareA64RestorePreflight(
              std::move(boundary), MakeCpuSnapshot(), code_control, dispatcher,
              std::chrono::milliseconds(100), &preflight, &error) ==
          StateProviderResult::kOk);
  REQUIRE(preflight);
  CHECK(preflight.boundary_generation() == generation);
  CHECK(preflight.code_generation() == 7);
  REQUIRE(preflight.threads().size() == 2);
  CHECK(preflight.threads()[0].thread_id == 1);
  CHECK(preflight.threads()[1].thread_id == 2);
  CHECK(dispatcher.observed_contexts[1] == &first_context);
  CHECK(dispatcher.observed_contexts[2] == &second_context);
  CHECK(dispatcher.active_preparations == 2);
  CHECK(code_state.frozen);
  CHECK(runtime.requested_generation() == generation);
  CHECK(first_context == UINT64_C(0x1111222233334444));
  CHECK(second_context == UINT64_C(0x5555666677778888));

  preflight.Reset();
  first_poll.join();
  second_poll.join();
  CHECK(dispatcher.active_preparations == 0);
  CHECK_FALSE(code_state.frozen);
  CHECK(runtime.requested_generation() == 0);
  CHECK(poll_results[0] == BarrierPollResult::kReleased);
  CHECK(poll_results[1] == BarrierPollResult::kReleased);
  CHECK(first_context == UINT64_C(0x1111222233334444));
  CHECK(second_context == UINT64_C(0x5555666677778888));
}

TEST_CASE("A64 restore preflight failure rolls back all detached ownership",
          "[save_state][runtime][cpu][preflight]") {
  LiveGuestRuntime runtime;
  uint64_t first_context = UINT64_C(0x0102030405060708);
  uint64_t second_context = UINT64_C(0x1112131415161718);
  REQUIRE(runtime.RegisterThread(1, &first_context));
  REQUIRE(runtime.RegisterThread(2, &second_context));

  LiveBoundaryError boundary_error = LiveBoundaryError::kNone;
  auto acquire = std::async(std::launch::async, [&]() {
    return runtime.AcquireBoundary(std::chrono::seconds(2), &boundary_error);
  });
  uint64_t generation = 0;
  REQUIRE(WaitForRequestedGeneration(&runtime, &generation));
  std::thread first_poll([&]() { runtime.Poll(1, generation); });
  std::thread second_poll([&]() { runtime.Poll(2, generation); });
  HeldGuestBoundary boundary = acquire.get();
  REQUIRE(boundary);

  TestCodeGenerationState code_state;
  TestCodeGenerationControl code_control(code_state);
  TestReentryDispatcher dispatcher;
  dispatcher.fail_thread_id = 2;
  A64RestorePreflight preflight;
  std::string error;
  CHECK(PrepareA64RestorePreflight(
            std::move(boundary), MakeCpuSnapshot(), code_control, dispatcher,
            std::chrono::milliseconds(100), &preflight, &error) ==
        StateProviderResult::kFailed);
  first_poll.join();
  second_poll.join();
  CHECK_FALSE(preflight);
  const std::vector<uint32_t> expected_calls = {1, 2};
  CHECK(dispatcher.calls == expected_calls);
  CHECK(dispatcher.active_preparations == 0);
  CHECK_FALSE(code_state.frozen);
  CHECK(runtime.requested_generation() == 0);
  CHECK(first_context == UINT64_C(0x0102030405060708));
  CHECK(second_context == UINT64_C(0x1112131415161718));
  CHECK_FALSE(error.empty());
}

TEST_CASE("A64 restore preflight rejects a stale frozen code generation",
          "[save_state][runtime][cpu][preflight]") {
  LiveGuestRuntime runtime;
  uint64_t first_context = 1;
  uint64_t second_context = 2;
  REQUIRE(runtime.RegisterThread(1, &first_context));
  REQUIRE(runtime.RegisterThread(2, &second_context));

  LiveBoundaryError boundary_error = LiveBoundaryError::kNone;
  auto acquire = std::async(std::launch::async, [&]() {
    return runtime.AcquireBoundary(std::chrono::seconds(2), &boundary_error);
  });
  uint64_t generation = 0;
  REQUIRE(WaitForRequestedGeneration(&runtime, &generation));
  std::thread first_poll([&]() { runtime.Poll(1, generation); });
  std::thread second_poll([&]() { runtime.Poll(2, generation); });
  HeldGuestBoundary boundary = acquire.get();
  REQUIRE(boundary);

  TestCodeGenerationState code_state;
  code_state.invalidate_after_lookup = 1;
  TestCodeGenerationControl code_control(code_state);
  TestReentryDispatcher dispatcher;
  A64RestorePreflight preflight;
  std::string error;
  CHECK(PrepareA64RestorePreflight(
            std::move(boundary), MakeCpuSnapshot(), code_control, dispatcher,
            std::chrono::milliseconds(100), &preflight, &error) ==
        StateProviderResult::kInvalidState);
  first_poll.join();
  second_poll.join();
  CHECK_FALSE(preflight);
  CHECK(dispatcher.calls.empty());
  CHECK(dispatcher.active_preparations == 0);
  CHECK_FALSE(code_state.frozen);
  CHECK(runtime.requested_generation() == 0);
  CHECK(first_context == 1);
  CHECK(second_context == 2);
  CHECK_FALSE(error.empty());
}

}  // namespace xe::save_state::test
