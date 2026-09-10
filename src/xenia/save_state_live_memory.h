/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_LIVE_MEMORY_H_
#define XENIA_SAVE_STATE_LIVE_MEMORY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/save_state_phase2.h"

namespace xe::save_state {

// Target and undo buffers are both this size at most, bounding detached
// staging to 1 GiB plus page descriptors.
constexpr uint64_t kMaximumStablePageTransactionContentSize =
    UINT64_C(1) << 29;

struct StablePageCopyBinding {
  uint64_t backing_identity = 0;
  uint8_t* live_data = nullptr;
  const uint8_t* target_data = nullptr;
  uint32_t size = 0;
};

// Optional ownership token used by xe::Memory to keep its private allocation
// lock held for the transaction lifetime. Detached tests do not supply one.
class StablePageTransactionLock {
 public:
  virtual ~StablePageTransactionLock() = default;
};

// Bounded content-only transaction for stable, unique backing pages. Prepare
// allocates and fills complete target and undo buffers. Commit and Rollback
// perform fixed-size copies only and never allocate.
//
// Production bindings and the owned global lock may only be supplied by
// xe::Memory after exact allocation/protection/alias topology validation.
class StablePageMemoryTransaction {
 public:
  ~StablePageMemoryTransaction();
  StablePageMemoryTransaction(const StablePageMemoryTransaction&) = delete;
  StablePageMemoryTransaction& operator=(const StablePageMemoryTransaction&) =
      delete;

  static StateProviderResult Prepare(
      const std::vector<StablePageCopyBinding>& bindings,
      const MemoryCaptureLimits& limits,
      std::unique_ptr<StablePageTransactionLock> lock,
      std::unique_ptr<StablePageMemoryTransaction>* output,
      std::string* error_message);

  StateProviderResult Commit(std::string* error_message) noexcept;
  StateProviderResult CommitWithFailureBeforePageForTesting(
      size_t page_index, std::string* error_message) noexcept;
  StateProviderResult Rollback() noexcept;
  void Finalize() noexcept;

  size_t page_count() const { return pages_.size(); }
  size_t content_size() const { return target_bytes_.size(); }

 private:
  enum class State {
    kStaged,
    kCommitFailed,
    kCommitted,
    kFinished,
  };

  struct Page {
    uint8_t* live_data = nullptr;
    size_t offset = 0;
    uint32_t size = 0;
  };

  StablePageMemoryTransaction() = default;
  StateProviderResult CommitInternal(size_t fail_before_page) noexcept;
  void ReleaseResources() noexcept;

  std::vector<Page> pages_;
  std::vector<uint8_t> target_bytes_;
  std::vector<uint8_t> undo_bytes_;
  std::unique_ptr<StablePageTransactionLock> lock_;
  State state_ = State::kStaged;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_LIVE_MEMORY_H_
