/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state_live_memory.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace xe::save_state::test {
namespace {

using TestPage = std::array<uint8_t, 64>;

std::vector<StablePageCopyBinding> MakeBindings(
    std::array<TestPage, 4>& live, const std::array<TestPage, 4>& target) {
  std::vector<StablePageCopyBinding> bindings;
  for (size_t index = 0; index < live.size(); ++index) {
    bindings.push_back({uint64_t(index + 1), live[index].data(),
                        target[index].data(), uint32_t(live[index].size())});
  }
  return bindings;
}

MemoryCaptureLimits TestLimits() {
  MemoryCaptureLimits limits;
  limits.maximum_page_count = 4;
  limits.maximum_content_bytes = 4 * sizeof(TestPage);
  return limits;
}

class TestTransactionLock final : public StablePageTransactionLock {
 public:
  explicit TestTransactionLock(bool& released) : released_(released) {}
  ~TestTransactionLock() override { released_ = true; }

 private:
  bool& released_;
};

}  // namespace

TEST_CASE("Stable-page transaction restores every injected partial copy",
          "[save_state][transaction][memory][live]") {
  std::array<TestPage, 4> original = {};
  std::array<TestPage, 4> target = {};
  for (size_t page = 0; page < original.size(); ++page) {
    original[page].fill(uint8_t(0x10 + page));
    target[page].fill(uint8_t(0x80 + page));
  }

  for (size_t fail_before_page = 0; fail_before_page < original.size();
       ++fail_before_page) {
    std::array<TestPage, 4> live = original;
    const auto bindings = MakeBindings(live, target);
    std::unique_ptr<StablePageMemoryTransaction> transaction;
    std::string error;
    REQUIRE(StablePageMemoryTransaction::Prepare(
                bindings, TestLimits(), nullptr, &transaction, &error) ==
            StateProviderResult::kOk);
    REQUIRE(transaction);
    CHECK(transaction->page_count() == original.size());
    CHECK(transaction->content_size() ==
          original.size() * sizeof(TestPage));

    CHECK(transaction->CommitWithFailureBeforePageForTesting(
              fail_before_page, &error) == StateProviderResult::kFailed);
    for (size_t page = 0; page < fail_before_page; ++page) {
      CHECK(live[page] == target[page]);
    }
    REQUIRE(transaction->Rollback() == StateProviderResult::kOk);
    CHECK(live == original);
  }
}

TEST_CASE("Stable-page transaction owns detached target and complete undo",
          "[save_state][transaction][memory][live]") {
  std::array<TestPage, 4> live = {};
  std::array<TestPage, 4> target = {};
  for (size_t page = 0; page < live.size(); ++page) {
    live[page].fill(uint8_t(page + 1));
    target[page].fill(uint8_t(page + 9));
  }
  const std::array<TestPage, 4> original = live;
  const std::array<TestPage, 4> expected_target = target;
  const auto bindings = MakeBindings(live, target);
  std::unique_ptr<StablePageMemoryTransaction> transaction;
  REQUIRE(StablePageMemoryTransaction::Prepare(
              bindings, TestLimits(), nullptr, &transaction, nullptr) ==
          StateProviderResult::kOk);

  // Staging must not retain pointers to target storage.
  for (TestPage& page : target) {
    page.fill(0xEE);
  }
  REQUIRE(transaction->Commit(nullptr) == StateProviderResult::kOk);
  CHECK(live == expected_target);
  REQUIRE(transaction->Rollback() == StateProviderResult::kOk);
  CHECK(live == original);
}

TEST_CASE("Stable-page transaction finalizes success without reverting",
          "[save_state][transaction][memory][live]") {
  std::array<TestPage, 4> live = {};
  std::array<TestPage, 4> target = {};
  for (size_t page = 0; page < live.size(); ++page) {
    live[page].fill(uint8_t(page));
    target[page].fill(uint8_t(0xF0 + page));
  }
  const auto bindings = MakeBindings(live, target);
  std::unique_ptr<StablePageMemoryTransaction> transaction;
  REQUIRE(StablePageMemoryTransaction::Prepare(
              bindings, TestLimits(), nullptr, &transaction, nullptr) ==
          StateProviderResult::kOk);
  REQUIRE(transaction->Commit(nullptr) == StateProviderResult::kOk);
  transaction->Finalize();
  CHECK(live == target);
}

TEST_CASE("Stable-page transaction owns its lock through rollback",
          "[save_state][transaction][memory][live]") {
  std::array<TestPage, 4> live = {};
  std::array<TestPage, 4> target = {};
  const std::array<TestPage, 4> original = live;
  for (TestPage& page : target) {
    page.fill(0xA5);
  }
  const auto bindings = MakeBindings(live, target);
  bool lock_released = false;
  std::unique_ptr<StablePageMemoryTransaction> transaction;
  REQUIRE(StablePageMemoryTransaction::Prepare(
              bindings, TestLimits(),
              std::make_unique<TestTransactionLock>(lock_released),
              &transaction, nullptr) == StateProviderResult::kOk);
  CHECK_FALSE(lock_released);
  REQUIRE(transaction->CommitWithFailureBeforePageForTesting(2, nullptr) ==
          StateProviderResult::kFailed);
  CHECK_FALSE(lock_released);

  // Destruction is also fail-closed and performs exact rollback.
  transaction.reset();
  CHECK(lock_released);
  CHECK(live == original);
}

TEST_CASE("Stable-page transaction rejects duplicate or overlapping backing",
          "[save_state][transaction][memory][live]") {
  std::array<uint8_t, 128> live = {};
  std::array<uint8_t, 128> target = {};
  MemoryCaptureLimits limits;
  limits.maximum_page_count = 2;
  limits.maximum_content_bytes = live.size();
  std::unique_ptr<StablePageMemoryTransaction> transaction;

  std::vector<StablePageCopyBinding> duplicate = {
      {1, live.data(), target.data(), 64},
      {1, live.data() + 64, target.data() + 64, 64},
  };
  CHECK(StablePageMemoryTransaction::Prepare(
            duplicate, limits, nullptr, &transaction, nullptr) ==
        StateProviderResult::kInvalidState);
  CHECK_FALSE(transaction);

  std::vector<StablePageCopyBinding> overlapping = {
      {1, live.data(), target.data(), 64},
      {2, live.data() + 32, target.data() + 64, 64},
  };
  CHECK(StablePageMemoryTransaction::Prepare(
            overlapping, limits, nullptr, &transaction, nullptr) ==
        StateProviderResult::kInvalidState);
  CHECK_FALSE(transaction);
  const std::array<uint8_t, 128> zero = {};
  CHECK(live == zero);
}

}  // namespace xe::save_state::test
