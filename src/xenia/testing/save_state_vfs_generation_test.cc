/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_vfs_generation.h"

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace xe::save_state::test {
namespace {

std::shared_ptr<const std::vector<uint8_t>> Bytes(
    std::initializer_list<uint8_t> values) {
  return std::make_shared<const std::vector<uint8_t>>(values);
}

VfsGenerationLimits TestLimits() {
  VfsGenerationLimits limits;
  limits.maximum_mount_count = 2;
  limits.maximum_file_count = 4;
  limits.maximum_open_file_count = 2;
  limits.maximum_path_length = 64;
  limits.maximum_file_content_bytes = 64;
  limits.maximum_total_content_bytes = 128;
  return limits;
}

VfsGenerationSnapshot InitialSnapshot() {
  auto cache_mount = std::make_shared<VfsMountGeneration>();
  cache_mount->mount_identity = 10;
  cache_mount->generation = 1;
  cache_mount->guest_mount_path = "cache:";
  cache_mount->backing_mode = VfsMountBackingMode::kManagedImmutableCow;
  cache_mount->writable = true;
  cache_mount->files = {
      {101, "cache:\\cache1.bin", Bytes({1, 2, 3, 4})},
      {102, "cache:\\profile.bin", Bytes({5, 6, 7})},
  };

  auto content_mount = std::make_shared<VfsMountGeneration>();
  content_mount->mount_identity = 20;
  content_mount->generation = 7;
  content_mount->guest_mount_path = "content:";
  content_mount->backing_mode = VfsMountBackingMode::kManagedImmutableCow;
  content_mount->writable = true;
  content_mount->files = {
      {201, "content:\\settings.bin", Bytes({8, 9})},
  };

  VfsGenerationSnapshot snapshot;
  snapshot.store_identity = 0x1234;
  snapshot.mounts = {std::move(cache_mount), std::move(content_mount)};
  snapshot.open_files = {
      {1001, 10, 101, 1, "cache:\\cache1.bin", 2, 1,
       VfsOpenFileMode::kSynchronous, false, false, false, false},
      {1002, 20, 201, 7, "content:\\settings.bin", 1, 1,
       VfsOpenFileMode::kSynchronous, false, false, false, false},
  };
  return snapshot;
}

std::unique_ptr<DetachedVfsGenerationStore> CreateStore() {
  std::unique_ptr<DetachedVfsGenerationStore> store;
  std::string error;
  REQUIRE(DetachedVfsGenerationStore::Create(
              InitialSnapshot(), TestLimits(), &store, &error) ==
          VfsGenerationResult::kOk);
  REQUIRE(store);
  return store;
}

VfsGenerationSnapshot Capture(DetachedVfsGenerationStore& store) {
  VfsGenerationSnapshot snapshot;
  std::string error;
  REQUIRE(store.CaptureSnapshot(&snapshot, &error) ==
          VfsGenerationResult::kOk);
  return snapshot;
}

const VfsFileGeneration& File(const VfsGenerationSnapshot& snapshot,
                              size_t mount_index, size_t file_index) {
  return snapshot.mounts[mount_index]->files[file_index];
}

bool SnapshotsEqual(const VfsGenerationSnapshot& left,
                    const VfsGenerationSnapshot& right) {
  if (left.store_identity != right.store_identity ||
      left.mounts.size() != right.mounts.size() ||
      left.open_files.size() != right.open_files.size()) {
    return false;
  }
  for (size_t mount_index = 0; mount_index < left.mounts.size();
       ++mount_index) {
    const VfsMountGeneration& left_mount = *left.mounts[mount_index];
    const VfsMountGeneration& right_mount = *right.mounts[mount_index];
    if (left_mount.mount_identity != right_mount.mount_identity ||
        left_mount.generation != right_mount.generation ||
        left_mount.guest_mount_path != right_mount.guest_mount_path ||
        left_mount.backing_mode != right_mount.backing_mode ||
        left_mount.writable != right_mount.writable ||
        left_mount.files.size() != right_mount.files.size()) {
      return false;
    }
    for (size_t file_index = 0; file_index < left_mount.files.size();
         ++file_index) {
      const VfsFileGeneration& left_file = left_mount.files[file_index];
      const VfsFileGeneration& right_file = right_mount.files[file_index];
      if (left_file.file_identity != right_file.file_identity ||
          left_file.guest_path != right_file.guest_path ||
          *left_file.content != *right_file.content) {
        return false;
      }
    }
  }
  for (size_t index = 0; index < left.open_files.size(); ++index) {
    const VfsOpenFileSnapshot& left_file = left.open_files[index];
    const VfsOpenFileSnapshot& right_file = right.open_files[index];
    if (left_file.handle_identity != right_file.handle_identity ||
        left_file.mount_identity != right_file.mount_identity ||
        left_file.file_identity != right_file.file_identity ||
        left_file.generation != right_file.generation ||
        left_file.guest_path != right_file.guest_path ||
        left_file.position != right_file.position ||
        left_file.file_access != right_file.file_access ||
        left_file.mode != right_file.mode ||
        left_file.is_directory != right_file.is_directory ||
        left_file.delete_on_close != right_file.delete_on_close ||
        left_file.has_pending_io != right_file.has_pending_io ||
        left_file.has_completion_port != right_file.has_completion_port) {
      return false;
    }
  }
  return true;
}

VfsGenerationSnapshot AdvanceStore(DetachedVfsGenerationStore& store) {
  REQUIRE(store.AdvanceWrite(10, 101, 1, {0xA0, 0xA1}, nullptr) ==
          VfsGenerationResult::kOk);
  REQUIRE(store.SetOpenFilePosition(1001, 4, nullptr) ==
          VfsGenerationResult::kOk);
  return Capture(store);
}

}  // namespace

TEST_CASE("Detached VFS generations preserve immutable saved roots",
          "[save_state][vfs][generation]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot saved = Capture(*store);
  const auto saved_untouched_file = File(saved, 0, 1).content;
  const auto saved_other_mount = saved.mounts[1];

  const VfsGenerationSnapshot advanced = AdvanceStore(*store);
  CHECK(advanced.mounts[0]->generation == 2);
  CHECK(*File(advanced, 0, 0).content ==
        std::vector<uint8_t>({1, 0xA0, 0xA1, 4}));
  CHECK(*File(saved, 0, 0).content == std::vector<uint8_t>({1, 2, 3, 4}));
  CHECK(File(advanced, 0, 1).content == saved_untouched_file);
  CHECK(advanced.mounts[1] == saved_other_mount);
  CHECK(advanced.open_files[0].generation == 2);
  CHECK(advanced.open_files[0].position == 4);
}

TEST_CASE("Detached VFS store severs caller-owned mutable aliases",
          "[save_state][vfs][generation]") {
  VfsGenerationSnapshot initial = InitialSnapshot();
  auto retained_mount =
      std::const_pointer_cast<VfsMountGeneration>(initial.mounts[0]);
  auto retained_content =
      std::make_shared<std::vector<uint8_t>>(
          std::initializer_list<uint8_t>{1, 2, 3, 4});
  retained_mount->files[0].content = retained_content;
  std::unique_ptr<DetachedVfsGenerationStore> store;
  REQUIRE(DetachedVfsGenerationStore::Create(
              initial, TestLimits(), &store, nullptr) ==
          VfsGenerationResult::kOk);

  retained_mount->generation = 99;
  (*retained_content)[0] = 0xEE;
  const VfsGenerationSnapshot captured = Capture(*store);
  CHECK(captured.mounts[0]->generation == 1);
  CHECK(*File(captured, 0, 0).content == std::vector<uint8_t>({1, 2, 3, 4}));
}

TEST_CASE("Detached VFS restore can commit saved generation and position",
          "[save_state][vfs][generation][transaction]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot saved = Capture(*store);
  AdvanceStore(*store);

  std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
  REQUIRE(store->PrepareRestore(saved, &transaction, nullptr) ==
          VfsGenerationResult::kOk);
  REQUIRE(transaction->Commit(nullptr) == VfsGenerationResult::kOk);
  REQUIRE(transaction->Finalize() == VfsGenerationResult::kOk);

  const VfsGenerationSnapshot restored = Capture(*store);
  CHECK(SnapshotsEqual(restored, saved));
  CHECK(restored.open_files[0].position == 2);
}

TEST_CASE("Detached VFS committed restore rolls back exactly",
          "[save_state][vfs][generation][transaction]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot saved = Capture(*store);
  const VfsGenerationSnapshot advanced = AdvanceStore(*store);

  std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
  REQUIRE(store->PrepareRestore(saved, &transaction, nullptr) ==
          VfsGenerationResult::kOk);
  REQUIRE(transaction->Commit(nullptr) == VfsGenerationResult::kOk);
  REQUIRE(transaction->Rollback() == VfsGenerationResult::kOk);

  CHECK(SnapshotsEqual(Capture(*store), advanced));
}

TEST_CASE("Detached VFS failure and destruction leave advanced root intact",
          "[save_state][vfs][generation][transaction]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot saved = Capture(*store);
  const VfsGenerationSnapshot advanced = AdvanceStore(*store);

  {
    std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
    REQUIRE(store->PrepareRestore(saved, &transaction, nullptr) ==
            VfsGenerationResult::kOk);
    CHECK(transaction->CommitWithInjectedFailureForTesting(nullptr) ==
          VfsGenerationResult::kFailed);
  }
  CHECK(SnapshotsEqual(Capture(*store), advanced));

  {
    std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
    REQUIRE(store->PrepareRestore(saved, &transaction, nullptr) ==
            VfsGenerationResult::kOk);
    REQUIRE(transaction->Commit(nullptr) == VfsGenerationResult::kOk);
  }
  CHECK(SnapshotsEqual(Capture(*store), advanced));
}

TEST_CASE("Detached VFS transaction blocks competing mutations",
          "[save_state][vfs][generation][transaction]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot saved = Capture(*store);
  AdvanceStore(*store);

  std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
  REQUIRE(store->PrepareRestore(saved, &transaction, nullptr) ==
          VfsGenerationResult::kOk);
  VfsGenerationSnapshot output;
  CHECK(store->CaptureSnapshot(&output, nullptr) ==
        VfsGenerationResult::kBusy);
  CHECK(store->AdvanceWrite(10, 101, 0, {1}, nullptr) ==
        VfsGenerationResult::kBusy);
  CHECK(store->SetOpenFilePosition(1001, 0, nullptr) ==
        VfsGenerationResult::kBusy);

  std::unique_ptr<DetachedVfsRestoreTransaction> competing;
  CHECK(store->PrepareRestore(saved, &competing, nullptr) ==
        VfsGenerationResult::kBusy);
  CHECK_FALSE(competing);
  REQUIRE(transaction->Rollback() == VfsGenerationResult::kOk);
}

TEST_CASE("VFS generations reject unmanaged host and open-file paths",
          "[save_state][vfs][generation][unsupported]") {
  for (VfsMountBackingMode mode :
       {VfsMountBackingMode::kDirectHostPath,
        VfsMountBackingMode::kMappedHostPath,
        VfsMountBackingMode::kAsyncHostPath,
        VfsMountBackingMode::kRawHostHandle}) {
    VfsGenerationSnapshot snapshot = InitialSnapshot();
    auto mount = std::make_shared<VfsMountGeneration>(*snapshot.mounts[0]);
    mount->backing_mode = mode;
    snapshot.mounts[0] = std::move(mount);
    CHECK(ValidateVfsGenerationSnapshot(snapshot, TestLimits()).result ==
          VfsGenerationResult::kUnsupported);
  }

  for (VfsOpenFileMode mode :
       {VfsOpenFileMode::kAsynchronous, VfsOpenFileMode::kMapped,
        VfsOpenFileMode::kRawHostHandle}) {
    VfsGenerationSnapshot snapshot = InitialSnapshot();
    snapshot.open_files[0].mode = mode;
    CHECK(ValidateVfsGenerationSnapshot(snapshot, TestLimits()).result ==
          VfsGenerationResult::kUnsupported);
  }

  VfsGenerationSnapshot directory = InitialSnapshot();
  directory.open_files[0].is_directory = true;
  CHECK(ValidateVfsGenerationSnapshot(directory, TestLimits()).result ==
        VfsGenerationResult::kUnsupported);
  VfsGenerationSnapshot pending = InitialSnapshot();
  pending.open_files[0].has_pending_io = true;
  CHECK(ValidateVfsGenerationSnapshot(pending, TestLimits()).result ==
        VfsGenerationResult::kUnsupported);
  VfsGenerationSnapshot completion = InitialSnapshot();
  completion.open_files[0].has_completion_port = true;
  CHECK(ValidateVfsGenerationSnapshot(completion, TestLimits()).result ==
        VfsGenerationResult::kUnsupported);
  VfsGenerationSnapshot deleting = InitialSnapshot();
  deleting.open_files[0].delete_on_close = true;
  CHECK(ValidateVfsGenerationSnapshot(deleting, TestLimits()).result ==
        VfsGenerationResult::kUnsupported);
}

TEST_CASE("Detached VFS restore rejects changed live handle identity",
          "[save_state][vfs][generation][unsupported]") {
  auto store = CreateStore();
  VfsGenerationSnapshot target = Capture(*store);
  target.open_files[0].handle_identity = 999;
  std::unique_ptr<DetachedVfsRestoreTransaction> transaction;
  CHECK(store->PrepareRestore(target, &transaction, nullptr) ==
        VfsGenerationResult::kUnsupported);
  CHECK_FALSE(transaction);

  target = Capture(*store);
  target.open_files.erase(target.open_files.begin());
  CHECK(store->PrepareRestore(target, &transaction, nullptr) ==
        VfsGenerationResult::kUnsupported);
  CHECK_FALSE(transaction);
}

TEST_CASE("Detached VFS writes enforce bounded content and canonical paths",
          "[save_state][vfs][generation][limits]") {
  auto store = CreateStore();
  const VfsGenerationSnapshot original = Capture(*store);
  CHECK(store->AdvanceWrite(10, 101, 0, {}, nullptr) ==
        VfsGenerationResult::kOk);
  CHECK(SnapshotsEqual(Capture(*store), original));
  CHECK(store->AdvanceWrite(10, 101, 64, {1}, nullptr) ==
        VfsGenerationResult::kLimitExceeded);
  CHECK(SnapshotsEqual(Capture(*store), original));

  VfsGenerationSnapshot invalid_path = InitialSnapshot();
  auto mount =
      std::make_shared<VfsMountGeneration>(*invalid_path.mounts[0]);
  mount->files[0].guest_path = "cache:\\..\\cache1.bin";
  invalid_path.mounts[0] = std::move(mount);
  CHECK(ValidateVfsGenerationSnapshot(invalid_path, TestLimits()).result ==
        VfsGenerationResult::kInvalidState);

  VfsGenerationSnapshot duplicate_identity = InitialSnapshot();
  auto duplicate_mount =
      std::make_shared<VfsMountGeneration>(*duplicate_identity.mounts[0]);
  duplicate_mount->files[1].file_identity =
      duplicate_mount->files[0].file_identity;
  duplicate_identity.mounts[0] = std::move(duplicate_mount);
  CHECK(ValidateVfsGenerationSnapshot(duplicate_identity, TestLimits()).result ==
        VfsGenerationResult::kInvalidState);
}

}  // namespace xe::save_state::test
