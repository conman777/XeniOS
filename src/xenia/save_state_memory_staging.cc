/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state_memory_staging.h"

#include <utility>

namespace xe::save_state {
namespace {

bool InventoryPageEqual(const MemoryInventoryPage& left,
                        const MemoryInventoryPage& right) {
  return left.address_space == right.address_space &&
         left.address == right.address && left.page_size == right.page_size &&
         left.state == right.state &&
         left.allocation_base == right.allocation_base &&
         left.allocation_page_count == right.allocation_page_count &&
         left.allocation_protect == right.allocation_protect &&
         left.current_protect == right.current_protect &&
         left.backing_physical_address == right.backing_physical_address;
}

bool PlansEqual(const MemoryRestorePlan& left,
                const MemoryRestorePlan& right) {
  if (left.transitions.size() != right.transitions.size()) {
    return false;
  }
  for (size_t index = 0; index < left.transitions.size(); ++index) {
    const MemoryRestoreTransition& left_transition = left.transitions[index];
    const MemoryRestoreTransition& right_transition = right.transitions[index];
    if (left_transition.current_exists != right_transition.current_exists ||
        left_transition.target_exists != right_transition.target_exists ||
        (left_transition.current_exists &&
         !InventoryPageEqual(left_transition.current,
                             right_transition.current)) ||
        (left_transition.target_exists &&
         !InventoryPageEqual(left_transition.target,
                             right_transition.target))) {
      return false;
    }
  }
  return true;
}

MemoryInventoryPage InventoryPage(const MemoryPageSnapshot& page) {
  return {
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

}  // namespace

DetachedMemoryRestoreBackend::DetachedMemoryRestoreBackend(
    MemorySnapshot& image, MemoryCaptureLimits limits)
    : image_(image), limits_(limits) {}

StateProviderResult DetachedMemoryRestoreBackend::CaptureInventory(
    MemoryAllocationInventory* inventory, std::string* error_message) {
  if (!inventory) {
    if (error_message) {
      *error_message = "Detached memory inventory output is missing.";
    }
    return StateProviderResult::kInvalidState;
  }
  const SnapshotCodecValidation validation = ValidateMemorySnapshot(image_);
  if (!validation.ok()) {
    if (error_message) {
      *error_message = validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  MemoryAllocationInventory captured;
  captured.pages.reserve(image_.pages.size());
  for (const MemoryPageSnapshot& page : image_.pages) {
    captured.pages.push_back(InventoryPage(page));
  }
  *inventory = std::move(captured);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

StateProviderResult DetachedMemoryRestoreBackend::Stage(
    const MemoryRestorePlan& plan, const MemorySnapshot& target,
    std::string* error_message) {
  if (staged_ready_ || committed_ || limits_.maximum_page_count == 0 ||
      limits_.maximum_page_count > kMaximumMemoryPageCount ||
      limits_.maximum_content_bytes > kMaximumMemorySnapshotSize ||
      target.pages.size() > limits_.maximum_page_count) {
    if (error_message) {
      *error_message = "Detached memory staging state or limits are invalid.";
    }
    return StateProviderResult::kInvalidState;
  }

  uint64_t content_bytes = 0;
  for (const MemoryPageSnapshot& page : target.pages) {
    if (page.data.size() >
        limits_.maximum_content_bytes - content_bytes) {
      if (error_message) {
        *error_message = "Detached memory staging content limit was exceeded.";
      }
      return StateProviderResult::kInvalidState;
    }
    content_bytes += page.data.size();
  }

  MemoryAllocationInventory current;
  StateProviderResult result = CaptureInventory(&current, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }
  MemoryRestorePlan expected_plan;
  const SnapshotCodecValidation plan_validation =
      BuildMemoryRestorePlan(current, target, &expected_plan);
  if (!plan_validation.ok() || !PlansEqual(plan, expected_plan)) {
    if (error_message) {
      *error_message = plan_validation.ok()
                           ? "Detached memory restore plan does not match."
                           : plan_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }

  std::vector<uint8_t> encoded;
  const SnapshotCodecValidation encode_validation =
      EncodeMemorySnapshot(target, &encoded);
  if (!encode_validation.ok()) {
    if (error_message) {
      *error_message = encode_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  MemorySnapshot canonical_target;
  const SnapshotCodecValidation decode_validation = DecodeMemorySnapshot(
      encoded.data(), encoded.size(), &canonical_target);
  if (!decode_validation.ok()) {
    if (error_message) {
      *error_message = decode_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  staged_ = std::move(canonical_target);
  staged_ready_ = true;
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

StateProviderResult DetachedMemoryRestoreBackend::Commit(
    std::string* error_message) {
  if (!staged_ready_ || committed_) {
    if (error_message) {
      *error_message = "Detached memory transaction is not commit-ready.";
    }
    return StateProviderResult::kInvalidState;
  }
  static_assert(
      noexcept(std::declval<std::vector<MemoryPageSnapshot>&>().swap(
          std::declval<std::vector<MemoryPageSnapshot>&>())));
  image_.pages.swap(staged_.pages);
  committed_ = true;
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

StateProviderResult DetachedMemoryRestoreBackend::Rollback() noexcept {
  if (committed_) {
    image_.pages.swap(staged_.pages);
  }
  staged_.pages.clear();
  staged_ready_ = false;
  committed_ = false;
  return StateProviderResult::kOk;
}

void DetachedMemoryRestoreBackend::Finalize() noexcept {
  staged_.pages.clear();
  staged_ready_ = false;
  committed_ = false;
}

}  // namespace xe::save_state
