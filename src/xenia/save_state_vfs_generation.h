/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_VFS_GENERATION_H_
#define XENIA_SAVE_STATE_VFS_GENERATION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xe::save_state {

constexpr size_t kMaximumVfsSnapshotMountCount = 64;
constexpr size_t kMaximumVfsSnapshotFileCount = 65536;
constexpr size_t kMaximumVfsSnapshotOpenFileCount = 4096;
constexpr size_t kMaximumVfsSnapshotPathLength = 1024;
constexpr uint64_t kMaximumVfsSnapshotContentSize = UINT64_C(1) << 30;

enum class VfsGenerationResult {
  kOk,
  kBusy,
  kUnsupported,
  kInvalidState,
  kLimitExceeded,
  kFailed,
};

enum class VfsMountBackingMode {
  kManagedImmutableCow,
  kDirectHostPath,
  kMappedHostPath,
  kAsyncHostPath,
  kRawHostHandle,
};

enum class VfsOpenFileMode {
  kSynchronous,
  kAsynchronous,
  kMapped,
  kRawHostHandle,
};

struct VfsGenerationLimits {
  size_t maximum_mount_count = kMaximumVfsSnapshotMountCount;
  size_t maximum_file_count = kMaximumVfsSnapshotFileCount;
  size_t maximum_open_file_count = kMaximumVfsSnapshotOpenFileCount;
  size_t maximum_path_length = kMaximumVfsSnapshotPathLength;
  uint64_t maximum_file_content_bytes = kMaximumVfsSnapshotContentSize;
  uint64_t maximum_total_content_bytes = kMaximumVfsSnapshotContentSize;
};

struct VfsFileGeneration {
  uint64_t file_identity = 0;
  std::string guest_path;
  std::shared_ptr<const std::vector<uint8_t>> content;
};

struct VfsMountGeneration {
  uint64_t mount_identity = 0;
  uint64_t generation = 0;
  std::string guest_mount_path;
  VfsMountBackingMode backing_mode =
      VfsMountBackingMode::kManagedImmutableCow;
  bool writable = false;
  std::vector<VfsFileGeneration> files;
};

struct VfsOpenFileSnapshot {
  uint64_t handle_identity = 0;
  uint64_t mount_identity = 0;
  uint64_t file_identity = 0;
  uint64_t generation = 0;
  std::string guest_path;
  uint64_t position = 0;
  uint32_t file_access = 0;
  VfsOpenFileMode mode = VfsOpenFileMode::kSynchronous;
  bool is_directory = false;
  bool delete_on_close = false;
  bool has_pending_io = false;
  bool has_completion_port = false;
};

// Mount and file nodes are immutable and may be shared between snapshots
// captured from one store. Store creation and restore staging deep-copy
// caller-owned roots so retained mutable aliases cannot change them.
// Open-file metadata is copied by value because position advances
// independently. This is a same-process detached model, not a serialized VFS
// section and not an adapter for live host files.
struct VfsGenerationSnapshot {
  uint64_t store_identity = 0;
  std::vector<std::shared_ptr<const VfsMountGeneration>> mounts;
  std::vector<VfsOpenFileSnapshot> open_files;
};

struct VfsGenerationValidation {
  VfsGenerationResult result = VfsGenerationResult::kInvalidState;
  std::string message;

  bool ok() const { return result == VfsGenerationResult::kOk; }
};

VfsGenerationValidation ValidateVfsGenerationSnapshot(
    const VfsGenerationSnapshot& snapshot,
    const VfsGenerationLimits& limits);

class DetachedVfsRestoreTransaction;

// Bounded immutable-generation prototype. AdvanceWrite clones only the target
// mount node and target file contents. Saved roots continue to reference the
// previous immutable generation.
//
// This store deliberately accepts only managed synchronous regular files. It
// rejects host-path, mapped, raw-handle, async, directory-enumeration,
// delete-on-close, pending-I/O, and completion-port state.
class DetachedVfsGenerationStore {
 public:
  DetachedVfsGenerationStore(const DetachedVfsGenerationStore&) = delete;
  DetachedVfsGenerationStore& operator=(
      const DetachedVfsGenerationStore&) = delete;

  static VfsGenerationResult Create(
      VfsGenerationSnapshot initial, VfsGenerationLimits limits,
      std::unique_ptr<DetachedVfsGenerationStore>* output,
      std::string* error_message);

  VfsGenerationResult CaptureSnapshot(VfsGenerationSnapshot* output,
                                      std::string* error_message) const;
  VfsGenerationResult AdvanceWrite(
      uint64_t mount_identity, uint64_t file_identity, uint64_t byte_offset,
      const std::vector<uint8_t>& data, std::string* error_message);
  VfsGenerationResult SetOpenFilePosition(uint64_t handle_identity,
                                          uint64_t position,
                                          std::string* error_message);

  VfsGenerationResult PrepareRestore(
      const VfsGenerationSnapshot& target,
      std::unique_ptr<DetachedVfsRestoreTransaction>* output,
      std::string* error_message);

 private:
  friend class DetachedVfsRestoreTransaction;

  DetachedVfsGenerationStore(VfsGenerationSnapshot initial,
                             VfsGenerationLimits limits)
      : current_(std::move(initial)), limits_(limits) {}

  mutable std::mutex mutex_;
  VfsGenerationSnapshot current_;
  VfsGenerationLimits limits_;
  uint64_t next_transaction_id_ = 1;
  uint64_t active_transaction_id_ = 0;
};

// Prepare allocates and validates a detached target root before ownership is
// acquired. Commit and rollback only swap the complete root graph while
// holding the store mutex; they perform no allocation and cannot expose a
// partially switched set of mounts or open-file positions.
class DetachedVfsRestoreTransaction {
 public:
  ~DetachedVfsRestoreTransaction();
  DetachedVfsRestoreTransaction(const DetachedVfsRestoreTransaction&) =
      delete;
  DetachedVfsRestoreTransaction& operator=(
      const DetachedVfsRestoreTransaction&) = delete;

  VfsGenerationResult Commit(std::string* error_message) noexcept;
  VfsGenerationResult CommitWithInjectedFailureForTesting(
      std::string* error_message) noexcept;
  VfsGenerationResult Rollback() noexcept;
  VfsGenerationResult Finalize() noexcept;

 private:
  friend class DetachedVfsGenerationStore;

  enum class State {
    kStaged,
    kCommitted,
    kFinished,
  };

  DetachedVfsRestoreTransaction(DetachedVfsGenerationStore* store,
                                VfsGenerationSnapshot staged)
      : store_(store), staged_(std::move(staged)) {}

  DetachedVfsGenerationStore* store_ = nullptr;
  VfsGenerationSnapshot staged_;
  uint64_t transaction_id_ = 0;
  State state_ = State::kStaged;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_VFS_GENERATION_H_
