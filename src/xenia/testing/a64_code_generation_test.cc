/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/a64/a64_code_generation.h"

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace xe::cpu::backend::a64::test {

TEST_CASE("A64 generation freeze rejects an active publication",
          "[save_state][a64][generation]") {
  A64CodeGenerationGate gate;
  auto publication = gate.BeginPublication();
  REQUIRE(publication);
  CHECK(gate.active_publication_count() == 1);

  uint64_t generation = UINT64_C(0xBAD0BAD0BAD0BAD0);
  CHECK_FALSE(
      gate.AcquireFreeze(std::chrono::milliseconds(20), &generation));
  CHECK(generation == UINT64_C(0xBAD0BAD0BAD0BAD0));
  CHECK_FALSE(gate.ValidateFreeze(gate.generation()));

  publication.Reset();
  CHECK(gate.active_publication_count() == 0);
  REQUIRE(gate.AcquireFreeze(std::chrono::milliseconds(100), &generation));
  CHECK(gate.ValidateFreeze(generation));
  CHECK(gate.ReleaseFreeze(generation));
}

TEST_CASE("A64 generation freeze blocks new publication through release",
          "[save_state][a64][generation]") {
  A64CodeGenerationGate gate;
  uint64_t frozen_generation = 0;
  REQUIRE(gate.AcquireFreeze(std::chrono::milliseconds(100),
                             &frozen_generation));

  std::atomic<bool> publication_acquired{false};
  auto publisher = std::async(std::launch::async, [&]() {
    auto publication = gate.BeginPublication();
    publication_acquired.store(true, std::memory_order_release);
    publication.Reset();
  });

  const auto blocked_deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  while (std::chrono::steady_clock::now() < blocked_deadline) {
    std::this_thread::yield();
  }
  CHECK_FALSE(publication_acquired.load(std::memory_order_acquire));
  CHECK(gate.ValidateFreeze(frozen_generation));

  REQUIRE(gate.ReleaseFreeze(frozen_generation));
  publisher.get();
  CHECK(publication_acquired.load(std::memory_order_acquire));
  CHECK(gate.generation() == frozen_generation + 1);
}

TEST_CASE("A64 generation validation rejects a token after publication",
          "[save_state][a64][generation]") {
  A64CodeGenerationGate gate;
  uint64_t old_generation = 0;
  REQUIRE(
      gate.AcquireFreeze(std::chrono::milliseconds(100), &old_generation));
  REQUIRE(gate.ReleaseFreeze(old_generation));

  {
    auto publication = gate.BeginPublication();
    REQUIRE(publication);
  }

  uint64_t current_generation = 0;
  REQUIRE(gate.AcquireFreeze(std::chrono::milliseconds(100),
                             &current_generation));
  CHECK(current_generation != old_generation);
  CHECK_FALSE(gate.ValidateFreeze(old_generation));
  CHECK(gate.ValidateFreeze(current_generation));
  CHECK_FALSE(gate.ReleaseFreeze(old_generation));
  CHECK(gate.ReleaseFreeze(current_generation));
}

}  // namespace xe::cpu::backend::a64::test
