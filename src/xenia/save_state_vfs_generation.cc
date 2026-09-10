/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_vfs_generation.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace xe::save_state {
namespace {

VfsGenerationResult Fail(VfsGenerationResult result, const char* message,
                         std::string* error_message) {
  if (error_message) {
    *error_message = message;
  }
  return result;
}

VfsGenerationValidation Invalid(VfsGenerationResult result,
                                const char* message) {
  return {result, message};
}

bool LimitsValid(const VfsGenerationLimits& limits) {
  return limits.maximum_mount_count != 0 &&
         limits.maximum_mount_count <= kMaximumVfsSnapshotMountCount &&
         limits.maximum_file_count != 0 &&
         limits.maximum_file_count <= kMaximumVfsSnapshotFileCount &&
         limits.maximum_open_file_count <=
             kMaximumVfsSnapshotOpenFileCount &&
         limits.maximum_path_length != 0 &&
         limits.maximum_path_length <= kMaximumVfsSnapshotPathLength &&
         limits.maximum_file_content_bytes != 0 &&
         limits.maximum_file_content_bytes <=
             kMaximumVfsSnapshotContentSize &&
         limits.maximum_total_content_bytes != 0 &&
         limits.maximum_total_content_bytes <=
             kMaximumVfsSnapshotContentSize;
}

bool IsCanonicalMountPath(const std::string& path, size_t maximum_length) {
  if (path.size() < 2 || path.size() > maximum_length ||
      path.back() != ':') {
    return false;
  }
  for (char value : path) {
    const uint8_t character = static_cast<uint8_t>(value);
    if (character <= 0x20 || character >= 0x7F || value == '/' ||
        value == '\\') {
      return false;
    }
  }
  return true;
}

bool IsCanonicalFilePath(const std::string& mount_path,
                         const std::string& path, size_t maximum_length) {
  if (path.size() <= mount_path.size() + 1 ||
      path.size() > maximum_length ||
      path.compare(0, mount_path.size(), mount_path) != 0 ||
      path[mount_path.size()] != '\\' || path.back() == '\\') {
    return false;
  }

  size_t component_start = mount_path.size() + 1;
  for (size_t index = component_start; index <= path.size(); ++index) {
    if (index != path.size() && path[index] != '\\') {
      const uint8_t character = static_cast<uint8_t>(path[index]);
      if (character <= 0x20 || character >= 0x7F || path[index] == '/') {
        return false;
      }
      continue;
    }
    const size_t component_size = index - component_start;
    if (component_size == 0 ||
        (component_size == 1 && path[component_start] == '.') ||
        (component_size == 2 && path[component_start] == '.' &&
         path[component_start + 1] == '.')) {
      return false;
    }
    component_start = index + 1;
  }
  return true;
}

const VfsMountGeneration* FindMount(const VfsGenerationSnapshot& snapshot,
                                    uint64_t mount_identity) {
  for (const auto& mount : snapshot.mounts) {
    if (mount && mount->mount_identity == mount_identity) {
      return mount.get();
    }
  }
  return nullptr;
}

const VfsFileGeneration* FindFile(const VfsMountGeneration& mount,
                                  uint64_t file_identity) {
  for (const VfsFileGeneration& file : mount.files) {
    if (file.file_identity == file_identity) {
      return &file;
    }
  }
  return nullptr;
}

bool RestoreTopologyCompatible(const VfsGenerationSnapshot& current,
                               const VfsGenerationSnapshot& target) {
  if (current.store_identity != target.store_identity ||
      current.mounts.size() != target.mounts.size() ||
      current.open_files.size() != target.open_files.size()) {
    return false;
  }
  for (size_t index = 0; index < current.mounts.size(); ++index) {
    const VfsMountGeneration& current_mount = *current.mounts[index];
    const VfsMountGeneration& target_mount = *target.mounts[index];
    if (current_mount.mount_identity != target_mount.mount_identity ||
        current_mount.guest_mount_path != target_mount.guest_mount_path ||
        current_mount.backing_mode != target_mount.backing_mode ||
        current_mount.writable != target_mount.writable) {
      return false;
    }
  }
  for (size_t index = 0; index < current.open_files.size(); ++index) {
    const VfsOpenFileSnapshot& current_file = current.open_files[index];
    const VfsOpenFileSnapshot& target_file = target.open_files[index];
    if (current_file.handle_identity != target_file.handle_identity ||
        current_file.mount_identity != target_file.mount_identity ||
        current_file.file_identity != target_file.file_identity ||
        current_file.guest_path != target_file.guest_path ||
        current_file.file_access != target_file.file_access ||
        current_file.mode != target_file.mode ||
        current_file.is_directory != target_file.is_directory ||
        current_file.delete_on_close != target_file.delete_on_close ||
        current_file.has_pending_io != target_file.has_pending_io ||
        current_file.has_completion_port != target_file.has_completion_port) {
      return false;
    }
  }
  return true;
}

VfsGenerationSnapshot CloneOwnedSnapshot(
    const VfsGenerationSnapshot& source) {
  VfsGenerationSnapshot clone;
  clone.store_identity = source.store_identity;
  clone.mounts.reserve(source.mounts.size());
  for (const auto& source_mount : source.mounts) {
    auto mount = std::make_shared<VfsMountGeneration>();
    mount->mount_identity = source_mount->mount_identity;
    mount->generation = source_mount->generation;
    mount->guest_mount_path = source_mount->guest_mount_path;
    mount->backing_mode = source_mount->backing_mode;
    mount->writable = source_mount->writable;
    mount->files.reserve(source_mount->files.size());
    for (const VfsFileGeneration& source_file : source_mount->files) {
      mount->files.push_back(
          {source_file.file_identity, source_file.guest_path,
           std::make_shared<const std::vector<uint8_t>>(
               *source_file.content)});
    }
    clone.mounts.push_back(std::move(mount));
  }
  clone.open_files = source.open_files;
  return clone;
}

}  // namespace

VfsGenerationValidation ValidateVfsGenerationSnapshot(
    const VfsGenerationSnapshot& snapshot,
    const VfsGenerationLimits& limits) {
  if (!LimitsValid(limits) || snapshot.store_identity == 0) {
    return Invalid(VfsGenerationResult::kInvalidState,
                   "VFS generation limits or store identity are invalid.");
  }
  if (snapshot.mounts.empty() ||
      snapshot.mounts.size() > limits.maximum_mount_count ||
      snapshot.open_files.size() > limits.maximum_open_file_count) {
    return Invalid(VfsGenerationResult::kLimitExceeded,
                   "VFS mount or open-file count exceeds its limit.");
  }

  size_t file_count = 0;
  uint64_t total_content_size = 0;
  uint64_t previous_mount_identity = 0;
  for (const auto& mount_pointer : snapshot.mounts) {
    if (!mount_pointer) {
      return Invalid(VfsGenerationResult::kInvalidState,
                     "VFS snapshot contains a null mount generation.");
    }
    const VfsMountGeneration& mount = *mount_pointer;
    if (mount.mount_identity == 0 ||
        mount.mount_identity <= previous_mount_identity ||
        mount.generation == 0 ||
        !IsCanonicalMountPath(mount.guest_mount_path,
                              limits.maximum_path_length)) {
      return Invalid(VfsGenerationResult::kInvalidState,
                     "VFS mount identity, generation, order, or path is "
                     "invalid.");
    }
    previous_mount_identity = mount.mount_identity;
    if (!mount.writable ||
        mount.backing_mode != VfsMountBackingMode::kManagedImmutableCow) {
      return Invalid(VfsGenerationResult::kUnsupported,
                     "VFS snapshot contains an unmanaged writable mount.");
    }
    if (mount.files.size() > limits.maximum_file_count - file_count) {
      return Invalid(VfsGenerationResult::kLimitExceeded,
                     "VFS file count exceeds its limit.");
    }
    file_count += mount.files.size();

    uint64_t previous_file_identity = 0;
    for (size_t file_index = 0; file_index < mount.files.size();
         ++file_index) {
      const VfsFileGeneration& file = mount.files[file_index];
      if (file.file_identity == 0 ||
          file.file_identity <= previous_file_identity || !file.content ||
          !IsCanonicalFilePath(mount.guest_mount_path, file.guest_path,
                               limits.maximum_path_length)) {
        return Invalid(VfsGenerationResult::kInvalidState,
                       "VFS file identity, order, path, or content is "
                       "invalid.");
      }
      previous_file_identity = file.file_identity;
      if (file.content->size() > limits.maximum_file_content_bytes ||
          file.content->size() >
              limits.maximum_total_content_bytes - total_content_size) {
        return Invalid(VfsGenerationResult::kLimitExceeded,
                       "VFS file content exceeds its limit.");
      }
      total_content_size += file.content->size();
      for (size_t other_index = 0; other_index < file_index; ++other_index) {
        if (mount.files[other_index].guest_path == file.guest_path) {
          return Invalid(VfsGenerationResult::kInvalidState,
                         "VFS generation contains duplicate guest paths.");
        }
      }
    }
  }

  uint64_t previous_handle_identity = 0;
  for (const VfsOpenFileSnapshot& open_file : snapshot.open_files) {
    if (open_file.handle_identity == 0 ||
        open_file.handle_identity <= previous_handle_identity ||
        open_file.mount_identity == 0 || open_file.file_identity == 0 ||
        open_file.generation == 0) {
      return Invalid(VfsGenerationResult::kInvalidState,
                     "VFS open-file identity, generation, or order is "
                     "invalid.");
    }
    previous_handle_identity = open_file.handle_identity;
    if (open_file.mode != VfsOpenFileMode::kSynchronous ||
        open_file.is_directory || open_file.delete_on_close ||
        open_file.has_pending_io || open_file.has_completion_port) {
      return Invalid(
          VfsGenerationResult::kUnsupported,
          "VFS open file uses unsupported async, mapped, raw, directory, "
          "delete-on-close, pending-I/O, or completion-port state.");
    }
    const VfsMountGeneration* mount =
        FindMount(snapshot, open_file.mount_identity);
    if (!mount || mount->generation != open_file.generation) {
      return Invalid(VfsGenerationResult::kInvalidState,
                     "VFS open file references a missing mount generation.");
    }
    const VfsFileGeneration* file =
        FindFile(*mount, open_file.file_identity);
    if (!file || file->guest_path != open_file.guest_path) {
      return Invalid(VfsGenerationResult::kInvalidState,
                     "VFS open file identity and guest path do not match.");
    }
  }
  return {VfsGenerationResult::kOk, {}};
}

VfsGenerationResult DetachedVfsGenerationStore::Create(
    VfsGenerationSnapshot initial, VfsGenerationLimits limits,
    std::unique_ptr<DetachedVfsGenerationStore>* output,
    std::string* error_message) {
  if (!output || *output) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS store output is invalid.", error_message);
  }
  const VfsGenerationValidation validation =
      ValidateVfsGenerationSnapshot(initial, limits);
  if (!validation.ok()) {
    return Fail(validation.result, validation.message.c_str(), error_message);
  }
  VfsGenerationSnapshot owned_initial = CloneOwnedSnapshot(initial);
  output->reset(
      new DetachedVfsGenerationStore(std::move(owned_initial), limits));
  if (error_message) {
    error_message->clear();
  }
  return VfsGenerationResult::kOk;
}

VfsGenerationResult DetachedVfsGenerationStore::CaptureSnapshot(
    VfsGenerationSnapshot* output, std::string* error_message) const {
  if (!output) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS snapshot output is missing.", error_message);
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_transaction_id_ != 0) {
    return Fail(VfsGenerationResult::kBusy,
                "Detached VFS restore transaction is active.",
                error_message);
  }
  *output = current_;
  if (error_message) {
    error_message->clear();
  }
  return VfsGenerationResult::kOk;
}

VfsGenerationResult DetachedVfsGenerationStore::AdvanceWrite(
    uint64_t mount_identity, uint64_t file_identity, uint64_t byte_offset,
    const std::vector<uint8_t>& data, std::string* error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_transaction_id_ != 0) {
    return Fail(VfsGenerationResult::kBusy,
                "Detached VFS restore transaction is active.",
                error_message);
  }

  size_t mount_index = SIZE_MAX;
  size_t file_index = SIZE_MAX;
  for (size_t index = 0; index < current_.mounts.size(); ++index) {
    if (current_.mounts[index]->mount_identity == mount_identity) {
      mount_index = index;
      for (size_t candidate = 0;
           candidate < current_.mounts[index]->files.size(); ++candidate) {
        if (current_.mounts[index]->files[candidate].file_identity ==
            file_identity) {
          file_index = candidate;
          break;
        }
      }
      break;
    }
  }
  if (mount_index == SIZE_MAX || file_index == SIZE_MAX) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS write target identity is missing.",
                error_message);
  }
  if (data.empty()) {
    if (error_message) {
      error_message->clear();
    }
    return VfsGenerationResult::kOk;
  }

  const VfsMountGeneration& old_mount = *current_.mounts[mount_index];
  const VfsFileGeneration& old_file = old_mount.files[file_index];
  if (old_mount.generation == std::numeric_limits<uint64_t>::max() ||
      byte_offset > limits_.maximum_file_content_bytes ||
      data.size() > limits_.maximum_file_content_bytes - byte_offset) {
    return Fail(VfsGenerationResult::kLimitExceeded,
                "Detached VFS write exceeds generation or content limits.",
                error_message);
  }
  const uint64_t new_file_size =
      std::max<uint64_t>(old_file.content->size(), byte_offset + data.size());
  uint64_t current_total_size = 0;
  for (const auto& mount : current_.mounts) {
    for (const VfsFileGeneration& file : mount->files) {
      current_total_size += file.content->size();
    }
  }
  if (new_file_size >
      limits_.maximum_total_content_bytes -
          (current_total_size - old_file.content->size())) {
    return Fail(VfsGenerationResult::kLimitExceeded,
                "Detached VFS total content limit would be exceeded.",
                error_message);
  }

  auto new_content =
      std::make_shared<std::vector<uint8_t>>(*old_file.content);
  new_content->resize(static_cast<size_t>(new_file_size));
  if (!data.empty()) {
    std::memcpy(new_content->data() + static_cast<size_t>(byte_offset),
                data.data(), data.size());
  }
  auto new_mount = std::make_shared<VfsMountGeneration>(old_mount);
  ++new_mount->generation;
  new_mount->files[file_index].content = std::move(new_content);

  VfsGenerationSnapshot advanced = current_;
  advanced.mounts[mount_index] = std::move(new_mount);
  for (VfsOpenFileSnapshot& open_file : advanced.open_files) {
    if (open_file.mount_identity == mount_identity) {
      open_file.generation = advanced.mounts[mount_index]->generation;
    }
  }
  const VfsGenerationValidation validation =
      ValidateVfsGenerationSnapshot(advanced, limits_);
  if (!validation.ok()) {
    return Fail(validation.result, validation.message.c_str(), error_message);
  }
  current_ = std::move(advanced);
  if (error_message) {
    error_message->clear();
  }
  return VfsGenerationResult::kOk;
}

VfsGenerationResult DetachedVfsGenerationStore::SetOpenFilePosition(
    uint64_t handle_identity, uint64_t position,
    std::string* error_message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_transaction_id_ != 0) {
    return Fail(VfsGenerationResult::kBusy,
                "Detached VFS restore transaction is active.",
                error_message);
  }
  for (VfsOpenFileSnapshot& open_file : current_.open_files) {
    if (open_file.handle_identity == handle_identity) {
      open_file.position = position;
      if (error_message) {
        error_message->clear();
      }
      return VfsGenerationResult::kOk;
    }
  }
  return Fail(VfsGenerationResult::kInvalidState,
              "Detached VFS open-file identity is missing.", error_message);
}

VfsGenerationResult DetachedVfsGenerationStore::PrepareRestore(
    const VfsGenerationSnapshot& target,
    std::unique_ptr<DetachedVfsRestoreTransaction>* output,
    std::string* error_message) {
  if (!output || *output) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS restore output is invalid.", error_message);
  }
  const VfsGenerationValidation validation =
      ValidateVfsGenerationSnapshot(target, limits_);
  if (!validation.ok()) {
    return Fail(validation.result, validation.message.c_str(), error_message);
  }
  VfsGenerationSnapshot staged = CloneOwnedSnapshot(target);
  auto transaction = std::unique_ptr<DetachedVfsRestoreTransaction>(
      new DetachedVfsRestoreTransaction(this, std::move(staged)));

  std::lock_guard<std::mutex> lock(mutex_);
  if (active_transaction_id_ != 0) {
    return Fail(VfsGenerationResult::kBusy,
                "Detached VFS restore transaction is already active.",
                error_message);
  }
  if (!RestoreTopologyCompatible(current_, target) ||
      next_transaction_id_ == 0) {
    return Fail(VfsGenerationResult::kUnsupported,
                "Detached VFS restore would change live mount or open-file "
                "identity.",
                error_message);
  }
  transaction->transaction_id_ = next_transaction_id_++;
  active_transaction_id_ = transaction->transaction_id_;
  *output = std::move(transaction);
  if (error_message) {
    error_message->clear();
  }
  return VfsGenerationResult::kOk;
}

DetachedVfsRestoreTransaction::~DetachedVfsRestoreTransaction() {
  if (transaction_id_ != 0) {
    Rollback();
  }
}

VfsGenerationResult DetachedVfsRestoreTransaction::Commit(
    std::string* error_message) noexcept {
  if (!store_ || transaction_id_ == 0 || state_ != State::kStaged) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS restore is not commit-ready.", error_message);
  }
  std::lock_guard<std::mutex> lock(store_->mutex_);
  if (store_->active_transaction_id_ != transaction_id_) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS restore ownership is stale.", error_message);
  }
  static_assert(
      noexcept(std::declval<VfsGenerationSnapshot&>().mounts.swap(
          std::declval<VfsGenerationSnapshot&>().mounts)));
  store_->current_.mounts.swap(staged_.mounts);
  store_->current_.open_files.swap(staged_.open_files);
  state_ = State::kCommitted;
  if (error_message) {
    error_message->clear();
  }
  return VfsGenerationResult::kOk;
}

VfsGenerationResult
DetachedVfsRestoreTransaction::CommitWithInjectedFailureForTesting(
    std::string* error_message) noexcept {
  if (!store_ || transaction_id_ == 0 || state_ != State::kStaged) {
    return Fail(VfsGenerationResult::kInvalidState,
                "Detached VFS restore is not commit-ready.", error_message);
  }
  return Fail(VfsGenerationResult::kFailed,
              "Injected detached VFS failure before atomic root switch.",
              error_message);
}

VfsGenerationResult DetachedVfsRestoreTransaction::Rollback() noexcept {
  if (!store_ || transaction_id_ == 0 || state_ == State::kFinished) {
    return VfsGenerationResult::kInvalidState;
  }
  std::lock_guard<std::mutex> lock(store_->mutex_);
  if (store_->active_transaction_id_ != transaction_id_) {
    return VfsGenerationResult::kInvalidState;
  }
  if (state_ == State::kCommitted) {
    store_->current_.mounts.swap(staged_.mounts);
    store_->current_.open_files.swap(staged_.open_files);
  }
  store_->active_transaction_id_ = 0;
  transaction_id_ = 0;
  state_ = State::kFinished;
  return VfsGenerationResult::kOk;
}

VfsGenerationResult DetachedVfsRestoreTransaction::Finalize() noexcept {
  if (!store_ || transaction_id_ == 0 || state_ != State::kCommitted) {
    return VfsGenerationResult::kInvalidState;
  }
  std::lock_guard<std::mutex> lock(store_->mutex_);
  if (store_->active_transaction_id_ != transaction_id_) {
    return VfsGenerationResult::kInvalidState;
  }
  store_->active_transaction_id_ = 0;
  transaction_id_ = 0;
  state_ = State::kFinished;
  staged_.mounts.clear();
  staged_.open_files.clear();
  return VfsGenerationResult::kOk;
}

}  // namespace xe::save_state
