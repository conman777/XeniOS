/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_PHASE2_H_
#define XENIA_SAVE_STATE_PHASE2_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/save_state.h"

namespace xe::save_state {

constexpr uint32_t kMaximumGuestThreadCount = 256;
constexpr std::chrono::milliseconds kMaximumGuestBarrierWait =
    std::chrono::seconds(30);

enum class BarrierError {
  kNone,
  kBusy,
  kInvalidParticipants,
  kStaleGeneration,
  kInvalidPhase,
};

enum class BarrierPhase {
  kIdle,
  kCollecting,
  kHeld,
};

struct BarrierBeginResult {
  uint64_t generation = 0;
  BarrierError error = BarrierError::kNone;

  bool accepted() const { return generation != 0; }
};

enum class BarrierWaitResult {
  kReached,
  kTimedOut,
  kInvalidTimeout,
  kStaleGeneration,
  kInvalidPhase,
};

enum class BarrierPollResult {
  kNotRequested,
  kReleased,
  kCancelled,
  kDuplicateArrival,
  kStaleGeneration,
};

struct BarrierStatus {
  uint64_t generation = 0;
  BarrierPhase phase = BarrierPhase::kIdle;
  uint32_t participant_count = 0;
  uint32_t arrived_count = 0;
  uint32_t waiter_count = 0;
};

// Cooperative stop-the-world foundation for guest threads. The operation
// owner snapshots the participant IDs and calls Begin, then each guest thread
// calls Poll only after its architectural context has been synchronized at a
// documented basic-block boundary. Poll blocks until Release or rollback.
//
// This class does not suspend host threads and has no A64 emitter hook yet.
class CooperativeGuestBarrier {
 public:
  CooperativeGuestBarrier() = default;
  CooperativeGuestBarrier(const CooperativeGuestBarrier&) = delete;
  CooperativeGuestBarrier& operator=(const CooperativeGuestBarrier&) = delete;

  BarrierBeginResult Begin(std::vector<uint32_t> participant_ids);
  BarrierWaitResult WaitForAll(uint64_t generation,
                               std::chrono::milliseconds timeout);
  BarrierPollResult Poll(uint32_t participant_id, uint64_t generation);
  bool Release(uint64_t generation);
  bool Cancel(uint64_t generation);

  // A JIT poll hook may use this as the cheap fast-path check. A nonzero value
  // must still be passed to Poll and revalidated under the barrier mutex.
  uint64_t requested_generation() const {
    return requested_generation_.load(std::memory_order_acquire);
  }
  const std::atomic<uint64_t>* requested_generation_address() const {
    return &requested_generation_;
  }
  BarrierStatus status() const;

 private:
  bool CompleteLocked(uint64_t generation, bool released);

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::atomic<uint64_t> requested_generation_{0};
  uint64_t next_generation_ = 1;
  uint64_t generation_ = 0;
  uint64_t last_completed_generation_ = 0;
  bool last_completion_released_ = false;
  BarrierPhase phase_ = BarrierPhase::kIdle;
  std::vector<uint32_t> participants_;
  std::vector<uint32_t> arrived_;
  uint32_t waiter_count_ = 0;
};

enum class SnapshotCodecError {
  kNone,
  kInvalidArgument,
  kInvalidMagic,
  kUnsupportedVersion,
  kUnsupportedBackend,
  kUnsupportedFlags,
  kInvalidCount,
  kInvalidSize,
  kInvalidThread,
  kInvalidClock,
  kInvalidMemoryPage,
  kDuplicateOrOverlappingMemory,
  kTruncated,
  kTrailingData,
};

struct SnapshotCodecValidation {
  SnapshotCodecError error = SnapshotCodecError::kNone;
  std::string message;

  bool ok() const { return error == SnapshotCodecError::kNone; }
};

enum class HostBackend : uint32_t {
  kA64 = 1,
};

constexpr uint32_t kCpuThreadFlagReservationValid = 1u << 0;
constexpr uint32_t kMaximumGuestStackpointCount = 65536;
constexpr uint32_t kMaximumCapturedHostStackSize = 1u << 20;

struct CpuStackpointSnapshot {
  uint32_t guest_stack_pointer = 0;
  uint32_t guest_return_address = 0;
  uint32_t host_stack_size = 0;
};

struct CpuThreadSnapshot {
  uint32_t thread_id = 0;
  uint32_t resume_pc = 0;
  uint32_t flags = 0;
  std::array<uint64_t, 32> gpr = {};
  uint64_t ctr = 0;
  uint64_t lr = 0;
  uint64_t msr = 0;
  std::array<uint64_t, 32> fpr_bits = {};
  std::array<std::array<uint8_t, 16>, 128> vector_registers = {};
  std::array<uint8_t, 16> vscr_vector = {};
  std::array<uint32_t, 8> condition_registers = {};
  uint32_t fpscr = 0;
  uint32_t vrsave = 0;
  uint64_t reservation_address = 0;
  uint64_t reservation_value = 0;
  uint32_t reservation_granule = 0;
  uint8_t xer_ca = 0;
  uint8_t xer_ov = 0;
  uint8_t xer_so = 0;
  uint8_t vscr_sat = 0;
  uint8_t njm_enabled = 1;
  // A64 JIT stackpoints, ordered from the currently executing frame toward
  // the outermost host-entry frame. Host SP/FP values are intentionally never
  // serialized.
  std::vector<CpuStackpointSnapshot> stackpoints;
};

struct CpuSnapshot {
  HostBackend host_backend = HostBackend::kA64;
  std::vector<CpuThreadSnapshot> threads;
};

constexpr uint32_t kClockSnapshotFlagScaled = 1u << 0;

struct ClockSnapshot {
  uint32_t flags = kClockSnapshotFlagScaled;
  uint64_t guest_tick_count = 0;
  uint64_t guest_tick_frequency = 0;
  uint64_t tick_ratio_numerator = 0;
  uint64_t tick_ratio_denominator = 0;
  uint64_t guest_system_time_base = 0;
  uint64_t guest_interrupt_time = 0;
  uint64_t time_scalar_bits = 0;
};

enum class MemoryAddressSpace : uint32_t {
  kVirtual = 1,
  kPhysical = 2,
};

constexpr uint32_t kNoPhysicalBacking = UINT32_MAX;
constexpr uint32_t kMemorySnapshotReserve = 1u << 0;
constexpr uint32_t kMemorySnapshotCommit = 1u << 1;
constexpr uint32_t kMemorySnapshotProtectRead = 1u << 0;
constexpr uint32_t kMemorySnapshotProtectWrite = 1u << 1;
constexpr uint32_t kMemorySnapshotProtectNoCache = 1u << 2;
constexpr uint32_t kMemorySnapshotProtectWriteCombine = 1u << 3;
constexpr uint32_t kMaximumMemoryPageCount = 262144;
constexpr uint64_t kMaximumMemorySnapshotSize = UINT64_C(1) << 30;

struct MemoryPageSnapshot {
  MemoryAddressSpace address_space = MemoryAddressSpace::kVirtual;
  uint32_t address = 0;
  uint32_t page_size = 0;
  uint32_t state = 0;
  uint32_t allocation_base = 0;
  uint32_t allocation_page_count = 0;
  uint32_t allocation_protect = 0;
  uint32_t current_protect = 0;
  uint32_t backing_physical_address = kNoPhysicalBacking;
  std::vector<uint8_t> data;
};

struct MemorySnapshot {
  std::vector<MemoryPageSnapshot> pages;
};

struct MemoryInventoryPage {
  MemoryAddressSpace address_space = MemoryAddressSpace::kVirtual;
  uint32_t address = 0;
  uint32_t page_size = 0;
  uint32_t state = 0;
  uint32_t allocation_base = 0;
  uint32_t allocation_page_count = 0;
  uint32_t allocation_protect = 0;
  uint32_t current_protect = 0;
  uint32_t backing_physical_address = kNoPhysicalBacking;
};

struct MemoryAllocationInventory {
  std::vector<MemoryInventoryPage> pages;
};

struct MemoryRestoreTransition {
  bool current_exists = false;
  bool target_exists = false;
  MemoryInventoryPage current;
  MemoryInventoryPage target;
};

// Immutable preflight output. Applying transitions is intentionally absent
// until page contents and host mapping/protection rollback can be staged.
struct MemoryRestorePlan {
  std::vector<MemoryRestoreTransition> transitions;
};

enum class StateProviderResult {
  kOk,
  kUnsupported,
  kInvalidState,
  kFailed,
};

struct MemoryCaptureLimits {
  uint32_t maximum_page_count = kMaximumMemoryPageCount;
  uint64_t maximum_content_bytes = kMaximumMemorySnapshotSize;
};

class MemoryContentReader {
 public:
  virtual ~MemoryContentReader() = default;

  virtual StateProviderResult ReadPage(MemoryAddressSpace address_space,
                                       uint32_t address, uint8_t* output,
                                       size_t output_size,
                                       std::string* error_message) = 0;
};

SnapshotCodecValidation ValidateCpuSnapshot(const CpuSnapshot& snapshot);
SnapshotCodecValidation EncodeCpuSnapshot(const CpuSnapshot& snapshot,
                                          std::vector<uint8_t>* encoded);
SnapshotCodecValidation DecodeCpuSnapshot(const uint8_t* data, size_t data_size,
                                          CpuSnapshot* snapshot);

SnapshotCodecValidation ValidateClockSnapshot(const ClockSnapshot& snapshot);
SnapshotCodecValidation EncodeClockSnapshot(const ClockSnapshot& snapshot,
                                            std::vector<uint8_t>* encoded);
SnapshotCodecValidation DecodeClockSnapshot(const uint8_t* data,
                                            size_t data_size,
                                            ClockSnapshot* snapshot);

SnapshotCodecValidation ValidateMemorySnapshot(const MemorySnapshot& snapshot);
SnapshotCodecValidation EncodeMemorySnapshot(const MemorySnapshot& snapshot,
                                             std::vector<uint8_t>* encoded);
SnapshotCodecValidation DecodeMemorySnapshot(const uint8_t* data,
                                             size_t data_size,
                                             MemorySnapshot* snapshot);
SnapshotCodecValidation ValidateMemoryAllocationInventory(
    const MemoryAllocationInventory& inventory);
SnapshotCodecValidation BuildMemoryRestorePlan(
    const MemoryAllocationInventory& current, const MemorySnapshot& target,
    MemoryRestorePlan* plan);
StateProviderResult CaptureMemorySnapshot(
    const MemoryAllocationInventory& inventory, MemoryContentReader& reader,
    const MemoryCaptureLimits& limits, MemorySnapshot* snapshot,
    std::string* error_message);

// Providers are deliberately fail-closed until a subsystem implements all
// required state. Capture and Restore may only run while a cooperative barrier
// is held. PreflightRestore must not mutate live emulator state.
template <typename Snapshot>
class StateProvider {
 public:
  virtual ~StateProvider() = default;

  virtual StateProviderResult Capture(Snapshot* snapshot,
                                      std::string* error_message) {
    (void)snapshot;
    if (error_message) {
      *error_message = "State provider is not implemented.";
    }
    return StateProviderResult::kUnsupported;
  }

  virtual StateProviderResult PreflightRestore(
      const Snapshot& snapshot, std::string* error_message) {
    (void)snapshot;
    if (error_message) {
      *error_message = "State provider is not implemented.";
    }
    return StateProviderResult::kUnsupported;
  }

  virtual StateProviderResult Restore(const Snapshot& snapshot,
                                      std::string* error_message) {
    (void)snapshot;
    if (error_message) {
      *error_message = "State provider is not implemented.";
    }
    return StateProviderResult::kUnsupported;
  }
};

using CpuStateProvider = StateProvider<CpuSnapshot>;
using ClockStateProvider = StateProvider<ClockSnapshot>;
using MemoryStateProvider = StateProvider<MemorySnapshot>;

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_PHASE2_H_
