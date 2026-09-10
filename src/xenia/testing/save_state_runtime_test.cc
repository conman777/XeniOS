/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/guest_interrupt_clock.h"
#include "xenia/cpu/backend/a64/a64_resume.h"
#if defined(XE_SAVE_STATE_ENABLE_A64_ADAPTER_TESTS)
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/backend/a64/a64_save_state.h"
#include "xenia/cpu/ppc/ppc_context.h"
#endif
#include "xenia/save_state_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <future>
#include <new>
#include <thread>

namespace xe::save_state::test {
namespace {

bool WaitForGeneration(LiveGuestRuntime* runtime, uint64_t* generation) {
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

class TestA64ResumeTargetLookup final : public A64ResumeTargetLookup {
 public:
  StateProviderResult Lookup(uint32_t guest_pc, uintptr_t* host_entry,
                             uint32_t* host_stack_size,
                             std::string* error_message) override {
    calls.push_back(guest_pc);
    if (guest_pc == fail_guest_pc) {
      if (error_message) {
        *error_message = "injected A64 target lookup failure";
      }
      return StateProviderResult::kUnsupported;
    }
    *host_entry = uintptr_t(0x10000000) + (guest_pc & 0x00FFFFFC);
    *host_stack_size = stack_size;
    return StateProviderResult::kOk;
  }

  uint32_t fail_guest_pc = 0;
  uint32_t stack_size = 0x40;
  std::vector<uint32_t> calls;
};

#if defined(XE_SAVE_STATE_ENABLE_A64_ADAPTER_TESTS)
struct TestA64Context {
  alignas(64) std::array<
      std::byte, 128 + sizeof(cpu::ppc::PPCContext)> storage = {};

  cpu::ppc::PPCContext* context() {
    return reinterpret_cast<cpu::ppc::PPCContext*>(storage.data() + 128);
  }
  cpu::backend::a64::A64BackendContext* backend_context() {
    return reinterpret_cast<cpu::backend::a64::A64BackendContext*>(
        reinterpret_cast<std::byte*>(context()) -
        sizeof(cpu::backend::a64::A64BackendContext));
  }
  std::array<cpu::backend::a64::A64BackendStackpoint, 2> stackpoints = {};
};
#endif

}  // namespace

TEST_CASE("Live runtime freezes enrollment for a held boundary",
          "[save_state][runtime][barrier]") {
  LiveGuestRuntime runtime;
  uint64_t first_context = 1;
  uint64_t second_context = 2;
  uint64_t third_context = 3;
  REQUIRE(runtime.RegisterThread(2, &second_context));
  REQUIRE(runtime.RegisterThread(1, &first_context));
  CHECK_FALSE(runtime.RegisterThread(1, &first_context));

  LiveBoundaryError error = LiveBoundaryError::kNone;
  auto acquire = std::async(std::launch::async, [&]() {
    return runtime.AcquireBoundary(std::chrono::seconds(2), &error);
  });
  uint64_t generation = 0;
  REQUIRE(WaitForGeneration(&runtime, &generation));
  std::array<BarrierPollResult, 2> poll_results = {};
  std::thread first(
      [&]() { poll_results[0] = runtime.Poll(1, generation); });
  std::thread second(
      [&]() { poll_results[1] = runtime.Poll(2, generation); });

  HeldGuestBoundary boundary = acquire.get();
  REQUIRE(boundary);
  REQUIRE(boundary.bindings().size() == 2);
  CHECK(boundary.bindings()[0].thread_id == 1);
  CHECK(boundary.bindings()[0].context == &first_context);
  CHECK(boundary.bindings()[1].thread_id == 2);

  std::atomic<bool> registration_finished{false};
  std::thread registration([&]() {
    runtime.RegisterThread(3, &third_context);
    registration_finished.store(true, std::memory_order_release);
  });
  const auto blocked_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  while (std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  CHECK_FALSE(registration_finished.load(std::memory_order_acquire));

  boundary.Reset();
  first.join();
  second.join();
  registration.join();
  CHECK(registration_finished.load(std::memory_order_acquire));
  CHECK(runtime.registered_thread_count() == 3);
  CHECK(poll_results[0] == BarrierPollResult::kReleased);
  CHECK(poll_results[1] == BarrierPollResult::kReleased);
}

TEST_CASE("Live runtime timeout rolls back and unfreezes enrollment",
          "[save_state][runtime][barrier]") {
  LiveGuestRuntime runtime;
  uint64_t context = 1;
  uint64_t late_context = 2;
  REQUIRE(runtime.RegisterThread(1, &context));

  LiveBoundaryError error = LiveBoundaryError::kNone;
  HeldGuestBoundary boundary =
      runtime.AcquireBoundary(std::chrono::milliseconds(20), &error);
  CHECK_FALSE(boundary);
  CHECK(error == LiveBoundaryError::kTimedOut);
  CHECK(runtime.requested_generation() == 0);
  CHECK(runtime.RegisterThread(2, &late_context));
  CHECK(runtime.registered_thread_count() == 2);
  CHECK(runtime.UnregisterThread(1) == &context);
  CHECK(runtime.UnregisterThread(1) == nullptr);
}

TEST_CASE("A64 resume planner rebuilds captured guest return segments",
          "[save_state][runtime][cpu]") {
  CpuThreadSnapshot thread;
  thread.thread_id = 1;
  thread.resume_pc = 0x82001000;
  thread.stackpoints = {
      {0x70000000, 0x82002000, 0x40},
      {0x70001000, 0x82003000, 0x40},
      {0x70002000, 0xBCBCBCBC, 0x40},
  };

  std::vector<A64ResumeSegment> segments;
  std::string error;
  REQUIRE(BuildA64ResumeSegments(thread, &segments, &error) ==
          StateProviderResult::kOk);
  REQUIRE(segments.size() == 3);
  CHECK(segments[0].guest_pc == 0x82001000);
  CHECK(segments[0].guest_stack_pointer == 0x70000000);
  CHECK(segments[0].guest_return_address == 0x82002000);
  CHECK(segments[1].guest_pc == 0x82002000);
  CHECK(segments[1].guest_return_address == 0x82003000);
  CHECK(segments[2].guest_pc == 0x82003000);
  CHECK(segments[2].guest_return_address == 0xBCBCBCBC);

  thread.stackpoints[1].guest_stack_pointer |= 1;
  CHECK(BuildA64ResumeSegments(thread, &segments, &error) ==
        StateProviderResult::kInvalidState);
  CHECK_FALSE(error.empty());
}

TEST_CASE("A64 resume dispatch preflights every existing target atomically",
          "[save_state][runtime][cpu]") {
  CpuThreadSnapshot thread;
  thread.thread_id = 1;
  thread.resume_pc = 0x82001000;
  thread.stackpoints = {
      {0x70000000, 0x82002000, 0x40},
      {0x70001000, 0x82003000, 0x40},
      {0x70002000, 0xBCBCBCBC, 0x40},
  };

  TestA64ResumeTargetLookup lookup;
  A64ResumeDispatchPlan plan;
  std::string error;
  REQUIRE(BuildA64ResumeDispatchPlan(thread, lookup, &plan, &error) ==
          StateProviderResult::kOk);
  REQUIRE(plan.segments.size() == 3);
  CHECK(plan.segments[0].guest.guest_pc == 0x82001000);
  CHECK(plan.segments[0].guest.guest_stack_pointer == 0x70000000);
  CHECK(plan.segments[1].guest.guest_pc == 0x82002000);
  CHECK(plan.segments[2].guest.guest_pc == 0x82003000);
  CHECK(plan.segments[0].host_entry != 0);
  CHECK(plan.segments[0].host_stack_size == 0x40);

  const A64ResumeDispatchPlan previous = plan;
  lookup.fail_guest_pc = 0x82002000;
  CHECK(BuildA64ResumeDispatchPlan(thread, lookup, &plan, &error) ==
        StateProviderResult::kUnsupported);
  REQUIRE(plan.segments.size() == previous.segments.size());
  CHECK(plan.segments[0].host_entry == previous.segments[0].host_entry);
  CHECK_FALSE(error.empty());

  lookup.fail_guest_pc = 0;
  lookup.stack_size = 0x50;
  CHECK(BuildA64ResumeDispatchPlan(thread, lookup, &plan, &error) ==
        StateProviderResult::kInvalidState);
  CHECK(plan.segments[0].host_entry == previous.segments[0].host_entry);
}

TEST_CASE("Guest interrupt clock restores an exact virtual time anchor",
          "[save_state][runtime][clock]") {
  GuestInterruptClock clock;
  REQUIRE(clock.Reset(50000000, 123000000, 50000000));
  CHECK(clock.Query(50000000) == 123000000);
  CHECK(clock.Query(75000000) == 128000000);
  CHECK(clock.Query(100000000) == 133000000);
  CHECK(clock.Query(1) == 123000000);

  REQUIRE(clock.Reset(900, 777, 3));
  CHECK(clock.Query(903) == 10000777);
  CHECK_FALSE(clock.Reset(0, 0, 0));
}

#if defined(XE_SAVE_STATE_ENABLE_A64_ADAPTER_TESTS)
TEST_CASE("A64 CPU adapter captures complete synchronized architecture state",
          "[save_state][runtime][cpu]") {
  TestA64Context storage;
  auto* context = new (storage.context()) cpu::ppc::PPCContext();
  auto* backend_context =
      new (storage.backend_context()) cpu::backend::a64::A64BackendContext();
  context->thread_id = 7;
  context->last_guest_pc = 0x82001234;
  context->r[3] = UINT64_C(0x1122334455667788);
  context->ctr = 0x1010;
  context->lr = 0x2020;
  context->msr = 0x3030;
  const uint64_t fpr_bits = UINT64_C(0x7FF8000000001234);
  std::memcpy(&context->f[4], &fpr_bits, sizeof(fpr_bits));
  context->v[5].u64[0] = UINT64_C(0xAABBCCDDEEFF0011);
  context->cr6.value = 0x01020304;
  context->fpscr.value = 0x12345678;
  context->fpscr.bits.ni = 1;
  context->vrsave = 0x88776655;
  context->xer_ca = 1;
  context->xer_so = 1;
  context->vscr_sat = 1;
  backend_context->non_ieee_mode = 1;
  backend_context->njm_enabled = 0;
  storage.stackpoints[0].guest_return_address = 0xBCBCBCBC;
  storage.stackpoints[0].guest_sp = 0x70001000;
  storage.stackpoints[0].stack_size = 0x50;
  storage.stackpoints[1].guest_return_address = 0x82002000;
  storage.stackpoints[1].guest_sp = 0x70000000;
  storage.stackpoints[1].stack_size = 0x40;
  backend_context->stackpoints = storage.stackpoints.data();
  backend_context->current_stackpoint_depth =
      uint32_t(storage.stackpoints.size());

  LiveGuestRuntime runtime;
  REQUIRE(runtime.RegisterThread(context->thread_id, context));
  LiveBoundaryError boundary_error = LiveBoundaryError::kNone;
  auto acquire = std::async(std::launch::async, [&]() {
    return runtime.AcquireBoundary(std::chrono::seconds(2), &boundary_error);
  });
  uint64_t generation = 0;
  REQUIRE(WaitForGeneration(&runtime, &generation));
  BarrierPollResult poll_result = BarrierPollResult::kNotRequested;
  std::thread poll([&]() {
    poll_result = runtime.Poll(context->thread_id, generation);
  });
  HeldGuestBoundary boundary = acquire.get();
  REQUIRE(boundary);

  CpuSnapshot snapshot;
  std::string error;
  REQUIRE(CaptureA64CpuSnapshot(boundary, &snapshot, &error) ==
          StateProviderResult::kOk);
  REQUIRE(snapshot.threads.size() == 1);
  const CpuThreadSnapshot& thread = snapshot.threads.front();
  CHECK(thread.thread_id == 7);
  CHECK(thread.resume_pc == context->last_guest_pc);
  CHECK(thread.gpr[3] == context->r[3]);
  CHECK(thread.fpr_bits[4] == fpr_bits);
  CHECK(thread.vector_registers[5][0] == context->v[5].u8[0]);
  CHECK(thread.condition_registers[6] == context->cr6.value);
  CHECK(thread.njm_enabled == 0);
  CHECK(thread.stackpoints[0].guest_stack_pointer == 0x70000000);
  CHECK(thread.stackpoints[0].guest_return_address == 0x82002000);
  CHECK(thread.stackpoints[0].host_stack_size == 0x40);
  CHECK(thread.stackpoints[1].guest_stack_pointer == 0x70001000);
  CHECK(thread.stackpoints[1].guest_return_address == 0xBCBCBCBC);
  CHECK(thread.stackpoints[1].host_stack_size == 0x50);

  backend_context->flags =
      1u << cpu::backend::a64::kA64BackendHasReserveBit;
  CHECK(CaptureA64CpuSnapshot(boundary, &snapshot, &error) ==
        StateProviderResult::kUnsupported);
  CHECK_FALSE(error.empty());
  boundary.Reset();
  poll.join();
  CHECK(poll_result == BarrierPollResult::kReleased);
}
#endif

}  // namespace xe::save_state::test
