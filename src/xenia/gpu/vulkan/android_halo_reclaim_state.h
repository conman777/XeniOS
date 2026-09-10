/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_ANDROID_HALO_RECLAIM_STATE_H_
#define XENIA_GPU_VULKAN_ANDROID_HALO_RECLAIM_STATE_H_

namespace xe {
namespace gpu {
namespace vulkan {

struct AndroidHaloReclaimStateInput {
  bool owner_was_presentable;
  bool owner_was_depth_alias;
  bool depth_alias_latched;
  bool presentable_tracked;
  bool presentable_rebound;
  bool msaa_scene_tracked;
  bool msaa_scene_rebound;
  bool shadow_valid;
  bool reclaim_shadow_required;
};

struct AndroidHaloReclaimStateDecision {
  bool preserve_owner;
  bool preserve_depth_alias_latch;
  bool preserve_shadow_valid;
  bool reclaim_shadow_required;
};

constexpr AndroidHaloReclaimStateDecision DecideAndroidHaloReclaimState(
    const AndroidHaloReclaimStateInput& input) {
  const bool tracked_source_rebound =
      (input.presentable_tracked && input.presentable_rebound) ||
      (input.msaa_scene_tracked && input.msaa_scene_rebound);
  const bool tracked_source_lost =
      (input.presentable_tracked || input.msaa_scene_tracked) &&
      !tracked_source_rebound;
  return {
      tracked_source_rebound || input.owner_was_depth_alias,
      input.depth_alias_latched,
      input.shadow_valid,
      input.shadow_valid && !tracked_source_rebound &&
          (input.reclaim_shadow_required ||
           (input.owner_was_presentable && tracked_source_lost)),
  };
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_ANDROID_HALO_RECLAIM_STATE_H_
