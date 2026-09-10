/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xiocompletion.h"

#include "xenia/base/byte_stream.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {

XIOCompletion::XIOCompletion(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {
  notification_semaphore_ = threading::Semaphore::Create(0, kMaxNotifications);
  assert_not_null(notification_semaphore_);
}

XIOCompletion::~XIOCompletion() {
  if (!kernel_state_) {
    return;
  }
  auto notification_admission =
      kernel_state_->AcquireSaveStateKernelAsyncAdmission(
          save_state::KernelAsyncDomain::kNotification);
  std::unique_lock<std::mutex> lock(notification_lock_);
  notification_admission.RecordDequeued(notifications_.size());
}

void XIOCompletion::QueueNotification(IONotification& notification) {
  auto notification_admission =
      kernel_state()->AcquireSaveStateKernelAsyncAdmission(
          save_state::KernelAsyncDomain::kNotification);
  std::unique_lock<std::mutex> lock(notification_lock_);

  notifications_.push(notification);
  notification_admission.RecordEnqueued();
  notification_semaphore_->Release(1, nullptr);
}

bool XIOCompletion::WaitForNotification(uint64_t wait_ticks,
                                        IONotification* notify) {
  auto wait_admission = kernel_state()->AcquireSaveStateKernelAsyncAdmission(
      save_state::KernelAsyncDomain::kWait);
  auto ms = std::chrono::milliseconds(TimeoutTicksToMs(wait_ticks));
  auto res = threading::Wait(notification_semaphore_.get(), false, ms);
  if (res == threading::WaitResult::kSuccess) {
    auto notification_admission =
        kernel_state()->AcquireSaveStateKernelAsyncAdmission(
            save_state::KernelAsyncDomain::kNotification);
    std::unique_lock<std::mutex> lock(notification_lock_);
    assert_false(notifications_.empty());

    std::memcpy(notify, &notifications_.front(), sizeof(IONotification));
    notifications_.pop();
    notification_admission.RecordDequeued();

    return true;
  }

  return false;
}

bool XIOCompletion::Save(ByteStream* stream) {
  std::lock_guard<std::mutex> lock(notification_lock_);
  if (notifications_.size() > kMaxNotifications || !SaveObject(stream)) {
    return false;
  }
  stream->Write(kIOCompletionSaveSignature);
  stream->Write<uint32_t>(uint32_t(notifications_.size()));
  auto copy = notifications_;
  while (!copy.empty()) {
    stream->Write(copy.front());
    copy.pop();
  }
  return true;
}

object_ref<XIOCompletion> XIOCompletion::Restore(KernelState* kernel_state,
                                                 ByteStream* stream) {
  auto completion = object_ref<XIOCompletion>(new XIOCompletion(kernel_state));
  if (!completion->RestoreObject(stream) ||
      stream->Read<uint32_t>() != kIOCompletionSaveSignature) {
    return nullptr;
  }
  const uint32_t count = stream->Read<uint32_t>();
  if (count > kMaxNotifications) {
    return nullptr;
  }
  for (uint32_t index = 0; index < count; ++index) {
    completion->notifications_.push(stream->Read<IONotification>());
  }
  if (count && !completion->notification_semaphore_->Release(count, nullptr)) {
    return nullptr;
  }
  return completion;
}

}  // namespace kernel
}  // namespace xe
