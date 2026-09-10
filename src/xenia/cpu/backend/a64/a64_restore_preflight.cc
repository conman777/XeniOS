/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_restore_preflight.h"

#include <algorithm>
#include <utility>

namespace xe::save_state {
namespace {

StateProviderResult Invalid(std::string message, std::string* error_message) {
  if (error_message) {
    *error_message = std::move(message);
  }
  return StateProviderResult::kInvalidState;
}

}  // namespace

A64RestorePreflight::~A64RestorePreflight() { Reset(); }

A64RestorePreflight::A64RestorePreflight(
    A64RestorePreflight&& other) noexcept
    : boundary_(std::move(other.boundary_)),
      code_generation_(std::move(other.code_generation_)),
      threads_(std::move(other.threads_)),
      detached_preparations_(std::move(other.detached_preparations_)) {}

A64RestorePreflight& A64RestorePreflight::operator=(
    A64RestorePreflight&& other) noexcept {
  if (this != &other) {
    Reset();
    boundary_ = std::move(other.boundary_);
    code_generation_ = std::move(other.code_generation_);
    threads_ = std::move(other.threads_);
    detached_preparations_ = std::move(other.detached_preparations_);
  }
  return *this;
}

void A64RestorePreflight::Reset() {
  // Detached preparations may refer to immutable code metadata, so destroy
  // them before releasing the generation lease. Guest context identities
  // remain valid until the boundary is released last.
  detached_preparations_.clear();
  threads_.clear();
  code_generation_.reset();
  boundary_.Reset();
}

StateProviderResult PrepareA64RestorePreflight(
    HeldGuestBoundary boundary, const CpuSnapshot& snapshot,
    A64CodeGenerationControl& code_generation_control,
    A64ReentryDispatcher& dispatcher, std::chrono::milliseconds timeout,
    A64RestorePreflight* output, std::string* error_message) {
  if (!output || *output) {
    return Invalid("A64 restore preflight output is missing or already owns "
                   "a live preflight.",
                   error_message);
  }
  if (!boundary) {
    return Invalid("A64 restore preflight requires a held guest boundary.",
                   error_message);
  }
  if (timeout <= std::chrono::milliseconds::zero() ||
      timeout > kMaximumGuestBarrierWait) {
    return Invalid("A64 code-generation freeze timeout is invalid.",
                   error_message);
  }

  const SnapshotCodecValidation snapshot_validation =
      ValidateCpuSnapshot(snapshot);
  if (!snapshot_validation.ok()) {
    if (error_message) {
      *error_message = snapshot_validation.message;
    }
    return StateProviderResult::kInvalidState;
  }

  const auto& bindings = boundary.bindings();
  if (bindings.size() != snapshot.threads.size()) {
    return Invalid(
        "Held guest-thread membership does not match the CPU snapshot.",
        error_message);
  }

  std::vector<A64PreparedThreadReentry> threads;
  threads.reserve(snapshot.threads.size());
  for (const CpuThreadSnapshot& thread : snapshot.threads) {
    auto binding = std::find_if(
        bindings.begin(), bindings.end(),
        [&thread](const GuestThreadBinding& candidate) {
          return candidate.thread_id == thread.thread_id;
        });
    if (binding == bindings.end() || !binding->context) {
      return Invalid(
          "Held guest-thread membership does not match the CPU snapshot.",
          error_message);
    }
    threads.push_back({thread.thread_id, binding->context, {}});
  }

  std::unique_ptr<A64FrozenCodeGeneration> frozen_code;
  StateProviderResult result = code_generation_control.AcquireFreeze(
      timeout, &frozen_code, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }
  if (!frozen_code || frozen_code->generation() == 0) {
    return Invalid("A64 code-generation freeze returned an invalid lease.",
                   error_message);
  }
  result = frozen_code->Validate(error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }

  for (size_t i = 0; i < snapshot.threads.size(); ++i) {
    result = BuildA64ResumeDispatchPlan(snapshot.threads[i], *frozen_code,
                                        &threads[i].dispatch, error_message);
    if (result != StateProviderResult::kOk) {
      return result;
    }
  }

  // Validate again after all lookups before any detached trampoline state is
  // accepted. A correct exclusive lease cannot change generation; this check
  // catches contract violations and stale adapters fail closed.
  result = frozen_code->Validate(error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }

  std::vector<std::unique_ptr<A64DetachedReentryPreparation>> preparations;
  preparations.reserve(threads.size());
  for (const A64PreparedThreadReentry& thread : threads) {
    std::unique_ptr<A64DetachedReentryPreparation> preparation;
    result =
        dispatcher.PrepareDetached(thread, &preparation, error_message);
    if (result != StateProviderResult::kOk) {
      return result;
    }
    if (!preparation || preparation->thread_id() != thread.thread_id) {
      return Invalid(
          "A64 re-entry dispatcher returned an invalid detached preparation.",
          error_message);
    }
    preparations.push_back(std::move(preparation));
  }

  result = frozen_code->Validate(error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }

  A64RestorePreflight built;
  built.boundary_ = std::move(boundary);
  built.code_generation_ = std::move(frozen_code);
  built.threads_ = std::move(threads);
  built.detached_preparations_ = std::move(preparations);
  *output = std::move(built);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

}  // namespace xe::save_state
