/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_RESUME_H_
#define XENIA_CPU_BACKEND_A64_A64_RESUME_H_

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/save_state_phase2.h"

namespace xe::cpu::backend::a64 {
class A64CodeCache;
}  // namespace xe::cpu::backend::a64

namespace xe::save_state {

struct A64ResumeSegment {
  uint32_t guest_pc = 0;
  uint32_t guest_stack_pointer = 0;
  uint32_t guest_return_address = 0;
  uint32_t captured_host_stack_size = 0;
};

struct A64ResolvedResumeSegment {
  A64ResumeSegment guest;
  uintptr_t host_entry = 0;
  uint32_t host_stack_size = 0;
};

struct A64ResumeDispatchPlan {
  std::vector<A64ResolvedResumeSegment> segments;
};

// Read-only lookup of already resolved generated functions. Implementations
// must not compile code or mutate the code cache while a restore is
// preflighting. The returned stack size is current generated-code metadata and
// must match the captured frame before dispatch can proceed.
class A64ResumeTargetLookup {
 public:
  virtual ~A64ResumeTargetLookup() = default;

  virtual StateProviderResult Lookup(uint32_t guest_pc, uintptr_t* host_entry,
                                     uint32_t* host_stack_size,
                                     std::string* error_message) = 0;
};

class A64ExistingCodeLookup final : public A64ResumeTargetLookup {
 public:
  explicit A64ExistingCodeLookup(cpu::backend::a64::A64CodeCache& code_cache)
      : code_cache_(code_cache) {}

  StateProviderResult Lookup(uint32_t guest_pc, uintptr_t* host_entry,
                             uint32_t* host_stack_size,
                             std::string* error_message) override;

 private:
  cpu::backend::a64::A64CodeCache& code_cache_;
};

// Converts a captured PC and deepest-to-outermost JIT stackpoint chain into a
// candidate guest control-flow sequence without preserving opaque host frames.
// This does not rebuild or enter backend stackpoints.
StateProviderResult BuildA64ResumeSegments(
    const CpuThreadSnapshot& thread, std::vector<A64ResumeSegment>* segments,
    std::string* error_message);
StateProviderResult BuildA64ResumeDispatchPlan(
    const CpuThreadSnapshot& thread, A64ResumeTargetLookup& lookup,
    A64ResumeDispatchPlan* plan, std::string* error_message);

}  // namespace xe::save_state

#endif  // XENIA_CPU_BACKEND_A64_A64_RESUME_H_
