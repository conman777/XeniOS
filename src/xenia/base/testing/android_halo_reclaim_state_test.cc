/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/gpu/vulkan/android_halo_reclaim_state.h"

namespace xe::gpu::vulkan::test {

TEST_CASE("Halo reclaim preserves a retained presentable source",
          "[android_halo_reclaim]") {
  AndroidHaloReclaimStateInput input = {};
  input.owner_was_presentable = true;
  input.presentable_tracked = true;
  input.presentable_rebound = true;
  input.shadow_valid = true;

  const AndroidHaloReclaimStateDecision decision =
      DecideAndroidHaloReclaimState(input);

  CHECK(decision.preserve_owner);
  CHECK(decision.preserve_shadow_valid);
  CHECK_FALSE(decision.reclaim_shadow_required);
}

TEST_CASE("Halo reclaim arms shadow only for a trimmed tracked source",
          "[android_halo_reclaim]") {
  AndroidHaloReclaimStateInput input = {};
  input.owner_was_presentable = true;
  input.presentable_tracked = true;
  input.shadow_valid = true;

  const AndroidHaloReclaimStateDecision decision =
      DecideAndroidHaloReclaimState(input);

  CHECK_FALSE(decision.preserve_owner);
  CHECK(decision.preserve_shadow_valid);
  CHECK(decision.reclaim_shadow_required);
}

TEST_CASE("Halo reclaim does not invent a fallback without a valid source",
          "[android_halo_reclaim]") {
  AndroidHaloReclaimStateInput input = {};
  input.owner_was_presentable = true;

  const AndroidHaloReclaimStateDecision decision =
      DecideAndroidHaloReclaimState(input);

  CHECK_FALSE(decision.preserve_owner);
  CHECK_FALSE(decision.preserve_shadow_valid);
  CHECK_FALSE(decision.reclaim_shadow_required);
}

TEST_CASE("Halo reclaim preserves the existing depth-alias fallback",
          "[android_halo_reclaim]") {
  AndroidHaloReclaimStateInput input = {};
  input.owner_was_depth_alias = true;
  input.depth_alias_latched = true;
  input.shadow_valid = true;

  const AndroidHaloReclaimStateDecision decision =
      DecideAndroidHaloReclaimState(input);

  CHECK(decision.preserve_owner);
  CHECK(decision.preserve_depth_alias_latch);
  CHECK(decision.preserve_shadow_valid);
  CHECK_FALSE(decision.reclaim_shadow_required);
}

TEST_CASE("Halo reclaim keeps an armed fallback across repeated trims",
          "[android_halo_reclaim]") {
  AndroidHaloReclaimStateInput input = {};
  input.shadow_valid = true;
  input.reclaim_shadow_required = true;

  const AndroidHaloReclaimStateDecision decision =
      DecideAndroidHaloReclaimState(input);

  CHECK(decision.reclaim_shadow_required);
}

}  // namespace xe::gpu::vulkan::test
