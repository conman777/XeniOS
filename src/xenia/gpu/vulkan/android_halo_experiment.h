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
#include <atomic>
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
  // Diagnostic A/B: bypass the mixed-format 1x composite and present the live
  // 4x 10-bit scene color target through the normal collapse/dump path.
  bool direct_msaa_scene_resolve = false;
  // Use a byte-exact vkCmdCopyImageToBuffer tile copy for the refresh above
  // instead of the format-converting compute dump shader.
  // Default OFF: raw_copy preserves host channel order that is G/B-swapped
  // relative to guest 8888 on Adreno (live 2026-07-08 dumps). Prefer the
  // compute dump + writer_gb_fix path below for presentable 8888.
  bool final_resolve_raw_copy = false;
  // Swap G/B while packing 8888 into EDRAM for the final-presentable dump.
  // Device A/B (2026-07-08): ON fixes solid blue title/menu panels; raw_copy
  // alone leaves them green in guest RAM. The corrected final refresh applies
  // this while retaining the guest's presentation swizzle.
  bool writer_gb_fix = true;
  // Skip the exact 4x-depth -> 1x-color ownership transfer that clobbers the
  // presentable color span at base 1350 with depth garbage. This also covers
  // fragmented pitch variants overlapping the same presentable span.
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
  // Diagnostic A/B: the EDRAM-dump compute shader treats 64bpp render target
  // tiles as storing 40x16 samples (half the standard 80x16 width) because
  // the true hardware addressing for 64bpp EDRAM tiles is undocumented in
  // this codebase ("exact addressing... is unknown" per the shader-builder
  // comment). When true, flip that guess: halve the tile HEIGHT (80x8)
  // instead of the WIDTH (40x16), keeping the same total byte capacity per
  // tile. Default off (keeps the existing width-halved behavior). Affects
  // both GetDumpPipeline's shader-side tile math and the CPU-side dispatch
  // sizing that must stay consistent with it.
  bool edram_64bpp_tile_height_halved = false;
  // WO25 host-RT bisect + candidate fix. The ownership-transfer/EDRAM-dump
  // read for k_16_16_16_16_FLOAT (and other 16-bit-per-component formats)
  // samples the render target through a VkImageView reinterpreting the image
  // as an *_UINT format (to preserve NaN bit patterns exactly) - the same
  // convention the D3D12 backend uses (DXGI_FORMAT_R16G16B16A16_UINT), which
  // works correctly on PC. When true, this switch skips that reinterpreting
  // view SPECIFICALLY for k_16_16_16_16_FLOAT and instead samples through the
  // render target's native VK_FORMAT_R16G16B16A16_SFLOAT view, bitcasting
  // each fetched float component to uint after extraction (same NaN-safe
  // intent, different Vulkan mechanism) - tests whether Adreno's driver
  // mishandles the reinterpreting view for this specific format/usage
  // combination (UBWC compression metadata mismatch is a plausible cause).
  // Default off (keeps the UINT-reinterpretation path, matching D3D12).
  bool native_float_view_for_16bpc_transfer = false;
  // 1 = value-convert collapsible owners to 8888 before EDRAM packing.
  // 0 = keep the owner's native pack format so raw bits land in EDRAM.
  uint32_t repack_mode = 0;
  // Diagnostic: include k_16_16 fixed-point owners in the Android MSAA
  // collapse/repack path and pack them as visible 8888 instead of raw 16-bit
  // words. Default off until verified on device.
  bool repack_16_16_to_8888 = false;
  // Diagnostic override for Vulkan fixed-point 16-bit render targets.
  // -1 = use the normal cvar/default, 0 = truncate to -1...1, 1 = full -32...32
  // remap. This lives in halo_experiment.txt because the Android profile
  // whitelist doesn't apply snorm16_render_target_full_range.
  int32_t snorm16_render_target_full_range = -1;
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
  // Per-run diagnostic logging switches. These intentionally live in
  // halo_experiment.txt rather than xenios_android_profile.txt because private
  // saved profiles can shadow the pushed external profile on Android.
  bool log_draws = false;
  bool log_texture_bindings = false;
  uint32_t log_texture_binding_skip_draws = 0;
  uint32_t log_texture_binding_max_lines = 1024;
  bool log_owner_history = false;
  bool log_presentable_source_owner = false;
  // WO26 Probe A: log the same per-draw write-mask/blend/shader-hash census
  // as log_draws, but specifically for draws targeting EDRAM base 675 (the
  // 64bpp F16 scene RT), independent of log_draws so it can run without the
  // base-1350 log volume.
  bool log_base675_draw_state = false;
  // Log draws targeting Reach's base-0, pitch-29 RGBA8 menu composition RT.
  // This is independent of the global draw cap, which expires before menus.
  bool log_base0_draw_state = false;
  // WO26 Probe B: override which MSAA sample index GetDumpPipeline's fetch
  // reads for 64bpp 4x MSAA sources, to test whether B/A are alive at a
  // different sample than whatever the default (sample 0) selects.
  // -1 = default/unmodified behavior.
  int32_t dump_msaa_sample_index_override = -1;
  // Force the swap texture to read this guest base page (0 = no override).
  uint32_t swap_base_page_override = 0;
  // Override the frontbuffer fetch swizzle (12-bit Xenos swizzle). Device A/B
  // confirms 0xA42 for Reach menus; the guest's raw 0xA0A is incorrect on the
  // Adreno presentation path.
  uint32_t swap_swizzle_override = 0xA42;
  // Auto-select swizzle: 0xA42 for menus/intro/UI, 0xAC2 during 3D gameplay.
  // Default OFF: auto misclassifies difficulty as gameplay and greens the UI.
  bool swap_swizzle_auto = false;
  uint32_t swap_swizzle_menu = 0xA42;
  uint32_t swap_swizzle_gameplay = 0xAC2;
  // Diagnostic override for DXT-family texture upload endian handling:
  // 0 = fetch constant / current path, 1 = force k8in16, 2 = force none.
  uint32_t tex_endian_mode = 0;
  // Avoid vectorized SSBO reads and writes for direct 32bpp texture uploads.
  // Reach uses format 6 for its LDR scene and menu scratch images.
  bool rgba8_safe_texture_load = false;
  // Replace direct 32bpp uploads with an X/Y gradient and 32-pixel checker.
  bool rgba8_texture_pattern = false;
  // Bind Reach scratch texture sources from shared-memory offset zero and pass
  // the guest base through push constants. This avoids nonzero storage-buffer
  // descriptor offsets on Adreno while preserving the same tiled addresses.
  bool texture_load_absolute_shared_memory_binding = false;
  // Emit a direct full-memory write-to-compute-read barrier immediately before
  // loading Reach's scratch textures from shared memory.
  bool texture_load_immediate_source_barrier = false;
  // Diagnostic A/B for Reach's menu composite resolve. The guest resolves a
  // 1152x720 8888 image from EDRAM base 0 with a 29-tile source pitch, while
  // the visible source is 15 tiles wide. Override only that exact resolve to
  // test whether the diagonal 80x16 tile drift is a source-row pitch issue.
  bool resolve_base0_pitch29_to15 = false;
  // Bypass the compute render-target dump for Reach's exact menu background
  // resolve. The source is a 1x RGBA8 host image at EDRAM base 0 / pitch 29;
  // copy its 15 visible tile columns directly into the corresponding EDRAM
  // buffer rows before the normal guest resolve reads them.
  bool menu_base0_transfer_dump = false;
  // Preserve Reach's base-0 menu color image when a 4x depth target briefly
  // aliases the same EDRAM span. The host-RT backend otherwise converts the
  // depth image back into the persistent 1x RGBA8 owner immediately before
  // the menu resolve, clobbering the color contents with depth-like tiles.
  bool menu_skip_depth_to_color_alias = false;
  // Initialize Reach's 2320-wide base-0 menu composition target from the
  // coherent 1152x720 RGBA8 scene owner at base 675. The normal host-RT
  // ownership transfer reinterprets physical tile ranges across incompatible
  // pitches, placing depth in the top rows and scene color in the bottom rows.
  bool menu_initialize_from_base675 = false;
  // Encode eligible RGBA8 ownership transfers inside the following guest draw
  // pass. This backports the upstream tile-GPU path while retaining the
  // standalone transfer pass as a fallback.
  bool menu_transfer_in_draw_pass = false;
  // Draw-level menu bisects after the base-675 initialization.
  bool menu_skip_base0_rectangle_draw = false;
  bool menu_skip_base0_triangle_fan_draw = false;
  bool menu_skip_base0_triangle_list_draw = false;
  // Force fixed-point RGBA16 textures through the float conversion shader.
  // Leave this disabled when native RGBA16 UNORM sampling and filtering are
  // supported so fixed-point values aren't needlessly re-encoded as float16.
  bool rgba16_fixed_texture_float_fallback = false;
  // Avoid vectorized SSBO reads for fixed-point RGBA16 texture uploads. This
  // works around corruption seen when Adreno samples Reach's tiled scene
  // resolve after the normal uint4-based detile.
  bool rgba16_safe_texture_load = false;
  // Replace the RGBA16 scene upload with an X/Y gradient and 32-pixel checker
  // pattern. This isolates staging-buffer writes, buffer-to-image copy and
  // guest sampling from shared-memory reads and tiled address calculation.
  bool rgba16_texture_pattern = false;
  // Use Vulkan's tightly packed buffer-to-image copy representation for the
  // 1152x720 Reach RGBA16 scene scratch image. Its computed scratch pitch is
  // already identical to the image extent, so explicit row dimensions are
  // redundant and may exercise a broken Adreno copy path.
  bool rgba16_tight_buffer_image_copy = false;
  // Transition the Reach scene scratch image to shader-read immediately after
  // its buffer copy, bypassing deferred barrier batching for this A/B path.
  bool rgba16_immediate_sample_barrier = false;
  // Force point sampling for Reach's reused 0x02354000 scene scratch texture.
  // This A/B applies to both the menu RGBA8 and gameplay RGBA16 incarnations.
  bool scene_scratch_force_point_sampling = false;
  // Force existing DXT/BC texture decompression fallbacks even when the driver
  // advertises native BC sampling support.
  bool force_dxt_fallback = false;
  // Diagnostic: use RGBA8 Vulkan attachments for Xenos 10:10:10:2 color
  // render targets. This mirrors halo_android_diag_force_1010102_rt_as_rgba8
  // without relying on the saved-profile/cvar path.
  bool force_1010102_rt_as_rgba8 = false;
  // Diagnostic A/B for base-1350 render-target ownership transfers.
  bool blit_rt_transfers = false;
  bool log_rt_transfers = false;
  // Diagnostic A/B: skip ColorToColor transfers from a single source RT format
  // into the presentable base-1350 8888 target. -1 disables the skip.
  int32_t skip_presentable_color_transfer_src_fmt = -1;
  // Optional source-format bitmask for combined skip A/Bs. Bit N skips source
  // resource format N; 0 disables the mask.
  uint32_t skip_presentable_color_transfer_src_fmt_mask = 0;
  // Optional refinements for the skip above. Destination format -1 preserves
  // the old behavior of targeting only 8888 destinations, while -2 targets any
  // color destination. Source MSAA 0 means any sample count; otherwise use
  // 1, 2, or 4.
  int32_t skip_presentable_color_transfer_dest_fmt = -1;
  uint32_t skip_presentable_color_transfer_src_msaa = 0;
  // Write sampled 10:10:10:2 values into the base-1350 8888 presentable target
  // with live W in byte1 instead of preserving raw packed guest bits.
  // When true, 1010102/7e3→8888 transfers use StoreAndroidHalo1010102To8888
  // (must preserve G). Default off until the presentable pack is stable;
  // live menus look better without this arming on base-1350 transfers.
  bool value_convert_1010102_to_8888 = false;
  // Diagnostic A/B: value-convert 16-bit fixed/float sources into the base-1350
  // 8888 presentable target instead of copying raw 16-bit words.
  bool value_convert_16bit_to_8888 = false;
  // Guest addresses dumped at the oracle frames, in addition to the swap
  // texture itself. The latest resolve writer overrides the configured bpp
  // because these scratch addresses are reused across formats.
  bool dump_files = true;
  static constexpr uint32_t kMaxDumpAddresses = 8;
  uint32_t dump_addresses[kMaxDumpAddresses] = {0x02354000, 0x02D08000,
                                                0x03044000};
  // Bytes-per-pixel for each dump address: 4 = k_8_8_8_8 (default), 8 =
  // k_16_16_16_16 F16 64bpp. Parsed from an optional "@N" (or ":N") suffix on
  // each dump_addresses entry, e.g. "0x02354000@8". Without a suffix an entry
  // defaults to 4bpp, preserving all existing configs.
  uint32_t dump_address_bpp[kMaxDumpAddresses] = {4, 4, 4, 4, 4, 4, 4, 4};
  uint32_t dump_address_count = 3;
  // WO27 phantom-owner diagnostics: log EDRAM tile ownership transitions
  // (ChangeOwnership) and the pre-resolve owner-map snapshot for tile ranges
  // overlapping [ownership_watch_start_tiles, ownership_watch_end_tiles).
  // Logging only; no functional change.
  bool log_ownership_changes = false;
  bool log_ownership_snapshot = false;
  uint32_t ownership_watch_start_tiles = 675;
  uint32_t ownership_watch_end_tiles = 1350;
  // WO27 fix: 1x 64bpp owners now bypass the source_to_1x collapse/repack
  // (the raw mainline dump is bit-exact for them; the collapse shader's 64bpp
  // path was never validated and demonstrably ran for 1x fmt7 owners). Set to
  // true to restore the old (pre-WO27) behavior for A/B comparison.
  bool legacy_collapse_64bpp_1x = false;
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
        auto parse_tristate_bool = [&value](int32_t& out) {
          if (value == "auto" || value == "-1") {
            out = -1;
          } else if (value == "1" || value == "true" || value == "on") {
            out = 1;
          } else if (value == "0" || value == "false" || value == "off") {
            out = 0;
          }
        };
        if (name == "direct_presentable_resolve") {
          parse_bool(result.direct_presentable_resolve);
        } else if (name == "direct_msaa_scene_resolve") {
          parse_bool(result.direct_msaa_scene_resolve);
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
        } else if (name == "edram_64bpp_tile_height_halved") {
          parse_bool(result.edram_64bpp_tile_height_halved);
        } else if (name == "native_float_view_for_16bpc_transfer") {
          parse_bool(result.native_float_view_for_16bpc_transfer);
        } else if (name == "repack_mode") {
          result.repack_mode =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "repack_16_16_to_8888") {
          parse_bool(result.repack_16_16_to_8888);
        } else if (name == "snorm16_render_target_full_range" ||
                   name == "snorm16_render_target_full_range_override") {
          parse_tristate_bool(result.snorm16_render_target_full_range);
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
        } else if (name == "log_draws" ||
                   name == "halo_android_diag_log_draws") {
          parse_bool(result.log_draws);
        } else if (name == "log_texture_bindings" ||
                   name == "halo_android_diag_log_texture_bindings") {
          parse_bool(result.log_texture_bindings);
        } else if (name == "log_texture_binding_skip_draws") {
          result.log_texture_binding_skip_draws =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "log_texture_binding_max_lines") {
          result.log_texture_binding_max_lines =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "log_owner_history") {
          parse_bool(result.log_owner_history);
        } else if (name == "log_presentable_source_owner") {
          parse_bool(result.log_presentable_source_owner);
        } else if (name == "log_base675_draw_state") {
          parse_bool(result.log_base675_draw_state);
        } else if (name == "log_base0_draw_state") {
          parse_bool(result.log_base0_draw_state);
        } else if (name == "dump_msaa_sample_index_override") {
          result.dump_msaa_sample_index_override =
              int32_t(std::strtol(value.c_str(), nullptr, 0));
        } else if (name == "swap_base_page_override") {
          result.swap_base_page_override =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "swap_swizzle_override") {
          result.swap_swizzle_override =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0)) & 0xFFF;
        } else if (name == "swap_swizzle_auto") {
          parse_bool(result.swap_swizzle_auto);
        } else if (name == "swap_swizzle_menu") {
          result.swap_swizzle_menu =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0)) & 0xFFF;
        } else if (name == "swap_swizzle_gameplay") {
          result.swap_swizzle_gameplay =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0)) & 0xFFF;
        } else if (name == "tex_endian_mode") {
          result.tex_endian_mode =
              std::min(uint32_t(std::strtoul(value.c_str(), nullptr, 0)),
                       uint32_t(2));
        } else if (name == "rgba8_safe_texture_load") {
          parse_bool(result.rgba8_safe_texture_load);
        } else if (name == "rgba8_texture_pattern") {
          parse_bool(result.rgba8_texture_pattern);
        } else if (name == "texture_load_absolute_shared_memory_binding") {
          parse_bool(result.texture_load_absolute_shared_memory_binding);
        } else if (name == "texture_load_immediate_source_barrier") {
          parse_bool(result.texture_load_immediate_source_barrier);
        } else if (name == "resolve_base0_pitch29_to15") {
          parse_bool(result.resolve_base0_pitch29_to15);
        } else if (name == "menu_base0_transfer_dump") {
          parse_bool(result.menu_base0_transfer_dump);
        } else if (name == "menu_skip_depth_to_color_alias") {
          parse_bool(result.menu_skip_depth_to_color_alias);
        } else if (name == "menu_initialize_from_base675") {
          parse_bool(result.menu_initialize_from_base675);
        } else if (name == "menu_transfer_in_draw_pass") {
          parse_bool(result.menu_transfer_in_draw_pass);
        } else if (name == "menu_skip_base0_rectangle_draw") {
          parse_bool(result.menu_skip_base0_rectangle_draw);
        } else if (name == "menu_skip_base0_triangle_fan_draw") {
          parse_bool(result.menu_skip_base0_triangle_fan_draw);
        } else if (name == "menu_skip_base0_triangle_list_draw") {
          parse_bool(result.menu_skip_base0_triangle_list_draw);
        } else if (name == "rgba16_fixed_texture_float_fallback") {
          parse_bool(result.rgba16_fixed_texture_float_fallback);
        } else if (name == "rgba16_safe_texture_load") {
          parse_bool(result.rgba16_safe_texture_load);
        } else if (name == "rgba16_texture_pattern") {
          parse_bool(result.rgba16_texture_pattern);
        } else if (name == "rgba16_tight_buffer_image_copy") {
          parse_bool(result.rgba16_tight_buffer_image_copy);
        } else if (name == "rgba16_immediate_sample_barrier") {
          parse_bool(result.rgba16_immediate_sample_barrier);
        } else if (name == "scene_scratch_force_point_sampling") {
          parse_bool(result.scene_scratch_force_point_sampling);
        } else if (name == "force_dxt_fallback") {
          parse_bool(result.force_dxt_fallback);
        } else if (name == "force_1010102_rt_as_rgba8" ||
                   name == "halo_android_diag_force_1010102_rt_as_rgba8") {
          parse_bool(result.force_1010102_rt_as_rgba8);
        } else if (name == "blit_rt_transfers" ||
                   name == "halo_android_diag_blit_rt_transfers") {
          parse_bool(result.blit_rt_transfers);
        } else if (name == "log_rt_transfers" ||
                   name == "halo_android_diag_log_rt_transfers") {
          parse_bool(result.log_rt_transfers);
        } else if (name == "skip_presentable_color_transfer_src_fmt") {
          result.skip_presentable_color_transfer_src_fmt =
              int32_t(std::strtol(value.c_str(), nullptr, 0));
        } else if (name == "skip_presentable_color_transfer_src_fmt_mask") {
          result.skip_presentable_color_transfer_src_fmt_mask =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "skip_presentable_color_transfer_dest_fmt") {
          result.skip_presentable_color_transfer_dest_fmt =
              int32_t(std::strtol(value.c_str(), nullptr, 0));
        } else if (name == "skip_presentable_color_transfer_src_msaa") {
          result.skip_presentable_color_transfer_src_msaa =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "value_convert_1010102_to_8888" ||
                   name == "value_convert_presentable_1010102_to_8888") {
          parse_bool(result.value_convert_1010102_to_8888);
        } else if (name == "value_convert_16bit_to_8888" ||
                   name == "value_convert_presentable_16bit_to_8888") {
          parse_bool(result.value_convert_16bit_to_8888);
        } else if (name == "legacy_collapse_64bpp_1x") {
          parse_bool(result.legacy_collapse_64bpp_1x);
        } else if (name == "log_ownership_changes") {
          parse_bool(result.log_ownership_changes);
        } else if (name == "log_ownership_snapshot") {
          parse_bool(result.log_ownership_snapshot);
        } else if (name == "ownership_watch_start_tiles") {
          result.ownership_watch_start_tiles =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "ownership_watch_end_tiles") {
          result.ownership_watch_end_tiles =
              uint32_t(std::strtoul(value.c_str(), nullptr, 0));
        } else if (name == "dump_files") {
          parse_bool(result.dump_files);
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
            uint32_t bytes_per_pixel = 4;
            if (*end == '@' || *end == ':') {
              char* bpp_end = nullptr;
              unsigned long parsed_bpp = std::strtoul(end + 1, &bpp_end, 0);
              if (bpp_end != end + 1 && parsed_bpp != 0) {
                bytes_per_pixel = uint32_t(parsed_bpp);
              }
              end = bpp_end;
            }
            if (address) {
              result.dump_address_bpp[result.dump_address_count] =
                  bytes_per_pixel;
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
        "direct_msaa_scene_resolve={} "
        "final_resolve_raw_copy={} writer_gb_fix={} "
        "skip_depth_to_color_alias={} "
        "shadow_fallback={} force_alpha_pass={} disable_geometry_shaders={} "
        "collapse_msaa_resolve={} edram_64bpp_tile_height_halved={} "
        "native_float_view_for_16bpc_transfer={} "
        "repack_mode={} repack_16_16_to_8888={} "
        "snorm16_render_target_full_range_override={} "
        "readback_resolve_full={} "
        "vblank_uncapped={} linear_to_tiled_frontbuffer={} force_fsi={} "
        "quiet_logs={} log_draws={} log_texture_bindings={} "
        "log_texture_binding_skip_draws={} log_texture_binding_max_lines={} "
        "log_owner_history={} log_presentable_source_owner={} "
        "log_base675_draw_state={} dump_msaa_sample_index_override={} "
        "log_base0_draw_state={} "
        "swap_base_page_override=0x{:X} swap_swizzle_override=0x{:03X} "
        "swap_swizzle_auto={} swap_swizzle_menu=0x{:03X} "
        "swap_swizzle_gameplay=0x{:03X} "
        "tex_endian_mode={} rgba8_safe_texture_load={} "
        "rgba8_texture_pattern={} texture_load_absolute_binding={} "
        "texture_load_immediate_source_barrier={} "
        "resolve_base0_pitch29_to15={} "
        "menu_base0_transfer_dump={} "
        "menu_skip_depth_to_color_alias={} "
        "menu_initialize_from_base675={} "
        "menu_transfer_in_draw_pass={} "
        "menu_skip_base0_rectangle_draw={} "
        "menu_skip_base0_triangle_fan_draw={} "
        "menu_skip_base0_triangle_list_draw={} "
        "rgba16_fixed_texture_float_fallback={} "
        "rgba16_safe_texture_load={} rgba16_texture_pattern={} "
        "rgba16_tight_buffer_image_copy={} "
        "rgba16_immediate_sample_barrier={} "
        "scene_scratch_force_point_sampling={} "
        "force_dxt_fallback={} "
        "force_1010102_rt_as_rgba8={} "
        "blit_rt_transfers={} log_rt_transfers={} "
        "skip_presentable_color_transfer_src_fmt={} "
        "skip_presentable_color_transfer_src_fmt_mask=0x{:X} "
        "skip_presentable_color_transfer_dest_fmt={} "
        "skip_presentable_color_transfer_src_msaa={} "
        "value_convert_1010102_to_8888={} "
        "value_convert_16bit_to_8888={} "
        "dump_files={} "
        "dump_addresses={:#010X},{:#010X},"
        "{:#010X} dump_address_count={}",
        loaded_path ? loaded_path : "none(defaults)",
        uint32_t(result.direct_presentable_resolve),
        uint32_t(result.direct_msaa_scene_resolve),
        uint32_t(result.final_resolve_raw_copy),
        uint32_t(result.writer_gb_fix),
        uint32_t(result.skip_depth_to_color_alias),
        uint32_t(result.shadow_fallback), uint32_t(result.force_alpha_pass),
        uint32_t(result.disable_geometry_shaders),
        uint32_t(result.collapse_msaa_resolve),
        uint32_t(result.edram_64bpp_tile_height_halved),
        uint32_t(result.native_float_view_for_16bpc_transfer),
        result.repack_mode,
        uint32_t(result.repack_16_16_to_8888),
        result.snorm16_render_target_full_range,
        uint32_t(result.readback_resolve_full),
        uint32_t(result.vblank_uncapped),
        uint32_t(result.linear_to_tiled_frontbuffer),
        uint32_t(result.force_fsi), uint32_t(result.quiet_logs),
        uint32_t(result.log_draws), uint32_t(result.log_texture_bindings),
        result.log_texture_binding_skip_draws,
        result.log_texture_binding_max_lines,
        uint32_t(result.log_owner_history),
        uint32_t(result.log_presentable_source_owner),
        uint32_t(result.log_base675_draw_state),
        result.dump_msaa_sample_index_override,
        uint32_t(result.log_base0_draw_state),
        result.swap_base_page_override,
        result.swap_swizzle_override,
        uint32_t(result.swap_swizzle_auto),
        result.swap_swizzle_menu,
        result.swap_swizzle_gameplay,
        result.tex_endian_mode,
        uint32_t(result.rgba8_safe_texture_load),
        uint32_t(result.rgba8_texture_pattern),
        uint32_t(result.texture_load_absolute_shared_memory_binding),
        uint32_t(result.texture_load_immediate_source_barrier),
        uint32_t(result.resolve_base0_pitch29_to15),
        uint32_t(result.menu_base0_transfer_dump),
        uint32_t(result.menu_skip_depth_to_color_alias),
        uint32_t(result.menu_initialize_from_base675),
        uint32_t(result.menu_transfer_in_draw_pass),
        uint32_t(result.menu_skip_base0_rectangle_draw),
        uint32_t(result.menu_skip_base0_triangle_fan_draw),
        uint32_t(result.menu_skip_base0_triangle_list_draw),
        uint32_t(result.rgba16_fixed_texture_float_fallback),
        uint32_t(result.rgba16_safe_texture_load),
        uint32_t(result.rgba16_texture_pattern),
        uint32_t(result.rgba16_tight_buffer_image_copy),
        uint32_t(result.rgba16_immediate_sample_barrier),
        uint32_t(result.scene_scratch_force_point_sampling),
        uint32_t(result.force_dxt_fallback),
        uint32_t(result.force_1010102_rt_as_rgba8),
        uint32_t(result.blit_rt_transfers),
        uint32_t(result.log_rt_transfers),
        result.skip_presentable_color_transfer_src_fmt,
        result.skip_presentable_color_transfer_src_fmt_mask,
        result.skip_presentable_color_transfer_dest_fmt,
        result.skip_presentable_color_transfer_src_msaa,
        uint32_t(result.value_convert_1010102_to_8888),
        uint32_t(result.value_convert_16bit_to_8888),
        uint32_t(result.dump_files),
        result.dump_address_count > 0 ? result.dump_addresses[0] : 0,
        result.dump_address_count > 1 ? result.dump_addresses[1] : 0,
        result.dump_address_count > 2 ? result.dump_addresses[2] : 0,
        result.dump_address_count);
    XELOGI(
        "HaloExperiment ownership: log_ownership_changes={} "
        "log_ownership_snapshot={} watch_tiles=[{},{}) "
        "legacy_collapse_64bpp_1x={}",
        uint32_t(result.log_ownership_changes),
        uint32_t(result.log_ownership_snapshot),
        result.ownership_watch_start_tiles, result.ownership_watch_end_tiles,
        uint32_t(result.legacy_collapse_64bpp_1x));
    return result;
  }();
  return experiment;
}

inline std::atomic<uint32_t>& AndroidHaloGameplayPresentFramesRemaining() {
  static std::atomic<uint32_t> frames{0};
  return frames;
}

inline std::atomic<uint32_t>& AndroidHaloGameplayDrawStreak() {
  static std::atomic<uint32_t> streak{0};
  return streak;
}

inline void AndroidHaloMarkGameplayPresentContent(uint32_t hold_frames = 240) {
  uint32_t current = AndroidHaloGameplayPresentFramesRemaining().load();
  if (hold_frames > current) {
    AndroidHaloGameplayPresentFramesRemaining().store(hold_frames);
  }
}

inline std::atomic<uint32_t>& AndroidHaloFramesSinceMenuUi() {
  // Start low so gameplay swizzle cannot latch before the first menu UI frame.
  static std::atomic<uint32_t> frames{0};
  return frames;
}

// Tracks the most recent resolve write seen at a small fixed set of guest
// addresses (the CPU-swap dump targets). RESOLVEMAP logging only records the
// first time a (dest, format, ...) combination is seen (deduped, capped at
// 3000 entries), so it cannot answer "what was the last thing written here
// right before this checkpoint dump fired" once a run has been going long
// enough for all combos to already be logged. These addresses are shared
// scratch buffers reused by resolves of DIFFERENT formats over the course of
// a run (e.g. 0x02354000 has been observed as both fmt=6 8888 32bpp and
// fmt=26 k_16_16_16_16 64bpp at different times) - a dump that blindly
// assumes a fixed bpp for one of these addresses can misinterpret stale or
// differently-formatted data as the wrong pixel format.
struct AndroidHaloLastWriterInfo {
  std::atomic<uint32_t> format{0xFFFFFFFFu};  // sentinel: never observed
  std::atomic<uint32_t> endian{0};
  std::atomic<uint32_t> edram_base{0};
  std::atomic<uint32_t> pitch_tiles{0};
  std::atomic<uint32_t> msaa{0};
  std::atomic<uint32_t> bpp{0};
  std::atomic<uint64_t> write_sequence{0};
};

inline AndroidHaloLastWriterInfo* AndroidHaloLastWriterForAddress(
    uint32_t guest_address) {
  static constexpr uint32_t kTrackedAddresses[3] = {0x03044000, 0x02354000,
                                                      0x02018000};
  static AndroidHaloLastWriterInfo trackers[3];
  for (uint32_t i = 0; i < 3; ++i) {
    if (kTrackedAddresses[i] == guest_address) {
      return &trackers[i];
    }
  }
  return nullptr;
}

inline void AndroidHaloRecordResolveWrite(uint32_t guest_address,
                                           uint32_t format,
                                           uint32_t endian,
                                           uint32_t edram_base,
                                           uint32_t pitch_tiles, uint32_t msaa,
                                           uint32_t bpp) {
  AndroidHaloLastWriterInfo* writer =
      AndroidHaloLastWriterForAddress(guest_address);
  if (!writer) {
    return;
  }
  writer->format.store(format, std::memory_order_relaxed);
  writer->endian.store(endian, std::memory_order_relaxed);
  writer->edram_base.store(edram_base, std::memory_order_relaxed);
  writer->pitch_tiles.store(pitch_tiles, std::memory_order_relaxed);
  writer->msaa.store(msaa, std::memory_order_relaxed);
  writer->bpp.store(bpp, std::memory_order_relaxed);
  writer->write_sequence.fetch_add(1, std::memory_order_relaxed);
}

inline void AndroidHaloClearGameplayPresentContent() {
  AndroidHaloGameplayPresentFramesRemaining().store(0);
  AndroidHaloGameplayDrawStreak().store(0);
}

inline void AndroidHaloNotifyMsaaSceneDrawFrame(bool msaa_scene_draw) {
  if (msaa_scene_draw) {
    const uint32_t streak = AndroidHaloGameplayDrawStreak().fetch_add(1) + 1;
    if (streak >= 60) {
      AndroidHaloMarkGameplayPresentContent(480);
      static std::atomic<uint32_t> gameplay_swizzle_log_count{0};
      if (gameplay_swizzle_log_count.fetch_add(1) < 8) {
        XELOGI("HaloCompat auto_swizzle gameplay latched streak={}", streak);
      }
    }
  } else if (AndroidHaloGameplayPresentFramesRemaining().load() == 0) {
    AndroidHaloGameplayDrawStreak().store(0);
  }
}

inline void AndroidHaloTickGameplayPresentCounter() {
  AndroidHaloFramesSinceMenuUi().fetch_add(1);
  uint32_t current = AndroidHaloGameplayPresentFramesRemaining().load();
  if (current) {
    AndroidHaloGameplayPresentFramesRemaining().store(current - 1);
  }
}

inline bool AndroidHaloUseGameplaySwizzle() {
  // Do not switch to gameplay swizzle during menus/intro UI.
  return AndroidHaloGameplayPresentFramesRemaining().load() > 0 &&
         AndroidHaloFramesSinceMenuUi().load() > 90;
}

inline uint32_t AndroidHaloGetEffectiveSwapSwizzle(uint32_t fetch_swizzle) {
  const AndroidHaloExperiment& experiment = GetAndroidHaloExperiment();
  if (experiment.swap_swizzle_override) {
    return experiment.swap_swizzle_override;
  }
  if (experiment.swap_swizzle_auto) {
    return AndroidHaloUseGameplaySwizzle() ? experiment.swap_swizzle_gameplay
                                           : experiment.swap_swizzle_menu;
  }
  return fetch_swizzle;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_ANDROID_HALO_EXPERIMENT_H_
