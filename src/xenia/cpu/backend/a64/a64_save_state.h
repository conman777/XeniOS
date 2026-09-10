/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_SAVE_STATE_H_
#define XENIA_CPU_BACKEND_A64_A64_SAVE_STATE_H_

#include <string>

#include "xenia/save_state_phase2.h"
#include "xenia/save_state_runtime.h"

namespace xe::save_state {

// Captures synchronized architectural A64 guest CPU state while a live
// cooperative boundary is held. Restore intentionally remains unsupported
// until generated-frame unwind and resume-PC redirection are implemented.
StateProviderResult CaptureA64CpuSnapshot(
    const HeldGuestBoundary& boundary, CpuSnapshot* snapshot,
    std::string* error_message);

}  // namespace xe::save_state

#endif  // XENIA_CPU_BACKEND_A64_A64_SAVE_STATE_H_
