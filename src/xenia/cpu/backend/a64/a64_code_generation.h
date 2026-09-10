/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_CODE_GENERATION_H_
#define XENIA_CPU_BACKEND_A64_A64_CODE_GENERATION_H_

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace xe::cpu::backend::a64 {

// Coordinates complete generated-function publication with save-state
// preflight. This is deliberately separate from the code-placement lock:
// publication spans emitter placement, source-map cloning, function Setup,
// stack-size visibility, and indirection installation.
class A64CodeGenerationGate {
 public:
  class PublicationLease {
   public:
    PublicationLease() = default;
    ~PublicationLease();
    PublicationLease(const PublicationLease&) = delete;
    PublicationLease& operator=(const PublicationLease&) = delete;
    PublicationLease(PublicationLease&& other) noexcept;
    PublicationLease& operator=(PublicationLease&& other) noexcept;

    explicit operator bool() const { return gate_ != nullptr; }
    void Reset();

   private:
    friend class A64CodeGenerationGate;
    explicit PublicationLease(A64CodeGenerationGate* gate) : gate_(gate) {}

    A64CodeGenerationGate* gate_ = nullptr;
  };

  A64CodeGenerationGate() = default;
  A64CodeGenerationGate(const A64CodeGenerationGate&) = delete;
  A64CodeGenerationGate& operator=(const A64CodeGenerationGate&) = delete;

  // Waits until no exclusive freeze is held. Publication completion advances
  // the generation even if the assembler later reports failure, conservatively
  // invalidating any token that could have observed an attempted publication.
  PublicationLease BeginPublication();

  // Acquires the sole generation freeze after all in-flight publications have
  // ended. The wait is bounded; failure leaves the gate unchanged.
  bool AcquireFreeze(std::chrono::milliseconds timeout, uint64_t* generation);
  bool ValidateFreeze(uint64_t generation) const;
  bool ReleaseFreeze(uint64_t generation);

  uint64_t generation() const;
  size_t active_publication_count() const;

 private:
  friend class PublicationLease;
  void EndPublication();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  uint64_t generation_ = 1;
  size_t active_publications_ = 0;
  bool frozen_ = false;
};

}  // namespace xe::cpu::backend::a64

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_GENERATION_H_
