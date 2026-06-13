/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_ANDROID_HALO_EXPERIMENT_H_
#define XENIA_GPU_VULKAN_ANDROID_HALO_EXPERIMENT_H_

// Halo: Reach Android frontbuffer experiment switches.
//
// The cvar/profile override path (OverrideAndroidConfigVar) has proven
// unreliable at runtime on the Android build (silent no-op on dynamic_cast or
// registry misses), which has repeatedly invalidated device experiments. This
// reader bypasses the cvar system entirely: it parses
// files/halo_experiment.txt directly, once, and logs every effective value so
// each run is self-verifying from logcat alone. Defaults encode the current
// best fix configuration, so no file is required on the device.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "xenia/base/logging.h"

namespace xe {
namespace gpu {
namespace vulkan {

struct AndroidHaloExperiment {
  // Copy the tracked presentable color RT into EDRAM base 1350 right before
  // the final frontbuffer resolve.
  bool direct_presentable_resolve = true;
  // Use a byte-exact vkCmdCopyImageToBuffer tile copy for the refresh above
  // instead of the format-converting compute dump shader.
  bool final_resolve_raw_copy = true;
  // Diagnostic A/B for the final-presentable writer: bypass raw image copies
  // and swap G/B while packing 8888 into EDRAM. Default off; the config-only
  // swap_swizzle_override remains the safe user-visible fix until verified.
  bool writer_gb_fix = false;
  // Skip the exact 4x-depth -> 1x-color ownership transfer that clobbers the
  // presentable color span at base 1350 with depth garbage.
  bool skip_depth_to_color_alias = true;
  // Fall back to the captured presentable shadow buffer when the direct
  // refresh is not possible at final resolve time.
  bool shadow_fallback = true;
  // Force the alpha test to always pass and disable alpha-to-mask in the
  // translated fragment shaders. Device A/B (2026-06-10) proved the
  // gl_SampleMask init alone fixes the dropped color writes, so this hack is
  // off by default; kept as a diagnostic switch.
  bool force_alpha_pass = false;
  // Declare geometry shaders unsupported so rectangle/point/quad lists use the
  // vertex-shader-expansion / index-conversion fallbacks (A/B switch: Halo's
  // fullscreen composite quad is a rectangle list; if Adreno geometry shaders
  // silently emit nothing, this restores those draws).
  bool disable_geometry_shaders = true;
  // When an MSAA color RT is dumped to EDRAM for a resolve the guest reads
  // as 1x (samples-as-pixels aliasing), read host sample 0 for every sample
  // slot so the 1x read is a clean image instead of interleaved stripes.
  bool collapse_msaa_resolve = true;
  // 1 = value-convert collapsible owners to 8888 before EDRAM packing.
  // 0 = keep the owner's native pack format so raw bits land in EDRAM.
  uint32_t repack_mode = 1;
  // Force readback_resolve=full (CPU-visible guest memory stays authoritative
  // for diagnostics, but the GPU stalls on every resolve - the dominant
  // performance cost). Set to 0 for performance A/B runs; note the GUESTDUMP
  // .ppm oracle reads stale data when off - judge by the screen only.
  bool readback_resolve_full = true;
  // Disable the guest display refresh cap so the vblank worker can fire at
  // the Android uncapped path rate instead of locking guest pacing to 60Hz
  // divisors.
  bool vblank_uncapped = false;
  // Legacy compat hack: re-tile the frontbuffer guest memory as if the compat
  // copies had written it linear. The resolve path writes tiled data, so this
  // scrambles every presented frame and was the top CPU hotspot (~20%).
  // Default OFF; kept only as a diagnostic switch.
  bool linear_to_tiled_frontbuffer = false;
  // Force the FSI (pixel-shader-interlock) render target path even though the
  // device lacks VK_EXT_fragment_shader_interlock. EDRAM becomes a raw
  // storage buffer (bit-exact format aliasing - what Halo Reach needs);
  // fragment ordering races may cause blending speckle on overlapping
  // geometry. Shader codegen omits interlock ops when the feature is absent.
  bool force_fsi = false;
  // Silence info/debug logs during load and shader-compile storms so logd
  // traffic does not contribute to Android watchdog kills.
  bool quiet_logs = true;
  // Force the swap texture to read this guest base page (0 = no override).
  uint32_t swap_base_page_override = 0;
  // Override the frontbuffer fetch swizzle (12-bit Xenos swizzle, 3 bits per
  // output component, source 0..3 = X,Y,Z,W of the fetched texel; 4=zero,
  // 5=one). Verified on-device 2026-06-12: 0xA42 presents Halo Reach UI in
  // correct colors; 0 uses the game's raw 0xA0A.
  uint32_t swap_swizzle_override = 0xA42;
  // Diagnostic override for DXT-family texture upload endian handling:
  // 0 = fetch constant / current path, 1 = force k8in16, 2 = force none.
  uint32_t tex_endian_mode = 0;
  // Guest addresses dumped as 1152x720 tiled k_8_8_8_8 PPMs at the oracle
  // frames, in addition to the swap texture itself.
  static constexpr uint32_t kMaxDumpAddresses = 8;
  uint32_t dump_addresses[kMaxDumpAddresses] = {0x02354000, 0x02D08000,
                                                0x03044000};
  uint32_t dump_address_count = 3;
};

inline const AndroidHaloExperiment& GetAndroidHaloExperiment() {
  static const AndroidHaloExperiment experiment = [] {
    AndroidHaloExperiment result;
    const char* kPaths[] = {
        "/sdcard/Android/data/jp.xenios.emulator.github.debug/files/"
        "halo_experiment.txt",
        "/sdcard/Android/data/jp.xenios.emulator.github/files/"
        "halo_experiment.txt",
    };
    const char* loaded_path = nullptr;
    for (const char* path : kPaths) {
      std::ifstream file(path);
      if (!file) {
        continue;
      }
      loaded_path = path;
      std::string line;
      while (std::getline(file, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) {
          line.resize(comment);
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
          continue;
        }
        auto trim = [](std::string token) {
          size_t first = token.find_first_not_of(" \t\r\n");
          size_t last = token.find_last_not_of(" \t\r\n");
          if (first == std::string::npos) {
            return std::string();
          }
          return token.substr(first, last - first + 1);
        };
        std::string name = trim(line.substr(0, equals));
        std::string value = trim(line.substr(equals + 1));
        if (name.empty() || value.empty()) {
          continue;
        }
        auto parse_bool = [&value](bool& out) {
          if (value == "1" || value == "true" || value == "on") {
            out = true;
          } else if (value == "0" || value == "false" || value == "off") {
            out = false;
          }
        };
        if (name == "direct_presentable_resolve") {
          parse_bool(result.direct_presentable_resolve);
        } else if (name == "final_resolve_raw_copy") {
          parse_bool(result.final_resolve_raw_copy);
        } else if (name == "writer_gb_fix") {
          parse_bool(result.writer_gb_fix);
        } else if (name == "skip_depth_to_color_alias") {
          parse_bool(result.skip_depth_to_color_alias);
        } else if (name == "shadow_fallback") {
          parse_bool(result.shadow_fallback);
        } else if (name == "force_alpha_pass") {
          parse_bool(result.force_alpha_pass);
        } else if (name == "disable_geometry_shaders") {
          parse_bool(result.disable_geometry_shaders);
        } else if (name == "collapse_msaa_resolve") {
          parse_bool(result.collapse_msaa_resolve);
        } else if (name == "repack_mode") {
          result.repack_mode =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "readback_resolve_full") {
          parse_bool(result.readback_resolve_full);
        } else if (name == "vblank_uncapped") {
          parse_bool(result.vblank_uncapped);
        } else if (name == "linear_to_tiled_frontbuffer") {
          parse_bool(result.linear_to_tiled_frontbuffer);
        } else if (name == "force_fsi") {
          parse_bool(result.force_fsi);
        } else if (name == "quiet_logs") {
          parse_bool(result.quiet_logs);
        } else if (name == "swap_base_page_override") {
          result.swap_base_page_override =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "swap_swizzle_override") {
          result.swap_swizzle_override =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0)) & 0xFFF;
        } else if (name == "tex_endian_mode") {
          result.tex_endian_mode =
              std::min(uint32_t(std::strtoul(value.c_str(), nullptr, 0)),
                       uint32_t(2));
        } else if (name == "dump_addresses") {
          result.dump_address_count = 0;
          const char* cursor = value.c_str();
          while (*cursor && result.dump_address_count <
                                AndroidHaloExperiment::kMaxDumpAddresses) {
            char* end = nullptr;
            unsigned long address = std::strtoul(cursor, &end, 0);
            if (end == cursor) {
              break;
            }
            if (address) {
              result.dump_addresses[result.dump_address_count++] =
                  uint32_t(address);
            }
            cursor = end;
            while (*cursor == ',' || *cursor == ' ') {
              ++cursor;
            }
          }
        }
      }
      break;
    }
    XELOGI(
        "HaloExperiment file={} direct_presentable_resolve={} "
        "final_resolve_raw_copy={} writer_gb_fix={} "
        "skip_depth_to_color_alias={} "
        "shadow_fallback={} force_alpha_pass={} disable_geometry_shaders={} "
        "collapse_msaa_resolve={} repack_mode={} readback_resolve_full={} "
        "vblank_uncapped={} linear_to_tiled_frontbuffer={} force_fsi={} "
        "quiet_logs={} "
        "swap_base_page_override=0x{:X} swap_swizzle_override=0x{:03X} "
        "tex_endian_mode={} "
        "dump_addresses={:#010X},{:#010X},"
        "{:#010X} dump_address_count={}",
        loaded_path ? loaded_path : "none(defaults)",
        uint32_t(result.direct_presentable_resolve),
        uint32_t(result.final_resolve_raw_copy),
        uint32_t(result.writer_gb_fix),
        uint32_t(result.skip_depth_to_color_alias),
        uint32_t(result.shadow_fallback), uint32_t(result.force_alpha_pass),
        uint32_t(result.disable_geometry_shaders),
        uint32_t(result.collapse_msaa_resolve), result.repack_mode,
        uint32_t(result.readback_resolve_full),
        uint32_t(result.vblank_uncapped),
        uint32_t(result.linear_to_tiled_frontbuffer),
        uint32_t(result.force_fsi), uint32_t(result.quiet_logs),
        result.swap_base_page_override,
        result.swap_swizzle_override,
        result.tex_endian_mode,
        result.dump_address_count > 0 ? result.dump_addresses[0] : 0,
        result.dump_address_count > 1 ? result.dump_addresses[1] : 0,
        result.dump_address_count > 2 ? result.dump_addresses[2] : 0,
        result.dump_address_count);
    return result;
  }();
  return experiment;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_ANDROID_HALO_EXPERIMENT_H_
