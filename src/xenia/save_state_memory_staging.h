/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_MEMORY_STAGING_H_
#define XENIA_SAVE_STATE_MEMORY_STAGING_H_

#include "xenia/save_state_transaction.h"

namespace xe::save_state {

// Transactional model for a bounded detached memory image. Stage performs all
// validation and allocation in private storage. Commit and Rollback are
// allocation-free vector swaps. This is not an adapter for live xe::Memory.
class DetachedMemoryRestoreBackend final : public PlannedMemoryRestoreAccess {
 public:
  DetachedMemoryRestoreBackend(MemorySnapshot& image,
                               MemoryCaptureLimits limits);

  StateProviderResult CaptureInventory(
      MemoryAllocationInventory* inventory,
      std::string* error_message) override;
  StateProviderResult Stage(const MemoryRestorePlan& plan,
                            const MemorySnapshot& target,
                            std::string* error_message) override;
  StateProviderResult Commit(std::string* error_message) override;
  StateProviderResult Rollback() noexcept override;
  void Finalize() noexcept override;

 private:
  MemorySnapshot& image_;
  MemoryCaptureLimits limits_;
  MemorySnapshot staged_;
  bool staged_ready_ = false;
  bool committed_ = false;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_MEMORY_STAGING_H_
