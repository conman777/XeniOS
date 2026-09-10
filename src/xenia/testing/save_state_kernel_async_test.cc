/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/save_state_kernel_async.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

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

}  // namespace

TEST_CASE("Kernel async admission drains work and blocks later mutations",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  auto active_apc = boundary.Enter(KernelAsyncDomain::kApc);
  REQUIRE(active_apc);
  REQUIRE(active_apc.RecordEnqueued());

  std::atomic<KernelAsyncCloseResult> close_result{
      KernelAsyncCloseResult::kInvalidRequest};
  std::thread closer([&]() {
    close_result.store(boundary.CloseAndWait(
        KernelAsyncDomain::kApc, 211,
        std::chrono::steady_clock::now() + std::chrono::seconds(1),
        KernelPendingPolicy::kAllowStablePending));
  });
  REQUIRE(WaitUntil([&]() {
    return boundary.Snapshot(KernelAsyncDomain::kApc).admission.closed;
  }));

  std::atomic<bool> later_started{false};
  std::atomic<bool> later_entered{false};
  std::thread later_apc([&]() {
    later_started.store(true);
    auto operation = boundary.Enter(KernelAsyncDomain::kApc);
    later_entered.store(true);
  });
  REQUIRE(WaitUntil([&]() { return later_started.load(); }));
  CHECK_FALSE(later_entered.load());

  active_apc.Reset();
  closer.join();
  CHECK(close_result.load() == KernelAsyncCloseResult::kReached);
  CHECK(boundary.Snapshot(KernelAsyncDomain::kApc).pending_items == 1);
  CHECK_FALSE(later_entered.load());

  REQUIRE(boundary.Reopen(KernelAsyncDomain::kApc, 211));
  later_apc.join();
  CHECK(later_entered.load());

  auto dequeue = boundary.Enter(KernelAsyncDomain::kApc);
  REQUIRE(dequeue.RecordDequeued());
  CHECK(boundary.Snapshot(KernelAsyncDomain::kApc).pending_items == 0);
}

TEST_CASE("Pending DPC state is rejected and admission reopens",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  {
    auto insert = boundary.Enter(KernelAsyncDomain::kDpc);
    REQUIRE(insert.RecordEnqueued());
  }

  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kDpc, 223,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            KernelPendingPolicy::kRequireEmpty) ==
        KernelAsyncCloseResult::kUnsupportedPendingState);
  KernelAsyncSnapshot snapshot =
      boundary.Snapshot(KernelAsyncDomain::kDpc);
  CHECK_FALSE(snapshot.admission.closed);
  CHECK(snapshot.pending_items == 1);

  auto remove = boundary.Enter(KernelAsyncDomain::kDpc);
  REQUIRE(remove.RecordDequeued());
  remove.Reset();
  REQUIRE(boundary.CloseAndWait(
              KernelAsyncDomain::kDpc, 227,
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              KernelPendingPolicy::kRequireEmpty) ==
          KernelAsyncCloseResult::kReached);
  CHECK(boundary.Reopen(KernelAsyncDomain::kDpc, 227));
}

TEST_CASE("Kernel async accounting underflow permanently fails closed",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  auto invalid_completion = boundary.Enter(KernelAsyncDomain::kNotification);
  CHECK_FALSE(invalid_completion.RecordDequeued());
  invalid_completion.Reset();

  const KernelAsyncSnapshot snapshot =
      boundary.Snapshot(KernelAsyncDomain::kNotification);
  CHECK(snapshot.pending_items == 0);
  CHECK(snapshot.accounting_error);
  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kNotification, 229,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            KernelPendingPolicy::kAllowStablePending) ==
        KernelAsyncCloseResult::kAccountingError);
  CHECK_FALSE(boundary.Snapshot(KernelAsyncDomain::kNotification)
                  .admission.closed);
}

TEST_CASE("Kernel async accounting overflow permanently fails closed",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  auto operation = boundary.Enter(KernelAsyncDomain::kApc);
  REQUIRE(operation.RecordEnqueued(SIZE_MAX));
  CHECK_FALSE(operation.RecordEnqueued());
  operation.Reset();

  const KernelAsyncSnapshot snapshot =
      boundary.Snapshot(KernelAsyncDomain::kApc);
  CHECK(snapshot.pending_items == SIZE_MAX);
  CHECK(snapshot.accounting_error);
  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kApc, 231,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            KernelPendingPolicy::kAllowStablePending) ==
        KernelAsyncCloseResult::kAccountingError);
  CHECK_FALSE(
      boundary.Snapshot(KernelAsyncDomain::kApc).admission.closed);
}

TEST_CASE("Kernel wait and timer timeout rollback leaves domains usable",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  auto active_wait = boundary.Enter(KernelAsyncDomain::kWait);
  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kWait, 233,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2),
            KernelPendingPolicy::kRequireEmpty) ==
        KernelAsyncCloseResult::kTimedOut);
  CHECK_FALSE(
      boundary.Snapshot(KernelAsyncDomain::kWait).admission.closed);
  active_wait.Reset();

  auto timer_fire = boundary.Enter(KernelAsyncDomain::kTimer);
  REQUIRE(timer_fire);
  REQUIRE(timer_fire.RecordEnqueued());
  timer_fire.Reset();
  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kTimer, 237,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            KernelPendingPolicy::kRequireEmpty) ==
        KernelAsyncCloseResult::kUnsupportedPendingState);

  auto timer_cancel = boundary.Enter(KernelAsyncDomain::kTimer);
  REQUIRE(timer_cancel.RecordDequeued());
  timer_cancel.Reset();
  REQUIRE(boundary.CloseAndWait(
              KernelAsyncDomain::kTimer, 239,
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              KernelPendingPolicy::kRequireEmpty) ==
          KernelAsyncCloseResult::kReached);
  CHECK(boundary.CloseAndWait(
            KernelAsyncDomain::kTimer, 241,
            std::chrono::steady_clock::now() + std::chrono::seconds(1),
            KernelPendingPolicy::kRequireEmpty) ==
        KernelAsyncCloseResult::kBusy);
  CHECK_FALSE(boundary.Reopen(KernelAsyncDomain::kTimer, 241));
  CHECK(boundary.Reopen(KernelAsyncDomain::kTimer, 239));
}

TEST_CASE("Kernel notification accounting remains exact across producers",
          "[save_state][kernel][async]") {
  KernelAsyncAdmissionBoundary boundary;
  {
    auto broadcast = boundary.Enter(KernelAsyncDomain::kNotification);
    REQUIRE(broadcast.RecordEnqueued(3));
  }
  {
    auto dequeue = boundary.Enter(KernelAsyncDomain::kNotification);
    REQUIRE(dequeue.RecordDequeued(2));
  }
  const KernelAsyncSnapshot snapshot =
      boundary.Snapshot(KernelAsyncDomain::kNotification);
  CHECK(snapshot.pending_items == 1);
  CHECK_FALSE(snapshot.accounting_error);

  REQUIRE(boundary.CloseAndWait(
              KernelAsyncDomain::kNotification, 251,
              std::chrono::steady_clock::now() + std::chrono::seconds(1),
              KernelPendingPolicy::kAllowStablePending) ==
          KernelAsyncCloseResult::kReached);
  CHECK(boundary.Reopen(KernelAsyncDomain::kNotification, 251));
}

TEST_CASE("Kernel wait inventory rejects unsupported object semantics",
          "[save_state][kernel][async][inventory]") {
  constexpr std::array known = {
      KernelWaitObjectKind::kEvent, KernelWaitObjectKind::kMutant,
      KernelWaitObjectKind::kNotifyListener,
      KernelWaitObjectKind::kSemaphore, KernelWaitObjectKind::kThread,
  };
  const KernelWaitInventoryResult known_result =
      ValidateKernelWaitObjectInventory(known.data(), known.size());
  CHECK(known_result.supported);
  CHECK(known_result.object_index == SIZE_MAX);

  constexpr std::array unsupported = {
      KernelWaitObjectKind::kEvent,
      KernelWaitObjectKind::kTimer,
      KernelWaitObjectKind::kIoCompletion,
  };
  const KernelWaitInventoryResult unsupported_result =
      ValidateKernelWaitObjectInventory(unsupported.data(),
                                        unsupported.size());
  CHECK_FALSE(unsupported_result.supported);
  CHECK(unsupported_result.object_index == 1);
  CHECK(unsupported_result.object_kind == KernelWaitObjectKind::kTimer);

  const KernelWaitInventoryResult invalid =
      ValidateKernelWaitObjectInventory(nullptr, 1);
  CHECK_FALSE(invalid.supported);
  CHECK(invalid.object_index == 0);
  CHECK(invalid.object_kind == KernelWaitObjectKind::kUnknown);
}

}  // namespace xe::save_state::test
