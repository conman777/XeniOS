/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_resume.h"

#include "xenia/cpu/backend/a64/a64_code_cache.h"

namespace xe::save_state {

StateProviderResult A64ExistingCodeLookup::Lookup(
    uint32_t guest_pc, uintptr_t* host_entry, uint32_t* host_stack_size,
    std::string* error_message) {
  if (!code_cache_.LookupExistingGuestCode(guest_pc, host_entry,
                                           host_stack_size)) {
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

}  // namespace xe::save_state
