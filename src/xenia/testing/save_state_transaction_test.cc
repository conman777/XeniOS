/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_memory_staging.h"
#include "xenia/save_state_transaction.h"

#include <cstring>
#include <string>
#include <vector>

namespace xe::save_state::test {
namespace {

ClockSnapshot ClockState(uint64_t tick_count) {
  ClockSnapshot clock;
  clock.guest_tick_count = tick_count;
  clock.guest_tick_frequency = 50000000;
  clock.tick_ratio_numerator = 1;
  clock.tick_ratio_denominator = 1;
  clock.guest_system_time_base =
      UINT64_C(133801632000000000) + tick_count;
  clock.guest_interrupt_time = UINT64_C(10000000) + tick_count;
  const double scalar = 1.0;
  std::memcpy(&clock.time_scalar_bits, &scalar, sizeof(scalar));
  return clock;
}

MemoryPageSnapshot MemoryPage(uint8_t value) {
  MemoryPageSnapshot page;
  page.address_space = MemoryAddressSpace::kVirtual;
  page.address = 0x1000;
  page.page_size = 0x1000;
  page.state = kMemorySnapshotReserve | kMemorySnapshotCommit;
  page.allocation_base = page.address;
  page.allocation_page_count = 1;
  page.allocation_protect =
      kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite;
  page.current_protect = page.allocation_protect;
  page.data.assign(page.page_size, value);
  return page;
}

std::vector<uint8_t> EncodedMemory(const MemorySnapshot& snapshot) {
  std::vector<uint8_t> encoded;
  REQUIRE(EncodeMemorySnapshot(snapshot, &encoded).ok());
  return encoded;
}

class TestClockAccess final : public FrozenClockAccess {
 public:
  explicit TestClockAccess(std::vector<std::string>* events)
      : events_(events), state(ClockState(10)) {}

  StateProviderResult Freeze(ClockSnapshot* original,
                             std::string* error_message) override {
    (void)error_message;
    events_->push_back("clock.prepare");
    if (frozen || !original) {
      return StateProviderResult::kInvalidState;
    }
    *original = state;
    frozen = true;
    return StateProviderResult::kOk;
  }

  StateProviderResult Restore(const ClockSnapshot& snapshot,
                              std::string* error_message) override {
    events_->push_back(fail_next_restore ? "clock.commit.fail"
                                         : "clock.restore");
    if (!frozen) {
      return StateProviderResult::kInvalidState;
    }
    state = snapshot;
    if (fail_next_restore) {
      fail_next_restore = false;
      ++state.guest_tick_count;
      if (error_message) {
        *error_message = "injected clock restore failure";
      }
      return StateProviderResult::kFailed;
    }
    return StateProviderResult::kOk;
  }

  void Unfreeze() noexcept override {
    events_->push_back("clock.unfreeze");
    frozen = false;
  }

  std::vector<std::string>* events_;
  ClockSnapshot state;
  bool frozen = false;
  bool fail_next_restore = false;
};

class TestMemoryAccess final : public PlannedMemoryRestoreAccess {
 public:
  explicit TestMemoryAccess(std::vector<std::string>* events)
      : events_(events) {}

  StateProviderResult CaptureInventory(
      MemoryAllocationInventory* inventory,
      std::string* error_message) override {
    (void)error_message;
    events_->push_back("memory.inventory");
    if (!inventory) {
      return StateProviderResult::kInvalidState;
    }
    inventory->pages = {
        {MemoryAddressSpace::kVirtual, 0x1000, 0x1000,
         kMemorySnapshotReserve | kMemorySnapshotCommit, 0x1000, 1,
         kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite,
         kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite,
         kNoPhysicalBacking},
    };
    return StateProviderResult::kOk;
  }

  StateProviderResult Stage(const MemoryRestorePlan& plan,
                            const MemorySnapshot& target,
                            std::string* error_message) override {
    events_->push_back("memory.prepare");
    if (fail_prepare || staged || plan.transitions.size() != 1 ||
        target.pages.size() != 1 || target.pages[0].data.empty()) {
      if (error_message) {
        *error_message = "injected memory stage failure";
      }
      return fail_prepare ? StateProviderResult::kFailed
                          : StateProviderResult::kInvalidState;
    }
    original_value = live_value;
    target_value = target.pages[0].data[0];
    staged = true;
    return StateProviderResult::kOk;
  }

  StateProviderResult Commit(std::string* error_message) override {
    events_->push_back(fail_commit ? "memory.commit.fail" : "memory.commit");
    if (!staged) {
      return StateProviderResult::kInvalidState;
    }
    live_value = target_value;
    if (fail_commit) {
      if (error_message) {
        *error_message = "injected memory commit failure";
      }
      return StateProviderResult::kFailed;
    }
    return StateProviderResult::kOk;
  }

  StateProviderResult Rollback() noexcept override {
    events_->push_back("memory.rollback");
    if (staged) {
      live_value = original_value;
    }
    staged = false;
    return fail_rollback ? StateProviderResult::kFailed
                         : StateProviderResult::kOk;
  }

  void Finalize() noexcept override {
    events_->push_back("memory.finalize");
    staged = false;
  }

  std::vector<std::string>* events_;
  uint8_t live_value = 1;
  uint8_t original_value = 0;
  uint8_t target_value = 0;
  bool staged = false;
  bool fail_prepare = false;
  bool fail_commit = false;
  bool fail_rollback = false;
};

}  // namespace

TEST_CASE("Subsystem transaction commits memory before releasing clock",
          "[save_state][transaction]") {
  std::vector<std::string> events;
  TestClockAccess clock_access(&events);
  TestMemoryAccess memory_access(&events);
  const ClockSnapshot target_clock = ClockState(20);
  const MemorySnapshot target_memory = {{MemoryPage(9)}};
  FrozenClockRestoreParticipant clock(clock_access, target_clock);
  PlannedMemoryRestoreParticipant memory(memory_access, target_memory);

  SubsystemTransactionCoordinator coordinator;
  const TransactionResult result = coordinator.Execute({&clock, &memory});

  REQUIRE(result.ok());
  CHECK(clock_access.state.guest_tick_count == 20);
  CHECK_FALSE(clock_access.frozen);
  CHECK(memory_access.live_value == 9);
  CHECK(events == std::vector<std::string>{
                      "clock.prepare", "memory.inventory", "memory.prepare",
                      "memory.commit", "clock.restore", "memory.finalize",
                      "clock.unfreeze"});
}

TEST_CASE("Clock commit failure rolls memory and clock back completely",
          "[save_state][transaction]") {
  std::vector<std::string> events;
  TestClockAccess clock_access(&events);
  TestMemoryAccess memory_access(&events);
  clock_access.fail_next_restore = true;
  const ClockSnapshot target_clock = ClockState(20);
  const MemorySnapshot target_memory = {{MemoryPage(9)}};
  FrozenClockRestoreParticipant clock(clock_access, target_clock);
  PlannedMemoryRestoreParticipant memory(memory_access, target_memory);

  SubsystemTransactionCoordinator coordinator;
  const TransactionResult result = coordinator.Execute({&clock, &memory});

  CHECK(result.result == StateProviderResult::kFailed);
  CHECK(result.failed_stage == TransactionStage::kCommitting);
  CHECK(result.participant_index == 0);
  CHECK(result.rollback_succeeded);
  CHECK(clock_access.state.guest_tick_count == 10);
  CHECK_FALSE(clock_access.frozen);
  CHECK(memory_access.live_value == 1);
  CHECK(events == std::vector<std::string>{
                      "clock.prepare", "memory.inventory", "memory.prepare",
                      "memory.commit", "clock.commit.fail", "memory.rollback",
                      "clock.restore", "clock.unfreeze"});
}

TEST_CASE("Memory failures never leave a partial transaction",
          "[save_state][transaction]") {
  std::vector<std::string> events;
  TestClockAccess clock_access(&events);
  TestMemoryAccess memory_access(&events);
  memory_access.fail_commit = true;
  const ClockSnapshot target_clock = ClockState(20);
  const MemorySnapshot target_memory = {{MemoryPage(9)}};
  FrozenClockRestoreParticipant clock(clock_access, target_clock);
  PlannedMemoryRestoreParticipant memory(memory_access, target_memory);

  SubsystemTransactionCoordinator coordinator;
  TransactionResult result = coordinator.Execute({&clock, &memory});
  CHECK(result.result == StateProviderResult::kFailed);
  CHECK(result.participant_index == 1);
  CHECK(result.rollback_succeeded);
  CHECK(memory_access.live_value == 1);
  CHECK(clock_access.state.guest_tick_count == 10);
  CHECK_FALSE(clock_access.frozen);

  events.clear();
  TestClockAccess second_clock_access(&events);
  TestMemoryAccess second_memory_access(&events);
  second_memory_access.fail_prepare = true;
  FrozenClockRestoreParticipant second_clock(second_clock_access, target_clock);
  PlannedMemoryRestoreParticipant second_memory(second_memory_access,
                                                 target_memory);
  result = coordinator.Execute({&second_clock, &second_memory});
  CHECK(result.failed_stage == TransactionStage::kPreparing);
  CHECK(result.participant_index == 1);
  CHECK(second_memory_access.live_value == 1);
  CHECK(second_clock_access.state.guest_tick_count == 10);
  CHECK_FALSE(second_clock_access.frozen);
}

TEST_CASE("Subsystem transaction reports rollback failure",
          "[save_state][transaction]") {
  std::vector<std::string> events;
  TestClockAccess clock_access(&events);
  TestMemoryAccess memory_access(&events);
  memory_access.fail_commit = true;
  memory_access.fail_rollback = true;
  const ClockSnapshot target_clock = ClockState(20);
  const MemorySnapshot target_memory = {{MemoryPage(9)}};
  FrozenClockRestoreParticipant clock(clock_access, target_clock);
  PlannedMemoryRestoreParticipant memory(memory_access, target_memory);

  SubsystemTransactionCoordinator coordinator;
  const TransactionResult result = coordinator.Execute({&clock, &memory});
  CHECK_FALSE(result.rollback_succeeded);
  CHECK(memory_access.live_value == 1);
  CHECK(clock_access.state.guest_tick_count == 10);
  CHECK_FALSE(clock_access.frozen);
}

TEST_CASE("Detached memory backend rolls back an exact committed image",
          "[save_state][transaction][memory]") {
  std::vector<std::string> events;
  TestClockAccess clock_access(&events);
  clock_access.fail_next_restore = true;
  MemorySnapshot image = {{MemoryPage(1)}};
  const std::vector<uint8_t> original = EncodedMemory(image);
  const MemorySnapshot target = {{MemoryPage(9)}};
  MemoryCaptureLimits limits;
  limits.maximum_page_count = 1;
  limits.maximum_content_bytes = 0x1000;
  DetachedMemoryRestoreBackend backend(image, limits);
  const ClockSnapshot target_clock = ClockState(20);
  FrozenClockRestoreParticipant clock(clock_access, target_clock);
  PlannedMemoryRestoreParticipant memory(backend, target);

  SubsystemTransactionCoordinator coordinator;
  const TransactionResult result = coordinator.Execute({&clock, &memory});

  CHECK(result.result == StateProviderResult::kFailed);
  CHECK(result.rollback_succeeded);
  CHECK(EncodedMemory(image) == original);
  CHECK(image.pages[0].data[0] == 1);
  CHECK_FALSE(clock_access.frozen);
}

TEST_CASE("Detached memory backend stages within bounds and swaps atomically",
          "[save_state][transaction][memory]") {
  MemorySnapshot image = {{MemoryPage(1)}};
  const std::vector<uint8_t> original = EncodedMemory(image);
  const MemorySnapshot target = {{MemoryPage(9)}};
  MemoryCaptureLimits limits;
  limits.maximum_page_count = 1;
  limits.maximum_content_bytes = 0x1000;
  DetachedMemoryRestoreBackend backend(image, limits);
  PlannedMemoryRestoreParticipant memory(backend, target);

  SubsystemTransactionCoordinator coordinator;
  REQUIRE(coordinator.Execute({&memory}).ok());
  CHECK(image.pages[0].data[0] == 9);

  MemorySnapshot bounded_image = {{MemoryPage(1)}};
  limits.maximum_content_bytes = 0x800;
  DetachedMemoryRestoreBackend bounded_backend(bounded_image, limits);
  PlannedMemoryRestoreParticipant bounded_memory(bounded_backend, target);
  const TransactionResult bounded_result =
      coordinator.Execute({&bounded_memory});
  CHECK(bounded_result.failed_stage == TransactionStage::kPreparing);
  CHECK(EncodedMemory(bounded_image) == original);

  MemorySnapshot plan_image = {{MemoryPage(1)}};
  limits.maximum_content_bytes = 0x1000;
  DetachedMemoryRestoreBackend plan_backend(plan_image, limits);
  MemoryAllocationInventory inventory;
  REQUIRE(plan_backend.CaptureInventory(&inventory, nullptr) ==
          StateProviderResult::kOk);
  MemoryRestorePlan wrong_plan;
  REQUIRE(BuildMemoryRestorePlan(inventory, target, &wrong_plan).ok());
  wrong_plan.transitions[0].current.address = 0x2000;
  CHECK(plan_backend.Stage(wrong_plan, target, nullptr) ==
        StateProviderResult::kInvalidState);
  CHECK(EncodedMemory(plan_image) == original);
}

}  // namespace xe::save_state::test
