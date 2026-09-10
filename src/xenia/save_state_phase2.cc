/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_phase2.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "xenia/base/logging.h"

namespace xe::save_state {
namespace {

constexpr uint16_t kSnapshotVersion = 1;
constexpr uint16_t kSnapshotHeaderSize = 24;
constexpr uint32_t kCpuSnapshotMagic =
    MakeLittleEndianFourCC('C', 'P', 'U', '3');
constexpr uint32_t kClockSnapshotMagic =
    MakeLittleEndianFourCC('C', 'L', 'K', '2');
constexpr uint32_t kMemorySnapshotMagic =
    MakeLittleEndianFourCC('M', 'E', 'M', '2');
constexpr uint64_t kCpuThreadFixedRecordSize = 2688;
constexpr uint64_t kClockSnapshotSize = kSnapshotHeaderSize + 7 * 8;
constexpr uint64_t kMemoryPageRecordSize = 48;

SnapshotCodecValidation CodecError(SnapshotCodecError error,
                                   std::string message) {
  return {error, std::move(message)};
}

class Writer {
 public:
  template <typename T>
  void WriteLittleEndian(T value) {
    for (size_t index = 0; index < sizeof(T); ++index) {
      data_.push_back(
          uint8_t((uint64_t(value) >> (index * 8)) & UINT64_C(0xFF)));
    }
  }

  void WriteBytes(const uint8_t* data, size_t size) {
    if (size == 0) {
      return;
    }
    data_.insert(data_.end(), data, data + size);
  }

  void WriteZeros(size_t size) { data_.insert(data_.end(), size, 0); }

  void Align(size_t alignment) {
    const size_t remainder = data_.size() % alignment;
    if (remainder) {
      WriteZeros(alignment - remainder);
    }
  }

  std::vector<uint8_t> Take() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  template <typename T>
  bool ReadLittleEndian(T* value) {
    if (!value || size_ - offset_ < sizeof(T)) {
      return false;
    }
    uint64_t decoded = 0;
    for (size_t index = 0; index < sizeof(T); ++index) {
      decoded |= uint64_t(data_[offset_ + index]) << (index * 8);
    }
    offset_ += sizeof(T);
    *value = T(decoded);
    return true;
  }

  bool ReadBytes(uint8_t* data, size_t size) {
    if (size == 0) {
      return true;
    }
    if (!data || size_ - offset_ < size) {
      return false;
    }
    std::memcpy(data, data_ + offset_, size);
    offset_ += size;
    return true;
  }

  bool Skip(size_t size) {
    if (size_ - offset_ < size) {
      return false;
    }
    offset_ += size;
    return true;
  }

  bool Align(size_t alignment) {
    const size_t remainder = offset_ % alignment;
    return !remainder || Skip(alignment - remainder);
  }

  size_t offset() const { return offset_; }
  size_t remaining() const { return size_ - offset_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

void WriteHeader(Writer* writer, uint32_t magic, uint64_t total_size,
                 uint32_t count, uint32_t flags) {
  writer->WriteLittleEndian(magic);
  writer->WriteLittleEndian(kSnapshotVersion);
  writer->WriteLittleEndian(kSnapshotHeaderSize);
  writer->WriteLittleEndian(total_size);
  writer->WriteLittleEndian(count);
  writer->WriteLittleEndian(flags);
}

SnapshotCodecValidation ReadHeader(Reader* reader, uint32_t expected_magic,
                                   uint32_t* count, uint32_t* flags,
                                   uint64_t expected_size) {
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t header_size = 0;
  uint64_t total_size = 0;
  if (!reader->ReadLittleEndian(&magic) ||
      !reader->ReadLittleEndian(&version) ||
      !reader->ReadLittleEndian(&header_size) ||
      !reader->ReadLittleEndian(&total_size) ||
      !reader->ReadLittleEndian(count) ||
      !reader->ReadLittleEndian(flags)) {
    return CodecError(SnapshotCodecError::kTruncated,
                      "Snapshot header is truncated.");
  }
  if (magic != expected_magic) {
    return CodecError(SnapshotCodecError::kInvalidMagic,
                      "Snapshot magic does not match.");
  }
  if (version != kSnapshotVersion) {
    return CodecError(SnapshotCodecError::kUnsupportedVersion,
                      "Snapshot version is unsupported.");
  }
  if (header_size != kSnapshotHeaderSize || total_size != expected_size) {
    return CodecError(SnapshotCodecError::kInvalidSize,
                      "Snapshot size is invalid.");
  }
  return {};
}

bool IsSupportedPageSize(uint32_t page_size) {
  return page_size == 0x1000 || page_size == 0x10000 ||
         page_size == 0x1000000;
}

bool MemoryPageLess(const MemoryPageSnapshot& left,
                    const MemoryPageSnapshot& right) {
  if (left.address_space != right.address_space) {
    return uint32_t(left.address_space) < uint32_t(right.address_space);
  }
  return left.address < right.address;
}

bool MemoryInventoryPageLess(const MemoryInventoryPage& left,
                             const MemoryInventoryPage& right) {
  if (left.address_space != right.address_space) {
    return uint32_t(left.address_space) < uint32_t(right.address_space);
  }
  return left.address < right.address;
}

}  // namespace

BarrierBeginResult CooperativeGuestBarrier::Begin(
    std::vector<uint32_t> participant_ids) {
  std::sort(participant_ids.begin(), participant_ids.end());
  if (participant_ids.empty() ||
      participant_ids.size() > kMaximumGuestThreadCount ||
      participant_ids.front() == 0 ||
      std::adjacent_find(participant_ids.begin(), participant_ids.end()) !=
          participant_ids.end()) {
    return {0, BarrierError::kInvalidParticipants};
  }

  std::lock_guard<std::mutex> lock(mutex_);
  // Completion makes the previous generation logically inactive before every
  // Poll caller has necessarily returned from its condition wait. This is
  // especially important for in-place restore: the completed generation's
  // suspended guest host threads are terminated and therefore may never
  // decrement the diagnostic waiter count. Generation checks in Poll keep
  // late callers isolated from the new boundary, so only an active phase is a
  // valid reason to reject Begin.
  if (phase_ != BarrierPhase::kIdle) {
    return {0, BarrierError::kBusy};
  }

  uint64_t generation = next_generation_++;
  if (generation == 0) {
    generation = next_generation_++;
  }
  generation_ = generation;
  phase_ = BarrierPhase::kCollecting;
  participants_ = std::move(participant_ids);
  arrived_.clear();
  requested_generation_.store(generation, std::memory_order_release);
  // Also wake any released caller that has not reacquired the mutex yet. Its
  // generation mismatch makes it return without joining this generation.
  condition_.notify_all();
  return {generation, BarrierError::kNone};
}

BarrierWaitResult CooperativeGuestBarrier::WaitForAll(
    uint64_t generation, std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumGuestBarrierWait) {
    return BarrierWaitResult::kInvalidTimeout;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (generation == 0 || generation != generation_) {
    return BarrierWaitResult::kStaleGeneration;
  }
  if (phase_ != BarrierPhase::kCollecting) {
    return BarrierWaitResult::kInvalidPhase;
  }

  const auto all_arrived = [this, generation]() {
    return generation != generation_ ||
           phase_ != BarrierPhase::kCollecting ||
           arrived_.size() == participants_.size();
  };
  if (!condition_.wait_for(lock, timeout, all_arrived)) {
    std::string missing;
    for (uint32_t participant : participants_) {
      if (std::find(arrived_.begin(), arrived_.end(), participant) ==
          arrived_.end()) {
        if (!missing.empty()) {
          missing += ",";
        }
        missing += std::to_string(participant);
      }
    }
    XELOGE(
        "Save-state guest barrier timed out: generation={} arrived={}/{} "
        "missing=[{}]",
        generation, arrived_.size(), participants_.size(), missing);
    CompleteLocked(generation, false);
    return BarrierWaitResult::kTimedOut;
  }
  if (generation != generation_) {
    return BarrierWaitResult::kStaleGeneration;
  }
  if (phase_ != BarrierPhase::kCollecting) {
    return BarrierWaitResult::kInvalidPhase;
  }

  phase_ = BarrierPhase::kHeld;
  return BarrierWaitResult::kReached;
}

BarrierPollResult CooperativeGuestBarrier::Poll(uint32_t participant_id,
                                                uint64_t generation) {
  if (generation == 0 ||
      requested_generation_.load(std::memory_order_acquire) != generation) {
    return BarrierPollResult::kNotRequested;
  }

  std::unique_lock<std::mutex> lock(mutex_);
  if (generation != generation_) {
    return BarrierPollResult::kStaleGeneration;
  }
  if (phase_ != BarrierPhase::kCollecting &&
      phase_ != BarrierPhase::kHeld) {
    return BarrierPollResult::kNotRequested;
  }
  if (!std::binary_search(participants_.begin(), participants_.end(),
                          participant_id)) {
    return BarrierPollResult::kNotRequested;
  }
  if (std::find(arrived_.begin(), arrived_.end(), participant_id) !=
      arrived_.end()) {
    return BarrierPollResult::kDuplicateArrival;
  }

  arrived_.push_back(participant_id);
  ++waiter_count_;
  condition_.notify_all();
  condition_.wait(lock, [this, generation]() {
    return generation != generation_ || phase_ == BarrierPhase::kIdle;
  });
  --waiter_count_;
  condition_.notify_all();

  if (last_completed_generation_ == generation) {
    return last_completion_released_ ? BarrierPollResult::kReleased
                                     : BarrierPollResult::kCancelled;
  }
  return BarrierPollResult::kStaleGeneration;
}

bool CooperativeGuestBarrier::CompleteLocked(uint64_t generation,
                                             bool released) {
  if (generation == 0 || generation != generation_ ||
      phase_ == BarrierPhase::kIdle) {
    return false;
  }
  last_completed_generation_ = generation;
  last_completion_released_ = released;
  requested_generation_.store(0, std::memory_order_release);
  phase_ = BarrierPhase::kIdle;
  participants_.clear();
  arrived_.clear();
  condition_.notify_all();
  return true;
}

bool CooperativeGuestBarrier::Release(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (phase_ != BarrierPhase::kHeld) {
    return false;
  }
  return CompleteLocked(generation, true);
}

bool CooperativeGuestBarrier::Cancel(uint64_t generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return CompleteLocked(generation, false);
}

BarrierStatus CooperativeGuestBarrier::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  BarrierStatus status;
  status.generation = generation_;
  status.phase = phase_;
  status.participant_count = uint32_t(participants_.size());
  status.arrived_count = uint32_t(arrived_.size());
  status.waiter_count = waiter_count_;
  return status;
}

SnapshotCodecValidation ValidateCpuSnapshot(const CpuSnapshot& snapshot) {
  if (snapshot.host_backend != HostBackend::kA64) {
    return CodecError(SnapshotCodecError::kUnsupportedBackend,
                      "CPU snapshot host backend is unsupported.");
  }
  if (snapshot.threads.empty() ||
      snapshot.threads.size() > kMaximumGuestThreadCount) {
    return CodecError(SnapshotCodecError::kInvalidCount,
                      "CPU snapshot thread count is invalid.");
  }

  std::vector<uint32_t> thread_ids;
  thread_ids.reserve(snapshot.threads.size());
  for (const CpuThreadSnapshot& thread : snapshot.threads) {
    if (thread.thread_id == 0 || thread.resume_pc == 0 ||
        (thread.resume_pc & 3) != 0 ||
        (thread.flags & ~kCpuThreadFlagReservationValid) != 0 ||
        thread.xer_ca > 1 || thread.xer_ov > 1 || thread.xer_so > 1 ||
        thread.vscr_sat > 1 || thread.njm_enabled > 1) {
      return CodecError(SnapshotCodecError::kInvalidThread,
                        "CPU snapshot contains invalid thread state.");
    }
    if (thread.stackpoints.empty() ||
        thread.stackpoints.size() > kMaximumGuestStackpointCount ||
        std::any_of(thread.stackpoints.begin(), thread.stackpoints.end(),
                    [](const CpuStackpointSnapshot& stackpoint) {
                      return stackpoint.guest_stack_pointer == 0 ||
                             (stackpoint.guest_stack_pointer & 15) != 0 ||
                             stackpoint.guest_return_address == 0 ||
                             (stackpoint.guest_return_address & 3) != 0 ||
                             stackpoint.host_stack_size == 0 ||
                             (stackpoint.host_stack_size & 15) != 0 ||
                             stackpoint.host_stack_size >
                                 kMaximumCapturedHostStackSize;
                    })) {
      return CodecError(SnapshotCodecError::kInvalidThread,
                        "CPU stackpoint chain is invalid.");
    }
    if (std::find(thread_ids.begin(), thread_ids.end(), thread.thread_id) !=
        thread_ids.end()) {
      return CodecError(SnapshotCodecError::kInvalidThread,
                        "CPU snapshot contains duplicate thread IDs.");
    }
    thread_ids.push_back(thread.thread_id);

    const bool reservation_valid =
        (thread.flags & kCpuThreadFlagReservationValid) != 0;
    if (reservation_valid) {
      if (thread.reservation_address > UINT32_MAX ||
          thread.reservation_granule != 0x10000) {
        return CodecError(SnapshotCodecError::kInvalidThread,
                          "CPU reservation state is unsupported.");
      }
    } else if (thread.reservation_address != 0 ||
               thread.reservation_value != 0 ||
               thread.reservation_granule != 0) {
      return CodecError(SnapshotCodecError::kInvalidThread,
                        "Inactive CPU reservation state is not canonical.");
    }
  }
  return {};
}

SnapshotCodecValidation EncodeCpuSnapshot(const CpuSnapshot& snapshot,
                                          std::vector<uint8_t>* encoded) {
  if (!encoded) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "CPU snapshot output is missing.");
  }
  SnapshotCodecValidation validation = ValidateCpuSnapshot(snapshot);
  if (!validation.ok()) {
    return validation;
  }

  CpuSnapshot canonical = snapshot;
  std::sort(canonical.threads.begin(), canonical.threads.end(),
            [](const CpuThreadSnapshot& left,
               const CpuThreadSnapshot& right) {
              return left.thread_id < right.thread_id;
            });
  uint64_t total_size = kSnapshotHeaderSize;
  for (const CpuThreadSnapshot& thread : canonical.threads) {
    total_size += kCpuThreadFixedRecordSize +
                  uint64_t(thread.stackpoints.size()) * 3 * sizeof(uint32_t);
  }

  Writer writer;
  WriteHeader(&writer, kCpuSnapshotMagic, total_size,
              uint32_t(canonical.threads.size()),
              uint32_t(canonical.host_backend));
  for (const CpuThreadSnapshot& thread : canonical.threads) {
    writer.WriteLittleEndian(thread.thread_id);
    writer.WriteLittleEndian(thread.resume_pc);
    writer.WriteLittleEndian(thread.flags);
    writer.WriteLittleEndian(uint32_t(0));
    for (uint64_t value : thread.gpr) {
      writer.WriteLittleEndian(value);
    }
    writer.WriteLittleEndian(thread.ctr);
    writer.WriteLittleEndian(thread.lr);
    writer.WriteLittleEndian(thread.msr);
    for (uint64_t value : thread.fpr_bits) {
      writer.WriteLittleEndian(value);
    }
    for (const auto& value : thread.vector_registers) {
      writer.WriteBytes(value.data(), value.size());
    }
    writer.WriteBytes(thread.vscr_vector.data(), thread.vscr_vector.size());
    for (uint32_t value : thread.condition_registers) {
      writer.WriteLittleEndian(value);
    }
    writer.WriteLittleEndian(thread.fpscr);
    writer.WriteLittleEndian(thread.vrsave);
    writer.WriteLittleEndian(thread.reservation_address);
    writer.WriteLittleEndian(thread.reservation_value);
    writer.WriteLittleEndian(thread.reservation_granule);
    writer.WriteLittleEndian(thread.xer_ca);
    writer.WriteLittleEndian(thread.xer_ov);
    writer.WriteLittleEndian(thread.xer_so);
    writer.WriteLittleEndian(thread.vscr_sat);
    writer.WriteLittleEndian(thread.njm_enabled);
    writer.WriteZeros(3);
    writer.WriteLittleEndian(uint32_t(thread.stackpoints.size()));
    for (const CpuStackpointSnapshot& stackpoint : thread.stackpoints) {
      writer.WriteLittleEndian(stackpoint.guest_stack_pointer);
      writer.WriteLittleEndian(stackpoint.guest_return_address);
      writer.WriteLittleEndian(stackpoint.host_stack_size);
    }
  }
  *encoded = writer.Take();
  return {};
}

SnapshotCodecValidation DecodeCpuSnapshot(const uint8_t* data, size_t data_size,
                                          CpuSnapshot* snapshot) {
  if (!data || !snapshot) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "CPU snapshot input is missing.");
  }
  Reader reader(data, data_size);
  uint32_t thread_count = 0;
  uint32_t backend = 0;
  SnapshotCodecValidation validation =
      ReadHeader(&reader, kCpuSnapshotMagic, &thread_count, &backend, data_size);
  if (!validation.ok()) {
    return validation;
  }
  if (thread_count == 0 || thread_count > kMaximumGuestThreadCount ||
      uint64_t(data_size) <
          kSnapshotHeaderSize +
              uint64_t(thread_count) * kCpuThreadFixedRecordSize) {
    return CodecError(SnapshotCodecError::kInvalidSize,
                      "CPU snapshot record count or size is invalid.");
  }

  CpuSnapshot decoded;
  decoded.host_backend = HostBackend(backend);
  decoded.threads.reserve(thread_count);
  for (uint32_t index = 0; index < thread_count; ++index) {
    CpuThreadSnapshot thread;
    uint32_t reserved = 0;
    std::array<uint8_t, 3> tail_reserved = {};
    if (!reader.ReadLittleEndian(&thread.thread_id) ||
        !reader.ReadLittleEndian(&thread.resume_pc) ||
        !reader.ReadLittleEndian(&thread.flags) ||
        !reader.ReadLittleEndian(&reserved)) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "CPU thread record is truncated.");
    }
    if (reserved != 0) {
      return CodecError(SnapshotCodecError::kUnsupportedFlags,
                        "CPU thread reserved fields are nonzero.");
    }
    for (uint64_t& value : thread.gpr) {
      if (!reader.ReadLittleEndian(&value)) {
        return CodecError(SnapshotCodecError::kTruncated,
                          "CPU GPR state is truncated.");
      }
    }
    if (!reader.ReadLittleEndian(&thread.ctr) ||
        !reader.ReadLittleEndian(&thread.lr) ||
        !reader.ReadLittleEndian(&thread.msr)) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "CPU control state is truncated.");
    }
    for (uint64_t& value : thread.fpr_bits) {
      if (!reader.ReadLittleEndian(&value)) {
        return CodecError(SnapshotCodecError::kTruncated,
                          "CPU FPR state is truncated.");
      }
    }
    for (auto& value : thread.vector_registers) {
      if (!reader.ReadBytes(value.data(), value.size())) {
        return CodecError(SnapshotCodecError::kTruncated,
                          "CPU vector state is truncated.");
      }
    }
    if (!reader.ReadBytes(thread.vscr_vector.data(),
                          thread.vscr_vector.size())) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "CPU VSCR vector is truncated.");
    }
    for (uint32_t& value : thread.condition_registers) {
      if (!reader.ReadLittleEndian(&value)) {
        return CodecError(SnapshotCodecError::kTruncated,
                          "CPU condition state is truncated.");
      }
    }
    if (!reader.ReadLittleEndian(&thread.fpscr) ||
        !reader.ReadLittleEndian(&thread.vrsave) ||
        !reader.ReadLittleEndian(&thread.reservation_address) ||
        !reader.ReadLittleEndian(&thread.reservation_value) ||
        !reader.ReadLittleEndian(&thread.reservation_granule) ||
        !reader.ReadLittleEndian(&thread.xer_ca) ||
        !reader.ReadLittleEndian(&thread.xer_ov) ||
        !reader.ReadLittleEndian(&thread.xer_so) ||
        !reader.ReadLittleEndian(&thread.vscr_sat) ||
        !reader.ReadLittleEndian(&thread.njm_enabled) ||
        !reader.ReadBytes(tail_reserved.data(), tail_reserved.size())) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "CPU tail state is truncated.");
    }
    if (std::any_of(tail_reserved.begin(), tail_reserved.end(),
                    [](uint8_t value) { return value != 0; })) {
      return CodecError(SnapshotCodecError::kUnsupportedFlags,
                        "CPU tail reserved fields are nonzero.");
    }
    uint32_t stackpoint_count = 0;
    if (!reader.ReadLittleEndian(&stackpoint_count)) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "CPU stackpoint count is truncated.");
    }
    if (stackpoint_count == 0 ||
        stackpoint_count > kMaximumGuestStackpointCount ||
        uint64_t(stackpoint_count) * 3 * sizeof(uint32_t) >
            reader.remaining()) {
      return CodecError(SnapshotCodecError::kInvalidCount,
                        "CPU stackpoint count is invalid.");
    }
    thread.stackpoints.resize(stackpoint_count);
    for (CpuStackpointSnapshot& stackpoint : thread.stackpoints) {
      if (!reader.ReadLittleEndian(&stackpoint.guest_stack_pointer) ||
          !reader.ReadLittleEndian(&stackpoint.guest_return_address) ||
          !reader.ReadLittleEndian(&stackpoint.host_stack_size)) {
        return CodecError(SnapshotCodecError::kTruncated,
                          "CPU stackpoint chain is truncated.");
      }
    }
    decoded.threads.push_back(std::move(thread));
  }
  if (reader.remaining() != 0) {
    return CodecError(SnapshotCodecError::kTrailingData,
                      "CPU snapshot contains trailing data.");
  }
  validation = ValidateCpuSnapshot(decoded);
  if (!validation.ok()) {
    return validation;
  }
  if (!std::is_sorted(decoded.threads.begin(), decoded.threads.end(),
                      [](const CpuThreadSnapshot& left,
                         const CpuThreadSnapshot& right) {
                        return left.thread_id < right.thread_id;
                      })) {
    return CodecError(SnapshotCodecError::kInvalidThread,
                      "CPU thread records are not in canonical order.");
  }
  *snapshot = std::move(decoded);
  return {};
}

SnapshotCodecValidation ValidateClockSnapshot(const ClockSnapshot& snapshot) {
  if (snapshot.flags != kClockSnapshotFlagScaled ||
      snapshot.guest_tick_frequency == 0 ||
      snapshot.tick_ratio_numerator == 0 ||
      snapshot.tick_ratio_denominator == 0 ||
      snapshot.guest_system_time_base == 0 ||
      snapshot.guest_interrupt_time == 0) {
    return CodecError(SnapshotCodecError::kInvalidClock,
                      "Clock snapshot state is invalid or unsupported.");
  }
  double scalar = 0.0;
  static_assert(sizeof(scalar) == sizeof(snapshot.time_scalar_bits));
  std::memcpy(&scalar, &snapshot.time_scalar_bits, sizeof(scalar));
  if (!std::isfinite(scalar) || scalar <= 0.0) {
    return CodecError(SnapshotCodecError::kInvalidClock,
                      "Clock scalar is invalid.");
  }
  return {};
}

SnapshotCodecValidation EncodeClockSnapshot(const ClockSnapshot& snapshot,
                                            std::vector<uint8_t>* encoded) {
  if (!encoded) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "Clock snapshot output is missing.");
  }
  SnapshotCodecValidation validation = ValidateClockSnapshot(snapshot);
  if (!validation.ok()) {
    return validation;
  }

  Writer writer;
  WriteHeader(&writer, kClockSnapshotMagic, kClockSnapshotSize, 1,
              snapshot.flags);
  writer.WriteLittleEndian(snapshot.guest_tick_count);
  writer.WriteLittleEndian(snapshot.guest_tick_frequency);
  writer.WriteLittleEndian(snapshot.tick_ratio_numerator);
  writer.WriteLittleEndian(snapshot.tick_ratio_denominator);
  writer.WriteLittleEndian(snapshot.guest_system_time_base);
  writer.WriteLittleEndian(snapshot.guest_interrupt_time);
  writer.WriteLittleEndian(snapshot.time_scalar_bits);
  *encoded = writer.Take();
  return {};
}

SnapshotCodecValidation DecodeClockSnapshot(const uint8_t* data,
                                            size_t data_size,
                                            ClockSnapshot* snapshot) {
  if (!data || !snapshot) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "Clock snapshot input is missing.");
  }
  Reader reader(data, data_size);
  uint32_t count = 0;
  uint32_t flags = 0;
  SnapshotCodecValidation validation = ReadHeader(
      &reader, kClockSnapshotMagic, &count, &flags, data_size);
  if (!validation.ok()) {
    return validation;
  }
  if (count != 1 || data_size != kClockSnapshotSize) {
    return CodecError(SnapshotCodecError::kInvalidSize,
                      "Clock snapshot size is invalid.");
  }

  ClockSnapshot decoded;
  decoded.flags = flags;
  if (!reader.ReadLittleEndian(&decoded.guest_tick_count) ||
      !reader.ReadLittleEndian(&decoded.guest_tick_frequency) ||
      !reader.ReadLittleEndian(&decoded.tick_ratio_numerator) ||
      !reader.ReadLittleEndian(&decoded.tick_ratio_denominator) ||
      !reader.ReadLittleEndian(&decoded.guest_system_time_base) ||
      !reader.ReadLittleEndian(&decoded.guest_interrupt_time) ||
      !reader.ReadLittleEndian(&decoded.time_scalar_bits)) {
    return CodecError(SnapshotCodecError::kTruncated,
                      "Clock snapshot is truncated.");
  }
  if (reader.remaining() != 0) {
    return CodecError(SnapshotCodecError::kTrailingData,
                      "Clock snapshot contains trailing data.");
  }
  validation = ValidateClockSnapshot(decoded);
  if (!validation.ok()) {
    return validation;
  }
  *snapshot = decoded;
  return {};
}

SnapshotCodecValidation ValidateMemorySnapshot(
    const MemorySnapshot& snapshot) {
  if (snapshot.pages.empty() ||
      snapshot.pages.size() > kMaximumMemoryPageCount) {
    return CodecError(SnapshotCodecError::kInvalidCount,
                      "Memory snapshot page count is invalid.");
  }
  if (!std::is_sorted(snapshot.pages.begin(), snapshot.pages.end(),
                      MemoryPageLess)) {
    return CodecError(SnapshotCodecError::kDuplicateOrOverlappingMemory,
                      "Memory pages are not in canonical order.");
  }

  uint64_t encoded_size =
      kSnapshotHeaderSize +
      uint64_t(snapshot.pages.size()) * kMemoryPageRecordSize;
  const MemoryPageSnapshot* previous = nullptr;
  for (const MemoryPageSnapshot& page : snapshot.pages) {
    if ((page.address_space != MemoryAddressSpace::kVirtual &&
         page.address_space != MemoryAddressSpace::kPhysical) ||
        !IsSupportedPageSize(page.page_size) ||
        (page.address % page.page_size) != 0 ||
        uint64_t(page.address) + page.page_size > UINT64_C(0x100000000) ||
        page.allocation_page_count == 0 ||
        (page.allocation_base % page.page_size) != 0 ||
        page.address < page.allocation_base ||
        uint64_t(page.allocation_base) +
                uint64_t(page.allocation_page_count) * page.page_size >
            UINT64_C(0x100000000) ||
        uint64_t(page.address) + page.page_size >
            uint64_t(page.allocation_base) +
                uint64_t(page.allocation_page_count) * page.page_size) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Memory page geometry is invalid.");
    }

    constexpr uint32_t kKnownState =
        kMemorySnapshotReserve | kMemorySnapshotCommit;
    constexpr uint32_t kKnownProtect =
        kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite |
        kMemorySnapshotProtectNoCache |
        kMemorySnapshotProtectWriteCombine;
    if ((page.state != kMemorySnapshotReserve &&
         page.state != kKnownState) ||
        (page.allocation_protect & ~kKnownProtect) != 0 ||
        (page.current_protect & ~kKnownProtect) != 0) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Memory page state or protection is unsupported.");
    }

    const bool committed = (page.state & kMemorySnapshotCommit) != 0;
    const bool aliases_physical =
        page.backing_physical_address != kNoPhysicalBacking;
    if (page.address_space == MemoryAddressSpace::kPhysical &&
        uint64_t(page.address) + page.page_size > UINT64_C(0x20000000)) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Physical memory page is outside the guest range.");
    }
    if (!committed) {
      if (!page.data.empty() || aliases_physical) {
        return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                          "Reserved memory page contains backing data.");
      }
    } else if (page.address_space == MemoryAddressSpace::kPhysical) {
      if (aliases_physical || page.data.size() != page.page_size) {
        return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                          "Physical memory page backing is invalid.");
      }
    } else if (aliases_physical) {
      if (!page.data.empty() ||
          (page.backing_physical_address % page.page_size) != 0 ||
          uint64_t(page.backing_physical_address) + page.page_size >
              UINT64_C(0x20000000)) {
        return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                          "Virtual memory alias is invalid.");
      }
    } else if (page.data.size() != page.page_size) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Committed virtual memory page data is invalid.");
    }

    if (previous &&
        previous->address_space == page.address_space &&
        uint64_t(previous->address) + previous->page_size > page.address) {
      return CodecError(SnapshotCodecError::kDuplicateOrOverlappingMemory,
                        "Memory pages overlap.");
    }
    previous = &page;

    encoded_size += page.data.size();
    encoded_size = (encoded_size + 7) & ~UINT64_C(7);
    if (encoded_size > kMaximumMemorySnapshotSize) {
      return CodecError(SnapshotCodecError::kInvalidSize,
                        "Memory snapshot exceeds the size limit.");
    }
  }
  return {};
}

SnapshotCodecValidation EncodeMemorySnapshot(const MemorySnapshot& snapshot,
                                             std::vector<uint8_t>* encoded) {
  if (!encoded) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "Memory snapshot output is missing.");
  }
  MemorySnapshot canonical = snapshot;
  std::sort(canonical.pages.begin(), canonical.pages.end(), MemoryPageLess);
  SnapshotCodecValidation validation = ValidateMemorySnapshot(canonical);
  if (!validation.ok()) {
    return validation;
  }

  uint64_t total_size =
      kSnapshotHeaderSize +
      uint64_t(canonical.pages.size()) * kMemoryPageRecordSize;
  for (const MemoryPageSnapshot& page : canonical.pages) {
    total_size += page.data.size();
    total_size = (total_size + 7) & ~UINT64_C(7);
  }

  Writer writer;
  WriteHeader(&writer, kMemorySnapshotMagic, total_size,
              uint32_t(canonical.pages.size()), 0);
  for (const MemoryPageSnapshot& page : canonical.pages) {
    writer.WriteLittleEndian(uint32_t(page.address_space));
    writer.WriteLittleEndian(page.address);
    writer.WriteLittleEndian(page.page_size);
    writer.WriteLittleEndian(page.state);
    writer.WriteLittleEndian(page.allocation_base);
    writer.WriteLittleEndian(page.allocation_page_count);
    writer.WriteLittleEndian(page.allocation_protect);
    writer.WriteLittleEndian(page.current_protect);
    writer.WriteLittleEndian(page.backing_physical_address);
    writer.WriteLittleEndian(uint32_t(page.data.size()));
    writer.WriteLittleEndian(uint64_t(0));
    writer.WriteBytes(page.data.data(), page.data.size());
    writer.Align(8);
  }
  *encoded = writer.Take();
  return {};
}

SnapshotCodecValidation DecodeMemorySnapshot(const uint8_t* data,
                                             size_t data_size,
                                             MemorySnapshot* snapshot) {
  if (!data || !snapshot) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "Memory snapshot input is missing.");
  }
  if (data_size > kMaximumMemorySnapshotSize) {
    return CodecError(SnapshotCodecError::kInvalidSize,
                      "Memory snapshot exceeds the size limit.");
  }

  Reader reader(data, data_size);
  uint32_t page_count = 0;
  uint32_t flags = 0;
  SnapshotCodecValidation validation = ReadHeader(
      &reader, kMemorySnapshotMagic, &page_count, &flags, data_size);
  if (!validation.ok()) {
    return validation;
  }
  if (flags != 0) {
    return CodecError(SnapshotCodecError::kUnsupportedFlags,
                      "Memory snapshot flags are unsupported.");
  }
  if (page_count == 0 || page_count > kMaximumMemoryPageCount ||
      uint64_t(page_count) * kMemoryPageRecordSize >
          uint64_t(reader.remaining())) {
    return CodecError(SnapshotCodecError::kInvalidCount,
                      "Memory snapshot page count is invalid.");
  }

  MemorySnapshot decoded;
  decoded.pages.reserve(page_count);
  for (uint32_t index = 0; index < page_count; ++index) {
    MemoryPageSnapshot page;
    uint32_t address_space = 0;
    uint32_t data_size_u32 = 0;
    uint64_t reserved = 0;
    if (!reader.ReadLittleEndian(&address_space) ||
        !reader.ReadLittleEndian(&page.address) ||
        !reader.ReadLittleEndian(&page.page_size) ||
        !reader.ReadLittleEndian(&page.state) ||
        !reader.ReadLittleEndian(&page.allocation_base) ||
        !reader.ReadLittleEndian(&page.allocation_page_count) ||
        !reader.ReadLittleEndian(&page.allocation_protect) ||
        !reader.ReadLittleEndian(&page.current_protect) ||
        !reader.ReadLittleEndian(&page.backing_physical_address) ||
        !reader.ReadLittleEndian(&data_size_u32) ||
        !reader.ReadLittleEndian(&reserved)) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "Memory page record is truncated.");
    }
    if (reserved != 0 || data_size_u32 > reader.remaining()) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Memory page record fields are invalid.");
    }
    if (!IsSupportedPageSize(page.page_size) ||
        (data_size_u32 != 0 && data_size_u32 != page.page_size)) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Memory page payload size is unsupported.");
    }
    page.address_space = MemoryAddressSpace(address_space);
    page.data.resize(data_size_u32);
    if (data_size_u32 &&
        !reader.ReadBytes(page.data.data(), page.data.size())) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "Memory page data is truncated.");
    }
    if (!reader.Align(8)) {
      return CodecError(SnapshotCodecError::kTruncated,
                        "Memory page padding is truncated.");
    }
    decoded.pages.push_back(std::move(page));
  }
  if (reader.remaining() != 0) {
    return CodecError(SnapshotCodecError::kTrailingData,
                      "Memory snapshot contains trailing data.");
  }
  validation = ValidateMemorySnapshot(decoded);
  if (!validation.ok()) {
    return validation;
  }
  *snapshot = std::move(decoded);
  return {};
}

SnapshotCodecValidation ValidateMemoryAllocationInventory(
    const MemoryAllocationInventory& inventory) {
  if (inventory.pages.size() > kMaximumMemoryPageCount) {
    return CodecError(SnapshotCodecError::kInvalidCount,
                      "Memory inventory page count is invalid.");
  }
  std::vector<MemoryInventoryPage> pages = inventory.pages;
  std::sort(pages.begin(), pages.end(), MemoryInventoryPageLess);
  constexpr uint32_t kKnownState =
      kMemorySnapshotReserve | kMemorySnapshotCommit;
  constexpr uint32_t kKnownProtect =
      kMemorySnapshotProtectRead | kMemorySnapshotProtectWrite |
      kMemorySnapshotProtectNoCache | kMemorySnapshotProtectWriteCombine;
  for (size_t index = 0; index < pages.size(); ++index) {
    const MemoryInventoryPage& page = pages[index];
    const uint64_t allocation_end =
        uint64_t(page.allocation_base) +
        uint64_t(page.allocation_page_count) * page.page_size;
    if ((page.address_space != MemoryAddressSpace::kVirtual &&
         page.address_space != MemoryAddressSpace::kPhysical) ||
        !IsSupportedPageSize(page.page_size) ||
        (page.address % page.page_size) != 0 ||
        uint64_t(page.address) + page.page_size > UINT64_C(0x100000000) ||
        page.allocation_page_count == 0 ||
        (page.allocation_base % page.page_size) != 0 ||
        page.address < page.allocation_base ||
        allocation_end > UINT64_C(0x100000000) ||
        uint64_t(page.address) + page.page_size > allocation_end ||
        (page.state != kMemorySnapshotReserve &&
         page.state != kKnownState) ||
        (page.allocation_protect & ~kKnownProtect) != 0 ||
        (page.current_protect & ~kKnownProtect) != 0 ||
        (page.address_space == MemoryAddressSpace::kPhysical &&
         (uint64_t(page.address) + page.page_size > UINT64_C(0x20000000) ||
          page.backing_physical_address != kNoPhysicalBacking)) ||
        (page.state == kMemorySnapshotReserve &&
         page.backing_physical_address != kNoPhysicalBacking) ||
        (page.backing_physical_address != kNoPhysicalBacking &&
         ((page.backing_physical_address % page.page_size) != 0 ||
          uint64_t(page.backing_physical_address) + page.page_size >
              UINT64_C(0x20000000))) ||
        (index && pages[index - 1].address_space == page.address_space &&
         uint64_t(pages[index - 1].address) +
                 pages[index - 1].page_size >
             page.address)) {
      return CodecError(SnapshotCodecError::kInvalidMemoryPage,
                        "Memory inventory contains an invalid page.");
    }
  }
  return {};
}

SnapshotCodecValidation BuildMemoryRestorePlan(
    const MemoryAllocationInventory& current, const MemorySnapshot& target,
    MemoryRestorePlan* plan) {
  if (!plan) {
    return CodecError(SnapshotCodecError::kInvalidArgument,
                      "Memory restore plan output is missing.");
  }
  MemorySnapshot canonical_target = target;
  std::sort(canonical_target.pages.begin(), canonical_target.pages.end(),
            MemoryPageLess);
  SnapshotCodecValidation validation =
      ValidateMemorySnapshot(canonical_target);
  if (!validation.ok()) {
    return validation;
  }

  std::vector<MemoryInventoryPage> current_pages = current.pages;
  const auto key = [](MemoryAddressSpace space, uint32_t address) {
    return (uint64_t(uint32_t(space)) << 32) | address;
  };
  validation = ValidateMemoryAllocationInventory(current);
  if (!validation.ok()) {
    return validation;
  }
  std::sort(current_pages.begin(), current_pages.end(),
            MemoryInventoryPageLess);

  MemoryRestorePlan built;
  size_t current_index = 0;
  size_t target_index = 0;
  while (current_index < current_pages.size() ||
         target_index < canonical_target.pages.size()) {
    const uint64_t current_key =
        current_index < current_pages.size()
            ? key(current_pages[current_index].address_space,
                  current_pages[current_index].address)
            : UINT64_MAX;
    const uint64_t target_key =
        target_index < canonical_target.pages.size()
            ? key(canonical_target.pages[target_index].address_space,
                  canonical_target.pages[target_index].address)
            : UINT64_MAX;
    MemoryRestoreTransition transition;
    if (current_key <= target_key) {
      transition.current_exists = true;
      transition.current = current_pages[current_index++];
    }
    if (target_key <= current_key) {
      const MemoryPageSnapshot& page = canonical_target.pages[target_index++];
      transition.target_exists = true;
      transition.target = {
          page.address_space,
          page.address,
          page.page_size,
          page.state,
          page.allocation_base,
          page.allocation_page_count,
          page.allocation_protect,
          page.current_protect,
          page.backing_physical_address,
      };
    }
    built.transitions.push_back(std::move(transition));
  }
  *plan = std::move(built);
  return {};
}

StateProviderResult CaptureMemorySnapshot(
    const MemoryAllocationInventory& inventory, MemoryContentReader& reader,
    const MemoryCaptureLimits& limits, MemorySnapshot* snapshot,
    std::string* error_message) {
  if (!snapshot || limits.maximum_page_count == 0 ||
      limits.maximum_page_count > kMaximumMemoryPageCount ||
      limits.maximum_content_bytes > kMaximumMemorySnapshotSize ||
      inventory.pages.empty() ||
      inventory.pages.size() > limits.maximum_page_count) {
    if (error_message) {
      *error_message = "Memory capture arguments or limits are invalid.";
    }
    return StateProviderResult::kInvalidState;
  }
  const SnapshotCodecValidation inventory_validation =
      ValidateMemoryAllocationInventory(inventory);
  if (!inventory_validation.ok()) {
    if (error_message) {
      *error_message = inventory_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }

  std::vector<MemoryInventoryPage> pages = inventory.pages;
  std::sort(pages.begin(), pages.end(), MemoryInventoryPageLess);
  MemorySnapshot captured;
  captured.pages.reserve(pages.size());
  uint64_t content_bytes = 0;
  for (const MemoryInventoryPage& source : pages) {
    MemoryPageSnapshot page = {
        source.address_space,
        source.address,
        source.page_size,
        source.state,
        source.allocation_base,
        source.allocation_page_count,
        source.allocation_protect,
        source.current_protect,
        source.backing_physical_address,
        {},
    };
    const bool committed = (source.state & kMemorySnapshotCommit) != 0;
    if (committed &&
        source.backing_physical_address == kNoPhysicalBacking) {
      if (source.page_size >
          limits.maximum_content_bytes - content_bytes) {
        if (error_message) {
          *error_message = "Memory capture content limit was exceeded.";
        }
        return StateProviderResult::kInvalidState;
      }
      page.data.resize(source.page_size);
      const StateProviderResult read_result =
          reader.ReadPage(source.address_space, source.address,
                          page.data.data(), page.data.size(), error_message);
      if (read_result != StateProviderResult::kOk) {
        return read_result;
      }
      content_bytes += source.page_size;
    }
    captured.pages.push_back(std::move(page));
  }

  const SnapshotCodecValidation snapshot_validation =
      ValidateMemorySnapshot(captured);
  if (!snapshot_validation.ok()) {
    if (error_message) {
      *error_message = snapshot_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  *snapshot = std::move(captured);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

}  // namespace xe::save_state
