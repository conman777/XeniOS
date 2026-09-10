/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_generation.h"

#include <limits>
#include <utility>

namespace xe::cpu::backend::a64 {

A64CodeGenerationGate::PublicationLease::~PublicationLease() { Reset(); }

A64CodeGenerationGate::PublicationLease::PublicationLease(
    PublicationLease&& other) noexcept
    : gate_(other.gate_) {
  other.gate_ = nullptr;
}

A64CodeGenerationGate::PublicationLease&
A64CodeGenerationGate::PublicationLease::operator=(
    PublicationLease&& other) noexcept {
  if (this != &other) {
    Reset();
    gate_ = other.gate_;
    other.gate_ = nullptr;
  }
  return *this;
}

void A64CodeGenerationGate::PublicationLease::Reset() {
  if (!gate_) {
    return;
  }
  A64CodeGenerationGate* gate = gate_;
  gate_ = nullptr;
  gate->EndPublication();
}

A64CodeGenerationGate::PublicationLease
A64CodeGenerationGate::BeginPublication() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this]() { return !frozen_; });
  ++active_publications_;
  return PublicationLease(this);
}

bool A64CodeGenerationGate::AcquireFreeze(
    std::chrono::milliseconds timeout, uint64_t* generation) {
  if (!generation || timeout <= std::chrono::milliseconds::zero()) {
    return false;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (!condition_.wait_for(lock, timeout, [this]() {
        return !frozen_ && active_publications_ == 0;
      })) {
    return false;
  }
  frozen_ = true;
  *generation = generation_;
  return true;
}

bool A64CodeGenerationGate::ValidateFreeze(uint64_t generation) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frozen_ && generation != 0 && generation == generation_;
}

bool A64CodeGenerationGate::ReleaseFreeze(uint64_t generation) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!frozen_ || generation == 0 || generation != generation_) {
      return false;
    }
    frozen_ = false;
  }
  condition_.notify_all();
  return true;
}

uint64_t A64CodeGenerationGate::generation() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_;
}

size_t A64CodeGenerationGate::active_publication_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_publications_;
}

void A64CodeGenerationGate::EndPublication() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_publications_) {
      return;
    }
    --active_publications_;
    generation_ = generation_ == std::numeric_limits<uint64_t>::max()
                      ? 1
                      : generation_ + 1;
  }
  condition_.notify_all();
}

}  // namespace xe::cpu::backend::a64
