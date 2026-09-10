/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026. All rights reserved.
 * Released under the BSD license - see LICENSE in the root for more details.
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_ANDROID_DIAGNOSTIC_STATE_H_
#define XENIA_GPU_VULKAN_ANDROID_DIAGNOSTIC_STATE_H_

#include <atomic>
#include <cstdint>

#include "xenia/base/platform.h"

namespace xe {
namespace gpu {
namespace vulkan {

#if XE_PLATFORM_ANDROID

enum class AndroidDiagnosticRenderTargetPath : uint32_t {
  kUnavailable,
  kHostRenderTargets,
  kFragmentShaderInterlock,
};

enum class AndroidDiagnosticOwnerState : uint32_t {
  kUnknown,
  kPresentableColor,
  kDepthColorAliasNonPresentable,
};

struct AndroidDiagnosticRenderSnapshot {
  bool renderer_initialized;
  bool requested_fragment_shader_interlock;
  AndroidDiagnosticRenderTargetPath render_target_path;
  bool fragment_sample_interlock;
  bool fragment_pixel_interlock;
  uint64_t resolve_count;
  uint32_t last_resolve_destination;
  uint32_t last_resolve_length;
  uint32_t last_resolve_destination_format;
  uint32_t last_resolve_source_base_tiles;
  uint32_t last_resolve_source_format;
  uint32_t last_resolve_msaa_samples;
  bool last_resolve_source_is_depth;
  AndroidDiagnosticOwnerState owner_state;
};

class AndroidDiagnosticRenderState {
 public:
  static AndroidDiagnosticRenderState& Get() {
    static AndroidDiagnosticRenderState state;
    return state;
  }

  void SetRenderer(
      bool requested_fragment_shader_interlock,
      AndroidDiagnosticRenderTargetPath render_target_path,
      bool fragment_sample_interlock, bool fragment_pixel_interlock) {
    requested_fragment_shader_interlock_.store(
        requested_fragment_shader_interlock, std::memory_order_relaxed);
    render_target_path_.store(uint32_t(render_target_path),
                              std::memory_order_relaxed);
    fragment_sample_interlock_.store(fragment_sample_interlock,
                                     std::memory_order_relaxed);
    fragment_pixel_interlock_.store(fragment_pixel_interlock,
                                    std::memory_order_relaxed);
    renderer_initialized_.store(true, std::memory_order_release);
  }

  void RecordResolve(uint32_t destination, uint32_t length,
                     uint32_t destination_format,
                     uint32_t source_base_tiles, uint32_t source_format,
                     uint32_t msaa_samples, bool source_is_depth) {
    last_resolve_destination_.store(destination, std::memory_order_relaxed);
    last_resolve_length_.store(length, std::memory_order_relaxed);
    last_resolve_destination_format_.store(destination_format,
                                           std::memory_order_relaxed);
    last_resolve_source_base_tiles_.store(source_base_tiles,
                                          std::memory_order_relaxed);
    last_resolve_source_format_.store(source_format,
                                      std::memory_order_relaxed);
    last_resolve_msaa_samples_.store(msaa_samples, std::memory_order_relaxed);
    last_resolve_source_is_depth_.store(source_is_depth,
                                        std::memory_order_relaxed);
    resolve_count_.fetch_add(1, std::memory_order_release);
  }

  void SetOwnerState(AndroidDiagnosticOwnerState owner_state) {
    owner_state_.store(uint32_t(owner_state), std::memory_order_relaxed);
  }

  AndroidDiagnosticRenderSnapshot Snapshot() const {
    AndroidDiagnosticRenderSnapshot snapshot = {};
    snapshot.renderer_initialized =
        renderer_initialized_.load(std::memory_order_acquire);
    snapshot.requested_fragment_shader_interlock =
        requested_fragment_shader_interlock_.load(std::memory_order_relaxed);
    snapshot.render_target_path = AndroidDiagnosticRenderTargetPath(
        render_target_path_.load(std::memory_order_relaxed));
    snapshot.fragment_sample_interlock =
        fragment_sample_interlock_.load(std::memory_order_relaxed);
    snapshot.fragment_pixel_interlock =
        fragment_pixel_interlock_.load(std::memory_order_relaxed);
    snapshot.resolve_count = resolve_count_.load(std::memory_order_acquire);
    snapshot.last_resolve_destination =
        last_resolve_destination_.load(std::memory_order_relaxed);
    snapshot.last_resolve_length =
        last_resolve_length_.load(std::memory_order_relaxed);
    snapshot.last_resolve_destination_format =
        last_resolve_destination_format_.load(std::memory_order_relaxed);
    snapshot.last_resolve_source_base_tiles =
        last_resolve_source_base_tiles_.load(std::memory_order_relaxed);
    snapshot.last_resolve_source_format =
        last_resolve_source_format_.load(std::memory_order_relaxed);
    snapshot.last_resolve_msaa_samples =
        last_resolve_msaa_samples_.load(std::memory_order_relaxed);
    snapshot.last_resolve_source_is_depth =
        last_resolve_source_is_depth_.load(std::memory_order_relaxed);
    snapshot.owner_state = AndroidDiagnosticOwnerState(
        owner_state_.load(std::memory_order_relaxed));
    return snapshot;
  }

 private:
  AndroidDiagnosticRenderState() = default;

  std::atomic<bool> renderer_initialized_{false};
  std::atomic<bool> requested_fragment_shader_interlock_{false};
  std::atomic<uint32_t> render_target_path_{
      uint32_t(AndroidDiagnosticRenderTargetPath::kUnavailable)};
  std::atomic<bool> fragment_sample_interlock_{false};
  std::atomic<bool> fragment_pixel_interlock_{false};
  std::atomic<uint64_t> resolve_count_{0};
  std::atomic<uint32_t> last_resolve_destination_{0};
  std::atomic<uint32_t> last_resolve_length_{0};
  std::atomic<uint32_t> last_resolve_destination_format_{0};
  std::atomic<uint32_t> last_resolve_source_base_tiles_{0};
  std::atomic<uint32_t> last_resolve_source_format_{0};
  std::atomic<uint32_t> last_resolve_msaa_samples_{0};
  std::atomic<bool> last_resolve_source_is_depth_{false};
  std::atomic<uint32_t> owner_state_{
      uint32_t(AndroidDiagnosticOwnerState::kUnknown)};
};

#endif  // XE_PLATFORM_ANDROID

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_ANDROID_DIAGNOSTIC_STATE_H_
