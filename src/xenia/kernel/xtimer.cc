/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  if (!kernel_state_) {
    return;
  }
  auto timer_admission =
      kernel_state_->AcquireSaveStateKernelAsyncAdmission(
          save_state::KernelAsyncDomain::kTimer);
  // Destroying the host timer while admitted makes callback teardown part of
  // the same reversible boundary.
  timer_.reset();
  if (save_state_pending_accounted_) {
    timer_admission.RecordDequeued();
  }
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  timer_type_ = timer_type;
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume,
                          XThread* callback_thread) {
  auto admission =
      kernel_state()->AcquireSaveStateKernelDispatchTimerAdmission();
  auto timer_admission =
      kernel_state()->AcquireSaveStateKernelAsyncAdmission(
          save_state::KernelAsyncDomain::kTimer);
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;
  // Caller is checking for STATUS_TIMER_RESUME_IGNORED.
  if (resume) {
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  std::lock_guard<std::mutex> lock(timer_lock_);

  save_state_due_time_ = due_time;
  save_state_period_ms_ = period_ms;
  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // Stash routine for callback.
  callback_thread_ =
      callback_thread ? callback_thread : XThread::GetCurrentThread();
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    auto* callback_kernel_state = kernel_state();
    callback = [callback_kernel_state, cb_thread, cb_routine,
                cb_routine_arg]() {
      auto timer_fire_admission =
          callback_kernel_state->AcquireSaveStateKernelAsyncAdmission(
              save_state::KernelAsyncDomain::kTimer);
      auto callback_admission =
          callback_kernel_state
              ->AcquireSaveStateKernelDispatchTimerAdmission();
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }
  if (result && !save_state_pending_accounted_) {
    timer_admission.RecordEnqueued();
    save_state_pending_accounted_ = true;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  auto admission =
      kernel_state()->AcquireSaveStateKernelDispatchTimerAdmission();
  auto timer_admission =
      kernel_state()->AcquireSaveStateKernelAsyncAdmission(
          save_state::KernelAsyncDomain::kTimer);
  std::lock_guard<std::mutex> lock(timer_lock_);
  const bool result = timer_->Cancel();
  if (result && save_state_pending_accounted_) {
    timer_admission.RecordDequeued();
    save_state_pending_accounted_ = false;
  }
  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

bool XTimer::Save(ByteStream* stream) {
  std::lock_guard<std::mutex> lock(timer_lock_);
  if (!SaveObject(stream)) {
    return false;
  }
  stream->Write(kTimerSaveSignature);
  stream->Write(timer_type_);
  stream->Write(save_state_pending_accounted_);
  stream->Write(save_state_due_time_);
  stream->Write(save_state_period_ms_);
  stream->Write(callback_routine_);
  stream->Write(callback_routine_arg_);
  stream->Write(callback_thread_ ? callback_thread_->handle() : uint32_t(0));
  return true;
}

object_ref<XTimer> XTimer::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  auto timer = object_ref<XTimer>(new XTimer(kernel_state));
  if (!timer->RestoreObject(stream) ||
      stream->Read<uint32_t>() != kTimerSaveSignature) {
    return nullptr;
  }
  const uint32_t timer_type = stream->Read<uint32_t>();
  const bool pending = stream->Read<bool>();
  const int64_t due_time = stream->Read<int64_t>();
  const uint32_t period_ms = stream->Read<uint32_t>();
  const uint32_t routine = stream->Read<uint32_t>();
  const uint32_t routine_arg = stream->Read<uint32_t>();
  const uint32_t callback_thread_handle = stream->Read<uint32_t>();
  if (timer_type > 1) {
    return nullptr;
  }
  timer->Initialize(timer_type);
  if (pending) {
    auto callback_thread =
        kernel_state->object_table()->LookupObject<XThread>(
            callback_thread_handle);
    if (!callback_thread ||
        XFAILED(timer->SetTimer(due_time, period_ms, routine, routine_arg,
                               false, callback_thread.get()))) {
      return nullptr;
    }
  }
  return timer;
}

}  // namespace kernel
}  // namespace xe
