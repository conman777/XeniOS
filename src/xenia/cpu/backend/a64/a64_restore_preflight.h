/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_H_
#define XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_H_

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xenia/cpu/backend/a64/a64_resume.h"
#include "xenia/save_state_runtime.h"

namespace xe::save_state {

// An exclusive, bounded lease over the set of published A64 generated
// functions. While this object exists, an implementation must not publish,
// replace, or remove generated functions or their source/stack metadata.
//
// Lookup is deliberately part of the lease so a restore preflight cannot
// accidentally resolve targets outside the frozen generation.
class A64FrozenCodeGeneration : public A64ResumeTargetLookup {
 public:
  ~A64FrozenCodeGeneration() override = default;

  virtual uint64_t generation() const = 0;
  virtual StateProviderResult Validate(
      std::string* error_message) const = 0;
};

class A64CodeGenerationControl {
 public:
  virtual ~A64CodeGenerationControl() = default;

  // On failure, output must remain empty and live code-cache state unchanged.
  virtual StateProviderResult AcquireFreeze(
      std::chrono::milliseconds timeout,
      std::unique_ptr<A64FrozenCodeGeneration>* output,
      std::string* error_message) = 0;
};

struct A64PreparedThreadReentry {
  uint32_t thread_id = 0;
  const void* live_context_identity = nullptr;
  A64ResumeDispatchPlan dispatch;
};

// Detached trampoline/dispatcher preparation. Destruction is the only
// operation currently exposed: there is intentionally no arm, execute, or
// live-context commit entry point.
class A64DetachedReentryPreparation {
 public:
  virtual ~A64DetachedReentryPreparation() = default;
  virtual uint32_t thread_id() const = 0;
};

class A64ReentryDispatcher {
 public:
  virtual ~A64ReentryDispatcher() = default;

  // May allocate and validate detached trampoline state only. The context is
  // const by contract and must not be modified. On failure, output must remain
  // empty.
  virtual StateProviderResult PrepareDetached(
      const A64PreparedThreadReentry& thread,
      std::unique_ptr<A64DetachedReentryPreparation>* output,
      std::string* error_message) = 0;
};

// Single owner of a multi-thread restore preflight. It keeps enrollment and
// all guest contexts frozen, then the code generation frozen, then detached
// per-thread preparations alive. Reset releases them in the reverse order.
class A64RestorePreflight {
 public:
  A64RestorePreflight() = default;
  ~A64RestorePreflight();
  A64RestorePreflight(const A64RestorePreflight&) = delete;
  A64RestorePreflight& operator=(const A64RestorePreflight&) = delete;
  A64RestorePreflight(A64RestorePreflight&& other) noexcept;
  A64RestorePreflight& operator=(A64RestorePreflight&& other) noexcept;

  explicit operator bool() const {
    return bool(boundary_) && code_generation_ != nullptr;
  }
  uint64_t boundary_generation() const { return boundary_.generation(); }
  uint64_t code_generation() const {
    return code_generation_ ? code_generation_->generation() : 0;
  }
  const std::vector<A64PreparedThreadReentry>& threads() const {
    return threads_;
  }

  void Reset();

 private:
  friend StateProviderResult PrepareA64RestorePreflight(
      HeldGuestBoundary boundary, const CpuSnapshot& snapshot,
      A64CodeGenerationControl& code_generation_control,
      A64ReentryDispatcher& dispatcher, std::chrono::milliseconds timeout,
      A64RestorePreflight* output, std::string* error_message);

  HeldGuestBoundary boundary_;
  std::unique_ptr<A64FrozenCodeGeneration> code_generation_;
  std::vector<A64PreparedThreadReentry> threads_;
  std::vector<std::unique_ptr<A64DetachedReentryPreparation>>
      detached_preparations_;
};

// Consumes boundary even on failure, guaranteeing that all parked guest
// threads are released if preflight cannot be completed. A successful output
// owns the boundary and every other preflight resource.
StateProviderResult PrepareA64RestorePreflight(
    HeldGuestBoundary boundary, const CpuSnapshot& snapshot,
    A64CodeGenerationControl& code_generation_control,
    A64ReentryDispatcher& dispatcher, std::chrono::milliseconds timeout,
    A64RestorePreflight* output, std::string* error_message);

}  // namespace xe::save_state

#endif  // XENIA_CPU_BACKEND_A64_A64_RESTORE_PREFLIGHT_H_
