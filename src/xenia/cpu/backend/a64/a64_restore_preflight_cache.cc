/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_restore_preflight_cache.h"

#include "xenia/cpu/backend/a64/a64_code_cache.h"

namespace xe::save_state {
namespace {

class FrozenA64CodeCacheGeneration final
    : public A64FrozenCodeGeneration {
 public:
  FrozenA64CodeCacheGeneration(
      cpu::backend::a64::A64CodeCache& code_cache, uint64_t generation)
      : code_cache_(code_cache), generation_(generation) {}
  ~FrozenA64CodeCacheGeneration() override {
    code_cache_.ReleaseCodeGenerationFreeze(generation_);
  }

  uint64_t generation() const override { return generation_; }

  StateProviderResult Validate(std::string* error_message) const override {
    if (!code_cache_.ValidateCodeGenerationFreeze(generation_)) {
      if (error_message) {
        *error_message = "A64 code-cache generation freeze is stale.";
      }
      return StateProviderResult::kInvalidState;
    }
    if (error_message) {
      error_message->clear();
    }
    return StateProviderResult::kOk;
  }

  StateProviderResult Lookup(uint32_t guest_pc, uintptr_t* host_entry,
                             uint32_t* host_stack_size,
                             std::string* error_message) override {
    StateProviderResult validation = Validate(error_message);
    if (validation != StateProviderResult::kOk) {
      return validation;
    }
    if (!code_cache_.LookupExistingGuestCodeFrozen(
            generation_, guest_pc, host_entry, host_stack_size)) {
      validation = Validate(error_message);
      if (validation != StateProviderResult::kOk) {
        return validation;
      }
      if (error_message) {
        *error_message =
            "A64 generated entry is absent, ambiguous, or unpublished.";
      }
      return StateProviderResult::kUnsupported;
    }
    if (error_message) {
      error_message->clear();
    }
    return StateProviderResult::kOk;
  }

 private:
  cpu::backend::a64::A64CodeCache& code_cache_;
  uint64_t generation_;
};

}  // namespace

StateProviderResult A64CodeCacheGenerationControl::AcquireFreeze(
    std::chrono::milliseconds timeout,
    std::unique_ptr<A64FrozenCodeGeneration>* output,
    std::string* error_message) {
  if (!output || *output || timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumGuestBarrierWait) {
    if (error_message) {
      *error_message =
          "A64 code-cache generation freeze arguments are invalid.";
    }
    return StateProviderResult::kInvalidState;
  }

  uint64_t generation = 0;
  if (!code_cache_.AcquireCodeGenerationFreeze(timeout, &generation) ||
      generation == 0) {
    if (error_message) {
      *error_message =
          "A64 code-cache publication did not quiesce before timeout.";
    }
    return StateProviderResult::kFailed;
  }

  *output = std::make_unique<FrozenA64CodeCacheGeneration>(code_cache_,
                                                           generation);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

}  // namespace xe::save_state
