/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XTIMER_H_
#define XENIA_KERNEL_XTIMER_H_

#include <mutex>

#include "xenia/base/threading.h"
#include "xenia/kernel/xobject.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {

constexpr fourcc_t kTimerSaveSignature = make_fourcc("XTMR");

class XThread;

class XTimer : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Timer;

  explicit XTimer(KernelState* kernel_state);
  ~XTimer() override;

  void Initialize(uint32_t timer_type);

  X_STATUS SetTimer(int64_t due_time, uint32_t period_ms, uint32_t routine,
                    uint32_t routine_arg, bool resume,
                    XThread* callback_thread = nullptr);
  X_STATUS Cancel();

  bool Save(ByteStream* stream) override;
  static object_ref<XTimer> Restore(KernelState* kernel_state,
                                    ByteStream* stream);

 protected:
  xe::threading::WaitHandle* GetWaitHandle() override { return timer_.get(); }

 private:
  std::unique_ptr<xe::threading::Timer> timer_;
  std::mutex timer_lock_;

  XThread* callback_thread_ = nullptr;
  uint32_t callback_routine_ = 0;
  uint32_t callback_routine_arg_ = 0;
  uint32_t timer_type_ = 0;
  int64_t save_state_due_time_ = 0;
  uint32_t save_state_period_ms_ = 0;
  // Conservatively remains true after a one-shot fires because the host timer
  // interface exposes no non-invasive armed-state query.
  bool save_state_pending_accounted_ = false;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XTIMER_H_
