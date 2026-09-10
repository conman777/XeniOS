/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_CACHE_H_
#define XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_CACHE_H_

#include "xenia/cpu/backend/a64/a64_restore_preflight.h"

namespace xe::cpu::backend::a64 {
class A64CodeCache;
}  // namespace xe::cpu::backend::a64

namespace xe::save_state {

// Production generation-freeze adapter for A64CodeCache. This supplies
// preflight lookup only; it does not arm or execute restored CPU state.
class A64CodeCacheGenerationControl final
    : public A64CodeGenerationControl {
 public:
  explicit A64CodeCacheGenerationControl(
      cpu::backend::a64::A64CodeCache& code_cache)
      : code_cache_(code_cache) {}

  StateProviderResult AcquireFreeze(
      std::chrono::milliseconds timeout,
      std::unique_ptr<A64FrozenCodeGeneration>* output,
      std::string* error_message) override;

 private:
  cpu::backend::a64::A64CodeCache& code_cache_;
};

}  // namespace xe::save_state

#endif  // XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_CACHE_H_
