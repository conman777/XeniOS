/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_resume.h"

#include <utility>

namespace xe::save_state {

StateProviderResult BuildA64ResumeSegments(
    const CpuThreadSnapshot& thread, std::vector<A64ResumeSegment>* segments,
    std::string* error_message) {
  if (!segments || thread.resume_pc == 0 || (thread.resume_pc & 3) != 0 ||
      thread.stackpoints.empty() ||
      thread.stackpoints.size() > kMaximumGuestStackpointCount) {
    if (error_message) {
      *error_message = "A64 resume state is incomplete or invalid.";
    }
    return StateProviderResult::kInvalidState;
  }

  std::vector<A64ResumeSegment> built;
  built.reserve(thread.stackpoints.size());
  uint32_t guest_pc = thread.resume_pc;
  for (const CpuStackpointSnapshot& stackpoint : thread.stackpoints) {
    if (!stackpoint.guest_stack_pointer ||
        (stackpoint.guest_stack_pointer & 15) != 0 ||
        !stackpoint.guest_return_address ||
        (stackpoint.guest_return_address & 3) != 0 ||
        !stackpoint.host_stack_size ||
        (stackpoint.host_stack_size & 15) != 0 ||
        stackpoint.host_stack_size > kMaximumCapturedHostStackSize) {
      if (error_message) {
        *error_message = "A64 resume stackpoint chain is invalid.";
      }
      return StateProviderResult::kInvalidState;
    }
    built.push_back({guest_pc, stackpoint.guest_stack_pointer,
                     stackpoint.guest_return_address,
                     stackpoint.host_stack_size});
    guest_pc = stackpoint.guest_return_address;
  }
  *segments = std::move(built);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

StateProviderResult BuildA64ResumeDispatchPlan(
    const CpuThreadSnapshot& thread, A64ResumeTargetLookup& lookup,
    A64ResumeDispatchPlan* plan, std::string* error_message) {
  if (!plan) {
    if (error_message) {
      *error_message = "A64 resume dispatch plan output is missing.";
    }
    return StateProviderResult::kInvalidState;
  }
  std::vector<A64ResumeSegment> guest_segments;
  StateProviderResult result =
      BuildA64ResumeSegments(thread, &guest_segments, error_message);
  if (result != StateProviderResult::kOk) {
    return result;
  }

  A64ResumeDispatchPlan built;
  built.segments.reserve(guest_segments.size());
  for (const A64ResumeSegment& segment : guest_segments) {
    uintptr_t host_entry = 0;
    uint32_t host_stack_size = 0;
    result = lookup.Lookup(segment.guest_pc, &host_entry, &host_stack_size,
                           error_message);
    if (result != StateProviderResult::kOk) {
      return result;
    }
    if (!host_entry || (host_entry & 3) != 0 ||
        host_stack_size != segment.captured_host_stack_size) {
      if (error_message) {
        *error_message =
            "A64 resume target is missing or its stack size changed.";
      }
      return StateProviderResult::kInvalidState;
    }
    built.segments.push_back({segment, host_entry, host_stack_size});
  }
  *plan = std::move(built);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

}  // namespace xe::save_state
