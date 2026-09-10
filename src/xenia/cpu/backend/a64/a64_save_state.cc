/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_save_state.h"

#include <cstddef>
#include <cstring>
#include <utility>

#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/ppc/ppc_context.h"

namespace xe::save_state {
namespace {

using cpu::backend::a64::A64BackendContext;
using cpu::backend::a64::kA64BackendHasReserveBit;
using cpu::ppc::PPCContext;

StateProviderResult CaptureThread(const GuestThreadBinding& binding,
                                  CpuThreadSnapshot* snapshot,
                                  std::string* error_message) {
  if (!binding.thread_id || !binding.context || !snapshot) {
    if (error_message) {
      *error_message = "A64 CPU capture received an invalid thread binding.";
    }
    return StateProviderResult::kInvalidState;
  }

  const auto* context = reinterpret_cast<const PPCContext*>(binding.context);
  const auto* backend_context = reinterpret_cast<const A64BackendContext*>(
      reinterpret_cast<const std::byte*>(context) -
      sizeof(A64BackendContext));
  if (context->thread_id != binding.thread_id ||
      !context->last_guest_pc || (context->last_guest_pc & 3) != 0) {
    if (error_message) {
      *error_message =
          "A64 CPU context is not at a valid synchronized guest PC.";
    }
    return StateProviderResult::kInvalidState;
  }
  if ((backend_context->flags & (1u << kA64BackendHasReserveBit)) != 0) {
    if (error_message) {
      *error_message =
          "A64 CPU capture cannot yet serialize an active reservation.";
    }
    return StateProviderResult::kUnsupported;
  }
  if (backend_context->pending_stack_sync != 0 ||
      backend_context->njm_enabled > 1 ||
      backend_context->non_ieee_mode != context->fpscr.bits.ni) {
    if (error_message) {
      *error_message =
          "A64 backend-derived CPU state is not capture-safe.";
    }
    return StateProviderResult::kInvalidState;
  }
  if (!backend_context->stackpoints ||
      backend_context->current_stackpoint_depth == 0 ||
      backend_context->current_stackpoint_depth >
          kMaximumGuestStackpointCount) {
    if (error_message) {
      *error_message =
          "A64 CPU capture requires a complete bounded stackpoint chain.";
    }
    return StateProviderResult::kUnsupported;
  }

  CpuThreadSnapshot captured;
  captured.thread_id = context->thread_id;
  captured.resume_pc = context->last_guest_pc;
  for (size_t index = 0; index < captured.gpr.size(); ++index) {
    captured.gpr[index] = context->r[index];
  }
  captured.ctr = context->ctr;
  captured.lr = context->lr;
  captured.msr = context->msr;
  for (size_t index = 0; index < captured.fpr_bits.size(); ++index) {
    std::memcpy(&captured.fpr_bits[index], &context->f[index],
                sizeof(captured.fpr_bits[index]));
  }
  for (size_t index = 0; index < captured.vector_registers.size(); ++index) {
    std::memcpy(captured.vector_registers[index].data(), &context->v[index],
                captured.vector_registers[index].size());
  }
  std::memcpy(captured.vscr_vector.data(), &context->vscr_vec,
              captured.vscr_vector.size());
  captured.condition_registers = {
      context->cr0.value, context->cr1.value, context->cr2.value,
      context->cr3.value, context->cr4.value, context->cr5.value,
      context->cr6.value, context->cr7.value,
  };
  captured.fpscr = context->fpscr.value;
  captured.vrsave = context->vrsave;
  captured.xer_ca = context->xer_ca;
  captured.xer_ov = context->xer_ov;
  captured.xer_so = context->xer_so;
  captured.vscr_sat = context->vscr_sat;
  captured.njm_enabled = uint8_t(backend_context->njm_enabled);
  captured.stackpoints.reserve(backend_context->current_stackpoint_depth);
  for (uint32_t index = backend_context->current_stackpoint_depth; index > 0;
       --index) {
    const auto& stackpoint = backend_context->stackpoints[index - 1];
    captured.stackpoints.push_back(
        {stackpoint.guest_sp, stackpoint.guest_return_address,
         stackpoint.stack_size});
  }
  *snapshot = std::move(captured);
  return StateProviderResult::kOk;
}

}  // namespace

StateProviderResult CaptureA64CpuSnapshot(
    const HeldGuestBoundary& boundary, CpuSnapshot* snapshot,
    std::string* error_message) {
  if (!boundary || !snapshot || boundary.bindings().empty()) {
    if (error_message) {
      *error_message =
          "A64 CPU capture requires a held cooperative guest boundary.";
    }
    return StateProviderResult::kInvalidState;
  }

  CpuSnapshot captured;
  captured.host_backend = HostBackend::kA64;
  captured.threads.reserve(boundary.bindings().size());
  for (const GuestThreadBinding& binding : boundary.bindings()) {
    CpuThreadSnapshot thread;
    const StateProviderResult result =
        CaptureThread(binding, &thread, error_message);
    if (result != StateProviderResult::kOk) {
      return result;
    }
    captured.threads.push_back(std::move(thread));
  }
  const SnapshotCodecValidation validation = ValidateCpuSnapshot(captured);
  if (!validation.ok()) {
    if (error_message) {
      *error_message = validation.message;
    }
    return StateProviderResult::kInvalidState;
  }
  *snapshot = std::move(captured);
  if (error_message) {
    error_message->clear();
  }
  return StateProviderResult::kOk;
}

}  // namespace xe::save_state
