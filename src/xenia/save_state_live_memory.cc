/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_live_memory.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace xe::save_state {

StablePageMemoryTransaction::~StablePageMemoryTransaction() {
  if (state_ != State::kFinished) {
    Rollback();
  }
}

StateProviderResult StablePageMemoryTransaction::Prepare(
    const std::vector<StablePageCopyBinding>& bindings,
    const MemoryCaptureLimits& limits,
    std::unique_ptr<StablePageTransactionLock> lock,
    std::unique_ptr<StablePageMemoryTransaction>* output,
    std::string* error_message) {
  if (!output || *output || bindings.empty() ||
      limits.maximum_page_count == 0 ||
      limits.maximum_page_count > kMaximumMemoryPageCount ||
      limits.maximum_content_bytes == 0 ||
      limits.maximum_content_bytes >
          kMaximumStablePageTransactionContentSize ||
      bindings.size() > limits.maximum_page_count) {
    if (error_message) {
      *error_message = "Stable-page transaction arguments are invalid.";
    }
    return StateProviderResult::kInvalidState;
  }

  uint64_t total_size = 0;
  std::unordered_set<uint64_t> backing_identities;
  std::vector<std::pair<uintptr_t, uintptr_t>> live_ranges;
  backing_identities.reserve(bindings.size());
  live_ranges.reserve(bindings.size());
  for (const StablePageCopyBinding& binding : bindings) {
    const uintptr_t live_start =
        reinterpret_cast<uintptr_t>(binding.live_data);
    if (!binding.backing_identity || !binding.live_data ||
        !binding.target_data || !binding.size ||
        live_start >
            std::numeric_limits<uintptr_t>::max() - binding.size ||
        !backing_identities.insert(binding.backing_identity).second ||
        binding.size > limits.maximum_content_bytes - total_size) {
      if (error_message) {
        *error_message =
            "Stable-page bindings are invalid, duplicated, or out of bounds.";
      }
      return StateProviderResult::kInvalidState;
    }
    total_size += binding.size;
    live_ranges.emplace_back(live_start, live_start + binding.size);
  }
  std::sort(live_ranges.begin(), live_ranges.end());
  for (size_t index = 1; index < live_ranges.size(); ++index) {
    if (live_ranges[index - 1].second > live_ranges[index].first) {
      if (error_message) {
        *error_message = "Stable-page live backing ranges overlap.";
      }
      return StateProviderResult::kInvalidState;
    }
  }

  auto built = std::unique_ptr<StablePageMemoryTransaction>(
      new StablePageMemoryTransaction());
  built->pages_.reserve(bindings.size());
  built->target_bytes_.resize(size_t(total_size));
  built->undo_bytes_.resize(size_t(total_size));
  built->lock_ = std::move(lock);

  size_t offset = 0;
  for (const StablePageCopyBinding& binding : bindings) {
    built->pages_.push_back({binding.live_data, offset, binding.size});
    std::memcpy(built->target_bytes_.data() + offset, binding.target_data,
                binding.size);
    std::memcpy(built->undo_bytes_.data() + offset, binding.live_data,
                binding.size);
    offset += binding.size;
  }

  *output = std::move(built);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

StateProviderResult StablePageMemoryTransaction::Commit(
    std::string* error_message) noexcept {
  (void)error_message;
  return CommitInternal(SIZE_MAX);
}

StateProviderResult
StablePageMemoryTransaction::CommitWithFailureBeforePageForTesting(
    size_t page_index, std::string* error_message) noexcept {
  (void)error_message;
  return CommitInternal(page_index);
}

StateProviderResult StablePageMemoryTransaction::CommitInternal(
    size_t fail_before_page) noexcept {
  if (state_ != State::kStaged) {
    return StateProviderResult::kInvalidState;
  }
  for (size_t index = 0; index < pages_.size(); ++index) {
    if (index == fail_before_page) {
      state_ = State::kCommitFailed;
      return StateProviderResult::kFailed;
    }
    const Page& page = pages_[index];
    std::memcpy(page.live_data, target_bytes_.data() + page.offset, page.size);
  }
  state_ = State::kCommitted;
  return StateProviderResult::kOk;
}

StateProviderResult StablePageMemoryTransaction::Rollback() noexcept {
  if (state_ == State::kFinished) {
    return StateProviderResult::kOk;
  }
  for (const Page& page : pages_) {
    std::memcpy(page.live_data, undo_bytes_.data() + page.offset, page.size);
  }
  ReleaseResources();
  return StateProviderResult::kOk;
}

void StablePageMemoryTransaction::Finalize() noexcept {
  if (state_ != State::kCommitted) {
    Rollback();
    return;
  }
  ReleaseResources();
}

void StablePageMemoryTransaction::ReleaseResources() noexcept {
  pages_.clear();
  target_bytes_.clear();
  undo_bytes_.clear();
  lock_.reset();
  state_ = State::kFinished;
}

}  // namespace xe::save_state
