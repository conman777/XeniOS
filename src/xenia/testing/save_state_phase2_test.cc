/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
// The bundled SHA-256 source includes POSIX endian.h for every non-MSVC host.
// MinGW has the required GCC byte-swap builtins but not that header.
#if defined(__MINGW32__) && !defined(_MSC_VER)
#define _MSC_VER 1
#define XE_SAVE_STATE_TEST_UNDEFINE_MSC_VER
#endif
#include "third_party/crypto/sha256.cpp"
#if defined(XE_SAVE_STATE_TEST_UNDEFINE_MSC_VER)
#undef XE_SAVE_STATE_TEST_UNDEFINE_MSC_VER
#undef _MSC_VER
#endif
#include "xenia/save_state_phase2.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <numeric>
#include <thread>

namespace xe::save_state::test {
namespace {

std::string Hash(const std::vector<uint8_t>& data) {
  sha256::SHA256 hash;
  return hash(data.data(), data.size());
}

CpuThreadSnapshot CpuThread(uint32_t thread_id) {
  CpuThreadSnapshot thread;
  thread.thread_id = thread_id;
  thread.resume_pc = 0x82000000 + thread_id * 4;
  for (size_t index = 0; index < thread.gpr.size(); ++index) {
    thread.gpr[index] = UINT64_C(0x100000000) * thread_id + index;
  }
  thread.ctr = 0x1000 + thread_id;
  thread.lr = 0x2000 + thread_id;
  thread.msr = 0x3000 + thread_id;
  for (size_t index = 0; index < thread.fpr_bits.size(); ++index) {
    thread.fpr_bits[index] =
        UINT64_C(0x3FF0000000000000) + index + thread_id;
  }
  for (size_t vector_index = 0;
       vector_index < thread.vector_registers.size(); ++vector_index) {
    for (size_t byte_index = 0;
         byte_index < thread.vector_registers[vector_index].size();
         ++byte_index) {
      thread.vector_registers[vector_index][byte_index] =
          uint8_t(thread_id + vector_index + byte_index);
    }
  }
  thread.vscr_vector.fill(uint8_t(0x80 + thread_id));
  for (size_t index = 0; index < thread.condition_registers.size(); ++index) {
    thread.condition_registers[index] =
        uint32_t(0x01010101 * (index + thread_id));
  }
  thread.fpscr = 0x12340000 + thread_id;
  thread.vrsave = 0x56780000 + thread_id;
  thread.xer_ca = 1;
  thread.xer_so = 1;
  thread.vscr_sat = 1;
  thread.stackpoints = {
      {0x70000000 - thread_id * 0x1000,
       0x82001000 + thread_id * 4, 0x40},
      {0x70001000 - thread_id * 0x1000, 0xBCBCBCBC, 0x50},
  };
  return thread;
}

ClockSnapshot ClockState() {
  ClockSnapshot clock;
  clock.guest_tick_count = 123456789;
  clock.guest_tick_frequency = 50000000;
  clock.tick_ratio_numerator = 5;
  clock.tick_ratio_denominator = 3;
  clock.guest_system_time_base = UINT64_C(133801632000000000);
  clock.guest_interrupt_time = UINT64_C(9876543210);
  const double scalar = 1.5;
  std::memcpy(&clock.time_scalar_bits, &scalar, sizeof(scalar));
  return clock;
}

MemoryPageSnapshot InlinePage(MemoryAddressSpace address_space,
                              uint32_t address, uint8_t fill) {
  MemoryPageSnapshot page;
  page.address_space = address_space;
  page.address = address;
  page.page_size = 0x1000;
  page.state = kMemorySnapshotReserve | kMemorySnapshotCommit;
  page.allocation_base = address;
  page.allocation_page_count = 1;
  page.allocation_protect =
      kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite;
  page.current_protect = page.allocation_protect;
  page.data.assign(page.page_size, fill);
  return page;
}

class TestMemoryContentReader final : public MemoryContentReader {
 public:
  StateProviderResult ReadPage(MemoryAddressSpace address_space,
                               uint32_t address, uint8_t* output,
                               size_t output_size,
                               std::string* error_message) override {
    (void)address_space;
    reads.push_back(address);
    if (address == fail_address) {
      if (output_size) {
        output[0] = 0xEE;
      }
      if (error_message) {
        *error_message = "injected memory read failure";
      }
      return StateProviderResult::kFailed;
    }
    std::fill(output, output + output_size, uint8_t(address >> 12));
    return StateProviderResult::kOk;
  }

  uint32_t fail_address = std::numeric_limits<uint32_t>::max();
  std::vector<uint32_t> reads;
};

}  // namespace

TEST_CASE("Cooperative barrier holds every participant until release",
          "[save_state][phase2][barrier]") {
  CooperativeGuestBarrier barrier;
  BarrierBeginResult begin = barrier.Begin({3, 1, 2});
  REQUIRE(begin.accepted());
  CHECK(barrier.requested_generation() == begin.generation);

  std::array<BarrierPollResult, 3> results = {};
  std::array<std::thread, 3> threads = {
      std::thread([&]() { results[0] = barrier.Poll(1, begin.generation); }),
      std::thread([&]() { results[1] = barrier.Poll(2, begin.generation); }),
      std::thread([&]() { results[2] = barrier.Poll(3, begin.generation); }),
  };

  REQUIRE(barrier.WaitForAll(begin.generation, std::chrono::seconds(2)) ==
          BarrierWaitResult::kReached);
  CHECK(barrier.status().phase == BarrierPhase::kHeld);
  CHECK(barrier.status().arrived_count == 3);
  REQUIRE(barrier.Release(begin.generation));
  for (std::thread& thread : threads) {
    thread.join();
  }
  for (BarrierPollResult result : results) {
    CHECK(result == BarrierPollResult::kReleased);
  }
  CHECK(barrier.requested_generation() == 0);
  CHECK(barrier.status().phase == BarrierPhase::kIdle);
}

TEST_CASE("Cooperative barrier timeout rolls back arrived threads",
          "[save_state][phase2][barrier]") {
  CooperativeGuestBarrier barrier;
  BarrierBeginResult begin = barrier.Begin({10, 11});
  REQUIRE(begin.accepted());

  BarrierPollResult result = BarrierPollResult::kNotRequested;
  std::thread arrived(
      [&]() { result = barrier.Poll(10, begin.generation); });
  const auto arrival_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (barrier.status().arrived_count != 1 &&
         std::chrono::steady_clock::now() < arrival_deadline) {
    std::this_thread::yield();
  }
  const bool did_arrive = barrier.status().arrived_count == 1;
  if (!did_arrive) {
    barrier.Cancel(begin.generation);
    arrived.join();
  }
  REQUIRE(did_arrive);
  REQUIRE(barrier.WaitForAll(begin.generation,
                            std::chrono::milliseconds(50)) ==
          BarrierWaitResult::kTimedOut);
  arrived.join();

  CHECK(result == BarrierPollResult::kCancelled);
  CHECK(barrier.requested_generation() == 0);
  CHECK(barrier.status().phase == BarrierPhase::kIdle);
  BarrierBeginResult retry = barrier.Begin({10, 11});
  REQUIRE(retry.accepted());
  CHECK(barrier.WaitForAll(retry.generation,
                           std::chrono::milliseconds::zero()) ==
        BarrierWaitResult::kInvalidTimeout);
  CHECK_FALSE(barrier.Release(retry.generation));
  CHECK(barrier.Cancel(retry.generation));
}

TEST_CASE("Cooperative barrier starts a new generation while waiters drain",
          "[save_state][phase2][barrier]") {
  CooperativeGuestBarrier barrier;
  std::vector<uint32_t> participants(128);
  std::iota(participants.begin(), participants.end(), 1);
  BarrierBeginResult first = barrier.Begin(participants);
  REQUIRE(first.accepted());

  std::vector<BarrierPollResult> results(participants.size());
  std::vector<std::thread> pollers;
  pollers.reserve(participants.size());
  for (size_t index = 0; index < participants.size(); ++index) {
    pollers.emplace_back([&, index]() {
      results[index] = barrier.Poll(participants[index], first.generation);
    });
  }
  REQUIRE(barrier.WaitForAll(first.generation, std::chrono::seconds(2)) ==
          BarrierWaitResult::kReached);
  REQUIRE(barrier.Release(first.generation));

  // Release is the logical end of a generation. Poll callers from that
  // generation may still be unwinding, and must not make a later capture
  // spuriously busy.
  BarrierBeginResult second = barrier.Begin({1000});
  REQUIRE(second.accepted());
  CHECK(barrier.Cancel(second.generation));

  for (std::thread& poller : pollers) {
    poller.join();
  }
  for (BarrierPollResult result : results) {
    CHECK((result == BarrierPollResult::kReleased ||
           result == BarrierPollResult::kStaleGeneration));
  }
}

TEST_CASE("Cooperative barrier rejects stale and duplicate arrivals",
          "[save_state][phase2][barrier]") {
  CooperativeGuestBarrier barrier;
  CHECK(barrier.Poll(1, 99) == BarrierPollResult::kNotRequested);
  CHECK_FALSE(barrier.Begin({1, 1}).accepted());
  BarrierBeginResult begin = barrier.Begin({1});
  REQUIRE(begin.accepted());

  BarrierPollResult first_result = BarrierPollResult::kNotRequested;
  std::thread first(
      [&]() { first_result = barrier.Poll(1, begin.generation); });
  const auto arrival_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (barrier.status().arrived_count != 1 &&
         std::chrono::steady_clock::now() < arrival_deadline) {
    std::this_thread::yield();
  }
  const bool did_arrive = barrier.status().arrived_count == 1;
  if (!did_arrive) {
    barrier.Cancel(begin.generation);
    first.join();
  }
  REQUIRE(did_arrive);
  CHECK(barrier.Poll(1, begin.generation) ==
        BarrierPollResult::kDuplicateArrival);
  CHECK(barrier.Cancel(begin.generation));
  first.join();
  CHECK(first_result == BarrierPollResult::kCancelled);
  CHECK(barrier.WaitForAll(begin.generation,
                           std::chrono::milliseconds(1)) ==
        BarrierWaitResult::kInvalidPhase);
}

TEST_CASE("CPU snapshot codec is canonical and hash-stable",
          "[save_state][phase2][cpu]") {
  CpuSnapshot source;
  source.threads = {CpuThread(9), CpuThread(2)};
  source.threads.front().njm_enabled = 0;
  source.threads.front().flags = kCpuThreadFlagReservationValid;
  source.threads.front().reservation_address = 0x12340000;
  source.threads.front().reservation_value = 0xCAFEBABE;
  source.threads.front().reservation_granule = 0x10000;

  std::vector<uint8_t> encoded;
  REQUIRE(EncodeCpuSnapshot(source, &encoded).ok());
  const std::string source_hash = Hash(encoded);

  CpuSnapshot decoded;
  REQUIRE(DecodeCpuSnapshot(encoded.data(), encoded.size(), &decoded).ok());
  REQUIRE(decoded.threads.size() == 2);
  CHECK(decoded.threads[0].thread_id == 2);
  CHECK(decoded.threads[1].thread_id == 9);
  CHECK(decoded.threads[1].reservation_value == 0xCAFEBABE);
  CHECK(decoded.threads[1].njm_enabled == 0);
  REQUIRE(decoded.threads[1].stackpoints.size() ==
          source.threads[0].stackpoints.size());
  CHECK(decoded.threads[1].stackpoints[0].guest_stack_pointer ==
        source.threads[0].stackpoints[0].guest_stack_pointer);
  CHECK(decoded.threads[1].stackpoints[0].guest_return_address ==
        source.threads[0].stackpoints[0].guest_return_address);
  CHECK(decoded.threads[1].stackpoints[0].host_stack_size ==
        source.threads[0].stackpoints[0].host_stack_size);

  std::vector<uint8_t> reencoded;
  REQUIRE(EncodeCpuSnapshot(decoded, &reencoded).ok());
  CHECK(reencoded == encoded);
  CHECK(Hash(reencoded) == source_hash);

  decoded.threads[0].stackpoints[0].host_stack_size = 3;
  CHECK(ValidateCpuSnapshot(decoded).error ==
        SnapshotCodecError::kInvalidThread);
  decoded.threads[0].stackpoints[0].host_stack_size =
      source.threads[1].stackpoints[0].host_stack_size;
  decoded.threads[0].flags = kCpuThreadFlagReservationValid;
  CHECK(ValidateCpuSnapshot(decoded).error ==
        SnapshotCodecError::kInvalidThread);
}

TEST_CASE("Clock snapshot codec preserves deterministic time state",
          "[save_state][phase2][clock]") {
  ClockSnapshot source = ClockState();
  std::vector<uint8_t> encoded;
  REQUIRE(EncodeClockSnapshot(source, &encoded).ok());
  const std::string source_hash = Hash(encoded);

  ClockSnapshot decoded;
  REQUIRE(DecodeClockSnapshot(encoded.data(), encoded.size(), &decoded).ok());
  CHECK(decoded.guest_tick_count == source.guest_tick_count);
  CHECK(decoded.guest_system_time_base == source.guest_system_time_base);
  CHECK(decoded.guest_interrupt_time == source.guest_interrupt_time);
  CHECK(decoded.time_scalar_bits == source.time_scalar_bits);

  std::vector<uint8_t> reencoded;
  REQUIRE(EncodeClockSnapshot(decoded, &reencoded).ok());
  CHECK(reencoded == encoded);
  CHECK(Hash(reencoded) == source_hash);

  decoded.flags = 0;
  CHECK(ValidateClockSnapshot(decoded).error ==
        SnapshotCodecError::kInvalidClock);
}

TEST_CASE("Memory snapshot codec canonicalizes pages and preserves aliases",
          "[save_state][phase2][memory]") {
  MemoryPageSnapshot virtual_page =
      InlinePage(MemoryAddressSpace::kVirtual, 0x1000, 0x5A);
  virtual_page.allocation_page_count = 2;

  MemoryPageSnapshot alias;
  alias.address_space = MemoryAddressSpace::kVirtual;
  alias.address = 0x2000;
  alias.page_size = 0x1000;
  alias.state = kMemorySnapshotReserve | kMemorySnapshotCommit;
  alias.allocation_base = 0x1000;
  alias.allocation_page_count = 2;
  alias.allocation_protect =
      kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite;
  alias.current_protect = alias.allocation_protect;
  alias.backing_physical_address = 0;

  MemoryPageSnapshot physical_page =
      InlinePage(MemoryAddressSpace::kPhysical, 0, 0xC3);
  MemorySnapshot source;
  source.pages = {physical_page, alias, virtual_page};

  std::vector<uint8_t> encoded;
  REQUIRE(EncodeMemorySnapshot(source, &encoded).ok());
  const std::string source_hash = Hash(encoded);

  MemorySnapshot decoded;
  REQUIRE(
      DecodeMemorySnapshot(encoded.data(), encoded.size(), &decoded).ok());
  REQUIRE(decoded.pages.size() == 3);
  CHECK(decoded.pages[0].address_space == MemoryAddressSpace::kVirtual);
  CHECK(decoded.pages[0].address == 0x1000);
  CHECK(decoded.pages[1].backing_physical_address == 0);
  CHECK(decoded.pages[1].data.empty());
  CHECK(decoded.pages[2].address_space == MemoryAddressSpace::kPhysical);

  std::vector<uint8_t> reencoded;
  REQUIRE(EncodeMemorySnapshot(decoded, &reencoded).ok());
  CHECK(reencoded == encoded);
  CHECK(Hash(reencoded) == source_hash);

  decoded.pages[1].address = decoded.pages[0].address;
  CHECK(ValidateMemorySnapshot(decoded).error ==
        SnapshotCodecError::kDuplicateOrOverlappingMemory);
}

TEST_CASE("Memory restore preflight is immutable and canonical",
          "[save_state][phase2][memory]") {
  MemoryAllocationInventory current;
  current.pages = {
      {MemoryAddressSpace::kVirtual, 0x3000, 0x1000,
       kMemorySnapshotReserve | kMemorySnapshotCommit, 0x3000, 1,
       kMemorySnapshotProtectRead, kMemorySnapshotProtectRead,
       kNoPhysicalBacking},
      {MemoryAddressSpace::kVirtual, 0x1000, 0x1000,
       kMemorySnapshotReserve | kMemorySnapshotCommit, 0x1000, 1,
       kMemorySnapshotProtectRead, kMemorySnapshotProtectRead,
       kNoPhysicalBacking},
  };
  MemorySnapshot target;
  target.pages = {
      InlinePage(MemoryAddressSpace::kVirtual, 0x2000, 0x22),
      InlinePage(MemoryAddressSpace::kVirtual, 0x1000, 0x11),
  };

  MemoryRestorePlan plan;
  REQUIRE(BuildMemoryRestorePlan(current, target, &plan).ok());
  REQUIRE(plan.transitions.size() == 3);
  CHECK(plan.transitions[0].current_exists);
  CHECK(plan.transitions[0].target_exists);
  CHECK(plan.transitions[0].current.address == 0x1000);
  CHECK_FALSE(plan.transitions[1].current_exists);
  CHECK(plan.transitions[1].target.address == 0x2000);
  CHECK(plan.transitions[2].current.address == 0x3000);
  CHECK_FALSE(plan.transitions[2].target_exists);
  CHECK(target.pages[0].address == 0x2000);

  MemoryAllocationInventory invalid_current = current;
  invalid_current.pages[0].backing_physical_address = 1;
  CHECK(BuildMemoryRestorePlan(invalid_current, target, &plan).error ==
        SnapshotCodecError::kInvalidMemoryPage);
  CHECK(plan.transitions.size() == 3);
}

TEST_CASE("Bounded memory content capture is canonical and failure-atomic",
          "[save_state][phase2][memory]") {
  const uint32_t committed =
      kMemorySnapshotReserve | kMemorySnapshotCommit;
  const uint32_t protect =
      kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite;
  MemoryAllocationInventory inventory;
  inventory.pages = {
      {MemoryAddressSpace::kPhysical, 0, 0x1000, committed, 0, 1, protect,
       protect, kNoPhysicalBacking},
      {MemoryAddressSpace::kVirtual, 0xA0000000, 0x1000, committed,
       0xA0000000, 1, protect, protect, 0},
      {MemoryAddressSpace::kVirtual, 0x1000, 0x1000, committed, 0x1000, 1,
       protect, protect, kNoPhysicalBacking},
  };
  MemoryCaptureLimits limits;
  limits.maximum_page_count = 3;
  limits.maximum_content_bytes = 0x2000;
  TestMemoryContentReader reader;
  MemorySnapshot captured;
  std::string error;
  REQUIRE(CaptureMemorySnapshot(inventory, reader, limits, &captured, &error) ==
          StateProviderResult::kOk);
  REQUIRE(captured.pages.size() == 3);
  CHECK(captured.pages[0].address == 0x1000);
  CHECK(captured.pages[0].data[0] == 1);
  CHECK(captured.pages[1].address == 0xA0000000);
  CHECK(captured.pages[1].data.empty());
  CHECK(captured.pages[1].backing_physical_address == 0);
  CHECK(captured.pages[2].address_space == MemoryAddressSpace::kPhysical);
  CHECK(captured.pages[2].data[0] == 0);
  CHECK(reader.reads == std::vector<uint32_t>{0x1000, 0});

  std::vector<uint8_t> encoded;
  REQUIRE(EncodeMemorySnapshot(captured, &encoded).ok());
  MemorySnapshot decoded;
  REQUIRE(
      DecodeMemorySnapshot(encoded.data(), encoded.size(), &decoded).ok());
  CHECK(decoded.pages.size() == captured.pages.size());

  MemorySnapshot unchanged = {
      {InlinePage(MemoryAddressSpace::kVirtual, 0x4000, 0x44)}};
  limits.maximum_content_bytes = 0x1000;
  CHECK(CaptureMemorySnapshot(inventory, reader, limits, &unchanged, &error) ==
        StateProviderResult::kInvalidState);
  REQUIRE(unchanged.pages.size() == 1);
  CHECK(unchanged.pages[0].address == 0x4000);
  CHECK(unchanged.pages[0].data[0] == 0x44);

  limits.maximum_content_bytes = 0x2000;
  reader.fail_address = 0;
  CHECK(CaptureMemorySnapshot(inventory, reader, limits, &unchanged, &error) ==
        StateProviderResult::kFailed);
  REQUIRE(unchanged.pages.size() == 1);
  CHECK(unchanged.pages[0].address == 0x4000);
  CHECK(unchanged.pages[0].data[0] == 0x44);
}

TEST_CASE("Unimplemented state providers fail closed",
          "[save_state][phase2][provider]") {
  CpuStateProvider cpu_provider;
  CpuSnapshot cpu;
  std::string error;
  CHECK(cpu_provider.Capture(&cpu, &error) ==
        StateProviderResult::kUnsupported);
  CHECK_FALSE(error.empty());

  ClockStateProvider clock_provider;
  ClockSnapshot clock = ClockState();
  CHECK(clock_provider.PreflightRestore(clock, nullptr) ==
        StateProviderResult::kUnsupported);

  MemoryStateProvider memory_provider;
  MemorySnapshot memory;
  CHECK(memory_provider.Restore(memory, nullptr) ==
        StateProviderResult::kUnsupported);
}

}  // namespace xe::save_state::test
