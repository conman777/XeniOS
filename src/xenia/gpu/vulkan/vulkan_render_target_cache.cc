/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_render_target_cache.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "third_party/glslang/SPIRV/GLSL.std.450.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_builder.h"
#include "xenia/gpu/spirv_compatibility.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_cache.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#if XE_PLATFORM_ANDROID
#include "xenia/gpu/vulkan/android_diagnostic_state.h"
#include "xenia/gpu/vulkan/android_halo_experiment.h"
#include "xenia/gpu/vulkan/android_halo_reclaim_state.h"
#endif
#include "xenia/gpu/xenos.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DEFINE_string(
    render_target_path_vulkan, "",
    "Render target emulation path to use on Vulkan.\n"
    "Use: [any, fbo, fsi]\n"
    " fbo:\n"
    "  Host framebuffers and fixed-function blending and depth / stencil "
    "testing, copying between render targets when needed.\n"
    "  Lower accuracy (limited pixel format support).\n"
    "  Performance limited primarily by render target layout changes requiring "
    "copying, but generally higher.\n"
    " fsi:\n"
    "  Manual pixel packing, blending and depth / stencil testing, with free "
    "render target layout changes.\n"
    "  Requires a GPU supporting fragment shader interlock.\n"
    "  Highest accuracy (all pixel formats handled in software).\n"
    "  Performance limited primarily by overdraw.\n"
    " Any other value:\n"
    "  Choose what is considered the most optimal for the system (currently "
    "always FB because the FSI path is much slower now).",
    "GPU");

DEFINE_uint32(
    vulkan_render_target_memory_limit_mb, 0,
    "Soft limit in megabytes for cached Vulkan host render target images. "
    "Zero disables automatic cache reclamation.",
    "Vulkan");

DEFINE_bool(halo_android_diag_strict_barriers, false,
            "Use broad Vulkan barriers around Android Halo resolve diagnostic "
            "handoffs.",
            "Android");
DEFINE_bool(halo_android_diag_log_resolve_constants, false,
            "Log detailed Android Halo resolve constants and derived pitch "
            "values.",
            "Android");
DEFINE_bool(halo_android_diag_force_full_32bpp_non80, false,
            "Route non-80-pixel-aligned Android Halo 32bpp fast resolves "
            "through the full 32bpp resolve shader.",
            "Android");
DEFINE_bool(halo_android_diag_synthetic_edram_fill, false,
            "Overwrite the Android Halo 1152x720 EDRAM resolve span with a "
            "known tiled 32bpp pattern after DumpRenderTargets.",
            "Android");
DEFINE_bool(halo_android_diag_dump_shader_pattern, false,
            "Make the Android Halo DumpRenderTargets shader write a generated "
            "coordinate pattern instead of sampling the host render target.",
            "Android");
DEFINE_bool(halo_android_diag_transfer_dump_8bpp, false,
            "Copy Android Halo 32bpp color render targets to EDRAM with "
            "vkCmdCopyImageToBuffer instead of the dump shader.",
            "Android");
DEFINE_bool(halo_android_diag_force_1010102_rt_as_rgba8, false,
            "Use RGBA8 host render targets for Android Halo 10:10:10:2 "
            "color formats as an Adreno diagnostic fallback.",
            "Android");
DEFINE_bool(halo_android_diag_blit_rt_transfers, false,
            "Use vkCmdBlitImage instead of the render-target ownership "
            "transfer shader for matching Android Halo color RT transfers.",
            "Android");
DEFINE_bool(halo_android_diag_log_rt_transfers, false,
            "Log Android Halo render-target ownership transfer source and "
            "destination details.",
            "Android");
DEFINE_bool(halo_android_diag_skip_rt_transfers_to_base_1350, false,
            "Skip Android Halo render-target ownership transfers into EDRAM "
            "base tile 1350.",
            "Android");
DEFINE_bool(halo_android_diag_skip_resolve_clear_to_base_1350, false,
            "Skip Android Halo resolve clears targeting EDRAM base tile 1350.",
            "Android");
DEFINE_bool(halo_android_diag_depth_to_color_pattern, false,
            "Write a deterministic destination-only pattern for Android Halo "
            "4x depth to 1x color ownership transfers.",
            "Android");
DEFINE_bool(halo_android_diag_depth_to_color_zero_stencil, false,
            "Force stencil to zero for Android Halo 4x depth to 1x color "
            "ownership transfers.",
            "Android");
DEFINE_int32(halo_android_diag_depth_to_color_sample_mode, 0,
             "Override Android Halo 4x depth to 1x color source sample "
             "mapping: 0 normal, 1 swap xy bits, 2 flip x bit, 3 flip y bit, "
             "4..7 force sample 0..3.",
             "Android");
DEFINE_int32(
    halo_android_diag_depth_to_color_mode, 0,
    "Android Halo 4x depth to 1x color diagnostic mode: 0 normal, "
    "1 no-source pattern, 2 constant packed output, 3 disable depth/color "
    "column swap, 4 depth-only stencil zero, 5 stencil-only constant depth, "
    "6 sample mapping override, 8 dump source depth directly to EDRAM, "
    "9 allow only the first matching ownership transfer, "
    "10 skip the first N matching transfers then run normally.",
    "Android");
DEFINE_bool(halo_android_diag_force_d32s8_depth_format, false,
            "Force VK_FORMAT_D32_SFLOAT_S8_UINT for D24S8 depth render "
            "targets on Android as an Adreno diagnostic.",
            "Android");
DEFINE_bool(halo_android_compat_presentable_color_shadow, true,
            "Android Adreno Halo Reach compatibility path that quarantines "
            "base-1350 DepthToColor aliases from final presentation and "
            "resolves from the last presentable color EDRAM shadow.",
            "Android");
DEFINE_bool(halo_android_compat_skip_depth_to_color_alias, true,
            "Skip Android Halo Reach 4x depth -> 1x color ownership transfers "
            "that alias the presentable 1152x720 EDRAM span at base tile 1350.",
            "Android");
DEFINE_bool(halo_android_compat_direct_presentable_resolve, true,
            "Before each final 1152x720 frontbuffer resolve on Android, "
            "refresh EDRAM base tile 1350 directly from the tracked "
            "presentable host color render target.",
            "Android");
DEFINE_bool(halo_android_compat_linear_to_tiled_frontbuffer, true,
            "Convert linear 1152x720 frontbuffer guest memory into the tiled "
            "layout expected by Xbox 360 texture fetches on Android.",
            "Android");

namespace xe {
namespace gpu {
namespace vulkan {

namespace {

spv::Id ConvertFloat32To7e3ForRenderTarget(
    SpirvBuilder& builder, spv::Id f32_scalar,
    spv::Id ext_inst_glsl_std_450, bool source_is_scaled_unorm) {
  if (source_is_scaled_unorm) {
    f32_scalar = builder.createBinOp(
        spv::OpFMul, builder.makeFloatType(32), f32_scalar,
        builder.makeFloatConstant(31.875f));
  }
#if XE_PLATFORM_ANDROID
  if (GetAndroidHaloExperiment().arithmetic_7e3_conversion) {
    return SpirvShaderTranslator::UnclampedFloat32To7e3Arithmetic(
        builder, f32_scalar, ext_inst_glsl_std_450);
  }
#endif
  return SpirvShaderTranslator::UnclampedFloat32To7e3(
      builder, f32_scalar, ext_inst_glsl_std_450);
}

spv::Id Convert7e3ToFloat32ForRenderTarget(
    SpirvBuilder& builder, spv::Id f10_uint_scalar, uint32_t f10_shift,
    bool result_as_uint, spv::Id ext_inst_glsl_std_450,
    bool result_is_scaled_unorm) {
  spv::Id result;
#if XE_PLATFORM_ANDROID
  if (GetAndroidHaloExperiment().arithmetic_7e3_conversion) {
    result = SpirvShaderTranslator::Float7e3To32Arithmetic(
        builder, f10_uint_scalar, f10_shift, result_as_uint);
  } else {
    result = SpirvShaderTranslator::Float7e3To32(
        builder, f10_uint_scalar, f10_shift, result_as_uint,
        ext_inst_glsl_std_450);
  }
#else
  result = SpirvShaderTranslator::Float7e3To32(
      builder, f10_uint_scalar, f10_shift, result_as_uint,
      ext_inst_glsl_std_450);
#endif
  if (result_is_scaled_unorm) {
    assert_false(result_as_uint);
    result = builder.createBinOp(spv::OpFMul, builder.makeFloatType(32), result,
                                 builder.makeFloatConstant(1.0f / 31.875f));
  }
  return result;
}

}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_1xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/host_depth_store_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/passthrough_position_xy_vs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_clear_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_32bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_1x2xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_4xmsaa_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_fast_64bpp_4xmsaa_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_128bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_16bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_32bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_64bpp_scaled_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_cs.h"
#include "xenia/gpu/shaders/bytecode/vulkan_spirv/resolve_full_8bpp_scaled_cs.h"
}  // namespace shaders

const VulkanRenderTargetCache::ResolveCopyShaderCode
    VulkanRenderTargetCache::kResolveCopyShaders[size_t(
        draw_util::ResolveCopyShaderIndex::kCount)] = {
        {shaders::resolve_fast_32bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_cs),
         shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_32bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_cs),
         shaders::resolve_fast_32bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_32bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_1x2xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_cs),
         shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_1x2xmsaa_scaled_cs)},
        {shaders::resolve_fast_64bpp_4xmsaa_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_cs),
         shaders::resolve_fast_64bpp_4xmsaa_scaled_cs,
         sizeof(shaders::resolve_fast_64bpp_4xmsaa_scaled_cs)},
        {shaders::resolve_full_8bpp_cs, sizeof(shaders::resolve_full_8bpp_cs),
         shaders::resolve_full_8bpp_scaled_cs,
         sizeof(shaders::resolve_full_8bpp_scaled_cs)},
        {shaders::resolve_full_16bpp_cs, sizeof(shaders::resolve_full_16bpp_cs),
         shaders::resolve_full_16bpp_scaled_cs,
         sizeof(shaders::resolve_full_16bpp_scaled_cs)},
        {shaders::resolve_full_32bpp_cs, sizeof(shaders::resolve_full_32bpp_cs),
         shaders::resolve_full_32bpp_scaled_cs,
         sizeof(shaders::resolve_full_32bpp_scaled_cs)},
        {shaders::resolve_full_64bpp_cs, sizeof(shaders::resolve_full_64bpp_cs),
         shaders::resolve_full_64bpp_scaled_cs,
         sizeof(shaders::resolve_full_64bpp_scaled_cs)},
        {shaders::resolve_full_128bpp_cs,
         sizeof(shaders::resolve_full_128bpp_cs),
         shaders::resolve_full_128bpp_scaled_cs,
         sizeof(shaders::resolve_full_128bpp_scaled_cs)},
};

const VulkanRenderTargetCache::TransferPipelineLayoutInfo
    VulkanRenderTargetCache::kTransferPipelineLayoutInfos[size_t(
        TransferPipelineLayoutIndex::kCount)] = {
        // kColor
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kDepth
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit},
        // kColorToStencilBit
        {kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordAddressBit |
             kTransferUsedPushConstantDwordStencilMaskBit},
        // kDepthToStencilBit
        {kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordAddressBit |
             kTransferUsedPushConstantDwordStencilMaskBit},
        // kColorAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kColorAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetColorTextureBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthTexture
        {kTransferUsedDescriptorSetHostDepthStencilTexturesBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
        // kDepthAndHostDepthBuffer
        {kTransferUsedDescriptorSetHostDepthBufferBit |
             kTransferUsedDescriptorSetDepthStencilTexturesBit,
         kTransferUsedPushConstantDwordHostDepthAddressBit |
             kTransferUsedPushConstantDwordAddressBit},
};

const VulkanRenderTargetCache::TransferModeInfo
    VulkanRenderTargetCache::kTransferModes[size_t(TransferMode::kCount)] = {
        // kColorToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kColor},
        // kColorToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kColor},
        // kDepthToDepth
        {TransferOutput::kDepth, TransferPipelineLayoutIndex::kDepth},
        // kDepthToColor
        {TransferOutput::kColor, TransferPipelineLayoutIndex::kDepth},
        // kColorToStencilBit
        {TransferOutput::kStencilBit,
         TransferPipelineLayoutIndex::kColorToStencilBit},
        // kDepthToStencilBit
        {TransferOutput::kStencilBit,
         TransferPipelineLayoutIndex::kDepthToStencilBit},
        // kColorAndHostDepthToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kColorAndHostDepthTexture},
        // kDepthAndHostDepthToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kDepthAndHostDepthTexture},
        // kColorAndHostDepthCopyToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kColorAndHostDepthBuffer},
        // kDepthAndHostDepthCopyToDepth
        {TransferOutput::kDepth,
         TransferPipelineLayoutIndex::kDepthAndHostDepthBuffer},
};

VulkanRenderTargetCache::VulkanRenderTargetCache(
    const RegisterFile& register_file, const Memory& memory,
    TraceWriter& trace_writer, uint32_t draw_resolution_scale_x,
    uint32_t draw_resolution_scale_y, VulkanCommandProcessor& command_processor)
    : RenderTargetCache(register_file, memory, &trace_writer,
                        draw_resolution_scale_x, draw_resolution_scale_y),
      command_processor_(command_processor),
      trace_writer_(trace_writer) {}

VulkanRenderTargetCache::~VulkanRenderTargetCache() { Shutdown(true); }

bool VulkanRenderTargetCache::Initialize(uint32_t shared_memory_binding_count) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanInstance::Functions& ifn =
      vulkan_device->vulkan_instance()->functions();
  const VkPhysicalDevice physical_device = vulkan_device->physical_device();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

#if XE_PLATFORM_ANDROID
  // Legacy alias for requesting FSI. Capability checks still apply.
  const bool force_fsi_experiment = GetAndroidHaloExperiment().force_fsi;
#else
  constexpr bool force_fsi_experiment = false;
#endif
  const bool requested_fsi =
      cvars::render_target_path_vulkan == "fsi" || force_fsi_experiment;
  if (requested_fsi) {
    path_ = Path::kPixelShaderInterlock;
  } else {
    path_ = Path::kHostRenderTargets;
  }
#if XE_PLATFORM_ANDROID
  XELOGI(
      "Android Vulkan RT path request: render_target_path={} "
      "render_target_path_vulkan={} requested_fsi={} shared_memory_bindings={}",
      cvars::render_target_path, cvars::render_target_path_vulkan,
      uint32_t(requested_fsi), shared_memory_binding_count);
#endif
  // Fragment shader interlock is a feature implemented by pretty advanced GPUs,
  // closer to Direct3D 11 / OpenGL ES 3.2 level mainly, not Direct3D 10 /
  // OpenGL ES 3.1. Thus, it's fine to demand a wide range of other optional
  // features for the fragment shader interlock backend to work.
  if (path_ == Path::kPixelShaderInterlock) {
    // Interlocking between fragments with common sample coverage is enough, but
    // interlocking more is acceptable too (fragmentShaderShadingRateInterlock
    // would be okay too, but it's unlikely that an implementation would
    // advertise only it and not any other ones, as it's a very specific feature
    // interacting with another optional feature that is variable shading rate,
    // so there's no need to overcomplicate the checks and the shader execution
    // mode setting).
    // Sample-rate shading is required by certain SPIR-V revisions to access the
    // sample mask fragment shader input.
    // Stanard sample locations are needed for calculating the depth at the
    // samples.
    // It's unlikely that a device exposing fragment shader interlock won't have
    // a large enough storage buffer range and a sufficient SSBO slot count for
    // all the shared memory buffers and the EDRAM buffer - an in a conflict
    // between, for instance, the ability to vfetch and memexport in fragment
    // shaders, and the usage of fragment shader interlock, prefer the former
    // for simplicity.
    if (!(device_properties.fragmentShaderSampleInterlock ||
          device_properties.fragmentShaderPixelInterlock) ||
        !device_properties.fragmentStoresAndAtomics ||
        !device_properties.sampleRateShading ||
        !device_properties.standardSampleLocations ||
        shared_memory_binding_count >=
            device_properties.maxPerStageDescriptorStorageBuffers) {
#if XE_PLATFORM_ANDROID
      XELOGW(
          "Android Vulkan RT path fallback fsi->fbo: "
          "fragment_sample_interlock={} fragment_pixel_interlock={} "
          "fragment_stores_atomics={} sample_rate_shading={} "
          "standard_sample_locations={} shared_memory_bindings={} "
          "max_stage_storage_buffers={}",
          uint32_t(device_properties.fragmentShaderSampleInterlock),
          uint32_t(device_properties.fragmentShaderPixelInterlock),
          uint32_t(device_properties.fragmentStoresAndAtomics),
          uint32_t(device_properties.sampleRateShading),
          uint32_t(device_properties.standardSampleLocations),
          shared_memory_binding_count,
          device_properties.maxPerStageDescriptorStorageBuffers);
#endif
      path_ = Path::kHostRenderTargets;
    }
  }
#if XE_PLATFORM_ANDROID
  XELOGI(
      "Android Vulkan RT path selected: final={} api={}.{}.{} device={} "
      "fragment_sample_interlock={} fragment_pixel_interlock={} "
      "vertex_stores_atomics={} fragment_stores_atomics={} "
      "sample_rate_shading={} standard_sample_locations={} "
      "shared_memory_bindings={} max_stage_storage_buffers={} "
      "max_fragment_combined_outputs={}",
      path_ == Path::kPixelShaderInterlock ? "fsi" : "fbo",
      VK_API_VERSION_MAJOR(device_properties.apiVersion),
      VK_API_VERSION_MINOR(device_properties.apiVersion),
      VK_API_VERSION_PATCH(device_properties.apiVersion),
      device_properties.deviceName,
      uint32_t(device_properties.fragmentShaderSampleInterlock),
      uint32_t(device_properties.fragmentShaderPixelInterlock),
      uint32_t(device_properties.vertexPipelineStoresAndAtomics),
      uint32_t(device_properties.fragmentStoresAndAtomics),
      uint32_t(device_properties.sampleRateShading),
      uint32_t(device_properties.standardSampleLocations),
      shared_memory_binding_count,
      device_properties.maxPerStageDescriptorStorageBuffers,
      device_properties.maxFragmentCombinedOutputResources);
  AndroidDiagnosticRenderState::Get().SetRenderer(
      requested_fsi,
      path_ == Path::kPixelShaderInterlock
          ? AndroidDiagnosticRenderTargetPath::kFragmentShaderInterlock
          : AndroidDiagnosticRenderTargetPath::kHostRenderTargets,
      device_properties.fragmentShaderSampleInterlock,
      device_properties.fragmentShaderPixelInterlock);
#endif

  // Format support.
  constexpr VkFormatFeatureFlags kUsedDepthFormatFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
  VkFormatProperties depth_unorm24_properties;
  ifn.vkGetPhysicalDeviceFormatProperties(
      physical_device, VK_FORMAT_D24_UNORM_S8_UINT, &depth_unorm24_properties);
  depth_unorm24_vulkan_format_supported_ =
      (depth_unorm24_properties.optimalTilingFeatures &
       kUsedDepthFormatFeatures) == kUsedDepthFormatFeatures;
#if XE_PLATFORM_ANDROID
  if (cvars::halo_android_diag_force_d32s8_depth_format) {
    XELOGI(
        "Android forcing D24S8 render targets to D32S8 diagnostic format; "
        "D24S8 optimal features=0x{:08X}",
        uint32_t(depth_unorm24_properties.optimalTilingFeatures));
    depth_unorm24_vulkan_format_supported_ = false;
  }
  XELOGI(
      "Android Vulkan depth format caps: D24S8 optimal=0x{:08X} "
      "sampledDepth=0x{:X} sampledStencil=0x{:X} sampledInteger=0x{:X} "
      "framebufferDepth=0x{:X} framebufferStencil=0x{:X}",
      uint32_t(depth_unorm24_properties.optimalTilingFeatures),
      uint32_t(device_properties.sampledImageDepthSampleCounts),
      uint32_t(device_properties.sampledImageStencilSampleCounts),
      uint32_t(device_properties.sampledImageIntegerSampleCounts),
      uint32_t(device_properties.framebufferDepthSampleCounts),
      uint32_t(device_properties.framebufferStencilSampleCounts));
#endif

  // SNORM16 color attachment support is optional in Vulkan; if absent (as on
  // some mobile drivers), guest k_16_16 / k_16_16_16_16 render targets would
  // silently receive no fragment output. Probe and fall back to float16.
  {
    constexpr VkFormatFeatureFlags kUsedColorFormatFeatures =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
#if XE_PLATFORM_ANDROID
    VkFormatProperties color_unorm10_properties, color_unorm16_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        &color_unorm10_properties);
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_R16G16B16A16_UNORM,
        &color_unorm16_properties);
    const bool color_unorm10_supported =
        (color_unorm10_properties.optimalTilingFeatures &
         kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
    const bool color_unorm16_supported =
        (color_unorm16_properties.optimalTilingFeatures &
         kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
    const uint32_t requested_scaled_unorm_mode =
        GetAndroidHaloExperiment().scaled_unorm_7e3_render_targets;
    if (GetPath() == Path::kHostRenderTargets &&
        ((requested_scaled_unorm_mode == 1 && color_unorm10_supported) ||
         (requested_scaled_unorm_mode == 2 && color_unorm16_supported))) {
      scaled_unorm_7e3_render_target_mode_ = requested_scaled_unorm_mode;
    }
    XELOGI(
        "HaloCompat 7e3 host storage: requested={} effective={} "
        "required_features=0x{:08X} A2B10G10R10_UNORM=0x{:08X} "
        "RGBA16_UNORM=0x{:08X} float_fallback={}",
        requested_scaled_unorm_mode,
        scaled_unorm_7e3_render_target_mode_,
        uint32_t(kUsedColorFormatFeatures),
        uint32_t(color_unorm10_properties.optimalTilingFeatures),
        uint32_t(color_unorm16_properties.optimalTilingFeatures),
        uint32_t(scaled_unorm_7e3_render_target_mode_ == 0));
#endif
    VkFormatProperties color_snorm16_properties_rg, color_snorm16_properties;
    ifn.vkGetPhysicalDeviceFormatProperties(
        physical_device, VK_FORMAT_R16G16_SNORM, &color_snorm16_properties_rg);
    ifn.vkGetPhysicalDeviceFormatProperties(physical_device,
                                            VK_FORMAT_R16G16B16A16_SNORM,
                                            &color_snorm16_properties);
    snorm16_color_attachments_supported_ =
        (color_snorm16_properties_rg.optimalTilingFeatures &
         kUsedColorFormatFeatures) == kUsedColorFormatFeatures &&
        (color_snorm16_properties.optimalTilingFeatures &
         kUsedColorFormatFeatures) == kUsedColorFormatFeatures;
    XELOGI(
        "Vulkan color attachment caps: R16G16_SNORM=0x{:08X} "
        "R16G16B16A16_SNORM=0x{:08X} snorm16_attachments_supported={}",
        uint32_t(color_snorm16_properties_rg.optimalTilingFeatures),
        uint32_t(color_snorm16_properties.optimalTilingFeatures),
        uint32_t(snorm16_color_attachments_supported_));
    XELOGI(
        "HaloCompat snorm16 mode: cvar_full_range={} override={} "
        "effective_full_range={} truncated_rg={} truncated_rgba={}",
        uint32_t(cvars::snorm16_render_target_full_range),
        GetAndroidHaloExperiment().snorm16_render_target_full_range,
        uint32_t(IsSnorm16RenderTargetFullRangeEnabled()),
        uint32_t(IsFixedRG16TruncatedToMinus1To1()),
        uint32_t(IsFixedRGBA16TruncatedToMinus1To1()));
  }

  // 2x MSAA support.
  // TODO(Triang3l): Handle sampledImageIntegerSampleCounts 4 not supported in
  // transfers.
  if (cvars::native_2x_msaa) {
    // Multisampled integer sampled images are optional in Vulkan and in Xenia.
    msaa_2x_attachments_supported_ =
        (device_properties.framebufferColorSampleCounts &
         device_properties.framebufferDepthSampleCounts &
         device_properties.framebufferStencilSampleCounts &
         device_properties.sampledImageColorSampleCounts &
         device_properties.sampledImageDepthSampleCounts &
         device_properties.sampledImageStencilSampleCounts &
         VK_SAMPLE_COUNT_2_BIT) &&
        (device_properties.sampledImageIntegerSampleCounts &
         (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT)) !=
            VK_SAMPLE_COUNT_4_BIT;
    msaa_2x_no_attachments_supported_ =
        (device_properties.framebufferNoAttachmentsSampleCounts &
         VK_SAMPLE_COUNT_2_BIT) != 0;
  } else {
    msaa_2x_attachments_supported_ = false;
    msaa_2x_no_attachments_supported_ = false;
  }

  // Descriptor set layouts.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings[2];
  descriptor_set_layout_bindings[0].binding = 0;
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_bindings[0].descriptorCount = 1;
  descriptor_set_layout_bindings[0].stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_storage_buffer_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one storage buffer");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with one sampled image");
    Shutdown();
    return false;
  }
  descriptor_set_layout_bindings[1].binding = 1;
  descriptor_set_layout_bindings[1].descriptorType =
      VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_bindings[1].descriptorCount = 1;
  descriptor_set_layout_bindings[1].stageFlags =
      descriptor_set_layout_bindings[0].stageFlags;
  descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 2;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_sampled_image_x2_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the descriptor set layout "
        "with two sampled images");
    Shutdown();
    return false;
  }

  // Descriptor set pools.
  // The pool sizes were chosen without a specific reason.
  VkDescriptorPoolSize descriptor_set_layout_size;
  descriptor_set_layout_size.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_layout_size.descriptorCount = 1;
  descriptor_set_pool_sampled_image_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_);
  descriptor_set_layout_size.descriptorCount = 2;
  descriptor_set_pool_sampled_image_x2_ =
      std::make_unique<ui::vulkan::SingleLayoutDescriptorSetPool>(
          vulkan_device, 256, 1, &descriptor_set_layout_size,
          descriptor_set_layout_sampled_image_x2_);

  // EDRAM contents reinterpretation buffer.
  // 90 MB with 9x resolution scaling - within the minimum
  // maxStorageBufferRange.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device,
          VkDeviceSize(xenos::kEdramSizeBytes *
                       (draw_resolution_scale_x() * draw_resolution_scale_y())),
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, edram_buffer_,
          edram_buffer_memory_)) {
    XELOGE("VulkanRenderTargetCache: Failed to create the EDRAM buffer");
    Shutdown();
    return false;
  }
  if (GetPath() == Path::kPixelShaderInterlock) {
    // The first operation will likely be drawing.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentReadWrite;
  } else {
    // The first operation will likely be depth self-comparison.
    edram_buffer_usage_ = EdramBufferUsage::kFragmentRead;
  }
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  VkDescriptorPoolSize edram_storage_buffer_descriptor_pool_size;
  edram_storage_buffer_descriptor_pool_size.type =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_pool_size.descriptorCount = 1;
  VkDescriptorPoolCreateInfo edram_storage_buffer_descriptor_pool_create_info;
  edram_storage_buffer_descriptor_pool_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  edram_storage_buffer_descriptor_pool_create_info.pNext = nullptr;
  edram_storage_buffer_descriptor_pool_create_info.flags = 0;
  edram_storage_buffer_descriptor_pool_create_info.maxSets = 1;
  edram_storage_buffer_descriptor_pool_create_info.poolSizeCount = 1;
  edram_storage_buffer_descriptor_pool_create_info.pPoolSizes =
      &edram_storage_buffer_descriptor_pool_size;
  if (dfn.vkCreateDescriptorPool(
          device, &edram_storage_buffer_descriptor_pool_create_info, nullptr,
          &edram_storage_buffer_descriptor_pool_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the EDRAM buffer storage "
        "buffer descriptor pool");
    Shutdown();
    return false;
  }
  VkDescriptorSetAllocateInfo edram_storage_buffer_descriptor_set_allocate_info;
  edram_storage_buffer_descriptor_set_allocate_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  edram_storage_buffer_descriptor_set_allocate_info.pNext = nullptr;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorPool =
      edram_storage_buffer_descriptor_pool_;
  edram_storage_buffer_descriptor_set_allocate_info.descriptorSetCount = 1;
  edram_storage_buffer_descriptor_set_allocate_info.pSetLayouts =
      &descriptor_set_layout_storage_buffer_;
  if (dfn.vkAllocateDescriptorSets(
          device, &edram_storage_buffer_descriptor_set_allocate_info,
          &edram_storage_buffer_descriptor_set_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to allocate the EDRAM buffer storage "
        "buffer descriptor set");
    Shutdown();
    return false;
  }
  VkDescriptorBufferInfo edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_buffer_info.buffer = edram_buffer_;
  edram_storage_buffer_descriptor_buffer_info.offset = 0;
  edram_storage_buffer_descriptor_buffer_info.range = VK_WHOLE_SIZE;
  VkWriteDescriptorSet edram_storage_buffer_descriptor_write;
  edram_storage_buffer_descriptor_write.sType =
      VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  edram_storage_buffer_descriptor_write.pNext = nullptr;
  edram_storage_buffer_descriptor_write.dstSet =
      edram_storage_buffer_descriptor_set_;
  edram_storage_buffer_descriptor_write.dstBinding = 0;
  edram_storage_buffer_descriptor_write.dstArrayElement = 0;
  edram_storage_buffer_descriptor_write.descriptorCount = 1;
  edram_storage_buffer_descriptor_write.descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  edram_storage_buffer_descriptor_write.pImageInfo = nullptr;
  edram_storage_buffer_descriptor_write.pBufferInfo =
      &edram_storage_buffer_descriptor_buffer_info;
  edram_storage_buffer_descriptor_write.pTexelBufferView = nullptr;
  dfn.vkUpdateDescriptorSets(device, 1, &edram_storage_buffer_descriptor_write,
                             0, nullptr);

#if XE_PLATFORM_ANDROID
  if (cvars::halo_android_compat_presentable_color_shadow &&
      path_ == Path::kHostRenderTargets) {
    if (ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, VkDeviceSize(kAndroidHaloShadowBytes),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            android_halo_present_shadow_buffer_,
            android_halo_present_shadow_memory_)) {
      XELOGI(
          "HaloCompat active=1 mode=presentable_color_shadow device={} "
          "base={} pitch={} rows={} bytes=0x{:X}",
          device_properties.deviceName, kAndroidHaloShadowBaseTiles,
          kAndroidHaloShadowPitchTiles, kAndroidHaloShadowRows,
          kAndroidHaloShadowBytes);
    } else {
      XELOGW(
          "HaloCompat active=0: failed to allocate presentable color shadow "
          "buffer bytes=0x{:X}",
          kAndroidHaloShadowBytes);
      android_halo_present_shadow_buffer_ = VK_NULL_HANDLE;
      android_halo_present_shadow_memory_ = VK_NULL_HANDLE;
    }
  }
  if (GetAndroidHaloExperiment().menu_initialize_from_base675 &&
      path_ == Path::kHostRenderTargets) {
    if (ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, VkDeviceSize(kAndroidHaloMenuSceneBytes),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal,
            android_halo_menu_scene_shadow_buffer_,
            android_halo_menu_scene_shadow_memory_)) {
      XELOGI(
          "MENU_SCENE_SHADOW active=1 base={} pitch={} rows={} bytes=0x{:X}",
          kAndroidHaloMenuSceneBaseTiles, kAndroidHaloMenuScenePitchTiles,
          kAndroidHaloMenuSceneRows, kAndroidHaloMenuSceneBytes);
    } else {
      XELOGW("MENU_SCENE_SHADOW active=0 allocation failed bytes=0x{:X}",
             kAndroidHaloMenuSceneBytes);
      android_halo_menu_scene_shadow_buffer_ = VK_NULL_HANDLE;
      android_halo_menu_scene_shadow_memory_ = VK_NULL_HANDLE;
    }
  }
#endif

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  // Resolve copy pipeline layout.
  VkDescriptorSetLayout
      resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetCount] = {};
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetEdram] =
      descriptor_set_layout_storage_buffer_;
  resolve_copy_descriptor_set_layouts[kResolveCopyDescriptorSetDest] =
      command_processor_.GetSingleTransientDescriptorLayout(
          VulkanCommandProcessor::SingleTransientDescriptorLayout ::
              kStorageBufferCompute);
  VkPushConstantRange resolve_copy_push_constant_range;
  resolve_copy_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_copy_push_constant_range.offset = 0;
  // Potentially binding all of the shared memory at 1x resolution, but only
  // portions with scaled resolution.
  resolve_copy_push_constant_range.size =
      draw_resolution_scaled
          ? sizeof(draw_util::ResolveCopyShaderConstants::DestRelative)
          : sizeof(draw_util::ResolveCopyShaderConstants);
  VkPipelineLayoutCreateInfo resolve_copy_pipeline_layout_create_info;
  resolve_copy_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  resolve_copy_pipeline_layout_create_info.pNext = nullptr;
  resolve_copy_pipeline_layout_create_info.flags = 0;
  resolve_copy_pipeline_layout_create_info.setLayoutCount =
      kResolveCopyDescriptorSetCount;
  resolve_copy_pipeline_layout_create_info.pSetLayouts =
      resolve_copy_descriptor_set_layouts;
  resolve_copy_pipeline_layout_create_info.pushConstantRangeCount = 1;
  resolve_copy_pipeline_layout_create_info.pPushConstantRanges =
      &resolve_copy_push_constant_range;
  if (dfn.vkCreatePipelineLayout(
          device, &resolve_copy_pipeline_layout_create_info, nullptr,
          &resolve_copy_pipeline_layout_) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the resolve copy pipeline "
        "layout");
    Shutdown();
    return false;
  }

  // Resolve copy pipelines.
  for (size_t i = 0; i < size_t(draw_util::ResolveCopyShaderIndex::kCount);
       ++i) {
    const draw_util::ResolveCopyShaderInfo& resolve_copy_shader_info =
        draw_util::resolve_copy_shader_info[i];
    const ResolveCopyShaderCode& resolve_copy_shader_code =
        kResolveCopyShaders[i];
    // Somewhat verification whether resolve_copy_shaders_ is up to date.
    assert_true(resolve_copy_shader_code.unscaled &&
                resolve_copy_shader_code.unscaled_size_bytes &&
                resolve_copy_shader_code.scaled &&
                resolve_copy_shader_code.scaled_size_bytes);
    VkPipeline resolve_copy_pipeline = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_copy_pipeline_layout_,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled
                               : resolve_copy_shader_code.unscaled,
        draw_resolution_scaled ? resolve_copy_shader_code.scaled_size_bytes
                               : resolve_copy_shader_code.unscaled_size_bytes,
        nullptr, "main", 64);
    if (resolve_copy_pipeline == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the resolve copy "
          "pipeline {}",
          resolve_copy_shader_info.debug_name);
      Shutdown();
      return false;
    }
    vulkan_device->SetObjectName(VK_OBJECT_TYPE_PIPELINE, resolve_copy_pipeline,
                                 resolve_copy_shader_info.debug_name);
    resolve_copy_pipelines_[i] = resolve_copy_pipeline;
  }

  // TODO(Triang3l): All paths (FSI).

  if (path_ == Path::kHostRenderTargets) {
    // Host render targets.

    // TODO(Triang3l): When color space conversion is implemented in the
    // ownership transfer and resolve dump shaders, allow
    // `gamma_render_target_as_unorm16` if VK_FORMAT_R16G16B16A16_UNORM supports
    // the SAMPLED_IMAGE | COLOR_ATTACHMENT | COLOR_ATTACHMENT_BLEND features.
    gamma_render_target_as_unorm16_ = false;

    depth_float24_round_ = cvars::depth_float24_round;

    // Host depth storing pipeline layout.
    VkDescriptorSetLayout host_depth_store_descriptor_set_layouts[] = {
        // Destination EDRAM storage buffer.
        descriptor_set_layout_storage_buffer_,
        // Source depth / stencil texture (only depth is used).
        descriptor_set_layout_sampled_image_x2_,
    };
    VkPushConstantRange host_depth_store_push_constant_range;
    host_depth_store_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    host_depth_store_push_constant_range.offset = 0;
    host_depth_store_push_constant_range.size = sizeof(HostDepthStoreConstants);
    VkPipelineLayoutCreateInfo host_depth_store_pipeline_layout_create_info;
    host_depth_store_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    host_depth_store_pipeline_layout_create_info.pNext = nullptr;
    host_depth_store_pipeline_layout_create_info.flags = 0;
    host_depth_store_pipeline_layout_create_info.setLayoutCount =
        uint32_t(xe::countof(host_depth_store_descriptor_set_layouts));
    host_depth_store_pipeline_layout_create_info.pSetLayouts =
        host_depth_store_descriptor_set_layouts;
    host_depth_store_pipeline_layout_create_info.pushConstantRangeCount = 1;
    host_depth_store_pipeline_layout_create_info.pPushConstantRanges =
        &host_depth_store_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &host_depth_store_pipeline_layout_create_info, nullptr,
            &host_depth_store_pipeline_layout_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the host depth storing "
          "pipeline layout");
      Shutdown();
      return false;
    }
    constexpr std::pair<const uint32_t*, size_t> host_depth_store_shaders[] = {
        {shaders::host_depth_store_1xmsaa_cs,
         sizeof(shaders::host_depth_store_1xmsaa_cs)},
        {shaders::host_depth_store_2xmsaa_cs,
         sizeof(shaders::host_depth_store_2xmsaa_cs)},
        {shaders::host_depth_store_4xmsaa_cs,
         sizeof(shaders::host_depth_store_4xmsaa_cs)},
    };
    for (size_t i = 0; i < xe::countof(host_depth_store_shaders); ++i) {
      const std::pair<const uint32_t*, size_t> host_depth_store_shader =
          host_depth_store_shaders[i];
      VkPipeline host_depth_store_pipeline =
          ui::vulkan::util::CreateComputePipeline(
              vulkan_device, host_depth_store_pipeline_layout_,
              host_depth_store_shader.first, host_depth_store_shader.second,
              nullptr, "main", 64);
      if (host_depth_store_pipeline == VK_NULL_HANDLE) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the {}-sample host "
            "depth storing pipeline",
            uint32_t(1) << i);
        Shutdown();
        return false;
      }
      host_depth_store_pipelines_[i] = host_depth_store_pipeline;
    }

    // Transfer and clear vertex buffer, for quads of up to tile granularity.
      transfer_vertex_buffer_pool_ =
          std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
              vulkan_device,
              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              std::max(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                       sizeof(float) * 2 * 6 *
                           Transfer::kMaxCutoutBorderRectangles *
                         xenos::kEdramTileCount));

    // Transfer vertex shader.
    transfer_passthrough_vertex_shader_ = ui::vulkan::util::CreateShaderModule(
        vulkan_device, shaders::passthrough_position_xy_vs,
        sizeof(shaders::passthrough_position_xy_vs));
    if (transfer_passthrough_vertex_shader_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the render target "
          "ownership transfer vertex shader");
      Shutdown();
      return false;
    }

    // Transfer pipeline layouts.
    VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layouts
        [kTransferUsedDescriptorSetCount];
    VkPushConstantRange transfer_pipeline_layout_push_constant_range;
    transfer_pipeline_layout_push_constant_range.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    transfer_pipeline_layout_push_constant_range.offset = 0;
    VkPipelineLayoutCreateInfo transfer_pipeline_layout_create_info;
    transfer_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    transfer_pipeline_layout_create_info.pNext = nullptr;
    transfer_pipeline_layout_create_info.flags = 0;
    transfer_pipeline_layout_create_info.pSetLayouts =
        transfer_pipeline_layout_descriptor_set_layouts;
    transfer_pipeline_layout_create_info.pPushConstantRanges =
        &transfer_pipeline_layout_push_constant_range;
    for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
      const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
          kTransferPipelineLayoutInfos[i];
      transfer_pipeline_layout_create_info.setLayoutCount = 0;
      uint32_t transfer_pipeline_layout_descriptor_sets_remaining =
          transfer_pipeline_layout_info.used_descriptor_sets;
      uint32_t transfer_pipeline_layout_descriptor_set_index;
      while (xe::bit_scan_forward(
          transfer_pipeline_layout_descriptor_sets_remaining,
          &transfer_pipeline_layout_descriptor_set_index)) {
        transfer_pipeline_layout_descriptor_sets_remaining &=
            ~(uint32_t(1) << transfer_pipeline_layout_descriptor_set_index);
        VkDescriptorSetLayout transfer_pipeline_layout_descriptor_set_layout =
            VK_NULL_HANDLE;
        switch (TransferUsedDescriptorSet(
            transfer_pipeline_layout_descriptor_set_index)) {
          case kTransferUsedDescriptorSetHostDepthBuffer:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_storage_buffer_;
            break;
          case kTransferUsedDescriptorSetHostDepthStencilTextures:
          case kTransferUsedDescriptorSetDepthStencilTextures:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_x2_;
            break;
          case kTransferUsedDescriptorSetColorTexture:
            transfer_pipeline_layout_descriptor_set_layout =
                descriptor_set_layout_sampled_image_;
            break;
          default:
            assert_unhandled_case(TransferUsedDescriptorSet(
                transfer_pipeline_layout_descriptor_set_index));
        }
        transfer_pipeline_layout_descriptor_set_layouts
            [transfer_pipeline_layout_create_info.setLayoutCount++] =
                transfer_pipeline_layout_descriptor_set_layout;
      }
      transfer_pipeline_layout_push_constant_range.size = uint32_t(
          sizeof(uint32_t) *
          xe::bit_count(
              transfer_pipeline_layout_info.used_push_constant_dwords));
      transfer_pipeline_layout_create_info.pushConstantRangeCount =
          transfer_pipeline_layout_info.used_push_constant_dwords ? 1 : 0;
      if (dfn.vkCreatePipelineLayout(
              device, &transfer_pipeline_layout_create_info, nullptr,
              &transfer_pipeline_layouts_[i]) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline layout {}",
            i);
        Shutdown();
        return false;
      }
    }

    // Dump pipeline layouts.
    VkDescriptorSetLayout
        dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetCount];
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetEdram] =
        descriptor_set_layout_storage_buffer_;
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_;
    VkPushConstantRange dump_pipeline_layout_push_constant_range;
    dump_pipeline_layout_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    dump_pipeline_layout_push_constant_range.offset = 0;
    dump_pipeline_layout_push_constant_range.size =
        sizeof(uint32_t) * kDumpPushConstantCount;
    VkPipelineLayoutCreateInfo dump_pipeline_layout_create_info;
    dump_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    dump_pipeline_layout_create_info.pNext = nullptr;
    dump_pipeline_layout_create_info.flags = 0;
    dump_pipeline_layout_create_info.setLayoutCount =
        uint32_t(xe::countof(dump_pipeline_layout_descriptor_set_layouts));
    dump_pipeline_layout_create_info.pSetLayouts =
        dump_pipeline_layout_descriptor_set_layouts;
    dump_pipeline_layout_create_info.pushConstantRangeCount = 1;
    dump_pipeline_layout_create_info.pPushConstantRanges =
        &dump_pipeline_layout_push_constant_range;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info,
                                   nullptr, &dump_pipeline_layout_color_) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the color render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
    dump_pipeline_layout_descriptor_set_layouts[kDumpDescriptorSetSource] =
        descriptor_set_layout_sampled_image_x2_;
    if (dfn.vkCreatePipelineLayout(device, &dump_pipeline_layout_create_info,
                                   nullptr, &dump_pipeline_layout_depth_) !=
        VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the depth render target "
          "dumping pipeline layout");
      Shutdown();
      return false;
    }
  } else if (path_ == Path::kPixelShaderInterlock) {
    // Pixel (fragment) shader interlock.

    // Piecewise linear gamma is 8-bit with programmable blending.
    gamma_render_target_as_unorm16_ = false;

    // Always true float24 depth rounded to the nearest even.
    depth_float24_round_ = true;

    // The pipeline layout and the pipelines for clearing the EDRAM buffer in
    // resolves.
    VkPushConstantRange resolve_fsi_clear_push_constant_range;
    resolve_fsi_clear_push_constant_range.stageFlags =
        VK_SHADER_STAGE_COMPUTE_BIT;
    resolve_fsi_clear_push_constant_range.offset = 0;
    resolve_fsi_clear_push_constant_range.size =
        sizeof(draw_util::ResolveClearShaderConstants);
    VkPipelineLayoutCreateInfo resolve_fsi_clear_pipeline_layout_create_info;
    resolve_fsi_clear_pipeline_layout_create_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    resolve_fsi_clear_pipeline_layout_create_info.pNext = nullptr;
    resolve_fsi_clear_pipeline_layout_create_info.flags = 0;
    resolve_fsi_clear_pipeline_layout_create_info.setLayoutCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pSetLayouts =
        &descriptor_set_layout_storage_buffer_;
    resolve_fsi_clear_pipeline_layout_create_info.pushConstantRangeCount = 1;
    resolve_fsi_clear_pipeline_layout_create_info.pPushConstantRanges =
        &resolve_fsi_clear_push_constant_range;
    if (dfn.vkCreatePipelineLayout(
            device, &resolve_fsi_clear_pipeline_layout_create_info, nullptr,
            &resolve_fsi_clear_pipeline_layout_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the resolve EDRAM buffer "
          "clear pipeline layout");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_32bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_32bpp_scaled_cs
                               : shaders::resolve_clear_32bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_32bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_32bpp_cs),
        nullptr, "main", 64);
    if (resolve_fsi_clear_32bpp_pipeline_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the 32bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }
    resolve_fsi_clear_64bpp_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_fsi_clear_pipeline_layout_,
        draw_resolution_scaled ? shaders::resolve_clear_64bpp_scaled_cs
                               : shaders::resolve_clear_64bpp_cs,
        draw_resolution_scaled ? sizeof(shaders::resolve_clear_64bpp_scaled_cs)
                               : sizeof(shaders::resolve_clear_64bpp_cs),
        nullptr, "main", 64);
    if (resolve_fsi_clear_64bpp_pipeline_ == VK_NULL_HANDLE) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the 64bpp resolve EDRAM "
          "buffer clear pipeline");
      Shutdown();
      return false;
    }

    // Common render pass.
    VkSubpassDescription fsi_subpass = {};
    fsi_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    // Fragment shader interlock provides synchronization and ordering within a
    // subpass, create an external by-region dependency to maintain interlocking
    // between passes. Framebuffer-global dependencies will be made with
    // explicit barriers when the addressing of the EDRAM buffer relatively to
    // the fragment coordinates is changed.
    VkSubpassDependency fsi_subpass_dependencies[2];
    fsi_subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    fsi_subpass_dependencies[0].dstSubpass = 0;
    fsi_subpass_dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fsi_subpass_dependencies[0].srcAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    fsi_subpass_dependencies[0].dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    VkDependencyFlags fsi_dependency_flags = VK_DEPENDENCY_BY_REGION_BIT;
#if XE_PLATFORM_ANDROID
    if (cvars::halo_android_diag_strict_barriers) {
      fsi_dependency_flags = 0;
    }
#endif
    fsi_subpass_dependencies[0].dependencyFlags = fsi_dependency_flags;
    fsi_subpass_dependencies[1] = fsi_subpass_dependencies[0];
    std::swap(fsi_subpass_dependencies[1].srcSubpass,
              fsi_subpass_dependencies[1].dstSubpass);
    VkRenderPassCreateInfo fsi_render_pass_create_info;
    fsi_render_pass_create_info.sType =
        VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fsi_render_pass_create_info.pNext = nullptr;
    fsi_render_pass_create_info.flags = 0;
    fsi_render_pass_create_info.attachmentCount = 0;
    fsi_render_pass_create_info.pAttachments = nullptr;
    fsi_render_pass_create_info.subpassCount = 1;
    fsi_render_pass_create_info.pSubpasses = &fsi_subpass;
    fsi_render_pass_create_info.dependencyCount =
        uint32_t(xe::countof(fsi_subpass_dependencies));
    fsi_render_pass_create_info.pDependencies = fsi_subpass_dependencies;
    if (dfn.vkCreateRenderPass(device, &fsi_render_pass_create_info, nullptr,
                               &fsi_render_pass_) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend render pass");
      Shutdown();
      return false;
    }

    // Common framebuffer.
    VkFramebufferCreateInfo fsi_framebuffer_create_info;
    fsi_framebuffer_create_info.sType =
        VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fsi_framebuffer_create_info.pNext = nullptr;
    fsi_framebuffer_create_info.flags = 0;
    fsi_framebuffer_create_info.renderPass = fsi_render_pass_;
    fsi_framebuffer_create_info.attachmentCount = 0;
    fsi_framebuffer_create_info.pAttachments = nullptr;
    fsi_framebuffer_create_info.width = std::min(
        xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_x(),
        device_properties.maxFramebufferWidth);
    fsi_framebuffer_create_info.height = std::min(
        xenos::kTexture2DCubeMaxWidthHeight * draw_resolution_scale_y(),
        device_properties.maxFramebufferHeight);
    fsi_framebuffer_create_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &fsi_framebuffer_create_info, nullptr,
                                &fsi_framebuffer_.framebuffer) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTargetCache: Failed to create the fragment shader "
          "interlock render backend framebuffer");
      Shutdown();
      return false;
    }
    fsi_framebuffer_.host_extent.width = fsi_framebuffer_create_info.width;
    fsi_framebuffer_.host_extent.height = fsi_framebuffer_create_info.height;
  } else {
    assert_unhandled_case(path_);
    Shutdown();
    return false;
  }

  // Reset the last update structures, to keep the defaults consistent between
  // paths regardless of whether the update for the path actually modifies them.
  last_update_render_pass_key_ = RenderPassKey();
  last_update_render_pass_ = VK_NULL_HANDLE;
  last_update_framebuffer_pitch_tiles_at_32bpp_ = 0;
  std::memset(last_update_framebuffer_attachments_, 0,
              sizeof(last_update_framebuffer_attachments_));
  last_update_framebuffer_ = VK_NULL_HANDLE;

  InitializeCommon();
  return true;
}

void VulkanRenderTargetCache::Shutdown(bool from_destructor) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Destroy all render targets before the descriptor set pool is destroyed -
  // may happen if shutting down the VulkanRenderTargetCache by destroying it,
  // so ShutdownCommon is called by the RenderTargetCache destructor, when it's
  // already too late.
  DestroyAllRenderTargets(true);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_64bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_fsi_clear_32bpp_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_fsi_clear_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                         fsi_framebuffer_.framebuffer);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         fsi_render_pass_);

  for (const auto& dump_pipeline_pair : dump_pipelines_) {
    // May be null to prevent recreation attempts.
    if (dump_pipeline_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyPipeline(device, dump_pipeline_pair.second, nullptr);
    }
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_depth_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         dump_pipeline_layout_color_);

  for (const auto& transfer_pipeline_array_pair : transfer_pipelines_) {
    for (VkPipeline transfer_pipeline : transfer_pipeline_array_pair.second) {
      // May be null to prevent recreation attempts.
      if (transfer_pipeline != VK_NULL_HANDLE) {
        dfn.vkDestroyPipeline(device, transfer_pipeline, nullptr);
      }
    }
  }
  transfer_pipelines_.clear();
  for (const auto& transfer_shader_pair : transfer_shaders_) {
    if (transfer_shader_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyShaderModule(device, transfer_shader_pair.second, nullptr);
    }
  }
  transfer_shaders_.clear();
  for (size_t i = 0; i < size_t(TransferPipelineLayoutIndex::kCount); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                           transfer_pipeline_layouts_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device,
                                         transfer_passthrough_vertex_shader_);
  transfer_vertex_buffer_pool_.reset();

  for (size_t i = 0; i < xe::countof(host_depth_store_pipelines_); ++i) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           host_depth_store_pipelines_[i]);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         host_depth_store_pipeline_layout_);

  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer,
                             nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    if (render_pass_pair.second != VK_NULL_HANDLE) {
      dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
    }
  }
  render_passes_.clear();

  for (VkPipeline& resolve_copy_pipeline : resolve_copy_pipelines_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                           resolve_copy_pipeline);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_copy_pipeline_layout_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         edram_storage_buffer_descriptor_pool_);
#if XE_PLATFORM_ANDROID
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyBuffer, device, android_halo_present_shadow_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkFreeMemory, device, android_halo_present_shadow_memory_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyBuffer, device, android_halo_menu_scene_shadow_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkFreeMemory, device, android_halo_menu_scene_shadow_memory_);
  android_halo_present_shadow_valid_ = false;
  android_halo_menu_scene_shadow_valid_ = false;
  android_halo_owner_kind_ = AndroidHaloOwnerKind::kUnknown;
  AndroidDiagnosticRenderState::Get().SetOwnerState(
      AndroidDiagnosticOwnerState::kUnknown);
#endif
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         edram_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         edram_buffer_memory_);

  descriptor_set_pool_sampled_image_x2_.reset();
  descriptor_set_pool_sampled_image_.reset();

  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyDescriptorSetLayout, device,
      descriptor_set_layout_sampled_image_x2_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_sampled_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout,
                                         device,
                                         descriptor_set_layout_storage_buffer_);

  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanRenderTargetCache::ClearCache() { ClearCache("deferred"); }

bool VulkanRenderTargetCache::SaveStateSubmitEdramDownload(
    VkBuffer destination) {
  if (destination == VK_NULL_HANDLE || IsDrawResolutionScaled()) {
    return false;
  }
  if (GetPath() == Path::kHostRenderTargets) {
    DumpRenderTargets(0, xenos::kEdramTileCount, 1,
                      xenos::kEdramTileCount);
  }
  UseEdramBuffer(EdramBufferUsage::kTransferRead);
  command_processor_.SubmitBarriers(true);
  VkBufferCopy* copy =
      command_processor_.deferred_command_buffer().CmdCopyBufferEmplace(
          edram_buffer_, destination, 1);
  copy->srcOffset = 0;
  copy->dstOffset = 0;
  copy->size = xenos::kEdramSizeBytes;
  command_processor_.PushBufferMemoryBarrier(
      destination, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);
  return true;
}

void VulkanRenderTargetCache::SaveStateSubmitEdramUpload(VkBuffer source) {
  assert_true(source != VK_NULL_HANDLE);
  UseEdramBuffer(EdramBufferUsage::kTransferWrite);
  command_processor_.SubmitBarriers(true);
  VkBufferCopy* copy =
      command_processor_.deferred_command_buffer().CmdCopyBufferEmplace(
          source, edram_buffer_, 1);
  copy->srcOffset = 0;
  copy->dstOffset = 0;
  copy->size = xenos::kEdramSizeBytes;
}

void VulkanRenderTargetCache::ClearCache(const char* reason) {
  // Queued pipeline creation requests hold the render pass they were recorded
  // with as a raw VkRenderPass, so any pass destroyed below may still be about
  // to be handed to vkCreateGraphicsPipelines by an async creation thread.
  // Drain those workers first or the driver faults inside pipeline creation.
  // Only reachable with async creation enabled: synchronous creation finishes
  // inside the draw, so it can never overlap with this.
  command_processor_.DrainPipelineCreationThreads();

#if XE_PLATFORM_ANDROID
  const bool android_halo_compat_active = IsAndroidHaloCompatActive();
  const bool android_halo_presentable_tracked =
      android_halo_presentable_color_rt_ != nullptr;
  const RenderTargetKey android_halo_presentable_key =
      android_halo_presentable_tracked
          ? android_halo_presentable_color_rt_->key()
          : RenderTargetKey();
  const bool android_halo_msaa_scene_tracked =
      android_halo_msaa_scene_color_rt_ != nullptr;
  const RenderTargetKey android_halo_msaa_scene_key =
      android_halo_msaa_scene_tracked
          ? android_halo_msaa_scene_color_rt_->key()
          : RenderTargetKey();
  const bool android_halo_present_shadow_valid_before =
      android_halo_present_shadow_valid_;
  const bool android_halo_menu_scene_shadow_valid_before =
      android_halo_menu_scene_shadow_valid_;
  const AndroidHaloOwnerKind android_halo_owner_kind_before =
      android_halo_owner_kind_;
  const bool android_halo_depth_alias_latched_before =
      android_halo_depth_alias_quarantine_latched_;
  const bool android_halo_reclaim_shadow_required_before =
      android_halo_reclaim_shadow_required_;
#endif

  const VkDeviceSize render_target_memory_usage_before =
      render_target_memory_usage_bytes_;
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Framebuffer objects must be destroyed because they reference views of
  // attachment images, which may be removed by the common ClearCache.
  last_update_framebuffer_ = VK_NULL_HANDLE;
  for (const auto& framebuffer_pair : framebuffers_) {
    dfn.vkDestroyFramebuffer(device, framebuffer_pair.second.framebuffer,
                             nullptr);
  }
  framebuffers_.clear();

  last_update_render_pass_ = VK_NULL_HANDLE;
  for (const auto& render_pass_pair : render_passes_) {
    dfn.vkDestroyRenderPass(device, render_pass_pair.second, nullptr);
  }
  render_passes_.clear();

  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->ClearCache();
  }

#if XE_PLATFORM_ANDROID
  android_depth_to_color_edram_fallback_source_ = nullptr;
  android_halo_presentable_color_rt_ = nullptr;
  android_halo_msaa_scene_color_rt_ = nullptr;
  android_halo_transfer_render_pass_active_ = false;
  android_halo_present_shadow_stage_mask_ = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  android_halo_present_shadow_access_mask_ = 0;
  android_halo_menu_scene_shadow_stage_mask_ =
      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  android_halo_menu_scene_shadow_access_mask_ = 0;
#endif

  RenderTargetCache::ClearCache();
#if XE_PLATFORM_ANDROID
  if (android_halo_presentable_tracked) {
    android_halo_presentable_color_rt_ =
        static_cast<VulkanRenderTarget*>(
            FindRenderTarget(android_halo_presentable_key));
  }
  if (android_halo_msaa_scene_tracked) {
    android_halo_msaa_scene_color_rt_ =
        static_cast<VulkanRenderTarget*>(
            FindRenderTarget(android_halo_msaa_scene_key));
  }
  const AndroidHaloReclaimStateDecision android_halo_reclaim_decision =
      DecideAndroidHaloReclaimState({
          android_halo_owner_kind_before ==
              AndroidHaloOwnerKind::kPresentableColor,
          android_halo_owner_kind_before ==
              AndroidHaloOwnerKind::kDepthColorAliasNonPresentable,
          android_halo_depth_alias_latched_before,
          android_halo_presentable_tracked,
          android_halo_presentable_color_rt_ != nullptr,
          android_halo_msaa_scene_tracked,
          android_halo_msaa_scene_color_rt_ != nullptr,
          android_halo_present_shadow_valid_before,
          android_halo_reclaim_shadow_required_before,
      });
  android_halo_present_shadow_valid_ =
      android_halo_reclaim_decision.preserve_shadow_valid;
  android_halo_menu_scene_shadow_valid_ =
      android_halo_menu_scene_shadow_valid_before;
  android_halo_depth_alias_quarantine_latched_ =
      android_halo_reclaim_decision.preserve_depth_alias_latch;
  android_halo_reclaim_shadow_required_ =
      android_halo_reclaim_decision.reclaim_shadow_required;
  android_halo_owner_kind_ =
      android_halo_reclaim_decision.preserve_owner
          ? android_halo_owner_kind_before
          : AndroidHaloOwnerKind::kUnknown;
  switch (android_halo_owner_kind_) {
    case AndroidHaloOwnerKind::kPresentableColor:
      AndroidDiagnosticRenderState::Get().SetOwnerState(
          AndroidDiagnosticOwnerState::kPresentableColor);
      break;
    case AndroidHaloOwnerKind::kDepthColorAliasNonPresentable:
      AndroidDiagnosticRenderState::Get().SetOwnerState(
          AndroidDiagnosticOwnerState::kDepthColorAliasNonPresentable);
      break;
    default:
      AndroidDiagnosticRenderState::Get().SetOwnerState(
          AndroidDiagnosticOwnerState::kUnknown);
      break;
  }
  if (android_halo_compat_active) {
    const uint64_t generation = ++android_halo_cache_reclaim_generation_;
    if (generation <= 64 || (generation % 300) == 0) {
      XELOGI(
          "HaloCompat cache_reclaim generation={} reason={} owner={}->{} "
          "presentable_tracked/rebound={}/{} msaa_tracked/rebound={}/{} "
          "shadow={}->{} menu_shadow={}->{} latch={}->{} "
          "reclaim_shadow_required={}->{}",
          generation, reason, GetAndroidHaloOwnerKindName(
                                  android_halo_owner_kind_before),
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_presentable_tracked),
          uint32_t(android_halo_presentable_color_rt_ != nullptr),
          uint32_t(android_halo_msaa_scene_tracked),
          uint32_t(android_halo_msaa_scene_color_rt_ != nullptr),
          uint32_t(android_halo_present_shadow_valid_before),
          uint32_t(android_halo_present_shadow_valid_),
          uint32_t(android_halo_menu_scene_shadow_valid_before),
          uint32_t(android_halo_menu_scene_shadow_valid_),
          uint32_t(android_halo_depth_alias_latched_before),
          uint32_t(android_halo_depth_alias_quarantine_latched_),
          uint32_t(android_halo_reclaim_shadow_required_before),
          uint32_t(android_halo_reclaim_shadow_required_));
    }
  }
#else
  static_cast<void>(reason);
#endif
  render_target_memory_clear_requested_ = false;
  if (render_target_memory_usage_bytes_ !=
      render_target_memory_usage_before) {
    XELOGI("Vulkan render target cache reclaimed {} MB; {} MB retained",
           (render_target_memory_usage_before -
            render_target_memory_usage_bytes_) >>
               20,
           render_target_memory_usage_bytes_ >> 20);
  }
}

void VulkanRenderTargetCache::CompletedSubmissionUpdated() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->Reclaim(
        command_processor_.GetCompletedSubmission());
  }
}

void VulkanRenderTargetCache::EndSubmission() {
  if (transfer_vertex_buffer_pool_) {
    transfer_vertex_buffer_pool_->FlushWrites();
  }
}

bool VulkanRenderTargetCache::Resolve(const Memory& memory,
                                      VulkanSharedMemory& shared_memory,
                                      VulkanTextureCache& texture_cache,
                                      uint32_t& written_address_out,
                                      uint32_t& written_length_out) {
  written_address_out = 0;
  written_length_out = 0;

  bool draw_resolution_scaled = IsDrawResolutionScaled();

  draw_util::ResolveInfo resolve_info;
  if (!draw_util::GetResolveInfo(
          register_file(), memory, trace_writer_, draw_resolution_scale_x(),
          draw_resolution_scale_y(), IsFixedRG16TruncatedToMinus1To1(),
          IsFixedRGBA16TruncatedToMinus1To1(), resolve_info)) {
    return false;
  }

  // Nothing to copy/clear.
  if (!resolve_info.coordinate_info.width_div_8 || !resolve_info.height_div_8) {
    return true;
  }

#if XE_PLATFORM_ANDROID
  AndroidDiagnosticRenderState::Get().RecordResolve(
      resolve_info.copy_dest_extent_start,
      resolve_info.copy_dest_extent_length,
      uint32_t(resolve_info.copy_dest_info.copy_dest_format),
      uint32_t(resolve_info.color_edram_info.base_tiles),
      uint32_t(resolve_info.color_edram_info.format),
      uint32_t(1) << uint32_t(resolve_info.color_edram_info.msaa_samples),
      resolve_info.color_edram_info.is_depth);

  // Permanent, allocation-free telemetry for Reach's reused scene scratch
  // range. Log each distinct register-derived resolve tuple at most once.
  constexpr uint32_t kAndroidHaloSceneResolveStart = UINT32_C(0x02354000);
  constexpr uint32_t kAndroidHaloSceneResolveEnd =
      kAndroidHaloSceneResolveStart + UINT32_C(0x00654000);
  const uint64_t android_resolve_dest_end =
      uint64_t(resolve_info.copy_dest_extent_start) +
      uint64_t(resolve_info.copy_dest_extent_length);
  if (resolve_info.copy_dest_extent_length &&
      !resolve_info.IsCopyingDepth() &&
      resolve_info.copy_dest_extent_start < kAndroidHaloSceneResolveEnd &&
      android_resolve_dest_end > kAndroidHaloSceneResolveStart) {
    struct AndroidSceneResolveLogKey {
      uint32_t dest;
      uint32_t dest_format;
      int32_t dest_exp_bias;
      uint32_t source_format;
      uint32_t source_base;
    };
    const AndroidSceneResolveLogKey key = {
        resolve_info.copy_dest_extent_start,
        uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
        uint32_t(resolve_info.color_edram_info.format),
        uint32_t(resolve_info.color_edram_info.base_tiles)};
    static AndroidSceneResolveLogKey logged_keys[32] = {};
    static uint32_t logged_key_count = 0;
    bool seen = false;
    for (uint32_t i = 0; i < logged_key_count; ++i) {
      const AndroidSceneResolveLogKey& logged = logged_keys[i];
      if (logged.dest == key.dest && logged.dest_format == key.dest_format &&
          logged.dest_exp_bias == key.dest_exp_bias &&
          logged.source_format == key.source_format &&
          logged.source_base == key.source_base) {
        seen = true;
        break;
      }
    }
    if (!seen && logged_key_count < xe::countof(logged_keys)) {
      logged_keys[logged_key_count++] = key;
      XELOGI(
          "HaloCompat scene resolve: dest=0x{:08X} dest_fmt={} "
          "dest_exp_bias={} src_fmt={} src_base={}",
          key.dest, key.dest_format, key.dest_exp_bias, key.source_format,
          key.source_base);
    }
  }
#endif

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  // Copying.
  bool copied = false;
  if (resolve_info.copy_dest_extent_length) {
    uint32_t dump_base = 0;
    uint32_t dump_row_length_used = 0;
    uint32_t dump_rows = 0;
    uint32_t dump_pitch = 0;
    if (GetPath() == Path::kHostRenderTargets) {
      // Dump the current contents of the render targets owning the affected
      // range to edram_buffer_.
      // TODO(Triang3l): Direct host render target -> shared memory resolve
      // shaders for non-converting cases.
      resolve_info.GetCopyEdramTileSpan(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
#if XE_PLATFORM_ANDROID
      // Whether the guest will read this span as 1x MSAA 8888. Games (Halo
      // Reach) alias 4x-MSAA render targets with 1x resolves to use samples
      // as pixels; the dump path consults this to collapse host samples (and
      // repack in the read format) so the 1x read doesn't interleave samples
      // into stripes or scramble 10-bit words as 8888.
      {
        const xenos::ColorRenderTargetFormat resolve_read_format =
            xenos::ColorRenderTargetFormat(
                resolve_info.color_edram_info.format);
        android_resolve_read_64bpp_ =
            resolve_info.color_edram_info.format_is_64bpp;
        // 64bpp dumps reassemble raw bits, so any same-bpp read format works;
        // 32bpp dumps repack via float, so only allow the 8888 read formats
        // the repacking targets.
        android_resolve_read_msaa_1x_ =
            !resolve_info.IsCopyingDepth() &&
            resolve_info.color_edram_info.msaa_samples ==
                xenos::MsaaSamples::k1X &&
            (android_resolve_read_64bpp_ ||
             resolve_read_format ==
                 xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
             resolve_read_format ==
                 xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA);
      }
#endif
#if XE_PLATFORM_ANDROID
      // WO27: snapshot who owns the tiles this resolve is about to read,
      // BEFORE the dump reads them, plus the register-derived source view the
      // resolve itself believes in (answers whether "fmt7 4x" comes from the
      // guest registers or from an inference of ours).
      {
        const AndroidHaloExperiment& android_experiment =
            GetAndroidHaloExperiment();
        const uint32_t dump_end_tiles = dump_base + dump_rows * dump_pitch;
        if (android_experiment.log_ownership_snapshot &&
            dump_base < android_experiment.ownership_watch_end_tiles &&
            dump_end_tiles > android_experiment.ownership_watch_start_tiles) {
          XELOGI(
              "OWNSNAP_RESOLVE dest=0x{:08X} dump_tiles=[{},{}) "
              "dump_pitch={} reg_src_base={} reg_src_pitch={} reg_src_msaa={} "
              "reg_src_depth={} reg_src_fmt={} reg_src_64bpp={} "
              "dest_fmt={} dest_endian={} dest_exp_bias={} dest_swap={}",
              resolve_info.copy_dest_extent_start, dump_base, dump_end_tiles,
              dump_pitch,
              uint32_t(resolve_info.color_edram_info.base_tiles),
              uint32_t(resolve_info.color_edram_info.pitch_tiles),
              UINT32_C(1)
                  << uint32_t(resolve_info.color_edram_info.msaa_samples),
              uint32_t(resolve_info.color_edram_info.is_depth),
              uint32_t(resolve_info.color_edram_info.format),
              uint32_t(resolve_info.color_edram_info.format_is_64bpp),
              uint32_t(resolve_info.copy_dest_info.copy_dest_format),
              uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
              int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
              uint32_t(resolve_info.copy_dest_info.copy_dest_swap));
          AndroidLogEdramOwnershipSnapshot(
              std::max(dump_base,
                       android_experiment.ownership_watch_start_tiles),
              std::min(dump_end_tiles,
                       android_experiment.ownership_watch_end_tiles),
              resolve_info.copy_dest_extent_start);
        }
      }
#endif
#if XE_PLATFORM_ANDROID
      AndroidHaloMaybeLogHost675Stats(
          dump_base, dump_row_length_used, dump_rows, dump_pitch,
          resolve_info.copy_dest_extent_start,
          uint32_t(resolve_info.copy_dest_info.copy_dest_format));
      // Only redirect when the resolve registers themselves claim a 7e3
      // source; a legitimately-LDR resolve to the same span must not be fed
      // stale HDR content (HOSTDUMP t150: register src was fmt3 while the
      // ownership map said fmt2 in every bad case).
      {
        const xenos::ColorRenderTargetFormat prefer_7e3_reg_src_format =
            xenos::ColorRenderTargetFormat(
                resolve_info.color_edram_info.format);
        android_prefer_7e3_dump_675_ =
            GetAndroidHaloExperiment().dump_scene_675_prefer_7e3_float &&
            !resolve_info.IsCopyingDepth() &&
            !resolve_info.color_edram_info.is_depth &&
            (prefer_7e3_reg_src_format ==
                 xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
             prefer_7e3_reg_src_format ==
                 xenos::ColorRenderTargetFormat::
                     k_2_10_10_10_FLOAT_AS_16_16_16_16) &&
            resolve_info.copy_dest_extent_start == UINT32_C(0x02354000) &&
            resolve_info.copy_dest_info.copy_dest_format ==
                xenos::ColorFormat::k_16_16_16_16 &&
            dump_base == 675;
      }
#endif
      DumpRenderTargets(dump_base, dump_row_length_used, dump_rows, dump_pitch);
#if XE_PLATFORM_ANDROID
      android_prefer_7e3_dump_675_ = false;
      android_resolve_read_msaa_1x_ = false;
#endif
      if (cvars::halo_android_diag_synthetic_edram_fill &&
          transfer_vertex_buffer_pool_ && dump_base == 1350 &&
          dump_row_length_used == 15 && dump_rows == 45 && dump_pitch == 15) {
        constexpr uint32_t kSyntheticVisibleWidth = 1152;
        constexpr uint32_t kSyntheticVisibleHeight = 720;
        constexpr uint32_t kSyntheticBpp = 4;
        constexpr uint32_t kSyntheticTileBytes =
            xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples *
            kSyntheticBpp;
        const uint32_t copy_width_check =
            resolve_info.coordinate_info.width_div_8
            << xenos::kResolveAlignmentPixelsLog2;
        const uint32_t copy_height_check =
            resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
        const bool synthetic_target_matches =
            copy_width_check == kSyntheticVisibleWidth &&
            copy_height_check == kSyntheticVisibleHeight &&
            resolve_info.copy_dest_info.copy_dest_format ==
                xenos::ColorFormat::k_8_8_8_8;
        if (synthetic_target_matches) {
          const VkDeviceSize edram_dst_offset =
              VkDeviceSize(dump_base) * kSyntheticTileBytes;
          const size_t synthetic_size =
              size_t(dump_rows) * size_t(dump_pitch) *
              size_t(kSyntheticTileBytes);
          size_t synthetic_bytes_written = 0;
          UseEdramBuffer(EdramBufferUsage::kTransferWrite);
          command_processor_.SubmitBarriers(true);
          while (synthetic_bytes_written < synthetic_size) {
            VkBuffer upload_buffer = VK_NULL_HANDLE;
            VkDeviceSize upload_offset = 0;
            VkDeviceSize upload_size = 0;
            uint8_t* upload = transfer_vertex_buffer_pool_->RequestPartial(
                command_processor_.GetCurrentSubmission(),
                synthetic_size - synthetic_bytes_written, kSyntheticBpp,
                upload_buffer, upload_offset, upload_size);
            if (!upload || !upload_size) {
              break;
            }
            const size_t chunk_size = std::min<size_t>(
                size_t(upload_size), synthetic_size - synthetic_bytes_written);
            for (size_t byte_index = 0; byte_index + 3 < chunk_size;
                 byte_index += kSyntheticBpp) {
              const size_t span_byte = synthetic_bytes_written + byte_index;
              const uint32_t tile_index =
                  uint32_t(span_byte / kSyntheticTileBytes);
              const uint32_t tile_byte =
                  uint32_t(span_byte % kSyntheticTileBytes);
              const uint32_t pixel_in_tile = tile_byte / kSyntheticBpp;
              const uint32_t in_tile_x =
                  pixel_in_tile % xenos::kEdramTileWidthSamples;
              const uint32_t in_tile_y =
                  pixel_in_tile / xenos::kEdramTileWidthSamples;
              const uint32_t tile_x = tile_index % dump_pitch;
              const uint32_t tile_y = tile_index / dump_pitch;
              const uint32_t x =
                  tile_x * xenos::kEdramTileWidthSamples + in_tile_x;
              const uint32_t y =
                  tile_y * xenos::kEdramTileHeightSamples + in_tile_y;

              uint8_t r = x < kSyntheticVisibleWidth
                              ? uint8_t((uint64_t(x) * 255) /
                                        (kSyntheticVisibleWidth - 1))
                              : 255;
              uint8_t g = y < kSyntheticVisibleHeight
                              ? uint8_t((uint64_t(y) * 255) /
                                        (kSyntheticVisibleHeight - 1))
                              : 0;
              uint8_t b = ((tile_x ^ tile_y) & 1) ? 255 : 32;
              if (x >= kSyntheticVisibleWidth) {
                r = 255;
                g = 0;
                b = 255;
              }
              const bool x_marker = x == 0 || x == 79 || x == 80 ||
                                    x == 1119 || x == 1120 ||
                                    x + 1 == kSyntheticVisibleWidth;
              const bool y_marker = y == 0 || y == 15 || y == 16 ||
                                    y + 1 == kSyntheticVisibleHeight;
              if (x_marker || y_marker) {
                r = x_marker ? 255 : 0;
                g = y_marker ? 255 : 0;
                b = (x_marker && y_marker) ? 255 : 0;
              }
              upload[byte_index + 0] = r;
              upload[byte_index + 1] = g;
              upload[byte_index + 2] = b;
              upload[byte_index + 3] = 0xFF;
            }
            VkBufferCopy copy_region = {};
            copy_region.srcOffset = upload_offset;
            copy_region.dstOffset = edram_dst_offset + synthetic_bytes_written;
            copy_region.size = chunk_size;
            command_buffer.CmdVkCopyBuffer(upload_buffer, edram_buffer_, 1,
                                           &copy_region);
            synthetic_bytes_written += chunk_size;
          }
          transfer_vertex_buffer_pool_->FlushWrites();
          XELOGI(
              "Android synthetic EDRAM fill: bytes=0x{:X}/0x{:X} "
              "edram_dst=0x{:X} dump_base={} rows={} pitch={}",
              uint32_t(synthetic_bytes_written), uint32_t(synthetic_size),
              uint32_t(edram_dst_offset), dump_base, dump_rows, dump_pitch);
        }
      }
      if (cvars::halo_android_diag_strict_barriers) {
        command_processor_.PushBufferMemoryBarrier(
            edram_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
        command_processor_.SubmitBarriers(true);
      }
    }

    draw_util::ResolveCopyShaderConstants copy_shader_constants;
    uint32_t copy_group_count_x, copy_group_count_y;
    draw_util::ResolveCopyShaderIndex copy_shader = resolve_info.GetCopyShader(
        draw_resolution_scale_x(), draw_resolution_scale_y(),
        copy_shader_constants, copy_group_count_x, copy_group_count_y);
#if XE_PLATFORM_ANDROID
    const uint32_t android_host_7e3_range_mode =
        GetAndroidHaloExperiment().host_7e3_range_mode;
    const bool android_normalize_7e3_resolve =
        !UsesScaledUNorm7e3RenderTargets() &&
        (android_host_7e3_range_mode == 1 ||
         android_host_7e3_range_mode == 3) &&
        !resolve_info.IsCopyingDepth() &&
        (copy_shader_constants.dest_relative.edram_info.format ==
             uint32_t(
                 xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) ||
         copy_shader_constants.dest_relative.edram_info.format ==
             uint32_t(xenos::ColorRenderTargetFormat::
                          k_2_10_10_10_FLOAT_AS_16_16_16_16)) &&
        copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias == -5;
    if (android_normalize_7e3_resolve) {
      copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias = 0;
      static uint32_t android_normalize_7e3_resolve_log_count = 0;
      if (android_normalize_7e3_resolve_log_count++ < 32) {
        XELOGI(
            "HaloCompat normalized 7e3 resolve: mode={} dest=0x{:08X} "
            "src_fmt={} exp_bias=-5->0",
            android_host_7e3_range_mode,
            resolve_info.copy_dest_extent_start,
            uint32_t(copy_shader_constants.dest_relative.edram_info.format));
      }
    }
    const AndroidHaloExperiment& android_experiment =
        GetAndroidHaloExperiment();
    const uint32_t android_resolve_source_format =
        uint32_t(copy_shader_constants.dest_relative.edram_info.format);
    const uint32_t android_resolve_dest_format = uint32_t(
        copy_shader_constants.dest_relative.dest_info.copy_dest_format);
    const bool android_7e3_to_fixed16_full_resolve =
        !resolve_info.IsCopyingDepth() &&
        copy_shader == draw_util::ResolveCopyShaderIndex::kFull64bpp &&
        (android_resolve_source_format ==
             uint32_t(
                 xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT) ||
         android_resolve_source_format ==
             uint32_t(xenos::ColorRenderTargetFormat::
                          k_2_10_10_10_FLOAT_AS_16_16_16_16)) &&
        (android_resolve_dest_format ==
             uint32_t(xenos::TextureFormat::k_16_16_16_16_EDRAM) ||
         android_resolve_dest_format ==
             uint32_t(xenos::ColorFormat::k_16_16_16_16));
    if (android_experiment.force_7e3_fixed16_resolve_exp_bias_minus5 &&
        android_7e3_to_fixed16_full_resolve) {
      const int32_t old_exp_bias =
          copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias;
      copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias = -5;
      static uint32_t android_force_7e3_fixed16_bias_log_count = 0;
      if (android_force_7e3_fixed16_bias_log_count++ < 16) {
        XELOGI(
            "HaloCompat forced 7e3 fixed16 resolve bias: dest=0x{:08X} "
            "src_base={} dest_fmt={} exp_bias={}->-5",
            resolve_info.copy_dest_extent_start,
            uint32_t(copy_shader_constants.dest_relative.edram_info.base_tiles),
            android_resolve_dest_format, old_exp_bias);
      }
    }
    if (android_experiment.signed_fixed16_resolve_pack &&
        android_7e3_to_fixed16_full_resolve) {
      // resolve_full_64bpp packs formats 21/26 as UNORM16 after exp_bias.
      // Optional A/B: further reduce bias by 1 to approximate SNORM codes.
      const int32_t old_exp_bias =
          copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias;
      const bool can_emulate_signed_pack =
          old_exp_bias <= -5 && old_exp_bias > -32;
      if (can_emulate_signed_pack) {
        copy_shader_constants.dest_relative.dest_info.copy_dest_exp_bias =
            old_exp_bias - 1;
      }
      static uint32_t android_signed_fixed16_pack_log_count = 0;
      if (android_signed_fixed16_pack_log_count++ < 16) {
        XELOGI(
            "HaloCompat signed fixed16 resolve pack emulation: "
            "dest=0x{:08X} src_base={} dest_fmt={} exp_bias={}->{} "
            "rgb_only=1 applied={}",
            resolve_info.copy_dest_extent_start,
            uint32_t(copy_shader_constants.dest_relative.edram_info.base_tiles),
            android_resolve_dest_format, old_exp_bias,
            int32_t(copy_shader_constants.dest_relative.dest_info
                        .copy_dest_exp_bias),
            uint32_t(can_emulate_signed_pack));
      }
    }
#endif
    const uint32_t copy_width =
        resolve_info.coordinate_info.width_div_8
        << xenos::kResolveAlignmentPixelsLog2;
    const uint32_t copy_height =
        resolve_info.height_div_8 << xenos::kResolveAlignmentPixelsLog2;
    const FormatInfo& copy_dest_format_info = *FormatInfo::Get(
        xenos::TextureFormat(resolve_info.copy_dest_info.copy_dest_format));
    const uint32_t copy_bpp = copy_dest_format_info.bits_per_pixel >> 3;
#if XE_PLATFORM_ANDROID
    // Always-on layout census paired with SCRATCH_SAMPLE_LAYOUT (fmt54 loads).
    if (!resolve_info.IsCopyingDepth() &&
        resolve_info.copy_dest_extent_start == UINT32_C(0x02354000) &&
        (copy_width == 1152 && copy_height == 720) &&
        (uint32_t(resolve_info.copy_dest_info.copy_dest_format) ==
             uint32_t(xenos::ColorFormat::k_16_16_16_16) ||
         uint32_t(resolve_info.copy_dest_info.copy_dest_format) ==
             uint32_t(xenos::ColorFormat::k_2_10_10_10_AS_16_16_16_16) ||
         uint32_t(resolve_info.copy_dest_info.copy_dest_format) ==
             uint32_t(xenos::ColorFormat::k_2_10_10_10) ||
         uint32_t(resolve_info.copy_dest_info.copy_dest_format) ==
             uint32_t(xenos::ColorFormat::k_8_8_8_8))) {
      static uint32_t android_scratch_resolve_layout_log_count = 0;
      if (android_scratch_resolve_layout_log_count < 64) {
        XELOGI(
            "SCRATCH_RESOLVE_LAYOUT {}: dest=0x02354000 {}x{} dest_fmt={} "
            "dest_endian={} dest_exp_bias={} dest_swap={} copy_bpp={} "
            "copy_bytes={} src_base={} src_pitch={} src_msaa={} src_fmt={} "
            "src_64bpp={} copy_shader={}",
            android_scratch_resolve_layout_log_count, copy_width, copy_height,
            uint32_t(resolve_info.copy_dest_info.copy_dest_format),
            uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
            int32_t(resolve_info.copy_dest_info.copy_dest_exp_bias),
            uint32_t(resolve_info.copy_dest_info.copy_dest_swap), copy_bpp,
            copy_width * copy_height * copy_bpp,
            uint32_t(copy_shader_constants.dest_relative.edram_info.base_tiles),
            uint32_t(copy_shader_constants.dest_relative.edram_info.pitch_tiles),
            UINT32_C(1) << uint32_t(
                copy_shader_constants.dest_relative.edram_info.msaa_samples),
            uint32_t(copy_shader_constants.dest_relative.edram_info.format),
            uint32_t(
                copy_shader_constants.dest_relative.edram_info.format_is_64bpp),
            uint32_t(copy_shader));
        ++android_scratch_resolve_layout_log_count;
      }
    }
#endif
#if XE_PLATFORM_ANDROID
    const bool android_override_base0_pitch29_to15 =
        GetAndroidHaloExperiment().resolve_base0_pitch29_to15 &&
        !resolve_info.IsCopyingDepth() &&
        resolve_info.copy_dest_extent_start == UINT32_C(0x02354000) &&
        resolve_info.copy_dest_info.copy_dest_format ==
            xenos::ColorFormat::k_8_8_8_8 &&
        uint32_t(resolve_info.copy_dest_info.copy_dest_endian) == 2 &&
        copy_width == 1152 && copy_height == 720 && copy_bpp == 4 &&
        copy_shader_constants.dest_relative.edram_info.base_tiles == 0 &&
        copy_shader_constants.dest_relative.edram_info.pitch_tiles == 29 &&
        copy_shader_constants.dest_relative.edram_info.msaa_samples ==
            xenos::MsaaSamples::k1X;
    if (android_override_base0_pitch29_to15) {
      copy_shader_constants.dest_relative.edram_info.pitch_tiles = 15;
      static uint32_t android_pitch_override_log_count = 0;
      if (android_pitch_override_log_count++ < 8) {
        XELOGI(
            "Android Halo resolve source pitch override: dest=0x{:08X} "
            "{}x{} fmt={} endian={} edram_base=0 pitch=29->15",
            resolve_info.copy_dest_extent_start, copy_width, copy_height,
            uint32_t(resolve_info.copy_dest_info.copy_dest_format),
            uint32_t(resolve_info.copy_dest_info.copy_dest_endian));
      }
    }
    struct AndroidResolveMapLogKey {
      uint32_t dest;
      uint32_t format;
      uint32_t endian;
      uint32_t width;
      uint32_t height;
      uint32_t bpp;
      uint32_t msaa_log2;
      uint32_t edram_base;
      uint32_t edram_pitch;
      uint32_t depth_src;
    };
    const AndroidResolveMapLogKey android_resolvemap_key = {
        resolve_info.copy_dest_extent_start,
        uint32_t(resolve_info.copy_dest_info.copy_dest_format),
        uint32_t(resolve_info.copy_dest_info.copy_dest_endian),
        copy_width,
        copy_height,
        copy_bpp,
        uint32_t(copy_shader_constants.dest_relative.edram_info.msaa_samples),
        uint32_t(copy_shader_constants.dest_relative.edram_info.base_tiles),
        uint32_t(copy_shader_constants.dest_relative.edram_info.pitch_tiles),
        uint32_t(resolve_info.IsCopyingDepth())};
    static std::vector<AndroidResolveMapLogKey> android_resolvemap_log_keys;
    bool android_resolvemap_seen = false;
    for (const AndroidResolveMapLogKey& logged_key :
         android_resolvemap_log_keys) {
      if (logged_key.dest == android_resolvemap_key.dest &&
          logged_key.format == android_resolvemap_key.format &&
          logged_key.endian == android_resolvemap_key.endian &&
          logged_key.width == android_resolvemap_key.width &&
          logged_key.height == android_resolvemap_key.height &&
          logged_key.bpp == android_resolvemap_key.bpp &&
          logged_key.msaa_log2 == android_resolvemap_key.msaa_log2 &&
          logged_key.edram_base == android_resolvemap_key.edram_base &&
          logged_key.edram_pitch == android_resolvemap_key.edram_pitch &&
          logged_key.depth_src == android_resolvemap_key.depth_src) {
        android_resolvemap_seen = true;
        break;
      }
    }
    if (!android_resolvemap_seen && android_resolvemap_log_keys.size() < 3000) {
      android_resolvemap_log_keys.push_back(android_resolvemap_key);
      XELOGI(
          "RESOLVEMAP dest=0x{:08X} fmt={} endian={} {}x{} bpp={} "
          "edram_base={} pitch_tiles={} msaa={} depth_src={}",
          android_resolvemap_key.dest, android_resolvemap_key.format,
          android_resolvemap_key.endian,
          android_resolvemap_key.width, android_resolvemap_key.height,
          android_resolvemap_key.bpp, android_resolvemap_key.edram_base,
          android_resolvemap_key.edram_pitch,
          UINT32_C(1) << android_resolvemap_key.msaa_log2,
          android_resolvemap_key.depth_src);
    }
    // Update the last-writer tracker on EVERY resolve (not just first-seen
    // unique combos) so a dump can tell what actually landed here most
    // recently, not just what has ever landed here across the whole run.
    AndroidHaloRecordResolveWrite(
        android_resolvemap_key.dest, android_resolvemap_key.format,
        android_resolvemap_key.endian,
        android_resolvemap_key.edram_base, android_resolvemap_key.edram_pitch,
        UINT32_C(1) << android_resolvemap_key.msaa_log2,
        android_resolvemap_key.bpp);
#endif
    bool android_halo_frontbuffer_resolve = false;
#if XE_PLATFORM_ANDROID
    android_halo_frontbuffer_resolve =
        !resolve_info.IsCopyingDepth() && copy_bpp == 4 &&
        copy_width == 1152 && copy_height == 720 &&
        resolve_info.copy_dest_extent_start == kAndroidHaloFrontbufferAddress &&
        resolve_info.copy_dest_info.copy_dest_format ==
            xenos::ColorFormat::k_8_8_8_8 &&
        IsAndroidHaloShadowSpan(dump_base, dump_row_length_used, dump_rows,
                                dump_pitch) &&
        copy_shader_constants.dest_relative.edram_info.base_tiles ==
            kAndroidHaloShadowBaseTiles &&
        copy_shader_constants.dest_relative.edram_info.pitch_tiles ==
            kAndroidHaloShadowPitchTiles;
#endif
    const bool android_force_full_32bpp =
        android_halo_frontbuffer_resolve ||
        (cvars::halo_android_diag_force_full_32bpp_non80 &&
         !resolve_info.IsCopyingDepth() && copy_bpp == 4 &&
         (copy_width % xenos::kEdramTileWidthSamples));
    if (android_force_full_32bpp &&
        (copy_shader == draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA ||
         copy_shader == draw_util::ResolveCopyShaderIndex::kFast32bpp4xMSAA)) {
      copy_shader = draw_util::ResolveCopyShaderIndex::kFull32bpp;
      const draw_util::ResolveCopyShaderInfo& forced_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];
      const uint32_t scaled_width = copy_width * draw_resolution_scale_x();
      const uint32_t scaled_height = copy_height * draw_resolution_scale_y();
      copy_group_count_x =
          (scaled_width + ((UINT32_C(1) << forced_shader_info.group_size_x_log2) -
                           1)) >>
          forced_shader_info.group_size_x_log2;
      copy_group_count_y =
          (scaled_height +
           ((UINT32_C(1) << forced_shader_info.group_size_y_log2) - 1)) >>
          forced_shader_info.group_size_y_log2;
      static uint32_t android_force_full_resolve_log_count = 0;
      if (android_force_full_resolve_log_count++ < 32) {
        XELOGI(
            "Android Halo resolve forcing full 32bpp shader: auto={} "
            "width={} height={} bpp={} pitch_tiles={} fb=0x{:08X}",
            uint32_t(android_halo_frontbuffer_resolve), copy_width, copy_height,
            copy_bpp,
            uint32_t(
                copy_shader_constants.dest_relative.edram_info.pitch_tiles),
            resolve_info.copy_dest_extent_start);
      }
    }
    if (cvars::halo_android_diag_log_resolve_constants) {
      const uint32_t dest_pitch_pixels =
          copy_shader_constants.dest_relative.dest_coordinate_info
              .pitch_aligned_div_32
          << xenos::kTextureTileWidthHeightLog2;
      const uint32_t visible_bytes = copy_width * copy_height * copy_bpp;
      const uint32_t extent_rows =
          dest_pitch_pixels && copy_bpp
              ? resolve_info.copy_dest_extent_length /
                    (dest_pitch_pixels * copy_bpp)
              : 0;
      const uint32_t edram_pitch_tiles =
          copy_shader_constants.dest_relative.edram_info.pitch_tiles;
      const uint32_t tile_bytes =
          xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples *
          copy_bpp;
      const uint32_t edram_band_bytes =
          edram_pitch_tiles * xenos::kEdramTileWidthSamples *
          xenos::kEdramTileHeightSamples * copy_bpp;
      const uint32_t visible_band_bytes =
          copy_width * xenos::kEdramTileHeightSamples * copy_bpp;
      const uint32_t band_padding_bytes =
          edram_band_bytes > visible_band_bytes
              ? edram_band_bytes - visible_band_bytes
              : 0;
      const uint32_t edram_tile_span_bytes =
          dump_rows ? ((dump_rows - 1) * dump_pitch + dump_row_length_used) *
                          tile_bytes
                    : 0;
      XELOGI(
          "Android Halo resolve diag: shader={} forced_full={} format={} "
          "bpp={} msaa={} dest=0x{:08X} extent=[0x{:08X},+0x{:X}] "
          "visible={}x{} visible_bytes=0x{:X} extent_rows={} dest_pitch_px={} "
          "dest_array={} edram_base={} edram_pitch_tiles={} dump_base={} "
          "dump_rows={} dump_row_len={} dump_pitch={} edram_span_bytes=0x{:X} "
          "groups={}x{} width_mod80={} ceil_width_tiles={} padding_px={} "
          "visible_band=0x{:X} edram_band=0x{:X} band_padding=0x{:X}",
          draw_util::resolve_copy_shader_info[size_t(copy_shader)].debug_name,
          uint32_t(copy_shader ==
                   draw_util::ResolveCopyShaderIndex::kFull32bpp),
          FormatInfo::GetName(xenos::TextureFormat(
              resolve_info.copy_dest_info.copy_dest_format)),
          copy_bpp,
          uint32_t(copy_shader_constants.dest_relative.edram_info.msaa_samples),
          resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
          resolve_info.copy_dest_extent_length, copy_width, copy_height,
          visible_bytes, extent_rows, dest_pitch_pixels,
          uint32_t(resolve_info.copy_dest_info.copy_dest_array),
          uint32_t(copy_shader_constants.dest_relative.edram_info.base_tiles),
          edram_pitch_tiles, dump_base, dump_rows, dump_row_length_used,
          dump_pitch, edram_tile_span_bytes,
          copy_group_count_x, copy_group_count_y,
          copy_width % xenos::kEdramTileWidthSamples,
          (copy_width + xenos::kEdramTileWidthSamples - 1) /
              xenos::kEdramTileWidthSamples,
          edram_pitch_tiles * xenos::kEdramTileWidthSamples > copy_width
              ? edram_pitch_tiles * xenos::kEdramTileWidthSamples - copy_width
              : 0,
          visible_band_bytes, edram_band_bytes, band_padding_bytes);
    }
    assert_true(copy_group_count_x && copy_group_count_y);
    if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
      const draw_util::ResolveCopyShaderInfo& copy_shader_info =
          draw_util::resolve_copy_shader_info[size_t(copy_shader)];

      // Make sure there is memory to write to.
      bool copy_dest_committed;
      // TODO(Triang3l): Resolution-scaled buffer committing.
      copy_dest_committed =
          shared_memory.RequestRange(resolve_info.copy_dest_extent_start,
                                     resolve_info.copy_dest_extent_length);
      if (!copy_dest_committed) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to obtain the resolve destination "
            "memory region");
      } else {
        // TODO(Triang3l): Switching between descriptors if exceeding
        // maxStorageBufferRange.
        // TODO(Triang3l): Use a single 512 MB shared memory binding if
        // possible.
        VkDescriptorSet descriptor_set_dest =
            command_processor_.AllocateSingleTransientDescriptor(
                VulkanCommandProcessor::SingleTransientDescriptorLayout ::
                    kStorageBufferCompute);
        if (descriptor_set_dest != VK_NULL_HANDLE) {
          // Write the destination descriptor.
          VkDescriptorBufferInfo write_descriptor_set_dest_buffer_info;

          bool scaled_buffer_ready = false;
          if (draw_resolution_scaled) {
            // For scaled resolve, ensure the scaled buffer exists and bind to
            // it
            uint32_t dest_address = resolve_info.copy_dest_base;
            uint32_t dest_length = resolve_info.copy_dest_extent_start -
                                   resolve_info.copy_dest_base +
                                   resolve_info.copy_dest_extent_length;

            // Ensure scaled resolve memory is committed
            scaled_buffer_ready = true;
            if (!texture_cache.EnsureScaledResolveMemoryCommittedPublic(
                    dest_address, dest_length)) {
              XELOGE(
                  "Failed to commit scaled resolve memory for resolve dest at "
                  "0x{:08X}",
                  dest_address);
              scaled_buffer_ready = false;
            }

            // Make the range current to get the buffer
            if (scaled_buffer_ready &&
                !texture_cache.MakeScaledResolveRangeCurrent(dest_address,
                                                             dest_length)) {
              XELOGE(
                  "Failed to make scaled resolve range current for resolve "
                  "dest at 0x{:08X}",
                  dest_address);
              scaled_buffer_ready = false;
            }

            // Get the current scaled buffer
            VkBuffer scaled_buffer = VK_NULL_HANDLE;
            if (scaled_buffer_ready) {
              scaled_buffer = texture_cache.GetCurrentScaledResolveBuffer();
              if (scaled_buffer == VK_NULL_HANDLE) {
                XELOGE(
                    "No current scaled resolve buffer for resolve dest at "
                    "0x{:08X}",
                    dest_address);
                scaled_buffer_ready = false;
              }
            }

            if (scaled_buffer_ready) {
              // Calculate offset within the scaled buffer
              uint32_t draw_resolution_scale_area =
                  draw_resolution_scale_x() * draw_resolution_scale_y();
              uint64_t scaled_offset =
                  uint64_t(dest_address) * draw_resolution_scale_area;
              uint64_t buffer_relative_offset =
                  scaled_offset -
                  texture_cache.GetCurrentScaledResolveBufferBaseOffset();

              write_descriptor_set_dest_buffer_info.buffer = scaled_buffer;
              write_descriptor_set_dest_buffer_info.offset =
                  buffer_relative_offset;
              write_descriptor_set_dest_buffer_info.range =
                  dest_length * draw_resolution_scale_area;
            }
          }

          if (!scaled_buffer_ready) {
            // Regular unscaled resolve - write to shared memory
            if (draw_resolution_scaled) {
              XELOGW(
                  "Falling back to unscaled resolve at 0x{:08X} - scaled "
                  "buffer not available",
                  resolve_info.copy_dest_base);
            }
            write_descriptor_set_dest_buffer_info.buffer =
                shared_memory.buffer();
            write_descriptor_set_dest_buffer_info.offset =
                resolve_info.copy_dest_base;
            write_descriptor_set_dest_buffer_info.range =
                resolve_info.copy_dest_extent_start -
                resolve_info.copy_dest_base +
                resolve_info.copy_dest_extent_length;
          }
          VkWriteDescriptorSet write_descriptor_set_dest;
          write_descriptor_set_dest.sType =
              VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write_descriptor_set_dest.pNext = nullptr;
          write_descriptor_set_dest.dstSet = descriptor_set_dest;
          write_descriptor_set_dest.dstBinding = 0;
          write_descriptor_set_dest.dstArrayElement = 0;
          write_descriptor_set_dest.descriptorCount = 1;
          write_descriptor_set_dest.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
          write_descriptor_set_dest.pImageInfo = nullptr;
          write_descriptor_set_dest.pBufferInfo =
              &write_descriptor_set_dest_buffer_info;
          write_descriptor_set_dest.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &write_descriptor_set_dest, 0,
                                     nullptr);

          // Submit the resolve.
          if (!scaled_buffer_ready) {
            // Regular unscaled - transition shared memory for write
            shared_memory.Use(VulkanSharedMemory::Usage::kComputeWrite,
                              std::pair<uint32_t, uint32_t>(
                                  resolve_info.copy_dest_extent_start,
                                  resolve_info.copy_dest_extent_length));
          } else {
            // Scaled - add barrier for the scaled resolve buffer
            // The buffer transitions from compute shader read (texture loading)
            // to compute shader write
            VkBuffer scaled_buffer =
                texture_cache.GetCurrentScaledResolveBuffer();
            if (scaled_buffer != VK_NULL_HANDLE) {
              VkBufferMemoryBarrier buffer_barrier = {};
              buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
              // More specific: previous compute shader reads to compute shader
              // write
              buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
              buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
              buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.buffer = scaled_buffer;
              buffer_barrier.offset = 0;
              buffer_barrier.size = VK_WHOLE_SIZE;

              command_buffer.CmdVkPipelineBarrier(
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,  // From compute shader
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,  // To compute shader
                  0, 0, nullptr, 1, &buffer_barrier, 0, nullptr);
            }
          }
#if XE_PLATFORM_ANDROID
          AndroidHaloMaybeOverrideEdramWithPresentableShadow(
              resolve_info, dump_base, dump_row_length_used, dump_rows,
              dump_pitch, copy_width, copy_height, copy_bpp,
              copy_shader_constants);
#endif
          UseEdramBuffer(EdramBufferUsage::kComputeRead);
          command_processor_.BindExternalComputePipeline(
              resolve_copy_pipelines_[size_t(copy_shader)]);
          VkDescriptorSet descriptor_sets[kResolveCopyDescriptorSetCount] = {};
          descriptor_sets[kResolveCopyDescriptorSetEdram] =
              edram_storage_buffer_descriptor_set_;
          descriptor_sets[kResolveCopyDescriptorSetDest] = descriptor_set_dest;
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, resolve_copy_pipeline_layout_, 0,
              uint32_t(xe::countof(descriptor_sets)), descriptor_sets, 0,
              nullptr);
          if (draw_resolution_scaled) {
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants.dest_relative),
                &copy_shader_constants.dest_relative);
          } else {
            // TODO(Triang3l): Proper dest_base in case of one 512 MB shared
            // memory binding, or multiple shared memory bindings in case of
            // splitting due to maxStorageBufferRange overflow.
            copy_shader_constants.dest_base -=
                uint32_t(write_descriptor_set_dest_buffer_info.offset);
            command_buffer.CmdVkPushConstants(
                resolve_copy_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(copy_shader_constants), &copy_shader_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(copy_group_count_x, copy_group_count_y,
                                       1);

          // Add barrier after writing to scaled resolve buffer
          if (scaled_buffer_ready) {
            VkBuffer scaled_buffer =
                texture_cache.GetCurrentScaledResolveBuffer();
            if (scaled_buffer != VK_NULL_HANDLE) {
              VkBufferMemoryBarrier buffer_barrier = {};
              buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
              buffer_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
              buffer_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
              buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
              buffer_barrier.buffer = scaled_buffer;
              buffer_barrier.offset = 0;
              buffer_barrier.size = VK_WHOLE_SIZE;

              command_buffer.CmdVkPipelineBarrier(
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                  &buffer_barrier, 0, nullptr);
            }
          }
          if (cvars::halo_android_diag_strict_barriers) {
            VkBuffer strict_buffer =
                scaled_buffer_ready ? texture_cache.GetCurrentScaledResolveBuffer()
                                    : shared_memory.buffer();
            if (strict_buffer != VK_NULL_HANDLE) {
              command_processor_.PushBufferMemoryBarrier(
                  strict_buffer,
                  scaled_buffer_ready ? 0 : resolve_info.copy_dest_extent_start,
                  scaled_buffer_ready ? VK_WHOLE_SIZE
                                      : resolve_info.copy_dest_extent_length,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                  VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
              command_processor_.SubmitBarriers(true);
            }
          }

          // Invalidate textures and mark the range as scaled if needed.
          texture_cache.MarkRangeAsResolved(
              resolve_info.copy_dest_extent_start,
              resolve_info.copy_dest_extent_length);
          written_address_out = resolve_info.copy_dest_extent_start;
          written_length_out = resolve_info.copy_dest_extent_length;
          copied = true;
        }
      }
    }
  } else {
    copied = true;
  }

  // Clearing.
  bool cleared = false;
  bool clear_depth = resolve_info.IsClearingDepth();
  bool clear_color = resolve_info.IsClearingColor();
  if (clear_depth || clear_color) {
    switch (GetPath()) {
      case Path::kHostRenderTargets: {
        Transfer::Rectangle clear_rectangle;
        RenderTarget* clear_render_targets[2];
        // If PrepareHostRenderTargetsResolveClear returns false, may be just an
        // empty region (success) or an error - don't care.
        if (PrepareHostRenderTargetsResolveClear(
                resolve_info, clear_rectangle, clear_render_targets[0],
                clear_transfers_[0], clear_render_targets[1],
                clear_transfers_[1])) {
          uint64_t clear_values[2];
          clear_values[0] = resolve_info.rb_depth_clear;
          clear_values[1] = resolve_info.rb_color_clear |
                            (uint64_t(resolve_info.rb_color_clear_lo) << 32);
          PerformTransfersAndResolveClears(2, clear_render_targets,
                                           clear_transfers_, clear_values,
                                           &clear_rectangle);
        }
        cleared = true;
      } break;
      case Path::kPixelShaderInterlock: {
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Should be safe to only commit once (if was accessed as unordered or
        // with fragment shader interlock previously - if there was nothing to
        // copy, only to clear, for some reason, for instance), overlap of the
        // depth and the color ranges is highly unlikely.
        CommitEdramBufferShaderWrites();
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, resolve_fsi_clear_pipeline_layout_,
            0, 1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
        std::pair<uint32_t, uint32_t> clear_group_count =
            resolve_info.GetClearShaderGroupCount(draw_resolution_scale_x(),
                                                  draw_resolution_scale_y());
        assert_true(clear_group_count.first && clear_group_count.second);
        if (clear_depth) {
          command_processor_.BindExternalComputePipeline(
              resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants depth_clear_constants;
          resolve_info.GetDepthClearShaderConstants(depth_clear_constants);
          command_buffer.CmdVkPushConstants(
              resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
              0, sizeof(depth_clear_constants), &depth_clear_constants);
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first,
                                       clear_group_count.second, 1);
        }
        if (clear_color) {
          command_processor_.BindExternalComputePipeline(
              resolve_info.color_edram_info.format_is_64bpp
                  ? resolve_fsi_clear_64bpp_pipeline_
                  : resolve_fsi_clear_32bpp_pipeline_);
          draw_util::ResolveClearShaderConstants color_clear_constants;
          resolve_info.GetColorClearShaderConstants(color_clear_constants);
          if (clear_depth) {
            // Non-RT-specific constants have already been set.
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                uint32_t(offsetof(draw_util::ResolveClearShaderConstants,
                                  rt_specific)),
                sizeof(color_clear_constants.rt_specific),
                &color_clear_constants.rt_specific);
          } else {
            command_buffer.CmdVkPushConstants(
                resolve_fsi_clear_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(color_clear_constants), &color_clear_constants);
          }
          command_processor_.SubmitBarriers(true);
          command_buffer.CmdVkDispatch(clear_group_count.first,
                                       clear_group_count.second, 1);
        }
        MarkEdramBufferModified();
        cleared = true;
      } break;
      default:
        assert_unhandled_case(GetPath());
    }
  } else {
    cleared = true;
  }

  return copied && cleared;
}

bool VulkanRenderTargetCache::Update(
    bool is_rasterization_done, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask, const Shader& vertex_shader) {
  if (!FlushPendingDrawPassTransfers()) {
    return false;
  }

  if (!RenderTargetCache::Update(is_rasterization_done,
                                 normalized_depth_control,
                                 normalized_color_mask, vertex_shader)) {
    return false;
  }

  auto rb_surface_info = register_file().Get<reg::RB_SURFACE_INFO>();

  RenderPassKey render_pass_key;
  // Needed even with the fragment shader interlock render backend for passing
  // the sample count to the pipeline cache.
  render_pass_key.msaa_samples = rb_surface_info.msaa_samples;

  switch (GetPath()) {
    case Path::kHostRenderTargets: {
      RenderTarget* const* depth_and_color_render_targets =
          last_update_accumulated_render_targets();

      if (depth_and_color_render_targets[0]) {
        render_pass_key.depth_and_color_used |= 1 << 0;
        render_pass_key.depth_format =
            depth_and_color_render_targets[0]->key().GetDepthFormat();
      }
      if (depth_and_color_render_targets[1]) {
        render_pass_key.depth_and_color_used |= 1 << 1;
        render_pass_key.color_0_view_format =
            depth_and_color_render_targets[1]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[2]) {
        render_pass_key.depth_and_color_used |= 1 << 2;
        render_pass_key.color_1_view_format =
            depth_and_color_render_targets[2]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[3]) {
        render_pass_key.depth_and_color_used |= 1 << 3;
        render_pass_key.color_2_view_format =
            depth_and_color_render_targets[3]->key().GetColorFormat();
      }
      if (depth_and_color_render_targets[4]) {
        render_pass_key.depth_and_color_used |= 1 << 4;
        render_pass_key.color_3_view_format =
            depth_and_color_render_targets[4]->key().GetColorFormat();
      }

      const std::vector<Transfer>* update_transfers = last_update_transfers();
      bool use_draw_pass_transfers = false;
#if XE_PLATFORM_ANDROID
      use_draw_pass_transfers =
          GetAndroidHaloExperiment().menu_transfer_in_draw_pass;
#endif
      if (use_draw_pass_transfers) {
        std::array<std::vector<Transfer>,
                   1 + xenos::kMaxColorRenderTargets>
            fallback_transfers;
        bool fallback_transfer_work = false;
        for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
          const std::vector<Transfer>& transfers = update_transfers[i];
          if (transfers.empty()) {
            continue;
          }
          if (CanQueueDrawPassTransfers(i, depth_and_color_render_targets,
                                        transfers)) {
            pending_draw_pass_render_targets_[i] =
                depth_and_color_render_targets[i];
            pending_draw_pass_transfers_[i] = transfers;
            pending_draw_pass_transfer_mask_ |= uint32_t(1) << i;
#if XE_PLATFORM_ANDROID
            static uint32_t android_draw_pass_transfer_queue_log_count = 0;
            if (android_draw_pass_transfer_queue_log_count++ < 128) {
              const RenderTargetKey dest_key =
                  depth_and_color_render_targets[i]->key();
              XELOGI(
                  "MENU_TRANSFER_IN_DRAW_PASS queued=1 rt_index={} base={} "
                  "pitch={} fmt={} transfers={}",
                  i, uint32_t(dest_key.base_tiles), dest_key.GetPitchTiles(),
                  uint32_t(dest_key.resource_format),
                  uint32_t(transfers.size()));
            }
#endif
          } else {
            fallback_transfers[i] = transfers;
            fallback_transfer_work = true;
          }
        }
        if (fallback_transfer_work) {
          PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                           depth_and_color_render_targets,
                                           fallback_transfers.data());
        }
        if (HasPendingDrawPassTransfers() &&
            !PreflightPendingDrawPassTransfers(render_pass_key)) {
          if (!FlushPendingDrawPassTransfers()) {
            return false;
          }
        }
      }

      const Framebuffer* framebuffer = last_update_framebuffer_;
      VkRenderPass render_pass = last_update_render_pass_key_ == render_pass_key
                                     ? last_update_render_pass_
                                     : VK_NULL_HANDLE;
      if (render_pass == VK_NULL_HANDLE) {
        render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
        if (render_pass == VK_NULL_HANDLE) {
          return false;
        }
        // Framebuffer for a different render pass needed now.
        framebuffer = nullptr;
      }

      uint32_t pitch_tiles_at_32bpp =
          ((rb_surface_info.surface_pitch << uint32_t(
                rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X)) +
           (xenos::kEdramTileWidthSamples - 1)) /
          xenos::kEdramTileWidthSamples;
      if (framebuffer) {
        if (last_update_framebuffer_pitch_tiles_at_32bpp_ !=
                pitch_tiles_at_32bpp ||
            std::memcmp(last_update_framebuffer_attachments_,
                        depth_and_color_render_targets,
                        sizeof(last_update_framebuffer_attachments_))) {
          framebuffer = nullptr;
        }
      }
      if (!framebuffer) {
        framebuffer = GetHostRenderTargetsFramebuffer(
            render_pass_key, pitch_tiles_at_32bpp,
            depth_and_color_render_targets);
        if (!framebuffer) {
          return false;
        }
      }

      // Successful update - write the new configuration.
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = render_pass;
      last_update_framebuffer_pitch_tiles_at_32bpp_ = pitch_tiles_at_32bpp;
      std::memcpy(last_update_framebuffer_attachments_,
                  depth_and_color_render_targets,
                  sizeof(last_update_framebuffer_attachments_));
      last_update_framebuffer_ = framebuffer;

      if (!use_draw_pass_transfers) {
        PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                         depth_and_color_render_targets,
                                         update_transfers);
      }

      // Transition the used render targets.
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        RenderTarget* rt = depth_and_color_render_targets[i];
        if (!rt) {
          continue;
        }
        auto& vulkan_rt = *static_cast<VulkanRenderTarget*>(rt);
        VkPipelineStageFlags rt_dst_stage_mask;
        VkAccessFlags rt_dst_access_mask;
        VkImageLayout rt_new_layout;
        VulkanRenderTarget::GetDrawUsage(i == 0, &rt_dst_stage_mask,
                                         &rt_dst_access_mask, &rt_new_layout);
        command_processor_.PushImageMemoryBarrier(
            vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                i ? VK_IMAGE_ASPECT_COLOR_BIT
                  : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)),
            vulkan_rt.current_stage_mask(), rt_dst_stage_mask,
            vulkan_rt.current_access_mask(), rt_dst_access_mask,
            vulkan_rt.current_layout(), rt_new_layout);
        vulkan_rt.SetUsage(rt_dst_stage_mask, rt_dst_access_mask,
                           rt_new_layout);
      }
      PreparePendingDrawPassTransferBarriers();
#if XE_PLATFORM_ANDROID
      AndroidHaloNoteColorDrawTargets(depth_and_color_render_targets);
#endif
    } break;

    case Path::kPixelShaderInterlock: {
      // For FSI, only the barrier is needed - already scheduled if required.
      // But the buffer will be used for FSI drawing now.
      UseEdramBuffer(EdramBufferUsage::kFragmentReadWrite);
      // Commit preceding unordered (but not FSI) writes like clears as they
      // aren't synchronized with FSI accesses.
      CommitEdramBufferShaderWrites(
          EdramBufferModificationStatus::kViaUnordered);
      // TODO(Triang3l): Check if this draw call modifies color or depth /
      // stencil, at least coarsely, to prevent useless barriers.
      MarkEdramBufferModified(
          EdramBufferModificationStatus::kViaFragmentShaderInterlock);
      last_update_render_pass_key_ = render_pass_key;
      last_update_render_pass_ = fsi_render_pass_;
      last_update_framebuffer_ = &fsi_framebuffer_;
    } break;

    default:
      assert_unhandled_case(GetPath());
      return false;
  }

  return true;
}

void VulkanRenderTargetCache::ClearPendingDrawPassTransfers() {
  for (auto& transfers : pending_draw_pass_transfers_) {
    transfers.clear();
  }
  pending_draw_pass_render_targets_.fill(nullptr);
  pending_draw_pass_transfer_mask_ = 0;
}

bool VulkanRenderTargetCache::BuildTransferRectanglePlans(
    RenderTargetKey dest_key, const std::vector<Transfer>& transfers,
    std::vector<TransferRectanglePlan>& transfer_rectangles_out) const {
  transfer_rectangles_out.clear();
  transfer_rectangles_out.reserve(transfers.size());
  for (const Transfer& transfer : transfers) {
    TransferRectanglePlan plan;
    plan.rectangle_count = transfer.GetRectangles(
        dest_key.base_tiles, dest_key.GetPitchTiles(), dest_key.msaa_samples,
        dest_key.Is64bpp(), plan.rectangles.data(), nullptr);
    if (!plan.rectangle_count) {
      transfer_rectangles_out.clear();
      return false;
    }
    transfer_rectangles_out.push_back(plan);
  }
  return true;
}

bool VulkanRenderTargetCache::CanQueueDrawPassTransfers(
    uint32_t render_target_index, RenderTarget* const* render_targets,
    const std::vector<Transfer>& transfers) const {
  // Keep this first backport intentionally narrow: native RGBA8 color
  // attachments only. All other ownership changes retain the existing path.
  if (!render_targets || transfers.empty() || render_target_index == 0 ||
      render_target_index > xenos::kMaxColorRenderTargets) {
    return false;
  }
  auto* dest_vulkan_rt =
      static_cast<VulkanRenderTarget*>(render_targets[render_target_index]);
  if (!dest_vulkan_rt) {
    return false;
  }
  const RenderTargetKey dest_key = dest_vulkan_rt->key();
  const bool watched_menu_target =
      !dest_key.is_depth && dest_key.base_tiles == 0 &&
      dest_key.GetPitchTiles() == 29 &&
      dest_key.msaa_samples == xenos::MsaaSamples::k1X;
  auto reject = [&](const char* reason) {
#if XE_PLATFORM_ANDROID
    static uint32_t android_draw_pass_transfer_reject_log_count = 0;
    if (watched_menu_target &&
        android_draw_pass_transfer_reject_log_count++ < 128) {
      XELOGI(
          "MENU_TRANSFER_IN_DRAW_PASS queued=0 reason={} rt_index={} base={} "
          "pitch={} fmt={} transfers={}",
          reason, render_target_index, uint32_t(dest_key.base_tiles),
          dest_key.GetPitchTiles(), uint32_t(dest_key.resource_format),
          uint32_t(transfers.size()));
    }
#endif
    return false;
  };
  if (dest_key.is_depth ||
      (dest_key.GetColorFormat() !=
           xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
       dest_key.GetColorFormat() !=
           xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
    return reject("dest_format");
  }

  bool dest_is_integer = false;
  const VkFormat transfer_format = GetColorOwnershipTransferVulkanFormat(
      dest_key.GetColorFormat(), &dest_is_integer);
  if (dest_is_integer ||
      transfer_format != GetColorVulkanFormat(dest_key.GetColorFormat()) ||
      dest_vulkan_rt->view_color_transfer() !=
          dest_vulkan_rt->view_depth_color()) {
    return reject("dest_view");
  }

  auto is_active_draw_pass_rt = [&](const RenderTarget* rt) {
    if (!rt) {
      return false;
    }
    for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
      if (render_targets[i] == rt) {
        return true;
      }
    }
    return false;
  };

  for (const Transfer& transfer : transfers) {
    if (!transfer.source) {
      return reject("source_null");
    }
    if (transfer.source == dest_vulkan_rt) {
      return reject("source_is_dest");
    }
    if (is_active_draw_pass_rt(transfer.source)) {
      return reject("source_is_active_attachment");
    }
    if (transfer.host_depth_source) {
      return reject("host_depth_source");
    }
    auto* source_vulkan_rt =
        static_cast<VulkanRenderTarget*>(transfer.source);
    if (source_vulkan_rt->key().is_depth &&
        !source_vulkan_rt->view_depth_stencil()) {
      return reject("source_depth_view");
    }
  }

  std::vector<TransferRectanglePlan> transfer_rectangle_plans;
  if (!BuildTransferRectanglePlans(dest_key, transfers,
                                   transfer_rectangle_plans)) {
    return reject("rectangles");
  }
  return true;
}

bool VulkanRenderTargetCache::PreflightPendingDrawPassTransfers(
    RenderPassKey render_pass_key) {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }

  std::vector<TransferRectanglePlan> transfer_rectangle_plans;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }
    auto* dest_vulkan_rt =
        static_cast<VulkanRenderTarget*>(pending_draw_pass_render_targets_[i]);
    if (!dest_vulkan_rt || pending_draw_pass_transfers_[i].empty()) {
      return false;
    }
    const RenderTargetKey dest_key = dest_vulkan_rt->key();
    if (dest_key.is_depth ||
        !BuildTransferRectanglePlans(dest_key,
                                     pending_draw_pass_transfers_[i],
                                     transfer_rectangle_plans)) {
      return false;
    }
    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto* source_vulkan_rt =
          static_cast<VulkanRenderTarget*>(transfer.source);
      if (!source_vulkan_rt || transfer.host_depth_source) {
        return false;
      }
      TransferShaderKey shader_key;
      shader_key.dest_msaa_samples = dest_key.msaa_samples;
      shader_key.dest_color_rt_index = i - 1;
      shader_key.dest_resource_format = dest_key.resource_format;
      shader_key.source_msaa_samples =
          source_vulkan_rt->key().msaa_samples;
      shader_key.host_depth_source_msaa_samples =
          xenos::MsaaSamples::k1X;
      shader_key.source_resource_format =
          source_vulkan_rt->key().resource_format;
      shader_key.mode = source_vulkan_rt->key().is_depth
                            ? TransferMode::kDepthToColor
                            : TransferMode::kColorToColor;
      if (!GetTransferPipelines(
              TransferPipelineKey(render_pass_key, shader_key))) {
        return false;
      }
    }
  }
  return true;
}

void VulkanRenderTargetCache::PreparePendingDrawPassTransferBarriers() {
  if (!HasPendingDrawPassTransfers()) {
    return;
  }

  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  std::vector<VulkanRenderTarget*> source_rts;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    if (!(pending_draw_pass_transfer_mask_ & (uint32_t(1) << i))) {
      continue;
    }
    for (const Transfer& transfer : pending_draw_pass_transfers_[i]) {
      auto* source_vulkan_rt =
          static_cast<VulkanRenderTarget*>(transfer.source);
      if (source_vulkan_rt &&
          std::find(source_rts.begin(), source_rts.end(), source_vulkan_rt) ==
              source_rts.end()) {
        source_rts.push_back(source_vulkan_rt);
      }
    }
  }

  for (VulkanRenderTarget* source_vulkan_rt : source_rts) {
    command_processor_.PushImageMemoryBarrier(
        source_vulkan_rt->image(),
        ui::vulkan::util::InitializeSubresourceRange(
            source_vulkan_rt->key().is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        source_vulkan_rt->current_stage_mask(), kSourceStageMask,
        source_vulkan_rt->current_access_mask(), kSourceAccessMask,
        source_vulkan_rt->current_layout(), kSourceLayout);
    source_vulkan_rt->SetUsage(kSourceStageMask, kSourceAccessMask,
                               kSourceLayout);
  }
}

bool VulkanRenderTargetCache::EncodePendingDrawPassTransfers() {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  if (!PreflightPendingDrawPassTransfers(last_update_render_pass_key_)) {
    return false;
  }
  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                   pending_draw_pass_render_targets_.data(),
                                   pending_draw_pass_transfers_.data(), nullptr,
                                   nullptr, true);
#if XE_PLATFORM_ANDROID
  static uint32_t android_draw_pass_transfer_encode_log_count = 0;
  if (android_draw_pass_transfer_encode_log_count++ < 128) {
    XELOGI("MENU_TRANSFER_IN_DRAW_PASS encoded=1 mask=0x{:X}",
           pending_draw_pass_transfer_mask_);
  }
#endif
  ClearPendingDrawPassTransfers();
  return true;
}

bool VulkanRenderTargetCache::FlushPendingDrawPassTransfers() {
  if (!HasPendingDrawPassTransfers()) {
    return true;
  }
  PerformTransfersAndResolveClears(1 + xenos::kMaxColorRenderTargets,
                                   pending_draw_pass_render_targets_.data(),
                                   pending_draw_pass_transfers_.data());
  ClearPendingDrawPassTransfers();
  return true;
}

VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(
    RenderPassKey key) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  auto it = render_passes_.find(key);
  if (it != render_passes_.end()) {
    return it->second;
  }

  VkSampleCountFlagBits samples;
  switch (key.msaa_samples) {
    case xenos::MsaaSamples::k1X:
      samples = VK_SAMPLE_COUNT_1_BIT;
      break;
    case xenos::MsaaSamples::k2X:
      samples = IsMsaa2xSupported(key.depth_and_color_used != 0)
                    ? VK_SAMPLE_COUNT_2_BIT
                    : VK_SAMPLE_COUNT_4_BIT;
      break;
    case xenos::MsaaSamples::k4X:
      samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    default:
      return VK_NULL_HANDLE;
  }

  VkAttachmentDescription attachments[1 + xenos::kMaxColorRenderTargets];
  if (key.depth_and_color_used & 0b1) {
    VkAttachmentDescription& attachment = attachments[0];
    attachment.flags = 0;
    attachment.format = GetDepthVulkanFormat(key.depth_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VulkanRenderTarget::kDepthDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kDepthDrawLayout;
  }
  VkAttachmentReference color_attachments[xenos::kMaxColorRenderTargets];
  xenos::ColorRenderTargetFormat color_formats[] = {
      key.color_0_view_format,
      key.color_1_view_format,
      key.color_2_view_format,
      key.color_3_view_format,
  };
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkAttachmentReference& color_attachment = color_attachments[i];
    color_attachment.layout = VulkanRenderTarget::kColorDrawLayout;
    uint32_t attachment_bit = uint32_t(1) << (1 + i);
    if (!(key.depth_and_color_used & attachment_bit)) {
      color_attachment.attachment = VK_ATTACHMENT_UNUSED;
      continue;
    }
    uint32_t attachment_index =
        xe::bit_count(key.depth_and_color_used & (attachment_bit - 1));
    color_attachment.attachment = attachment_index;
    VkAttachmentDescription& attachment = attachments[attachment_index];
    attachment.flags = 0;
    xenos::ColorRenderTargetFormat color_format = color_formats[i];
    attachment.format =
        key.color_rts_use_transfer_formats
            ? GetColorOwnershipTransferVulkanFormat(color_format)
            : GetColorVulkanFormat(color_format);
    attachment.samples = samples;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VulkanRenderTarget::kColorDrawLayout;
    attachment.finalLayout = VulkanRenderTarget::kColorDrawLayout;
  }

  VkAttachmentReference depth_stencil_attachment;
  depth_stencil_attachment.attachment =
      (key.depth_and_color_used & 0b1) ? 0 : VK_ATTACHMENT_UNUSED;
  depth_stencil_attachment.layout = VulkanRenderTarget::kDepthDrawLayout;

  VkSubpassDescription subpass;
  subpass.flags = 0;
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.inputAttachmentCount = 0;
  subpass.pInputAttachments = nullptr;
  subpass.colorAttachmentCount =
      32 - xe::lzcnt(uint32_t(key.depth_and_color_used >> 1));
  subpass.pColorAttachments = color_attachments;
  subpass.pResolveAttachments = nullptr;
  subpass.pDepthStencilAttachment =
      (key.depth_and_color_used & 0b1) ? &depth_stencil_attachment : nullptr;
  subpass.preserveAttachmentCount = 0;
  subpass.pPreserveAttachments = nullptr;

  VkPipelineStageFlags dependency_stage_mask = 0;
  VkAccessFlags dependency_access_mask = 0;
  if (key.depth_and_color_used & 0b1) {
    dependency_stage_mask |= VulkanRenderTarget::kDepthDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kDepthDrawAccessMask;
  }
  if (key.depth_and_color_used >> 1) {
    dependency_stage_mask |= VulkanRenderTarget::kColorDrawStageMask;
    dependency_access_mask |= VulkanRenderTarget::kColorDrawAccessMask;
  }
  VkSubpassDependency subpass_dependencies[2];
  subpass_dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[0].dstSubpass = 0;
  subpass_dependencies[0].srcStageMask = dependency_stage_mask;
  subpass_dependencies[0].dstStageMask = dependency_stage_mask;
  subpass_dependencies[0].srcAccessMask = dependency_access_mask;
  subpass_dependencies[0].dstAccessMask = dependency_access_mask;
  VkDependencyFlags dependency_flags = VK_DEPENDENCY_BY_REGION_BIT;
#if XE_PLATFORM_ANDROID
  if (cvars::halo_android_diag_strict_barriers) {
    dependency_flags = 0;
  }
#endif
  subpass_dependencies[0].dependencyFlags = dependency_flags;
  subpass_dependencies[1].srcSubpass = 0;
  subpass_dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  subpass_dependencies[1].srcStageMask = dependency_stage_mask;
  subpass_dependencies[1].dstStageMask = dependency_stage_mask;
  subpass_dependencies[1].srcAccessMask = dependency_access_mask;
  subpass_dependencies[1].dstAccessMask = dependency_access_mask;
  subpass_dependencies[1].dependencyFlags = dependency_flags;

  VkRenderPassCreateInfo render_pass_create_info;
  render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  render_pass_create_info.pNext = nullptr;
  render_pass_create_info.flags = 0;
  render_pass_create_info.attachmentCount =
      xe::bit_count(key.depth_and_color_used);
  render_pass_create_info.pAttachments = attachments;
  render_pass_create_info.subpassCount = 1;
  render_pass_create_info.pSubpasses = &subpass;
  render_pass_create_info.dependencyCount =
      key.depth_and_color_used ? uint32_t(xe::countof(subpass_dependencies))
                               : 0;
  render_pass_create_info.pDependencies = subpass_dependencies;

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  VkRenderPass render_pass;
  if (dfn.vkCreateRenderPass(device, &render_pass_create_info, nullptr,
                             &render_pass) != VK_SUCCESS) {
    XELOGE("VulkanRenderTargetCache: Failed to create a render pass");
    render_passes_.emplace(key, VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }
  render_passes_.emplace(key, render_pass);
  return render_pass;
}

VkFormat VulkanRenderTargetCache::GetDepthVulkanFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24S8 &&
      depth_unorm24_vulkan_format_supported()) {
    return VK_FORMAT_D24_UNORM_S8_UINT;
  }
  return VK_FORMAT_D32_SFLOAT_S8_UINT;
}

VkFormat VulkanRenderTargetCache::GetColorVulkanFormat(
    xenos::ColorRenderTargetFormat format) const {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
      return gamma_render_target_as_unorm16_ ? VK_FORMAT_R16G16B16A16_UNORM
                                             : VK_FORMAT_R8G8B8A8_UNORM;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
#if XE_PLATFORM_ANDROID
      if (GetAndroidHaloExperiment().force_1010102_rt_as_rgba8) {
        return VK_FORMAT_R8G8B8A8_UNORM;
      }
#endif
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
#if XE_PLATFORM_ANDROID
      if (scaled_unorm_7e3_render_target_mode_ == 1) {
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      }
      if (scaled_unorm_7e3_render_target_mode_ == 2) {
        return VK_FORMAT_R16G16B16A16_UNORM;
      }
      if (GetAndroidHaloExperiment().rgba32f_for_7e3_render_targets) {
        return VK_FORMAT_R32G32B32A32_SFLOAT;
      }
#endif
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16:
      // SNORM16 color attachments are optional in Vulkan; fall back to float16
      // when unsupported (disregarding clearing correctness likely).
      return snorm16_color_attachments_supported_ ? VK_FORMAT_R16G16_SNORM
                                                  : VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return snorm16_color_attachments_supported_
                 ? VK_FORMAT_R16G16B16A16_SNORM
                 : VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_SFLOAT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_SFLOAT;
    default:
      assert_unhandled_case(format);
      return VK_FORMAT_UNDEFINED;
  }
}

VkFormat VulkanRenderTargetCache::GetColorOwnershipTransferVulkanFormat(
    xenos::ColorRenderTargetFormat format, bool* is_integer_out) const {
  if (is_integer_out) {
    *is_integer_out = true;
  }
  // Floating-point numbers have NaNs that need to be propagated without
  // modifications to the bit representation, and SNORM has two representations
  // of -1.
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_16_16:
    case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      return VK_FORMAT_R16G16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      return VK_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
#if XE_PLATFORM_ANDROID
      // WO25: test whether Adreno's driver mishandles the UINT-reinterpreting
      // view for this specific format (same convention D3D12 uses and works
      // on PC) by bypassing it - sample through the native SFLOAT view
      // instead. See android_halo_experiment.h for the full rationale.
      if (GetAndroidHaloExperiment().native_float_view_for_16bpc_transfer) {
        if (is_integer_out) {
          *is_integer_out = false;
        }
        return GetColorVulkanFormat(format);
      }
#endif
      return VK_FORMAT_R16G16B16A16_UINT;
    case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      return VK_FORMAT_R32_UINT;
    case xenos::ColorRenderTargetFormat::k_32_32_FLOAT:
      return VK_FORMAT_R32G32_UINT;
    default:
      if (is_integer_out) {
        *is_integer_out = false;
      }
      return GetColorVulkanFormat(format);
  }
}

VulkanRenderTargetCache::VulkanRenderTarget::~VulkanRenderTarget() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      render_target_cache_.command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key().is_depth
          ? *render_target_cache_.descriptor_set_pool_sampled_image_x2_
          : *render_target_cache_.descriptor_set_pool_sampled_image_;
  descriptor_set_pool.Free(descriptor_set_index_transfer_source_);
  if (view_color_transfer_separate_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_color_transfer_separate_, nullptr);
  }
  if (view_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_stencil_, nullptr);
  }
  if (view_depth_stencil_ != VK_NULL_HANDLE) {
    dfn.vkDestroyImageView(device, view_depth_stencil_, nullptr);
  }
  dfn.vkDestroyImageView(device, view_depth_color_, nullptr);
  dfn.vkDestroyImage(device, image_, nullptr);
  dfn.vkFreeMemory(device, memory_, nullptr);
  assert_true(memory_size_ <=
              render_target_cache_.render_target_memory_usage_bytes_);
  render_target_cache_.render_target_memory_usage_bytes_ -= memory_size_;
}

void VulkanRenderTargetCache::MaybeRequestRenderTargetMemoryClear() {
  const uint32_t limit_mb = cvars::vulkan_render_target_memory_limit_mb;
  const VkDeviceSize limit_bytes = VkDeviceSize(limit_mb) << 20;
  if (!limit_bytes || render_target_memory_clear_requested_ ||
      render_target_memory_usage_bytes_ <= limit_bytes) {
    return;
  }
  render_target_memory_clear_requested_ = true;
  XELOGW(
      "Vulkan render target cache reached {} MB (limit {} MB); requesting "
      "cache reclamation",
      (render_target_memory_usage_bytes_ + ((VkDeviceSize(1) << 20) - 1)) >> 20,
      limit_mb);
  command_processor_.ClearCaches();
}

bool VulkanRenderTargetCache::IsGammaFormatHostStorageSeparate() const {
  return gamma_render_target_as_unorm16_;
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetWidth() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferWidth,
                  device_properties.maxImageDimension2D);
}

uint32_t VulkanRenderTargetCache::GetMaxRenderTargetHeight() const {
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      command_processor_.GetVulkanDevice()->properties();
  return std::min(device_properties.maxFramebufferHeight,
                  device_properties.maxImageDimension2D);
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(
    RenderTargetKey key) {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Create the image.

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.extent.width = key.GetWidth() * draw_resolution_scale_x();
  image_create_info.extent.height =
      GetRenderTargetHeight(key.pitch_tiles_at_32bpp, key.msaa_samples) *
      draw_resolution_scale_y();
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  if (key.msaa_samples == xenos::MsaaSamples::k2X &&
      !msaa_2x_attachments_supported_) {
    image_create_info.samples = VK_SAMPLE_COUNT_4_BIT;
  } else {
    image_create_info.samples =
        VkSampleCountFlagBits(uint32_t(1) << uint32_t(key.msaa_samples));
  }
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkFormat transfer_format;
  if (key.is_depth) {
    image_create_info.format = GetDepthVulkanFormat(key.GetDepthFormat());
    transfer_format = image_create_info.format;
    image_create_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  } else {
    xenos::ColorRenderTargetFormat color_format = key.GetColorFormat();
    image_create_info.format = GetColorVulkanFormat(color_format);
    transfer_format = GetColorOwnershipTransferVulkanFormat(color_format);
    if (image_create_info.format != transfer_format) {
      image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    }
    image_create_info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  if (image_create_info.format == VK_FORMAT_UNDEFINED) {
    XELOGE("VulkanRenderTargetCache: Unknown {} render target format {}",
           key.is_depth ? "depth" : "color", key.resource_format);
    return nullptr;
  }
  VkImage image;
  VkDeviceMemory memory;
  VkDeviceSize memory_size;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, image, memory, nullptr,
          &memory_size)) {
    XELOGE(
        "VulkanRenderTarget: Failed to create a {}x{} {}xMSAA {} render target "
        "image",
        image_create_info.extent.width, image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    return nullptr;
  }

  // Create the image views.

  VkImageViewCreateInfo view_create_info;
  view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_create_info.pNext = nullptr;
  view_create_info.flags = 0;
  view_create_info.image = image;
  view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_create_info.format = image_create_info.format;
  view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  view_create_info.subresourceRange =
      ui::vulkan::util::InitializeSubresourceRange(
          key.is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
  VkImageView view_depth_color;
  if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                            &view_depth_color) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTarget: Failed to create a {} view for a {}x{} {}xMSAA {} "
        "render target",
        key.is_depth ? "depth" : "color", image_create_info.extent.width,
        image_create_info.extent.height,
        uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkImageView view_depth_stencil = VK_NULL_HANDLE;
  VkImageView view_stencil = VK_NULL_HANDLE;
  VkImageView view_color_transfer_separate = VK_NULL_HANDLE;
  if (key.is_depth) {
    view_create_info.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                              &view_depth_stencil) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTarget: Failed to create a depth / stencil view for a "
          "{}x{} {}xMSAA {} render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
    view_create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                              &view_stencil) != VK_SUCCESS) {
      XELOGE(
          "VulkanRenderTarget: Failed to create a stencil view for a {}x{} "
          "{}xMSAA render target",
          image_create_info.extent.width, image_create_info.extent.height,
          uint32_t(1) << uint32_t(key.msaa_samples),
          xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat()));
      dfn.vkDestroyImageView(device, view_depth_stencil, nullptr);
      dfn.vkDestroyImageView(device, view_depth_color, nullptr);
      dfn.vkDestroyImage(device, image, nullptr);
      dfn.vkFreeMemory(device, memory, nullptr);
      return nullptr;
    }
  } else {
    if (transfer_format != image_create_info.format) {
      view_create_info.format = transfer_format;
      if (dfn.vkCreateImageView(device, &view_create_info, nullptr,
                                &view_color_transfer_separate) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTarget: Failed to create a transfer view for a {}x{} "
            "{}xMSAA {} render target",
            image_create_info.extent.width, image_create_info.extent.height,
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetFormatName());
        dfn.vkDestroyImageView(device, view_depth_color, nullptr);
        dfn.vkDestroyImage(device, image, nullptr);
        dfn.vkFreeMemory(device, memory, nullptr);
        return nullptr;
      }
    }
  }

  ui::vulkan::SingleLayoutDescriptorSetPool& descriptor_set_pool =
      key.is_depth ? *descriptor_set_pool_sampled_image_x2_
                   : *descriptor_set_pool_sampled_image_;
  size_t descriptor_set_index_transfer_source = descriptor_set_pool.Allocate();
  if (descriptor_set_index_transfer_source == SIZE_MAX) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to allocate sampled image descriptors "
        "for a {} render target",
        key.is_depth ? "depth/stencil" : "color");
    if (view_color_transfer_separate != VK_NULL_HANDLE) {
      dfn.vkDestroyImageView(device, view_color_transfer_separate, nullptr);
    }
    dfn.vkDestroyImageView(device, view_depth_color, nullptr);
    dfn.vkDestroyImage(device, image, nullptr);
    dfn.vkFreeMemory(device, memory, nullptr);
    return nullptr;
  }
  VkDescriptorSet descriptor_set_transfer_source =
      descriptor_set_pool.Get(descriptor_set_index_transfer_source);
  VkWriteDescriptorSet descriptor_set_write[2];
  VkDescriptorImageInfo descriptor_set_write_depth_color;
  descriptor_set_write_depth_color.sampler = VK_NULL_HANDLE;
  descriptor_set_write_depth_color.imageView =
      view_color_transfer_separate != VK_NULL_HANDLE
          ? view_color_transfer_separate
          : view_depth_color;
  descriptor_set_write_depth_color.imageLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  descriptor_set_write[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptor_set_write[0].pNext = nullptr;
  descriptor_set_write[0].dstSet = descriptor_set_transfer_source;
  descriptor_set_write[0].dstBinding = 0;
  descriptor_set_write[0].dstArrayElement = 0;
  descriptor_set_write[0].descriptorCount = 1;
  descriptor_set_write[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  descriptor_set_write[0].pImageInfo = &descriptor_set_write_depth_color;
  descriptor_set_write[0].pBufferInfo = nullptr;
  descriptor_set_write[0].pTexelBufferView = nullptr;
  VkDescriptorImageInfo descriptor_set_write_stencil;
  if (key.is_depth) {
    descriptor_set_write_stencil.sampler = VK_NULL_HANDLE;
    descriptor_set_write_stencil.imageView = view_stencil;
    descriptor_set_write_stencil.imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_set_write[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write[1].pNext = nullptr;
    descriptor_set_write[1].dstSet = descriptor_set_transfer_source;
    descriptor_set_write[1].dstBinding = 1;
    descriptor_set_write[1].dstArrayElement = 0;
    descriptor_set_write[1].descriptorCount = 1;
    descriptor_set_write[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write[1].pImageInfo = &descriptor_set_write_stencil;
    descriptor_set_write[1].pBufferInfo = nullptr;
    descriptor_set_write[1].pTexelBufferView = nullptr;
  }
  dfn.vkUpdateDescriptorSets(device, key.is_depth ? 2 : 1, descriptor_set_write,
                             0, nullptr);

  auto* render_target = new VulkanRenderTarget(
      key, *this, image, memory, memory_size, view_depth_color,
      view_depth_stencil, view_stencil, view_color_transfer_separate,
      descriptor_set_index_transfer_source);
  render_target_memory_usage_bytes_ += memory_size;
  MaybeRequestRenderTargetMemoryClear();
  return render_target;
}

bool VulkanRenderTargetCache::IsHostDepthEncodingDifferent(
    xenos::DepthRenderTargetFormat format) const {
  // TODO(Triang3l): Conversion directly in shaders.
  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24S8:
      return !depth_unorm24_vulkan_format_supported();
    case xenos::DepthRenderTargetFormat::kD24FS8:
      return true;
  }
  return false;
}

void VulkanRenderTargetCache::RequestPixelShaderInterlockBarrier() {
  if (edram_buffer_usage_ == EdramBufferUsage::kFragmentReadWrite) {
    CommitEdramBufferShaderWrites();
  }
}

void VulkanRenderTargetCache::GetEdramBufferUsageMasks(
    EdramBufferUsage usage, VkPipelineStageFlags& stage_mask_out,
    VkAccessFlags& access_mask_out) {
  switch (usage) {
    case EdramBufferUsage::kFragmentRead:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kFragmentReadWrite:
      stage_mask_out = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kComputeRead:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_READ_BIT;
      break;
    case EdramBufferUsage::kComputeWrite:
      stage_mask_out = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask_out = VK_ACCESS_SHADER_WRITE_BIT;
      break;
    case EdramBufferUsage::kTransferRead:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case EdramBufferUsage::kTransferWrite:
      stage_mask_out = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask_out = VK_ACCESS_TRANSFER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanRenderTargetCache::UseEdramBuffer(EdramBufferUsage new_usage) {
  if (edram_buffer_usage_ == new_usage) {
    return;
  }
  VkPipelineStageFlags src_stage_mask, dst_stage_mask;
  VkAccessFlags src_access_mask, dst_access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, src_stage_mask,
                           src_access_mask);
  GetEdramBufferUsageMasks(new_usage, dst_stage_mask, dst_access_mask);
  if (command_processor_.PushBufferMemoryBarrier(
          edram_buffer_, 0, VK_WHOLE_SIZE, src_stage_mask, dst_stage_mask,
          src_access_mask, dst_access_mask)) {
    // Resetting edram_buffer_modification_status_ only if the barrier has been
    // truly inserted.
    edram_buffer_modification_status_ =
        EdramBufferModificationStatus::kUnmodified;
  }
  edram_buffer_usage_ = new_usage;
}

void VulkanRenderTargetCache::MarkEdramBufferModified(
    EdramBufferModificationStatus modification_status) {
  assert_true(modification_status !=
              EdramBufferModificationStatus::kUnmodified);
  switch (edram_buffer_usage_) {
    case EdramBufferUsage::kFragmentReadWrite:
      // max because being modified via unordered access requires stricter
      // synchronization than via fragment shader interlocks.
      edram_buffer_modification_status_ =
          std::max(edram_buffer_modification_status_, modification_status);
      break;
    case EdramBufferUsage::kComputeWrite:
      assert_true(modification_status ==
                  EdramBufferModificationStatus::kViaUnordered);
      modification_status = EdramBufferModificationStatus::kViaUnordered;
      break;
    default:
      assert_always(
          "While changing the usage of the EDRAM buffer before marking it as "
          "modified is handled safely (but will cause spurious marking as "
          "modified after the changes have been implicitly committed by the "
          "usage switch), normally that shouldn't be done and is an "
          "indication of architectural mistakes. Alternatively, this may "
          "indicate that the usage switch has been forgotten before writing, "
          "which is a clearly invalid situation.");
  }
}

void VulkanRenderTargetCache::CommitEdramBufferShaderWrites(
    EdramBufferModificationStatus commit_status) {
  assert_true(commit_status != EdramBufferModificationStatus::kUnmodified);
  if (edram_buffer_modification_status_ < commit_status) {
    return;
  }
  VkPipelineStageFlags stage_mask;
  VkAccessFlags access_mask;
  GetEdramBufferUsageMasks(edram_buffer_usage_, stage_mask, access_mask);
  assert_not_zero(access_mask & VK_ACCESS_SHADER_WRITE_BIT);
  command_processor_.PushBufferMemoryBarrier(
      edram_buffer_, 0, VK_WHOLE_SIZE, stage_mask, stage_mask, access_mask,
      access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  edram_buffer_modification_status_ =
      EdramBufferModificationStatus::kUnmodified;
  PixelShaderInterlockFullEdramBarrierPlaced();
}

#if XE_PLATFORM_ANDROID
namespace {

bool IsAndroidHalo1010102ColorFormat(xenos::ColorRenderTargetFormat format) {
  switch (format) {
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return true;
    default:
      return false;
  }
}

bool IsAndroidHalo8888ColorFormat(xenos::ColorRenderTargetFormat format) {
  return format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
         format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA;
}

void StoreAndroidHalo1010102To8888ValueConvert(
    spv::Builder& builder, spv::Id (&source_color_sample)[4],
    spv::Id type_fragment_data, spv::Id output_fragment_data,
    std::vector<spv::Id>& id_vector_temp) {
  id_vector_temp.clear();
  // Preserve true RGBA. The old R,W,B,W pack killed green (byte1=alpha) and
  // forced the 0xAC2 present remap. With writer_gb_fix + 0xA42 present path,
  // keep real G so menus and lit scene colors can be correct.
  id_vector_temp.push_back(source_color_sample[0]);
  id_vector_temp.push_back(source_color_sample[1]);
  id_vector_temp.push_back(source_color_sample[2]);
  id_vector_temp.push_back(source_color_sample[3]);
  builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                       id_vector_temp),
                      output_fragment_data);
}

}  // namespace

VkImageView VulkanRenderTargetCache::RequestAndroidHaloHostSwapTexture(
    uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    uint32_t& width_scaled_out, uint32_t& height_scaled_out,
    xenos::TextureFormat& format_out) {
  static uint32_t request_count = 0;
  static uint32_t success_count = 0;
  ++request_count;
  const bool log_request = request_count <= 16 || (request_count % 300) == 0;

  const char* rejection = nullptr;
  if (!IsAndroidHaloCompatActive()) {
    rejection = "compat_inactive";
  } else if (frontbuffer_width != 1152 || frontbuffer_height != 720) {
    rejection = "frontbuffer_extent";
  } else if (android_halo_presentable_color_rt_ == nullptr) {
    rejection = "no_tracked_rt";
  }

  VulkanRenderTarget* source_rt = android_halo_presentable_color_rt_;
  RenderTargetKey source_key{};
  if (!rejection) {
    source_key = source_rt->key();
    if (!IsAndroidHaloPresentableColorKey(source_key) ||
        !IsAndroidHalo8888ColorFormat(source_key.GetColorFormat())) {
      rejection = "tracked_rt_not_8888";
    } else if (source_rt->view_depth_color() == VK_NULL_HANDLE) {
      rejection = "no_color_view";
    }
  }

  if (rejection) {
    if (log_request) {
      XELOGI(
          "HaloCompat host present fallback count={} reason={} fb={}x{} "
          "tracked_rt=0x{:016X}",
          request_count, rejection, frontbuffer_width, frontbuffer_height,
          uint64_t(reinterpret_cast<uintptr_t>(source_rt)));
    }
    return VK_NULL_HANDLE;
  }

  command_processor_.PushImageMemoryBarrier(
      source_rt->image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
      source_rt->current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      source_rt->current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
      source_rt->current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, true);
  source_rt->SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  width_scaled_out = frontbuffer_width * draw_resolution_scale_x();
  height_scaled_out = frontbuffer_height * draw_resolution_scale_y();
  format_out = xenos::TextureFormat::k_8_8_8_8;
  ++success_count;
  if (success_count <= 16 || (success_count % 300) == 0) {
    XELOGI(
        "HaloCompat host present source count={} request={} rt_base={} "
        "rt_pitch={} rt_fmt={} rt_color_fmt={} source={}x{} output={}x{}",
        success_count, request_count, uint32_t(source_key.base_tiles),
        source_key.GetPitchTiles(), uint32_t(source_key.resource_format),
        uint32_t(source_key.GetColorFormat()), source_key.GetWidth(),
        GetRenderTargetHeight(source_key.pitch_tiles_at_32bpp,
                              source_key.msaa_samples),
        width_scaled_out, height_scaled_out);
  }
  return source_rt->view_depth_color();
}

bool VulkanRenderTargetCache::IsAndroidHaloCompatActive() const {
  return cvars::halo_android_compat_presentable_color_shadow &&
         GetPath() == Path::kHostRenderTargets &&
         android_halo_present_shadow_buffer_ != VK_NULL_HANDLE;
}

bool VulkanRenderTargetCache::IsAndroidHaloShadowSpan(
    uint32_t base, uint32_t row_length_used, uint32_t rows, uint32_t pitch) {
  return base == kAndroidHaloShadowBaseTiles &&
         row_length_used == kAndroidHaloShadowRowLengthTiles &&
         rows == kAndroidHaloShadowRows &&
         pitch == kAndroidHaloShadowPitchTiles;
}

bool VulkanRenderTargetCache::IsAndroidHaloPresentableColorKey(
    const RenderTargetKey& key) {
  if (key.is_depth || key.base_tiles != kAndroidHaloShadowBaseTiles ||
      key.GetPitchTiles() != kAndroidHaloShadowPitchTiles ||
      key.msaa_samples != xenos::MsaaSamples::k1X ||
      key.GetWidth() != kAndroidHaloShadowPitchTiles *
                            xenos::kEdramTileWidthSamples) {
    return false;
  }
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_8_8_8_8:
    case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
      return true;
    default:
      return false;
  }
}

bool VulkanRenderTargetCache::IsAndroidHaloMsaaSceneColorKey(
    const RenderTargetKey& key) {
  if (key.is_depth || key.base_tiles != kAndroidHaloShadowBaseTiles ||
      key.GetPitchTiles() != kAndroidHaloShadowPitchTiles ||
      key.msaa_samples != xenos::MsaaSamples::k4X ||
      key.GetWidth() != 600) {
    return false;
  }
  switch (key.GetColorFormat()) {
    case xenos::ColorRenderTargetFormat::k_2_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
    case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
    case xenos::ColorRenderTargetFormat::
        k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return true;
    default:
      return false;
  }
}

const char* VulkanRenderTargetCache::GetAndroidHaloOwnerKindName(
    AndroidHaloOwnerKind kind) {
  switch (kind) {
    case AndroidHaloOwnerKind::kUnknown:
      return "unknown";
    case AndroidHaloOwnerKind::kPresentableColor:
      return "presentable";
    case AndroidHaloOwnerKind::kDepthColorAliasNonPresentable:
      return "depth_alias";
  }
  return "invalid";
}

void VulkanRenderTargetCache::AndroidHaloNoteColorDrawTargets(
    RenderTarget* const* depth_and_color_render_targets) {
  if (!IsAndroidHaloCompatActive()) {
    return;
  }
  if (android_halo_transfer_render_pass_active_) {
    if (android_halo_presentable_owner_suppressed_count_ < 64) {
      XELOGI(
          "HaloCompat presentable_owner suppressed: transfer_render_pass=1 "
          "owner_kind={} shadow_valid={}",
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_present_shadow_valid_));
      ++android_halo_presentable_owner_suppressed_count_;
    }
    AndroidHaloNotifyMsaaSceneDrawFrame(false);
    return;
  }
  if (android_halo_depth_alias_quarantine_latched_) {
    if (android_halo_presentable_owner_suppressed_count_ < 64) {
      XELOGI(
          "HaloCompat presentable_owner recovery check: "
          "depth_alias_latched=1 owner_kind={} shadow_valid={}",
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_present_shadow_valid_));
      ++android_halo_presentable_owner_suppressed_count_;
    }
  }
  bool msaa_scene_draw_this_frame = false;
  bool presentable_8888_this_frame = false;
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* rt = depth_and_color_render_targets[i];
    if (!rt) {
      continue;
    }
    const RenderTargetKey key = rt->key();
    if (IsAndroidHaloMsaaSceneColorKey(key)) {
      msaa_scene_draw_this_frame = true;
      android_halo_msaa_scene_color_rt_ =
          static_cast<VulkanRenderTarget*>(rt);
    }
    if (!IsAndroidHaloPresentableColorKey(key)) {
      continue;
    }
    switch (key.GetColorFormat()) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        presentable_8888_this_frame = true;
        break;
      default:
        break;
    }
  }
  const bool gameplay_scene_draw_this_frame = msaa_scene_draw_this_frame;
  if (presentable_8888_this_frame && !gameplay_scene_draw_this_frame &&
      AndroidHaloGameplayPresentFramesRemaining().load() == 0) {
    AndroidHaloFramesSinceMenuUi().store(0);
    AndroidHaloGameplayDrawStreak().store(0);
  }
  for (uint32_t i = 1; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
    RenderTarget* rt = depth_and_color_render_targets[i];
    if (!rt) {
      continue;
    }
    const RenderTargetKey key = rt->key();
    if (!IsAndroidHaloPresentableColorKey(key)) {
      continue;
    }
    if (GetAndroidHaloExperiment().log_owner_history) {
      static uint32_t owner_trace_color_draw_count = 0;
      if (owner_trace_color_draw_count < 512) {
        XELOGI(
            "OWNER_TRACE event=color_draw count={} slot={} owner_before={} "
            "latched={} shadow_valid={} rt_ptr=0x{:016X} base={} pitch={} "
            "fmt={} color_fmt={} msaa={} width={} height={}",
            owner_trace_color_draw_count, i,
            GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
            uint32_t(android_halo_depth_alias_quarantine_latched_),
            uint32_t(android_halo_present_shadow_valid_),
            uint64_t(reinterpret_cast<uintptr_t>(rt)),
            uint32_t(key.base_tiles), key.GetPitchTiles(),
            uint32_t(key.resource_format), uint32_t(key.GetColorFormat()),
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetWidth(),
            GetRenderTargetHeight(key.pitch_tiles_at_32bpp,
                                  key.msaa_samples));
        ++owner_trace_color_draw_count;
      }
    }
    static uint32_t presentable_fmt_log_count = 0;
    if (presentable_fmt_log_count < 150) {
      XELOGI("PRESENTABLE_FMT resource_format={} color_format={}",
             uint32_t(key.resource_format), uint32_t(key.GetColorFormat()));
      ++presentable_fmt_log_count;
    }
    if (android_halo_owner_kind_ != AndroidHaloOwnerKind::kPresentableColor) {
      static uint32_t presentable_owner_log_count = 0;
      if (presentable_owner_log_count < 64) {
        XELOGI(
            "HaloCompat presentable_owner count={} slot={} base={} pitch={} "
            "fmt={} msaa={} width={} previous_owner={}",
            presentable_owner_log_count, i, uint32_t(key.base_tiles),
            key.GetPitchTiles(), uint32_t(key.resource_format),
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetWidth(),
            GetAndroidHaloOwnerKindName(android_halo_owner_kind_));
        ++presentable_owner_log_count;
      }
    }
    if (android_halo_depth_alias_quarantine_latched_) {
      XELOGI(
          "HaloCompat presentable_owner recovered from depth alias: slot={} "
          "base={} pitch={} fmt={} msaa={} width={}",
          i, uint32_t(key.base_tiles), key.GetPitchTiles(),
          uint32_t(key.resource_format),
          uint32_t(1) << uint32_t(key.msaa_samples), key.GetWidth());
      android_halo_depth_alias_quarantine_latched_ = false;
    }
    android_halo_owner_kind_ = AndroidHaloOwnerKind::kPresentableColor;
    AndroidDiagnosticRenderState::Get().SetOwnerState(
        AndroidDiagnosticOwnerState::kPresentableColor);
    android_halo_presentable_color_rt_ = static_cast<VulkanRenderTarget*>(rt);
    AndroidHaloNotifyMsaaSceneDrawFrame(gameplay_scene_draw_this_frame);
    return;
  }
  AndroidHaloNotifyMsaaSceneDrawFrame(gameplay_scene_draw_this_frame);
}

void VulkanRenderTargetCache::AndroidHaloQuarantineDepthToColor(
    const Transfer& transfer, const RenderTargetKey& source_key,
    const RenderTargetKey& dest_key) {
  if (!IsAndroidHaloCompatActive()) {
    return;
  }
  android_halo_owner_kind_ =
      AndroidHaloOwnerKind::kDepthColorAliasNonPresentable;
  AndroidDiagnosticRenderState::Get().SetOwnerState(
      AndroidDiagnosticOwnerState::kDepthColorAliasNonPresentable);
  android_halo_depth_alias_quarantine_latched_ = true;
  ++android_halo_depth_to_color_quarantine_count_;
  if (GetAndroidHaloExperiment().log_owner_history) {
    static uint32_t owner_trace_depth_alias_count = 0;
    if (owner_trace_depth_alias_count < 512) {
      XELOGI(
          "OWNER_TRACE event=depth_alias count={} quarantine_count={} "
          "shadow_valid={} src_base={} src_pitch={} src_fmt={} src_depth={} "
          "src_msaa={} src_width={} src_height={} dst_base={} dst_pitch={} "
          "dst_fmt={} dst_msaa={} dst_width={} dst_height={} tiles=[{}, {})",
          owner_trace_depth_alias_count,
          android_halo_depth_to_color_quarantine_count_,
          uint32_t(android_halo_present_shadow_valid_),
          uint32_t(source_key.base_tiles), source_key.GetPitchTiles(),
          uint32_t(source_key.resource_format), uint32_t(source_key.is_depth),
          uint32_t(1) << uint32_t(source_key.msaa_samples),
          source_key.GetWidth(),
          GetRenderTargetHeight(source_key.pitch_tiles_at_32bpp,
                                source_key.msaa_samples),
          uint32_t(dest_key.base_tiles), dest_key.GetPitchTiles(),
          uint32_t(dest_key.resource_format),
          uint32_t(1) << uint32_t(dest_key.msaa_samples),
          dest_key.GetWidth(),
          GetRenderTargetHeight(dest_key.pitch_tiles_at_32bpp,
                                dest_key.msaa_samples),
          transfer.start_tiles, transfer.end_tiles);
      ++owner_trace_depth_alias_count;
    }
  }
  if (android_halo_depth_to_color_quarantine_count_ <= 64 ||
      (android_halo_depth_to_color_quarantine_count_ % 300) == 0) {
    XELOGI(
        "HaloCompat DepthToColor quarantined count={} "
        "src={}/p{}/msaa{}/w{} dst={}/p{}/msaa{}/w{} "
        "tiles=[{}, {}) shadow_valid={}",
        android_halo_depth_to_color_quarantine_count_,
        uint32_t(source_key.base_tiles), source_key.GetPitchTiles(),
        uint32_t(1) << uint32_t(source_key.msaa_samples),
        source_key.GetWidth(), uint32_t(dest_key.base_tiles),
        dest_key.GetPitchTiles(),
        uint32_t(1) << uint32_t(dest_key.msaa_samples), dest_key.GetWidth(),
        transfer.start_tiles, transfer.end_tiles,
        uint32_t(android_halo_present_shadow_valid_));
  }
}

void VulkanRenderTargetCache::AndroidHaloTransitionShadowBuffer(
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags dst_access_mask) {
  if (!IsAndroidHaloCompatActive()) {
    return;
  }
  if (android_halo_present_shadow_stage_mask_ == dst_stage_mask &&
      android_halo_present_shadow_access_mask_ == dst_access_mask) {
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      android_halo_present_shadow_buffer_, 0, kAndroidHaloShadowBytes,
      android_halo_present_shadow_stage_mask_, dst_stage_mask,
      android_halo_present_shadow_access_mask_, dst_access_mask,
      VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
  android_halo_present_shadow_stage_mask_ = dst_stage_mask;
  android_halo_present_shadow_access_mask_ = dst_access_mask;
}

void VulkanRenderTargetCache::AndroidHaloTransitionMenuSceneShadowBuffer(
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags dst_access_mask) {
  if (android_halo_menu_scene_shadow_buffer_ == VK_NULL_HANDLE ||
      (android_halo_menu_scene_shadow_stage_mask_ == dst_stage_mask &&
       android_halo_menu_scene_shadow_access_mask_ == dst_access_mask)) {
    return;
  }
  command_processor_.PushBufferMemoryBarrier(
      android_halo_menu_scene_shadow_buffer_, 0,
      kAndroidHaloMenuSceneBytes, android_halo_menu_scene_shadow_stage_mask_,
      dst_stage_mask, android_halo_menu_scene_shadow_access_mask_,
      dst_access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
      false);
  android_halo_menu_scene_shadow_stage_mask_ = dst_stage_mask;
  android_halo_menu_scene_shadow_access_mask_ = dst_access_mask;
}

void VulkanRenderTargetCache::AndroidHaloCaptureMenuSceneShadow(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch) {
  if (android_halo_menu_scene_shadow_buffer_ == VK_NULL_HANDLE ||
      dump_base != kAndroidHaloMenuSceneBaseTiles ||
      dump_row_length_used != kAndroidHaloMenuScenePitchTiles ||
      dump_rows != kAndroidHaloMenuSceneRows ||
      dump_pitch != kAndroidHaloMenuScenePitchTiles ||
      dump_rectangles_.size() != 1 || draw_resolution_scale_x() != 1 ||
      draw_resolution_scale_y() != 1) {
    return;
  }

  const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
  auto& source_rt =
      *static_cast<VulkanRenderTarget*>(rectangle.render_target);
  const RenderTargetKey source_key = source_rt.key();
  if (source_key.is_depth ||
      source_key.base_tiles != kAndroidHaloMenuSceneBaseTiles ||
      source_key.GetPitchTiles() != kAndroidHaloMenuScenePitchTiles ||
      source_key.msaa_samples != xenos::MsaaSamples::k1X ||
      source_key.GetColorFormat() !=
          xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
      source_key.GetWidth() != 1200 || rectangle.row_first != 0 ||
      rectangle.rows != kAndroidHaloMenuSceneRows ||
      rectangle.row_first_start != 0 ||
      rectangle.row_last_end != kAndroidHaloMenuScenePitchTiles) {
    return;
  }

  command_processor_.PushImageMemoryBarrier(
      source_rt.image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
      source_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
      source_rt.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT,
      source_rt.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  source_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  AndroidHaloTransitionMenuSceneShadowBuffer(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                              VK_ACCESS_TRANSFER_WRITE_BIT);
  command_processor_.SubmitBarriers(true);

  constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
  constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
  std::vector<VkBufferImageCopy> copy_regions;
  copy_regions.reserve(kAndroidHaloMenuScenePitchTiles *
                       kAndroidHaloMenuSceneRows);
  for (uint32_t tile_y = 0; tile_y < kAndroidHaloMenuSceneRows; ++tile_y) {
    for (uint32_t tile_x = 0; tile_x < kAndroidHaloMenuScenePitchTiles;
         ++tile_x) {
      VkBufferImageCopy& region = copy_regions.emplace_back();
      region.bufferOffset =
          VkDeviceSize(tile_y * kAndroidHaloMenuScenePitchTiles + tile_x) *
          VkDeviceSize(kAndroidHaloShadowTileBytes);
      region.bufferRowLength = kTileWidth;
      region.bufferImageHeight = kTileHeight;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = 0;
      region.imageSubresource.layerCount = 1;
      region.imageOffset = {int32_t(tile_x * kTileWidth),
                            int32_t(tile_y * kTileHeight), 0};
      region.imageExtent = {kTileWidth, kTileHeight, 1};
    }
  }
  command_processor_.deferred_command_buffer().CmdVkCopyImageToBuffer(
      source_rt.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      android_halo_menu_scene_shadow_buffer_, uint32_t(copy_regions.size()),
      copy_regions.data());
  android_halo_menu_scene_shadow_valid_ = true;
  ++android_halo_menu_scene_shadow_capture_count_;
  if (android_halo_menu_scene_shadow_capture_count_ <= 64 ||
      (android_halo_menu_scene_shadow_capture_count_ % 300) == 0) {
    XELOGI(
        "MENU_SCENE_SHADOW_CAPTURE count={} source_base={} pitch={} "
        "tiles={} bytes=0x{:X}",
        android_halo_menu_scene_shadow_capture_count_,
        uint32_t(source_key.base_tiles), source_key.GetPitchTiles(),
        uint32_t(copy_regions.size()), kAndroidHaloMenuSceneBytes);
  }
}

void VulkanRenderTargetCache::AndroidHaloCapturePresentableShadow(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch) {
  // The shadow only feeds the direct-resolve/shadow-override compat paths;
  // capturing it redumps the tracked 1x RT over the span, clobbering the real
  // owner's dump. Skip entirely when neither consumer is enabled.
  if (!GetAndroidHaloExperiment().direct_presentable_resolve &&
      !GetAndroidHaloExperiment().shadow_fallback) {
    return;
  }
  if (!IsAndroidHaloCompatActive() ||
      !IsAndroidHaloShadowSpan(dump_base, dump_row_length_used, dump_rows,
                               dump_pitch)) {
    return;
  }
  bool has_presentable_color_source = false;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    const auto& vulkan_rt =
        *static_cast<const VulkanRenderTarget*>(rectangle.render_target);
    if (IsAndroidHaloPresentableColorKey(vulkan_rt.key())) {
      has_presentable_color_source = true;
      break;
    }
  }
  if (!has_presentable_color_source &&
      android_halo_presentable_color_rt_ != nullptr &&
      IsAndroidHaloPresentableColorKey(
          android_halo_presentable_color_rt_->key())) {
    dump_rectangles_.clear();
    dump_rectangles_.emplace_back(android_halo_presentable_color_rt_, 0,
                                  dump_rows, 0, dump_row_length_used);
    AndroidHaloExecutePendingDumpRectangles(
        dump_base, dump_row_length_used, dump_rows, dump_pitch);
    has_presentable_color_source = true;
    if (GetAndroidHaloExperiment().log_owner_history) {
      const RenderTargetKey key = android_halo_presentable_color_rt_->key();
      static uint32_t owner_trace_shadow_track_count = 0;
      if (owner_trace_shadow_track_count < 256) {
        XELOGI(
            "OWNER_TRACE event=shadow_from_tracked count={} owner_kind={} "
            "latched={} rt_ptr=0x{:016X} base={} pitch={} fmt={} "
            "color_fmt={} msaa={} width={} height={}",
            owner_trace_shadow_track_count,
            GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
            uint32_t(android_halo_depth_alias_quarantine_latched_),
            uint64_t(reinterpret_cast<uintptr_t>(
                android_halo_presentable_color_rt_)),
            uint32_t(key.base_tiles), key.GetPitchTiles(),
            uint32_t(key.resource_format), uint32_t(key.GetColorFormat()),
            uint32_t(1) << uint32_t(key.msaa_samples), key.GetWidth(),
            GetRenderTargetHeight(key.pitch_tiles_at_32bpp,
                                  key.msaa_samples));
        ++owner_trace_shadow_track_count;
      }
    }
    if (android_halo_present_shadow_update_count_ < 64) {
      XELOGI(
          "HaloCompat shadow_update using tracked presentable host RT base={} "
          "pitch={} rows={}",
          dump_base, dump_pitch, dump_rows);
    }
  }
  if (!has_presentable_color_source) {
    return;
  }

  if (android_halo_owner_kind_ ==
          AndroidHaloOwnerKind::kDepthColorAliasNonPresentable ||
      android_halo_depth_alias_quarantine_latched_) {
    if (android_halo_present_shadow_skip_log_count_ < 64) {
      XELOGI(
          "HaloCompat shadow_update recovering from depth alias: "
          "owner_kind={} shadow_valid={} latched={} base={} pitch={} rows={}",
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_present_shadow_valid_),
          uint32_t(android_halo_depth_alias_quarantine_latched_), dump_base,
          dump_pitch, dump_rows);
      ++android_halo_present_shadow_skip_log_count_;
    }
    android_halo_depth_alias_quarantine_latched_ = false;
  }

  UseEdramBuffer(EdramBufferUsage::kTransferRead);
  AndroidHaloTransitionShadowBuffer(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT);
  command_processor_.SubmitBarriers(true);

  VkBufferCopy copy_region = {};
  copy_region.srcOffset =
      VkDeviceSize(kAndroidHaloShadowBaseTiles) * kAndroidHaloShadowTileBytes;
  copy_region.dstOffset = 0;
  copy_region.size = kAndroidHaloShadowBytes;
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      edram_buffer_, android_halo_present_shadow_buffer_, 1, &copy_region);

  android_halo_present_shadow_valid_ = true;
  android_halo_owner_kind_ = AndroidHaloOwnerKind::kPresentableColor;
  AndroidDiagnosticRenderState::Get().SetOwnerState(
      AndroidDiagnosticOwnerState::kPresentableColor);
  ++android_halo_present_shadow_update_count_;
  if (GetAndroidHaloExperiment().log_owner_history) {
    static uint32_t owner_trace_shadow_update_count = 0;
    if (owner_trace_shadow_update_count < 512) {
      XELOGI(
          "OWNER_TRACE event=shadow_update count={} shadow_updates={} "
          "base={} pitch={} rows={} owner_kind={} latched={} "
          "tracked_rt=0x{:016X}",
          owner_trace_shadow_update_count, android_halo_present_shadow_update_count_,
          dump_base, dump_pitch, dump_rows,
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_depth_alias_quarantine_latched_),
          uint64_t(reinterpret_cast<uintptr_t>(
              android_halo_presentable_color_rt_)));
      ++owner_trace_shadow_update_count;
    }
  }
  if (android_halo_present_shadow_update_count_ <= 64 ||
      (android_halo_present_shadow_update_count_ % 300) == 0) {
    XELOGI(
        "HaloCompat shadow_update count={} base={} pitch={} rows={} "
        "bytes=0x{:X} owner_kind={}",
        android_halo_present_shadow_update_count_, dump_base, dump_pitch,
        dump_rows, kAndroidHaloShadowBytes,
        GetAndroidHaloOwnerKindName(android_halo_owner_kind_));
  }
}

bool VulkanRenderTargetCache::AndroidHaloRefreshPresentableEdramForFinalResolve(
    const draw_util::ResolveInfo& resolve_info, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
    uint32_t copy_width, uint32_t copy_height, uint32_t copy_bpp,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants) {
  // NOTE: the halo_android_compat_direct_presentable_resolve cvar is unreliably
  // false at runtime on this build (its profile override is rejected), which
  // disabled the only path that writes the real presentable color into EDRAM
  // base 1350 before the final resolve. Gate on the experiment file (reliable)
  // and on compat being active.
  if (!GetAndroidHaloExperiment().direct_presentable_resolve ||
      !IsAndroidHaloCompatActive()) {
    return false;
  }

  const uint32_t visible_frontbuffer_bytes = copy_width * copy_height * copy_bpp;
  const bool final_frontbuffer_match =
      copy_width == 1152 && copy_height == 720 && copy_bpp == 4 &&
      resolve_info.copy_dest_extent_start == kAndroidHaloFrontbufferAddress &&
      resolve_info.copy_dest_extent_length >= visible_frontbuffer_bytes &&
      resolve_info.copy_dest_info.copy_dest_format ==
          xenos::ColorFormat::k_8_8_8_8 &&
      IsAndroidHaloShadowSpan(dump_base, dump_row_length_used, dump_rows,
                              dump_pitch) &&
      copy_shader_constants.dest_relative.edram_info.base_tiles ==
          kAndroidHaloShadowBaseTiles &&
      copy_shader_constants.dest_relative.edram_info.pitch_tiles ==
          kAndroidHaloShadowPitchTiles;
  if (GetAndroidHaloExperiment().log_owner_history && final_frontbuffer_match) {
    static uint32_t owner_trace_final_probe_count = 0;
    if (owner_trace_final_probe_count < 1024) {
      const bool has_tracked_rt = android_halo_presentable_color_rt_ != nullptr;
      const RenderTargetKey tracked_key =
          has_tracked_rt ? android_halo_presentable_color_rt_->key()
                         : RenderTargetKey();
      const bool tracked_valid =
          has_tracked_rt && IsAndroidHaloPresentableColorKey(tracked_key);
      XELOGI(
          "OWNER_TRACE event=final_probe count={} owner_kind={} latched={} "
          "shadow_valid={} direct_count={} fb_addr=0x{:08X} fb={}x{} "
          "tracked_rt=0x{:016X} tracked_valid={} rt_base={} rt_pitch={} "
          "rt_fmt={} rt_color_fmt={} rt_msaa={} rt_width={} rt_height={}",
          owner_trace_final_probe_count,
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_depth_alias_quarantine_latched_),
          uint32_t(android_halo_present_shadow_valid_),
          android_halo_direct_presentable_resolve_count_,
          resolve_info.copy_dest_extent_start, copy_width, copy_height,
          uint64_t(reinterpret_cast<uintptr_t>(
              android_halo_presentable_color_rt_)),
          uint32_t(tracked_valid), uint32_t(tracked_key.base_tiles),
          tracked_key.GetPitchTiles(), uint32_t(tracked_key.resource_format),
          has_tracked_rt ? uint32_t(tracked_key.GetColorFormat()) : 0,
          uint32_t(1) << uint32_t(tracked_key.msaa_samples),
          has_tracked_rt ? tracked_key.GetWidth() : 0,
          has_tracked_rt ? GetRenderTargetHeight(
                               tracked_key.pitch_tiles_at_32bpp,
                               tracked_key.msaa_samples)
                         : 0);
      ++owner_trace_final_probe_count;
    }
  }
  const AndroidHaloExperiment& experiment = GetAndroidHaloExperiment();
  VulkanRenderTarget* refresh_source_rt = android_halo_presentable_color_rt_;
  const bool direct_msaa_scene =
      experiment.direct_msaa_scene_resolve &&
      android_halo_msaa_scene_color_rt_ != nullptr &&
      IsAndroidHaloMsaaSceneColorKey(android_halo_msaa_scene_color_rt_->key());
  if (direct_msaa_scene) {
    refresh_source_rt = android_halo_msaa_scene_color_rt_;
  }
  const bool refresh_source_valid =
      refresh_source_rt != nullptr &&
      (direct_msaa_scene
           ? IsAndroidHaloMsaaSceneColorKey(refresh_source_rt->key())
           : IsAndroidHaloPresentableColorKey(refresh_source_rt->key()));
  if (!final_frontbuffer_match || !refresh_source_valid) {
    return false;
  }

  VulkanRenderTarget& source_rt = *refresh_source_rt;
  const RenderTargetKey source_rt_key = source_rt.key();
  const xenos::ColorRenderTargetFormat source_color_format =
      source_rt_key.GetColorFormat();
  if (GetAndroidHaloExperiment().log_presentable_source_owner) {
    static uint32_t presentable_source_final_log_count = 0;
    if (presentable_source_final_log_count < 512) {
      XELOGI(
          "PRESENTABLE_SOURCE transfer_kind=final_refresh count={} "
          "mode_candidate={} source_addr=host_rt dest_addr=0x{:08X} "
          "rt_ptr=0x{:016X} source_base={} source_pitch={} src_fmt={} "
          "src_color_fmt={} src_msaa={} src_width={} src_height={} "
          "dest_base={} dest_pitch={} dest_fmt={} dest_width={} "
          "dest_height={} dump_rows={} dump_row_len={} shadow_valid={} "
          "owner_kind={} source_to_1x={}",
          presentable_source_final_log_count,
          direct_msaa_scene
              ? "direct_msaa_scene_compute_dump"
              : (experiment.final_resolve_raw_copy && !experiment.writer_gb_fix
                     ? "raw_copy"
                     : "compute_dump"),
          resolve_info.copy_dest_extent_start,
          uint64_t(reinterpret_cast<uintptr_t>(&source_rt)),
          uint32_t(source_rt_key.base_tiles), source_rt_key.GetPitchTiles(),
          uint32_t(source_rt_key.resource_format),
          uint32_t(source_color_format),
          uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
          source_rt_key.GetWidth(),
          GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                source_rt_key.msaa_samples),
          dump_base, dump_pitch,
          uint32_t(resolve_info.copy_dest_info.copy_dest_format), copy_width,
          copy_height, dump_rows, dump_row_length_used,
          uint32_t(android_halo_present_shadow_valid_),
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(direct_msaa_scene));
      ++presentable_source_final_log_count;
    }
  }
  // Byte-exact path: copy the presentable RT image tiles straight into the
  // EDRAM buffer with vkCmdCopyImageToBuffer, bypassing the format-converting
  // compute dump shader (one less corruption suspect). Only valid for 8888
  // where the host texel bytes match the guest EDRAM encoding.
  const bool raw_copy =
      !direct_msaa_scene && experiment.final_resolve_raw_copy &&
      !experiment.writer_gb_fix &&
      (source_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
       source_color_format ==
           xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) &&
      draw_resolution_scale_x() == 1 && draw_resolution_scale_y() == 1;
  if (raw_copy) {
    constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
    constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
    constexpr uint32_t kTileBytes = kTileWidth * kTileHeight * 4;
    command_processor_.PushImageMemoryBarrier(
        source_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
        source_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
        source_rt.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT,
        source_rt.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    source_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    UseEdramBuffer(EdramBufferUsage::kTransferWrite);
    command_processor_.SubmitBarriers(true);
    std::vector<VkBufferImageCopy> copy_regions;
    copy_regions.reserve(size_t(dump_pitch) * size_t(dump_rows));
    for (uint32_t tile_y = 0; tile_y < dump_rows; ++tile_y) {
      for (uint32_t tile_x = 0; tile_x < dump_row_length_used; ++tile_x) {
        VkBufferImageCopy& region = copy_regions.emplace_back();
        region.bufferOffset =
            VkDeviceSize(dump_base + tile_y * dump_pitch + tile_x) *
            VkDeviceSize(kTileBytes);
        region.bufferRowLength = kTileWidth;
        region.bufferImageHeight = kTileHeight;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {int32_t(tile_x * kTileWidth),
                              int32_t(tile_y * kTileHeight), 0};
        region.imageExtent = {kTileWidth, kTileHeight, 1};
      }
    }
    command_processor_.deferred_command_buffer().CmdVkCopyImageToBuffer(
        source_rt.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        edram_buffer_, uint32_t(copy_regions.size()), copy_regions.data());
  } else {
    dump_rectangles_.clear();
    dump_rectangles_.emplace_back(refresh_source_rt, 0, dump_rows, 0,
                                  dump_row_length_used);
    const bool previous_force_writer_gb_fix =
        android_halo_force_writer_gb_fix_;
    const bool previous_force_8888_repack = android_halo_force_8888_repack_;
    android_halo_force_writer_gb_fix_ = experiment.writer_gb_fix;
    android_halo_force_8888_repack_ = direct_msaa_scene;
    ExecutePendingDumpRectanglesToEdram(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
    android_halo_force_writer_gb_fix_ = previous_force_writer_gb_fix;
    android_halo_force_8888_repack_ = previous_force_8888_repack;
  }

  android_halo_owner_kind_ = AndroidHaloOwnerKind::kPresentableColor;
  AndroidDiagnosticRenderState::Get().SetOwnerState(
      AndroidDiagnosticOwnerState::kPresentableColor);
  android_halo_depth_alias_quarantine_latched_ = false;
  ++android_halo_direct_presentable_resolve_count_;
  if (GetAndroidHaloExperiment().log_owner_history) {
    static uint32_t owner_trace_final_refresh_count = 0;
    if (owner_trace_final_refresh_count < 1024) {
      XELOGI(
          "OWNER_TRACE event=final_refresh count={} direct_count={} mode={} "
          "owner_kind={} shadow_valid={} fb_addr=0x{:08X} rt_ptr=0x{:016X} "
          "rt_base={} rt_pitch={} rt_fmt={} rt_color_fmt={} rt_msaa={} "
          "rt_width={} rt_height={}",
          owner_trace_final_refresh_count,
          android_halo_direct_presentable_resolve_count_,
          direct_msaa_scene
              ? "direct_msaa_scene_compute_dump"
              : (raw_copy ? "raw_copy" : "compute_dump"),
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_present_shadow_valid_),
          resolve_info.copy_dest_extent_start,
          uint64_t(reinterpret_cast<uintptr_t>(&source_rt)),
          uint32_t(source_rt_key.base_tiles), source_rt_key.GetPitchTiles(),
          uint32_t(source_rt_key.resource_format),
          uint32_t(source_color_format),
          uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
          source_rt_key.GetWidth(),
          GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                source_rt_key.msaa_samples));
      ++owner_trace_final_refresh_count;
    }
  }
  if (android_halo_direct_presentable_resolve_count_ <= 64 ||
      (android_halo_direct_presentable_resolve_count_ % 300) == 0) {
    XELOGI(
        "HaloCompat direct_presentable_resolve count={} mode={} "
        "fb_addr=0x{:08X} fb={}x{} dump_base={} pitch={} rt_base={} "
        "rt_width={} rt_fmt={} writer_gb_fix={}",
        android_halo_direct_presentable_resolve_count_,
        direct_msaa_scene
            ? "direct_msaa_scene_compute_dump"
            : (raw_copy ? "raw_copy" : "compute_dump"),
        resolve_info.copy_dest_extent_start, copy_width, copy_height, dump_base,
        dump_pitch, uint32_t(source_rt_key.base_tiles), source_rt_key.GetWidth(),
        uint32_t(source_rt_key.resource_format),
        uint32_t(experiment.writer_gb_fix));
  }
  return true;
}

bool VulkanRenderTargetCache::AndroidHaloMaybeOverrideEdramWithPresentableShadow(
    const draw_util::ResolveInfo& resolve_info, uint32_t dump_base,
    uint32_t dump_row_length_used, uint32_t dump_rows, uint32_t dump_pitch,
    uint32_t copy_width, uint32_t copy_height, uint32_t copy_bpp,
    const draw_util::ResolveCopyShaderConstants& copy_shader_constants) {
  if (!IsAndroidHaloCompatActive()) {
    return false;
  }

  const uint32_t visible_frontbuffer_bytes = copy_width * copy_height * copy_bpp;
  const bool final_frontbuffer_match =
      copy_width == 1152 && copy_height == 720 && copy_bpp == 4 &&
      resolve_info.copy_dest_extent_start == kAndroidHaloFrontbufferAddress &&
      resolve_info.copy_dest_extent_length >= visible_frontbuffer_bytes &&
      resolve_info.copy_dest_info.copy_dest_format ==
          xenos::ColorFormat::k_8_8_8_8 &&
      IsAndroidHaloShadowSpan(dump_base, dump_row_length_used, dump_rows,
                              dump_pitch) &&
      copy_shader_constants.dest_relative.edram_info.base_tiles ==
          kAndroidHaloShadowBaseTiles &&
      copy_shader_constants.dest_relative.edram_info.pitch_tiles ==
          kAndroidHaloShadowPitchTiles;
  if (!final_frontbuffer_match) {
    return false;
  }

  if (AndroidHaloRefreshPresentableEdramForFinalResolve(
          resolve_info, dump_base, dump_row_length_used, dump_rows, dump_pitch,
          copy_width, copy_height, copy_bpp, copy_shader_constants)) {
    android_halo_reclaim_shadow_required_ = false;
    return false;
  }

  const bool should_use_shadow =
      GetAndroidHaloExperiment().shadow_fallback &&
      (android_halo_reclaim_shadow_required_ ||
       android_halo_owner_kind_ ==
           AndroidHaloOwnerKind::kDepthColorAliasNonPresentable ||
       android_halo_depth_alias_quarantine_latched_) &&
      android_halo_present_shadow_valid_;
  if (!should_use_shadow) {
    if (android_halo_present_shadow_skip_log_count_ < 128) {
      XELOGI(
          "HaloCompat final_override skipped: shadow_valid={} owner_kind={} "
          "latched={} reclaim_required={} fb_addr=0x{:08X} fb={}x{} fmt={} "
          "dump_base={} pitch={}",
          uint32_t(android_halo_present_shadow_valid_),
          GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
          uint32_t(android_halo_depth_alias_quarantine_latched_),
          uint32_t(android_halo_reclaim_shadow_required_),
          resolve_info.copy_dest_extent_start, copy_width, copy_height,
          uint32_t(resolve_info.copy_dest_info.copy_dest_format), dump_base,
          dump_pitch);
      ++android_halo_present_shadow_skip_log_count_;
    }
    return false;
  }

  AndroidHaloTransitionShadowBuffer(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT);
  UseEdramBuffer(EdramBufferUsage::kTransferWrite);
  command_processor_.SubmitBarriers(true);

  VkBufferCopy copy_region = {};
  copy_region.srcOffset = 0;
  copy_region.dstOffset =
      VkDeviceSize(kAndroidHaloShadowBaseTiles) * kAndroidHaloShadowTileBytes;
  copy_region.size = kAndroidHaloShadowBytes;
  command_processor_.deferred_command_buffer().CmdVkCopyBuffer(
      android_halo_present_shadow_buffer_, edram_buffer_, 1, &copy_region);

  ++android_halo_present_shadow_override_count_;
  XELOGI(
      "HaloCompat final_override count={} using_shadow=1 shadow_valid=1 "
      "owner_kind={} latched={} fb_addr=0x{:08X} fb={}x{} fmt={} "
      "bytes=0x{:X}",
      android_halo_present_shadow_override_count_,
      GetAndroidHaloOwnerKindName(android_halo_owner_kind_),
      uint32_t(android_halo_depth_alias_quarantine_latched_),
      resolve_info.copy_dest_extent_start, copy_width, copy_height,
      uint32_t(resolve_info.copy_dest_info.copy_dest_format),
      kAndroidHaloShadowBytes);
  android_halo_depth_alias_quarantine_latched_ = false;
  android_halo_reclaim_shadow_required_ = false;
  android_halo_owner_kind_ = AndroidHaloOwnerKind::kPresentableColor;
  AndroidDiagnosticRenderState::Get().SetOwnerState(
      AndroidDiagnosticOwnerState::kPresentableColor);
  return true;
}
#endif

const VulkanRenderTargetCache::Framebuffer*
VulkanRenderTargetCache::GetHostRenderTargetsFramebuffer(
    RenderPassKey render_pass_key, uint32_t pitch_tiles_at_32bpp,
    const RenderTarget* const* depth_and_color_render_targets) {
  FramebufferKey key;
  key.render_pass_key = render_pass_key;
  key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
  if (render_pass_key.depth_and_color_used & (1 << 0)) {
    key.depth_base_tiles = depth_and_color_render_targets[0]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 1)) {
    key.color_0_base_tiles =
        depth_and_color_render_targets[1]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 2)) {
    key.color_1_base_tiles =
        depth_and_color_render_targets[2]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 3)) {
    key.color_2_base_tiles =
        depth_and_color_render_targets[3]->key().base_tiles;
  }
  if (render_pass_key.depth_and_color_used & (1 << 4)) {
    key.color_3_base_tiles =
        depth_and_color_render_targets[4]->key().base_tiles;
  }
  auto it = framebuffers_.find(key);
  if (it != framebuffers_.end()) {
    return &it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  VkRenderPass render_pass = GetHostRenderTargetsRenderPass(render_pass_key);
  if (render_pass == VK_NULL_HANDLE) {
    return nullptr;
  }

  VkImageView attachments[1 + xenos::kMaxColorRenderTargets];
  uint32_t attachment_count = 0;
  uint32_t depth_and_color_rts_remaining = render_pass_key.depth_and_color_used;
  uint32_t rt_index;
  while (xe::bit_scan_forward(depth_and_color_rts_remaining, &rt_index)) {
    depth_and_color_rts_remaining &= ~(uint32_t(1) << rt_index);
    const auto& vulkan_rt = *static_cast<const VulkanRenderTarget*>(
        depth_and_color_render_targets[rt_index]);
    VkImageView attachment;
    if (rt_index) {
      attachment = render_pass_key.color_rts_use_transfer_formats
                       ? vulkan_rt.view_color_transfer()
                       : vulkan_rt.view_depth_color();
    } else {
      attachment = vulkan_rt.view_depth_stencil();
    }
    attachments[attachment_count++] = attachment;
  }

  VkFramebufferCreateInfo framebuffer_create_info;
  framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  framebuffer_create_info.pNext = nullptr;
  framebuffer_create_info.flags = 0;
  framebuffer_create_info.renderPass = render_pass;
  framebuffer_create_info.attachmentCount = attachment_count;
  framebuffer_create_info.pAttachments = attachments;
  VkExtent2D host_extent;
  if (pitch_tiles_at_32bpp) {
    host_extent.width = RenderTargetKey::GetWidth(pitch_tiles_at_32bpp,
                                                  render_pass_key.msaa_samples);
    host_extent.height = GetRenderTargetHeight(pitch_tiles_at_32bpp,
                                               render_pass_key.msaa_samples);
  } else {
    assert_zero(render_pass_key.depth_and_color_used);
    // Still needed for occlusion queries.
    host_extent.width = xenos::kTexture2DCubeMaxWidthHeight;
    host_extent.height = xenos::kTexture2DCubeMaxWidthHeight;
  }
  // Limiting to the device limit for the case of no attachments, for which
  // there's no limit imposed by the sizes of the attachments that have been
  // created successfully.
  host_extent.width = std::min(host_extent.width * draw_resolution_scale_x(),
                               device_properties.maxFramebufferWidth);
  host_extent.height = std::min(host_extent.height * draw_resolution_scale_y(),
                                device_properties.maxFramebufferHeight);
  framebuffer_create_info.width = host_extent.width;
  framebuffer_create_info.height = host_extent.height;
  framebuffer_create_info.layers = 1;
  VkFramebuffer framebuffer;
  if (dfn.vkCreateFramebuffer(device, &framebuffer_create_info, nullptr,
                              &framebuffer) != VK_SUCCESS) {
    return nullptr;
  }
  // Creates at a persistent location - safe to use pointers.
  return &framebuffers_
              .emplace(std::piecewise_construct, std::forward_as_tuple(key),
                       std::forward_as_tuple(framebuffer, host_extent))
              .first->second;
}

VkShaderModule VulkanRenderTargetCache::GetTransferShader(
    TransferShaderKey key) {
  auto shader_it = transfer_shaders_.find(key);
  if (shader_it != transfer_shaders_.end()) {
    return shader_it->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  std::vector<spv::Id> id_vector_temp;
  std::vector<unsigned int> uint_vector_temp;
  SpirvBuilder builder(spv::Spv_1_0,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_bool = builder.makeBoolType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint4 = builder.makeVectorType(type_uint, 4);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float2 = builder.makeVectorType(type_float, 2);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  const TransferModeInfo& mode = kTransferModes[size_t(key.mode)];
  const TransferPipelineLayoutInfo& pipeline_layout_info =
      kTransferPipelineLayoutInfos[size_t(mode.pipeline_layout)];

  // If not dest_is_color, it's depth, or stencil bit - 40-sample columns are
  // swapped as opposed to color source.
  bool dest_is_color = (mode.output == TransferOutput::kColor);
  xenos::ColorRenderTargetFormat dest_color_format =
      xenos::ColorRenderTargetFormat(key.dest_resource_format);
  xenos::DepthRenderTargetFormat dest_depth_format =
      xenos::DepthRenderTargetFormat(key.dest_resource_format);
  bool dest_is_64bpp =
      dest_is_color && xenos::IsColorRenderTargetFormat64bpp(dest_color_format);
#if XE_PLATFORM_ANDROID
  const uint32_t android_diag_depth_to_color_mode =
      key.android_depth_to_color_diag_mode;
  const uint32_t android_diag_depth_to_color_sample_mode =
      key.android_depth_to_color_sample_mode;
  const bool android_diag_depth_to_color_pattern =
      android_diag_depth_to_color_mode == 1;
  const bool android_diag_depth_to_color_constant_packed =
      android_diag_depth_to_color_mode == 2;
  const bool android_diag_depth_to_color_disable_column_swap =
      android_diag_depth_to_color_mode == 3;
  const bool android_diag_depth_to_color_depth_only =
      android_diag_depth_to_color_mode == 4;
  const bool android_diag_depth_to_color_stencil_only =
      android_diag_depth_to_color_mode == 5;
  const bool android_diag_depth_to_color_sample_override =
      android_diag_depth_to_color_mode == 6 &&
      android_diag_depth_to_color_sample_mode != 0;
  const bool android_value_convert_1010102_to_8888 =
      key.android_value_convert_1010102_to_8888 != 0;
  const bool android_value_convert_16bit_to_8888 =
      key.android_value_convert_16bit_to_8888 != 0;
#else
  const uint32_t android_diag_depth_to_color_mode = 0;
  const uint32_t android_diag_depth_to_color_sample_mode = 0;
  const bool android_diag_depth_to_color_pattern = false;
  const bool android_diag_depth_to_color_constant_packed = false;
  const bool android_diag_depth_to_color_disable_column_swap = false;
  const bool android_diag_depth_to_color_depth_only = false;
  const bool android_diag_depth_to_color_stencil_only = false;
  const bool android_diag_depth_to_color_sample_override = false;
  const bool android_value_convert_1010102_to_8888 = false;
  const bool android_value_convert_16bit_to_8888 = false;
#endif

  xenos::ColorRenderTargetFormat source_color_format =
      xenos::ColorRenderTargetFormat(key.source_resource_format);
  xenos::DepthRenderTargetFormat source_depth_format =
      xenos::DepthRenderTargetFormat(key.source_resource_format);
  // If not source_is_color, it's depth / stencil - 40-sample columns are
  // swapped as opposed to color destination.
  bool source_is_color = (pipeline_layout_info.used_descriptor_sets &
                          kTransferUsedDescriptorSetColorTextureBit) != 0;
  bool source_is_64bpp;
  uint32_t source_color_format_component_count;
  uint32_t source_color_texture_component_mask;
  bool source_color_is_uint;
  spv::Id source_color_component_type;
  if (source_is_color) {
    assert_zero(pipeline_layout_info.used_descriptor_sets &
                kTransferUsedDescriptorSetDepthStencilTexturesBit);
    source_is_64bpp =
        xenos::IsColorRenderTargetFormat64bpp(source_color_format);
    source_color_format_component_count =
        xenos::GetColorRenderTargetFormatComponentCount(source_color_format);
    if (mode.output == TransferOutput::kStencilBit) {
      if (source_is_64bpp && !dest_is_64bpp) {
        // Need one component, but choosing from the two 32bpp halves of the
        // 64bpp sample.
        source_color_texture_component_mask =
            0b1 | (0b1 << (source_color_format_component_count >> 1));
      } else {
        // Red is at least 8 bits per component in all formats.
        source_color_texture_component_mask = 0b1;
      }
    } else {
      source_color_texture_component_mask =
          (uint32_t(1) << source_color_format_component_count) - 1;
    }
    GetColorOwnershipTransferVulkanFormat(source_color_format,
                                          &source_color_is_uint);
    source_color_component_type = source_color_is_uint ? type_uint : type_float;
  } else {
    source_is_64bpp = false;
    source_color_format_component_count = 0;
    source_color_texture_component_mask = 0;
    source_color_is_uint = false;
    source_color_component_type = spv::NoType;
  }

  std::vector<spv::Id> main_interface;

  // Outputs.
  bool shader_uses_stencil_reference_output =
      mode.output == TransferOutput::kDepth &&
      vulkan_device->extensions().ext_EXT_shader_stencil_export;
  bool dest_color_is_uint = false;
  uint32_t dest_color_component_count = 0;
  spv::Id type_fragment_data_component = spv::NoResult;
  spv::Id type_fragment_data = spv::NoResult;
  spv::Id output_fragment_data = spv::NoResult;
  spv::Id output_fragment_depth = spv::NoResult;
  spv::Id output_fragment_stencil_ref = spv::NoResult;
  switch (mode.output) {
    case TransferOutput::kColor:
      GetColorOwnershipTransferVulkanFormat(dest_color_format,
                                            &dest_color_is_uint);
      dest_color_component_count =
          xenos::GetColorRenderTargetFormatComponentCount(dest_color_format);
      type_fragment_data_component =
          dest_color_is_uint ? type_uint : type_float;
      type_fragment_data =
          dest_color_component_count > 1
              ? builder.makeVectorType(type_fragment_data_component,
                                       dest_color_component_count)
              : type_fragment_data_component;
      output_fragment_data = builder.createVariable(
          spv::NoPrecision, spv::StorageClassOutput, type_fragment_data,
          "xe_transfer_fragment_data");
      builder.addDecoration(output_fragment_data, spv::DecorationLocation,
                            key.dest_color_rt_index);
      main_interface.push_back(output_fragment_data);
      break;
    case TransferOutput::kDepth:
      output_fragment_depth =
          builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                 type_float, "gl_FragDepth");
      builder.addDecoration(output_fragment_depth, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::FragDepth));
      main_interface.push_back(output_fragment_depth);
      if (shader_uses_stencil_reference_output) {
        builder.addExtension("SPV_EXT_shader_stencil_export");
        builder.addCapability(spv::CapabilityStencilExportEXT);
        output_fragment_stencil_ref =
            builder.createVariable(spv::NoPrecision, spv::StorageClassOutput,
                                   type_int, "gl_FragStencilRefARB");
        builder.addDecoration(
            output_fragment_stencil_ref, spv::DecorationBuiltIn,
            static_cast<int>(spv::BuiltIn::FragStencilRefEXT));
        main_interface.push_back(output_fragment_stencil_ref);
      }
      break;
    default:
      break;
  }

  // Bindings.
  // Generating SPIR-V 1.0, no need to add bindings to the entry point's
  // interface until SPIR-V 1.4.
  // Color source.
  bool source_is_multisampled =
      key.source_msaa_samples != xenos::MsaaSamples::k1X;
  spv::Id source_color_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetColorTextureBit) {
    source_color_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(source_color_component_type, spv::Dim2D, false,
                              false, source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_transfer_color");
    builder.addDecoration(
        source_color_texture, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetColorTextureBit - 1)));
    builder.addDecoration(source_color_texture, spv::DecorationBinding, 0);
  }
  // Depth / stencil source.
  spv::Id source_depth_texture = spv::NoResult;
  spv::Id source_stencil_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetDepthStencilTexturesBit) {
    uint32_t source_depth_stencil_descriptor_set =
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1));
    // Using `depth == false` in makeImageType because comparisons are not
    // required, and other values of `depth` are causing issues in drivers.
    // https://github.com/microsoft/DirectXShaderCompiler/issues/1107
    if (mode.output != TransferOutput::kStencilBit) {
      source_depth_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_float, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_depth");
      builder.addDecoration(source_depth_texture, spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_depth_texture, spv::DecorationBinding, 0);
    }
    if (mode.output != TransferOutput::kDepth ||
        shader_uses_stencil_reference_output) {
      source_stencil_texture = builder.createVariable(
          spv::NoPrecision, spv::StorageClassUniformConstant,
          builder.makeImageType(type_uint, spv::Dim2D, false, false,
                                source_is_multisampled, 1,
                                spv::ImageFormatUnknown),
          "xe_transfer_stencil");
      builder.addDecoration(source_stencil_texture,
                            spv::DecorationDescriptorSet,
                            source_depth_stencil_descriptor_set);
      builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
    }
  }
  // Host depth source buffer.
  spv::Id host_depth_source_buffer = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthBufferBit) {
    id_vector_temp.clear();
    id_vector_temp.push_back(builder.makeRuntimeArray(type_uint));
    // Storage buffers have std430 packing, no padding to 4-component vectors.
    builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                          sizeof(uint32_t));
    spv::Id type_host_depth_source_buffer =
        builder.makeStructType(id_vector_temp, "XeTransferHostDepthBuffer");
    builder.addMemberName(type_host_depth_source_buffer, 0, "host_depth");
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationNonWritable);
    builder.addMemberDecoration(type_host_depth_source_buffer, 0,
                                spv::DecorationOffset, 0);
    // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // BufferBlock.
    builder.addDecoration(type_host_depth_source_buffer,
                          spv::DecorationBufferBlock);
    // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
    // Uniform.
    host_depth_source_buffer = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniform,
        type_host_depth_source_buffer, "xe_transfer_host_depth_buffer");
    builder.addDecoration(
        host_depth_source_buffer, spv::DecorationDescriptorSet,
        xe::bit_count(pipeline_layout_info.used_descriptor_sets &
                      (kTransferUsedDescriptorSetHostDepthBufferBit - 1)));
    builder.addDecoration(host_depth_source_buffer, spv::DecorationBinding, 0);
  }
  // Host depth source texture (the depth / stencil descriptor set is reused,
  // but stencil is not needed).
  spv::Id host_depth_source_texture = spv::NoResult;
  if (pipeline_layout_info.used_descriptor_sets &
      kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
    host_depth_source_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(
            type_float, spv::Dim2D, false, false,
            key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X, 1,
            spv::ImageFormatUnknown),
        "xe_transfer_host_depth");
    builder.addDecoration(
        host_depth_source_texture, spv::DecorationDescriptorSet,
        xe::bit_count(
            pipeline_layout_info.used_descriptor_sets &
            (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)));
    builder.addDecoration(host_depth_source_texture, spv::DecorationBinding, 0);
  }
  // Push constants.
  id_vector_temp.clear();
  uint32_t push_constants_member_host_depth_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordHostDepthAddressBit) {
    push_constants_member_host_depth_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_address = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordAddressBit) {
    push_constants_member_address = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  uint32_t push_constants_member_stencil_mask = UINT32_MAX;
  if (pipeline_layout_info.used_push_constant_dwords &
      kTransferUsedPushConstantDwordStencilMaskBit) {
    push_constants_member_stencil_mask = uint32_t(id_vector_temp.size());
    id_vector_temp.push_back(type_uint);
  }
  spv::Id push_constants = spv::NoResult;
  if (!id_vector_temp.empty()) {
    spv::Id type_push_constants =
        builder.makeStructType(id_vector_temp, "XeTransferPushConstants");
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordHostDepthAddressBit) {
      assert_true(push_constants_member_host_depth_address != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_host_depth_address,
                            "host_depth_address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_host_depth_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordAddressBit) {
      assert_true(push_constants_member_address != UINT32_MAX);
      builder.addMemberName(type_push_constants, push_constants_member_address,
                            "address");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_address,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(pipeline_layout_info.used_push_constant_dwords &
                            (kTransferUsedPushConstantDwordAddressBit - 1)));
    }
    if (pipeline_layout_info.used_push_constant_dwords &
        kTransferUsedPushConstantDwordStencilMaskBit) {
      assert_true(push_constants_member_stencil_mask != UINT32_MAX);
      builder.addMemberName(type_push_constants,
                            push_constants_member_stencil_mask, "stencil_mask");
      builder.addMemberDecoration(
          type_push_constants, push_constants_member_stencil_mask,
          spv::DecorationOffset,
          sizeof(uint32_t) *
              xe::bit_count(
                  pipeline_layout_info.used_push_constant_dwords &
                  (kTransferUsedPushConstantDwordStencilMaskBit - 1)));
    }
    builder.addDecoration(type_push_constants, spv::DecorationBlock);
    push_constants = builder.createVariable(
        spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
        "xe_transfer_push_constants");
  }

  // Coordinate inputs.
  spv::Id input_fragment_coord = builder.createVariable(
      spv::NoPrecision, spv::StorageClassInput, type_float4, "gl_FragCoord");
  builder.addDecoration(input_fragment_coord, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::FragCoord));
  main_interface.push_back(input_fragment_coord);
  spv::Id input_sample_id = spv::NoResult;
  spv::Id spec_const_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      // One draw for all samples.
      builder.addCapability(spv::CapabilitySampleRateShading);
      input_sample_id = builder.createVariable(
          spv::NoPrecision, spv::StorageClassInput, type_int, "gl_SampleID");
      builder.addDecoration(input_sample_id, spv::DecorationFlat);
      builder.addDecoration(input_sample_id, spv::DecorationBuiltIn,
                            static_cast<int>(spv::BuiltIn::SampleId));
      main_interface.push_back(input_sample_id);
    } else {
      // One sample per draw, with different sample masks.
      spec_const_sample_id = builder.makeUintConstant(0, true);
      builder.addName(spec_const_sample_id, "xe_transfer_sample_id");
      builder.addDecoration(spec_const_sample_id, spv::DecorationSpecId, 0);
    }
  }

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // Working with unsigned numbers for simplicity now, bitcasting to signed will
  // be done at texture fetch.

  uint32_t tile_width_samples =
      xenos::kEdramTileWidthSamples * draw_resolution_scale_x();
  uint32_t tile_height_samples =
      xenos::kEdramTileHeightSamples * draw_resolution_scale_y();

  // Split the destination pixel index into 32bpp tile and 32bpp-tile-relative
  // pixel index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the pixel position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  uint_vector_temp.clear();
  uint_vector_temp.push_back(0);
  uint_vector_temp.push_back(1);
  spv::Id dest_pixel_coord = builder.createUnaryOp(
      spv::OpConvertFToU, type_uint2,
      builder.createRvalueSwizzle(
          spv::NoPrecision, type_float2,
          builder.createLoad(input_fragment_coord, spv::NoPrecision),
          uint_vector_temp));
  spv::Id dest_pixel_x =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 0);
  spv::Id const_dest_tile_width_pixels = builder.makeUintConstant(
      tile_width_samples >>
      (uint32_t(dest_is_64bpp) +
       uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k4X)));
  spv::Id dest_tile_index_x = builder.createBinOp(
      spv::OpUDiv, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_tile_pixel_x = builder.createBinOp(
      spv::OpUMod, type_uint, dest_pixel_x, const_dest_tile_width_pixels);
  spv::Id dest_pixel_y =
      builder.createCompositeExtract(dest_pixel_coord, type_uint, 1);
  spv::Id const_dest_tile_height_pixels = builder.makeUintConstant(
      tile_height_samples >>
      uint32_t(key.dest_msaa_samples >= xenos::MsaaSamples::k2X));
  spv::Id dest_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, dest_pixel_y, const_dest_tile_height_pixels);
  spv::Id dest_tile_pixel_y = builder.createBinOp(
      spv::OpUMod, type_uint, dest_pixel_y, const_dest_tile_height_pixels);

  assert_true(push_constants_member_address != UINT32_MAX);
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeIntConstant(int32_t(push_constants_member_address)));
  spv::Id address_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);

  // Calculate the 32bpp tile index from its X and Y parts.
  spv::Id dest_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, address_constant,
              builder.makeUintConstant(0),
              builder.makeUintConstant(xenos::kEdramPitchTilesBits)),
          dest_tile_index_y),
      dest_tile_index_x);

  // Load the destination sample index.
  spv::Id dest_sample_id = spv::NoResult;
  if (key.dest_msaa_samples != xenos::MsaaSamples::k1X) {
    if (device_properties.sampleRateShading) {
      assert_true(input_sample_id != spv::NoResult);
      dest_sample_id = builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createLoad(input_sample_id, spv::NoPrecision));
    } else {
      assert_true(spec_const_sample_id != spv::NoResult);
      // Already uint.
      dest_sample_id = spec_const_sample_id;
    }
  }

  // Transform the destination framebuffer pixel and sample coordinates into the
  // source texture pixel and sample coordinates.

  // First sample bit at 4x with Vulkan standard locations - horizontal sample.
  // Second sample bit at 4x with Vulkan standard locations - vertical sample.
  // At 2x:
  // - Native 2x: top is 1 in Vulkan, bottom is 0.
  // - 2x as 4x: top is 0, bottom is 3.

  spv::Id source_sample_id = dest_sample_id;
  spv::Id source_tile_pixel_x = dest_tile_pixel_x;
  spv::Id source_tile_pixel_y = dest_tile_pixel_y;
  spv::Id source_color_half = spv::NoResult;
  if (!source_is_64bpp && dest_is_64bpp) {
    // 32bpp -> 64bpp, need two samples of the source.
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 32bpp -> 64bpp, 4x ->.
      // Source has 32bpp halves in two adjacent samples.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 4x -> 4x.
        // 1 destination horizontal sample = 2 source horizontal samples.
        // D p0,0 s0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1,0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p0,0 s0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p0,0 s1,1 = S p1,0 s0,1 | S p1,0 s1,1
        // Thus destination horizontal sample -> source horizontal pixel,
        // vertical samples are 1:1.
        source_sample_id =
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                builder.makeUintConstant(1 << 1));
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
            builder.makeUintConstant(1), builder.makeUintConstant(31));
      } else if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
        // 32bpp -> 64bpp, 4x -> 2x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 s0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,0 s1 = S p0,0 s0,1 | S p0,0 s1,1
        // D p1,0 s0 = S p1,0 s0,0 | S p1,0 s1,0
        // D p1,0 s1 = S p1,0 s0,1 | S p1,0 s1,1
        // Pixel index can be reused. Sample 1 (for native 2x) or 0 (for 2x as
        // 4x) should become samples 01, sample 0 or 3 should become samples 23.
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(
              spv::OpShiftLeftLogical, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              builder.makeUintConstant(1));
        } else {
          source_sample_id =
              builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1 << 1));
        }
      } else {
        // 32bpp -> 64bpp, 4x -> 1x.
        // 1 destination horizontal pixel = 2 source horizontal samples.
        // D p0,0 = S p0,0 s0,0 | S p0,0 s1,0
        // D p0,1 = S p0,0 s0,1 | S p0,0 s1,1
        // Horizontal pixel index can be reused. Vertical pixel 1 should
        // become sample 2.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, builder.makeUintConstant(0),
            dest_tile_pixel_y, builder.makeUintConstant(1),
            builder.makeUintConstant(1));
        source_tile_pixel_y =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_y, builder.makeUintConstant(1));
      }
    } else {
      // 32bpp -> 64bpp, 1x/2x ->.
      // Source has 32bpp halves in two adjacent pixels.
      if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 32bpp -> 64bpp, 1x/2x -> 4x.
        // The X part.
        // 1 destination horizontal sample = 2 source horizontal pixels.
        source_tile_pixel_x = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(2)),
            dest_sample_id, builder.makeUintConstant(1),
            builder.makeUintConstant(1));
        // Y is handled by common code.
      } else {
        // 32bpp -> 64bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 1 destination horizontal pixel = 2 source horizontal pixels.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
    }
  } else if (source_is_64bpp && !dest_is_64bpp) {
    // 64bpp -> 32bpp, also the half to load.
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 64bpp -> 32bpp, -> 4x.
      // The needed half is in the destination horizontal sample index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 4x.
        // D p0,0 s0,0 = S s0,0 low
        // D p0,0 s1,0 = S s0,0 high
        // D p1,0 s0,0 = S s1,0 low
        // D p1,0 s1,0 = S s1,0 high
        // Vertical pixel and sample (second bit) addressing is the same.
        // However, 1 horizontal destination pixel = 1 horizontal source sample.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, dest_sample_id, dest_tile_pixel_x,
            builder.makeUintConstant(0), builder.makeUintConstant(1));
        // 2 destination horizontal samples = 1 source horizontal sample, thus
        // 2 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 4x.
        // 2 destination horizontal samples = 1 source horizontal pixel, thus
        // 1 destination horizontal pixel = 1 source horizontal pixel. Can reuse
        // horizontal pixel index.
        // Y is handled by common code.
      }
      // Half from the destination horizontal sample index.
      source_color_half =
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_sample_id,
                              builder.makeUintConstant(1));
    } else {
      // 64bpp -> 32bpp, -> 1x/2x.
      // The needed half is in the destination horizontal pixel index.
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // 64bpp -> 32bpp, 4x -> 1x/2x.
        // (Destination horizontal pixel >> 1) & 1 = source horizontal sample
        // (first bit).
        source_sample_id = builder.createTriOp(
            spv::OpBitFieldUExtract, type_uint, dest_tile_pixel_x,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // 64bpp -> 32bpp, 4x -> 2x.
          // Destination vertical samples (1/0 in the first bit for native 2x or
          // 0/1 in the second bit for 2x as 4x) = source vertical samples
          // (second bit).
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, source_sample_id,
                builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                    dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(1));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id,
                source_sample_id, builder.makeUintConstant(0),
                builder.makeUintConstant(1));
          }
        } else {
          // 64bpp -> 32bpp, 4x -> 1x.
          // 1 destination vertical pixel = 1 source vertical sample.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
              builder.makeUintConstant(1));
        }
        // 2 destination horizontal pixels = 1 source horizontal sample.
        // 4 destination horizontal pixels = 1 source horizontal pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(2));
      } else {
        // 64bpp -> 32bpp, 1x/2x -> 1x/2x.
        // The X part.
        // 2 destination horizontal pixels = 1 destination source pixel.
        source_tile_pixel_x =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_x, builder.makeUintConstant(1));
        // Y is handled by common code.
      }
      // Half from the destination horizontal pixel index.
      source_color_half =
          builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
                              builder.makeUintConstant(1));
    }
    assert_true(source_color_half != spv::NoResult);
  } else {
    // Same bit count.
    if (key.source_msaa_samples != key.dest_msaa_samples) {
      if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
        // Same BPP, 4x -> 1x/2x.
        if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
          // Same BPP, 4x -> 2x.
          // Horizontal pixels to samples. Vertical sample (1/0 in the first bit
          // for native 2x or 0/1 in the second bit for 2x as 4x) to second
          // sample bit.
          if (msaa_2x_attachments_supported_) {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                    dest_sample_id,
                                    builder.makeUintConstant(1)),
                builder.makeUintConstant(1), builder.makeUintConstant(31));
          } else {
            source_sample_id = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, dest_sample_id,
                dest_tile_pixel_x, builder.makeUintConstant(0),
                builder.makeUintConstant(1));
          }
          source_tile_pixel_x = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
              builder.makeUintConstant(1));
        } else {
          // Same BPP, 4x -> 1x.
          // Pixels to samples.
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseAnd, type_uint,
                                  dest_tile_pixel_x,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
          source_tile_pixel_x = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
              builder.makeUintConstant(1));
          source_tile_pixel_y = builder.createBinOp(
              spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
              builder.makeUintConstant(1));
        }
      } else {
        // Same BPP, 1x/2x -> 1x/2x/4x (as long as they're different).
        // Only the X part - Y is handled by common code.
        if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
          // Horizontal samples to pixels.
          source_tile_pixel_x = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, dest_sample_id,
              dest_tile_pixel_x, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        }
      }
    }
  }
  // Common source Y and sample index for 1x/2x AA sources, independent of bits
  // per sample.
  if (key.source_msaa_samples < xenos::MsaaSamples::k4X &&
      key.source_msaa_samples != key.dest_msaa_samples) {
    if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
      // 1x/2x -> 4x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 4x.
        // Vertical samples (second bit) of 4x destination to vertical sample
        // (1, 0 for native 2x, or 0, 3 for 2x as 4x) of 2x source.
        source_sample_id =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_sample_id, builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                                 source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_sample_id, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
        }
      } else {
        // 1x -> 4x.
        // Vertical samples (second bit) to Y pixels.
        source_tile_pixel_y = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_sample_id, builder.makeUintConstant(1)),
            dest_tile_pixel_y, builder.makeUintConstant(1),
            builder.makeUintConstant(31));
      }
    } else {
      // 1x/2x -> different 1x/2x.
      if (key.source_msaa_samples == xenos::MsaaSamples::k2X) {
        // 2x -> 1x.
        // Vertical pixels of 2x destination to vertical samples (1, 0 for
        // native 2x, or 0, 3 for 2x as 4x) of 1x source.
        source_sample_id =
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                                builder.makeUintConstant(1));
        if (msaa_2x_attachments_supported_) {
          source_sample_id = builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                                 source_sample_id,
                                                 builder.makeUintConstant(1));
        } else {
          source_sample_id = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_sample_id,
              source_sample_id, builder.makeUintConstant(1),
              builder.makeUintConstant(1));
        }
        source_tile_pixel_y =
            builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                dest_tile_pixel_y, builder.makeUintConstant(1));
      } else {
        // 1x -> 2x.
        // Vertical samples (1/0 in the first bit for native 2x or 0/1 in the
        // second bit for 2x as 4x) of 2x destination to vertical pixels of 1x
        // source.
        if (msaa_2x_attachments_supported_) {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_sample_id,
                                  builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        } else {
          source_tile_pixel_y = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint,
              builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                  dest_sample_id, builder.makeUintConstant(1)),
              dest_tile_pixel_y, builder.makeUintConstant(1),
              builder.makeUintConstant(31));
        }
      }
    }
  }

  uint32_t source_pixel_width_dwords_log2 =
      uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k4X) +
      uint32_t(source_is_64bpp);

  if (source_is_color != dest_is_color &&
      !android_diag_depth_to_color_disable_column_swap) {
    // Copying between color and depth / stencil - swap 40-32bpp-sample columns
    // in the pixel index within the source 32bpp tile.
    if (android_diag_depth_to_color_sample_override) {
      source_tile_pixel_x =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                              dest_tile_pixel_x, builder.makeUintConstant(1));
      source_tile_pixel_y =
          builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                              dest_tile_pixel_y, builder.makeUintConstant(1));
      spv::Id sample_x = builder.createBinOp(
          spv::OpBitwiseAnd, type_uint, dest_tile_pixel_x,
          builder.makeUintConstant(1));
      spv::Id sample_y = builder.createBinOp(
          spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
          builder.makeUintConstant(1));
      if (android_diag_depth_to_color_sample_mode == 1) {
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, sample_y, sample_x,
            builder.makeUintConstant(1), builder.makeUintConstant(1));
      } else if (android_diag_depth_to_color_sample_mode == 2) {
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpBitwiseXor, type_uint, sample_x,
                                builder.makeUintConstant(1)),
            sample_y, builder.makeUintConstant(1), builder.makeUintConstant(1));
      } else if (android_diag_depth_to_color_sample_mode == 3) {
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, sample_x,
            builder.createBinOp(spv::OpBitwiseXor, type_uint, sample_y,
                                builder.makeUintConstant(1)),
            builder.makeUintConstant(1), builder.makeUintConstant(1));
      } else if (android_diag_depth_to_color_sample_mode >= 4 &&
                 android_diag_depth_to_color_sample_mode <= 7) {
        source_sample_id = builder.makeUintConstant(
            uint32_t(android_diag_depth_to_color_sample_mode - 4));
      }
    }
    uint32_t source_32bpp_tile_half_pixels =
        tile_width_samples >> (1 + source_pixel_width_dwords_log2);
    source_tile_pixel_x = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  source_tile_pixel_x),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(
                    spv::OpULessThan, builder.makeBoolType(),
                    source_tile_pixel_x,
                    builder.makeUintConstant(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(int32_t(source_32bpp_tile_half_pixels)),
                builder.makeIntConstant(
                    -int32_t(source_32bpp_tile_half_pixels)))));
  }

  // Transform the destination 32bpp tile index into the source. After the
  // addition, it may be negative - in which case, the transfer is done across
  // EDRAM addressing wrapping, and xenos::kEdramTileCount must be added to it,
  // but `& (xenos::kEdramTileCount - 1)` handles that regardless of the sign.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(
                  spv::OpBitFieldSExtract, type_int,
                  builder.createUnaryOp(spv::OpBitcast, type_int,
                                        address_constant),
                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
  // Split the source 32bpp tile index into X and Y tile index within the source
  // image.
  spv::Id source_pitch_tiles = builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint, address_constant,
      builder.makeUintConstant(xenos::kEdramPitchTilesBits),
      builder.makeUintConstant(xenos::kEdramPitchTilesBits));
  spv::Id source_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x = builder.createBinOp(
      spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Finally calculate the source texture coordinates.
  spv::Id source_pixel_x_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(tile_width_samples >>
                                       source_pixel_width_dwords_log2),
              source_tile_index_x),
          source_tile_pixel_x));
  spv::Id source_pixel_y_int = builder.createUnaryOp(
      spv::OpBitcast, type_int,
      builder.createBinOp(
          spv::OpIAdd, type_uint,
          builder.createBinOp(
              spv::OpIMul, type_uint,
              builder.makeUintConstant(
                  tile_height_samples >>
                  uint32_t(key.source_msaa_samples >= xenos::MsaaSamples::k2X)),
              source_tile_index_y),
          source_tile_pixel_y));

  // Load the source.

  spv::Builder::TextureParameters source_texture_parameters = {};
  id_vector_temp.clear();
  id_vector_temp.push_back(source_pixel_x_int);
  id_vector_temp.push_back(source_pixel_y_int);
  spv::Id source_coordinates[2] = {
      builder.createCompositeConstruct(type_int2, id_vector_temp),
  };
  spv::Id source_sample_ids_int[2] = {};
  if (key.source_msaa_samples != xenos::MsaaSamples::k1X) {
    source_sample_ids_int[0] =
        builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
  } else {
    source_texture_parameters.lod = builder.makeIntConstant(0);
  }
  // Go to the next sample or pixel along X if need to load two dwords.
  bool source_load_is_two_32bpp_samples = !source_is_64bpp && dest_is_64bpp;
  if (source_load_is_two_32bpp_samples) {
    if (key.source_msaa_samples >= xenos::MsaaSamples::k4X) {
      source_coordinates[1] = source_coordinates[0];
      source_sample_ids_int[1] = builder.createBinOp(
          spv::OpBitwiseOr, type_int, source_sample_ids_int[0],
          builder.makeIntConstant(1));
    } else {
      id_vector_temp.clear();
      id_vector_temp.push_back(builder.createBinOp(spv::OpBitwiseOr, type_int,
                                                   source_pixel_x_int,
                                                   builder.makeIntConstant(1)));
      id_vector_temp.push_back(source_pixel_y_int);
      source_coordinates[1] =
          builder.createCompositeConstruct(type_int2, id_vector_temp);
      source_sample_ids_int[1] = source_sample_ids_int[0];
    }
  }
  spv::Id source_color[2][4] = {};
  if (source_color_texture != spv::NoResult) {
    source_texture_parameters.sampler =
        builder.createLoad(source_color_texture, spv::NoPrecision);
    assert_true(source_color_component_type != spv::NoType);
    spv::Id source_color_vec4_type =
        builder.makeVectorType(source_color_component_type, 4);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      spv::Id source_color_vec4 = builder.createTextureCall(
          spv::NoPrecision, source_color_vec4_type, false, true, false, false,
          false, source_texture_parameters, spv::ImageOperandsMaskNone);
      uint32_t source_color_components_remaining =
          source_color_texture_component_mask;
      uint32_t source_color_component_index;
      while (xe::bit_scan_forward(source_color_components_remaining,
                                  &source_color_component_index)) {
        source_color_components_remaining &=
            ~(uint32_t(1) << source_color_component_index);
        source_color[i][source_color_component_index] =
            builder.createCompositeExtract(source_color_vec4,
                                           source_color_component_type,
                                           source_color_component_index);
      }
    }
  }
  spv::Id source_depth_float[2] = {};
  if (source_depth_texture != spv::NoResult &&
      !android_diag_depth_to_color_pattern &&
      !android_diag_depth_to_color_constant_packed &&
      !android_diag_depth_to_color_stencil_only) {
    source_texture_parameters.sampler =
        builder.createLoad(source_depth_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_depth_float[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_float4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_float, 0);
    }
  }
  spv::Id source_stencil[2] = {};
  if (source_stencil_texture != spv::NoResult &&
      !android_diag_depth_to_color_pattern &&
      !android_diag_depth_to_color_constant_packed &&
      !android_diag_depth_to_color_depth_only) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    for (uint32_t i = 0; i <= uint32_t(source_load_is_two_32bpp_samples); ++i) {
      source_texture_parameters.coords = source_coordinates[i];
      source_texture_parameters.sample = source_sample_ids_int[i];
      source_stencil[i] = builder.createCompositeExtract(
          builder.createTextureCall(
              spv::NoPrecision, type_uint4, false, true, false, false, false,
              source_texture_parameters, spv::ImageOperandsMaskNone),
          type_uint, 0);
    }
  }
  if (android_diag_depth_to_color_depth_only) {
    source_stencil[0] = builder.makeUintConstant(0);
    source_stencil[1] = builder.makeUintConstant(0);
  }
  if (android_diag_depth_to_color_stencil_only) {
    source_depth_float[0] = builder.makeFloatConstant(0.5f);
    source_depth_float[1] = builder.makeFloatConstant(0.5f);
  }

  // Pick the needed 32bpp half of the 64bpp color.
  if (source_is_64bpp && !dest_is_64bpp) {
    uint32_t source_color_half_component_count =
        source_color_format_component_count >> 1;
    assert_true(source_color_half != spv::NoResult);
    spv::Id source_color_is_second_half =
        builder.createBinOp(spv::OpINotEqual, type_bool, source_color_half,
                            builder.makeUintConstant(0));
    if (mode.output == TransferOutput::kStencilBit) {
      source_color[0][0] = builder.createTriOp(
          spv::OpSelect, source_color_component_type,
          source_color_is_second_half,
          source_color[0][source_color_half_component_count],
          source_color[0][0]);
    } else {
      for (uint32_t i = 0; i < source_color_half_component_count; ++i) {
        source_color[0][i] = builder.createTriOp(
            spv::OpSelect, source_color_component_type,
            source_color_is_second_half,
            source_color[0][source_color_half_component_count + i],
            source_color[0][i]);
      }
    }
  }

  if (output_fragment_stencil_ref != spv::NoResult &&
      source_stencil[0] != spv::NoResult) {
    // For the depth -> depth case, write the stencil directly to the output.
    assert_true(mode.output == TransferOutput::kDepth);
    builder.createStore(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_stencil[0]),
        output_fragment_stencil_ref);
  }

  if (dest_is_64bpp) {
    // Construct the 64bpp color from two 32-bit samples or one 64-bit sample.
    // If `packed` (two uints) are created, use the generic path involving
    // unpacking.
    // Otherwise, the fragment data output must be written to directly by the
    // reached control flow path.
    spv::Id packed[2] = {};
    if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
          spv::Id component_width = builder.makeUintConstant(8);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              source_color[i][j], unorm_scale),
                          unorm_round_offset)),
                  builder.makeUintConstant(8 * j), component_width);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[i][0], unorm_scale_rgb),
                    unorm_round_offset));
            for (uint32_t j = 1; j < 4; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(
                              spv::OpFMul, type_float, source_color[i][j],
                              j == 3 ? unorm_scale_a : unorm_scale_rgb),
                          unorm_round_offset)),
                  builder.makeUintConstant(10 * j),
                  j == 3 ? width_a : width_rgb);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          spv::Id width_rgb = builder.makeUintConstant(10);
          spv::Id float_0 = builder.makeFloatConstant(0.0f);
          spv::Id float_1 = builder.makeFloatConstant(1.0f);
          spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
          spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
          spv::Id offset_a = builder.makeUintConstant(30);
          spv::Id width_a = builder.makeUintConstant(2);
          for (uint32_t i = 0; i < 2; ++i) {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed[i] = ConvertFloat32To7e3ForRenderTarget(
                builder, source_color[i][0], ext_inst_glsl_std_450,
                UsesScaledUNorm7e3RenderTargets());
            for (uint32_t j = 1; j < 3; ++j) {
              packed[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed[i],
                  ConvertFloat32To7e3ForRenderTarget(
                      builder, source_color[i][j], ext_inst_glsl_std_450,
                      UsesScaledUNorm7e3RenderTargets()),
                  builder.makeUintConstant(10 * j), width_rgb);
            }
            // Saturate and convert the alpha.
            spv::Id alpha_saturated = builder.createTriBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                source_color[i][3], float_0, float_1);
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint, packed[i],
                builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createBinOp(
                        spv::OpFAdd, type_float,
                        builder.createBinOp(spv::OpFMul, type_float,
                                            alpha_saturated, unorm_scale_a),
                        unorm_round_offset)),
                offset_a, width_a);
          }
        } break;
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[i][0],
                  source_color[i][1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[i >> 1][i & 1]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (dest_color_format ==
              xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
            spv::Id component_offset_width = builder.makeUintConstant(16);
            spv::Id color_16_in_32[2];
            for (uint32_t i = 0; i < 2; ++i) {
              color_16_in_32[i] = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, source_color[0][i << 1],
                  source_color[0][(i << 1) + 1], component_offset_width,
                  component_offset_width);
            }
            id_vector_temp.clear();
            id_vector_temp.push_back(color_16_in_32[0]);
            id_vector_temp.push_back(color_16_in_32[1]);
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[i][0];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          for (uint32_t i = 0; i < 2; ++i) {
            packed[i] = source_color[0][i];
            if (!source_color_is_uint) {
              packed[i] =
                  builder.createUnaryOp(spv::OpBitcast, type_uint, packed[i]);
            }
          }
        } break;
      }
    } else {
      assert_true(source_depth_texture != spv::NoResult);
      assert_true(source_stencil_texture != spv::NoResult);
      spv::Id depth_offset = builder.makeUintConstant(8);
      spv::Id depth_width = builder.makeUintConstant(24);
      for (uint32_t i = 0; i < 2; ++i) {
        spv::Id depth24 = spv::NoResult;
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the
            // correct conversion, adding +0.5 and rounding towards zero results
            // in red instead of black in the 4D5307E6 clear shader.
            depth24 = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[i],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[i], depth_float24_round(), true,
                ext_inst_glsl_std_450);
          } break;
        }
        // Merge depth and stencil.
        packed[i] = builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                         source_stencil[i], depth24,
                                         depth_offset, depth_width);
      }
    }
    // Common path unless there was a specialized one - unpack two packed 32-bit
    // parts.
    if (packed[0] != spv::NoResult) {
      assert_true(packed[1] != spv::NoResult);
      if (dest_color_format == xenos::ColorRenderTargetFormat::k_32_32_FLOAT) {
        id_vector_temp.clear();
        id_vector_temp.push_back(packed[0]);
        id_vector_temp.push_back(packed[1]);
        // Multisampled sampled images are optional in Vulkan, and image views
        // of different formats can't be created separately for sampled image
        // and color attachment usages, so no multisampled integer sampled image
        // support implies no multisampled integer framebuffer attachment
        // support in Xenia.
        if (!dest_color_is_uint) {
          for (spv::Id& float32 : id_vector_temp) {
            float32 =
                builder.createUnaryOp(spv::OpBitcast, type_float, float32);
          }
        }
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      } else {
        spv::Id const_uint_0 = builder.makeUintConstant(0);
        spv::Id const_uint_16 = builder.makeUintConstant(16);
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 4; ++i) {
          id_vector_temp.push_back(builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, packed[i >> 1],
              (i & 1) ? const_uint_16 : const_uint_0, const_uint_16));
        }
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                             id_vector_temp),
                            output_fragment_data);
      }
    }
  } else {
    // If `packed` is created, use the generic path involving unpacking.
    // - For a color destination, the packed 32bpp color.
    // - For a depth / stencil destination, stencil in 0:7, depth in 8:31
    //   normally, or depth in 0:23 and zeros in 24:31 with packed_only_depth.
    // - For a stencil bit, stencil in 0:7.
    // Otherwise, the fragment data or fragment depth / stencil output must be
    // written to directly by the reached control flow path.
    spv::Id packed = spv::NoResult;
    bool packed_only_depth = false;
    if (android_diag_depth_to_color_constant_packed) {
      // Packed D24S8 diagnostic value: depth24 0x555555, stencil 0xAA.
      packed = builder.makeUintConstant(0x555555AAu);
    } else if (android_diag_depth_to_color_pattern) {
      spv::Id const_uint_0 = builder.makeUintConstant(0);
      spv::Id dest_tile_x = builder.createBinOp(
          spv::OpUDiv, type_uint, dest_pixel_x,
          builder.makeUintConstant(xenos::kEdramTileWidthSamples));
      spv::Id dest_tile_y = builder.createBinOp(
          spv::OpUDiv, type_uint, dest_pixel_y,
          builder.makeUintConstant(xenos::kEdramTileHeightSamples));
      spv::Id pattern_r = builder.createBinOp(
          spv::OpFDiv, type_float,
          builder.createUnaryOp(
              spv::OpConvertUToF, type_float,
              builder.createBinOp(spv::OpUMod, type_uint, dest_tile_x,
                                  builder.makeUintConstant(16))),
          builder.makeFloatConstant(15.0f));
      spv::Id pattern_g = builder.createBinOp(
          spv::OpFDiv, type_float,
          builder.createUnaryOp(
              spv::OpConvertUToF, type_float,
              builder.createBinOp(spv::OpUMod, type_uint, dest_tile_y,
                                  builder.makeUintConstant(16))),
          builder.makeFloatConstant(15.0f));
      spv::Id pattern_checker = builder.createBinOp(
          spv::OpBitwiseAnd, type_uint,
          builder.createBinOp(spv::OpBitwiseXor, type_uint, dest_pixel_x,
                              dest_pixel_y),
          builder.makeUintConstant(1));
      spv::Id pattern_b = builder.createTriOp(
          spv::OpSelect, type_float,
          builder.createBinOp(spv::OpINotEqual, builder.makeBoolType(),
                              pattern_checker, const_uint_0),
          builder.makeFloatConstant(1.0f), builder.makeFloatConstant(0.0f));
      id_vector_temp.clear();
      id_vector_temp.push_back(pattern_r);
      id_vector_temp.push_back(pattern_g);
      id_vector_temp.push_back(pattern_b);
      id_vector_temp.push_back(builder.makeFloatConstant(1.0f));
      builder.createStore(builder.createCompositeConstruct(type_fragment_data,
                                                           id_vector_temp),
                          output_fragment_data);
    } else if (source_is_color) {
      switch (source_color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)) {
            // Same format - passthrough.
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
            uint32_t packed_component_offset = 0;
            if (mode.output == TransferOutput::kDepth) {
              // When need only depth, not stencil, skip the red component, and
              // put the depth from GBA directly in the lower bits.
              packed_component_offset = 1;
              packed_only_depth = true;
              if (output_fragment_stencil_ref != spv::NoResult) {
                builder.createStore(
                    builder.createUnaryOp(
                        spv::OpBitcast, type_int,
                        builder.createUnaryOp(
                            spv::OpConvertFToU, type_uint,
                            builder.createBinOp(
                                spv::OpFAdd, type_float,
                                builder.createBinOp(spv::OpFMul, type_float,
                                                    source_color[0][0],
                                                    unorm_scale),
                                unorm_round_offset))),
                    output_fragment_stencil_ref);
              }
            }
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(
                        spv::OpFMul, type_float,
                        source_color[0][packed_component_offset], unorm_scale),
                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id component_width = builder.makeUintConstant(8);
              for (uint32_t i = 1; i < 4 - packed_component_offset; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float,
                                source_color[0][packed_component_offset + i],
                                unorm_scale),
                            unorm_round_offset)),
                    builder.makeUintConstant(8 * i), component_width);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_AS_10_10_10_10)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else if (android_value_convert_1010102_to_8888 && dest_is_color &&
                     IsAndroidHalo8888ColorFormat(dest_color_format)) {
            StoreAndroidHalo1010102To8888ValueConvert(
                builder, source_color[0], type_fragment_data,
                output_fragment_data, id_vector_temp);
          } else {
            spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
            spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        source_color[0][0], unorm_scale_rgb),
                    unorm_round_offset));
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              for (uint32_t i = 1; i < 4; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    builder.createUnaryOp(
                        spv::OpConvertFToU, type_uint,
                        builder.createBinOp(
                            spv::OpFAdd, type_float,
                            builder.createBinOp(
                                spv::OpFMul, type_float, source_color[0][i],
                                i == 3 ? unorm_scale_a : unorm_scale_rgb),
                            unorm_round_offset)),
                    builder.makeUintConstant(10 * i),
                    i == 3 ? width_a : width_rgb);
              }
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::
            k_2_10_10_10_FLOAT_AS_16_16_16_16: {
          if (dest_is_color &&
              (dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
               dest_color_format == xenos::ColorRenderTargetFormat::
                                        k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 4; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else if (android_value_convert_1010102_to_8888 && dest_is_color &&
                     IsAndroidHalo8888ColorFormat(dest_color_format)) {
            StoreAndroidHalo1010102To8888ValueConvert(
                builder, source_color[0], type_fragment_data,
                output_fragment_data, id_vector_temp);
          } else {
            // Float16 has a wider range for both color and alpha, also NaNs -
            // clamp and convert.
            packed = ConvertFloat32To7e3ForRenderTarget(
                builder, source_color[0][0], ext_inst_glsl_std_450,
                UsesScaledUNorm7e3RenderTargets());
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 1; i < 3; ++i) {
                packed = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, packed,
                    ConvertFloat32To7e3ForRenderTarget(
                        builder, source_color[0][i], ext_inst_glsl_std_450,
                        UsesScaledUNorm7e3RenderTargets()),
                    builder.makeUintConstant(10 * i), width_rgb);
              }
              // Saturate and convert the alpha.
              spv::Id alpha_saturated = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                  source_color[0][3], builder.makeFloatConstant(0.0f),
                  builder.makeFloatConstant(1.0f));
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed,
                  builder.createUnaryOp(
                      spv::OpConvertFToU, type_uint,
                      builder.createBinOp(
                          spv::OpFAdd, type_float,
                          builder.createBinOp(spv::OpFMul, type_float,
                                              alpha_saturated,
                                              builder.makeFloatConstant(3.0f)),
                          builder.makeFloatConstant(0.5f))),
                  builder.makeUintConstant(30), builder.makeUintConstant(2));
            }
          }
        } break;
        case xenos::ColorRenderTargetFormat::k_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
          if (android_value_convert_16bit_to_8888 && dest_is_color &&
              dest_color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8) {
            const bool source_16bit_is_float =
                source_color_format ==
                    xenos::ColorRenderTargetFormat::k_16_16_FLOAT ||
                source_color_format ==
                    xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT;
            const spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
            const spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
            id_vector_temp.clear();
            if (source_16bit_is_float) {
              const spv::Id component_offset_width =
                  builder.makeUintConstant(16);
              for (uint32_t pair = 0; pair < 2; ++pair) {
                spv::Id packed_16 = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint,
                    source_color[0][pair * 2],
                    source_color[0][pair * 2 + 1], component_offset_width,
                    component_offset_width);
                spv::Id components_float2 = builder.createUnaryBuiltinCall(
                    type_float2, ext_inst_glsl_std_450,
                    GLSLstd450UnpackHalf2x16, packed_16);
                for (uint32_t component = 0; component < 2; ++component) {
                  id_vector_temp.push_back(builder.createCompositeExtract(
                      components_float2, type_float, component));
                }
              }
            } else {
              const spv::Id component_scale =
                  builder.makeFloatConstant(32.0f / 32767.0f);
              const spv::Id component_width = builder.makeUintConstant(16);
              const spv::Id component_offset = builder.makeUintConstant(0);
              for (uint32_t component = 0; component < 4; ++component) {
                spv::Id signed_component = builder.createTriOp(
                    spv::OpBitFieldSExtract, type_int,
                    builder.createUnaryOp(spv::OpBitcast, type_int,
                                          source_color[0][component]),
                    component_offset, component_width);
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(spv::OpConvertSToF, type_float,
                                          signed_component),
                    component_scale));
              }
            }
            for (spv::Id& component : id_vector_temp) {
              component = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                  component, const_float_0, const_float_1);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
          // All 64bpp formats, and all 16 bits per component formats, are
          // represented as integers in ownership transfer for safe handling of
          // NaN encodings and -32768 / -32767.
          // TODO(Triang3l): Handle the case when that's not true (no
          // multisampled sampled images, no 16-bit UNORM, no cross-packing
          // 32bpp aliasing on a portability subset device or a 64bpp format
          // where that wouldn't help anyway).
          if (dest_is_color &&
              (dest_color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
               dest_color_format ==
                   xenos::ColorRenderTargetFormat::k_16_16_FLOAT)) {
            id_vector_temp.clear();
            for (uint32_t i = 0; i < 2; ++i) {
              id_vector_temp.push_back(source_color[0][i]);
            }
            builder.createStore(builder.createCompositeConstruct(
                                    type_fragment_data, id_vector_temp),
                                output_fragment_data);
          } else {
            packed = source_color[0][0];
            if (mode.output != TransferOutput::kStencilBit) {
              spv::Id component_offset_width = builder.makeUintConstant(16);
              packed = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint, packed, source_color[0][1],
                  component_offset_width, component_offset_width);
            }
          }
          }
        } break;
        // Float32 is transferred as uint32 to preserve NaN encodings. However,
        // multisampled sampled image support is optional in Vulkan.
        case xenos::ColorRenderTargetFormat::k_32_FLOAT:
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
          packed = source_color[0][0];
          if (!source_color_is_uint) {
            packed = builder.createUnaryOp(spv::OpBitcast, type_uint, packed);
          }
        } break;
      }
    } else if (source_depth_float[0] != spv::NoResult) {
      if (mode.output == TransferOutput::kDepth &&
          dest_depth_format == source_depth_format) {
        builder.createStore(source_depth_float[0], output_fragment_depth);
      } else {
        switch (source_depth_format) {
          case xenos::DepthRenderTargetFormat::kD24S8: {
            // Round to the nearest even integer. This seems to be the correct
            // conversion, adding +0.5 and rounding towards zero results in red
            // instead of black in the 4D5307E6 clear shader.
            packed = builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createUnaryBuiltinCall(
                    type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                    builder.createBinOp(
                        spv::OpFMul, type_float, source_depth_float[0],
                        builder.makeFloatConstant(float(0xFFFFFF)))));
          } break;
          case xenos::DepthRenderTargetFormat::kD24FS8: {
            packed = SpirvShaderTranslator::PreClampedDepthTo20e4(
                builder, source_depth_float[0], depth_float24_round(), true,
                ext_inst_glsl_std_450);
          } break;
        }
        if (mode.output == TransferOutput::kDepth) {
          packed_only_depth = true;
        } else {
          // Merge depth and stencil.
          packed = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, source_stencil[0], packed,
              builder.makeUintConstant(8), builder.makeUintConstant(24));
        }
      }
    }
    // For stencil bit output, use stencil directly for the discard check.
    if (packed == spv::NoResult && mode.output == TransferOutput::kStencilBit) {
      packed = source_stencil[0];
    }
    switch (mode.output) {
      case TransferOutput::kColor: {
        // Unless a special path was taken, unpack the raw 32bpp value into the
        // 32bpp color output.
        if (packed != spv::NoResult) {
          switch (dest_color_format) {
            case xenos::ColorRenderTargetFormat::k_8_8_8_8:
            case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
              spv::Id component_width = builder.makeUintConstant(8);
              spv::Id unorm_scale = builder.makeFloatConstant(1.0f / 255.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(
                            spv::OpBitFieldUExtract, type_uint, packed,
                            builder.makeUintConstant(8 * i), component_width)),
                    unorm_scale));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10:
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
              spv::Id width_rgb = builder.makeUintConstant(10);
              spv::Id unorm_scale_rgb =
                  builder.makeFloatConstant(1.0f / 1023.0f);
              spv::Id width_a = builder.makeUintConstant(2);
              spv::Id unorm_scale_a = builder.makeFloatConstant(1.0f / 3.0f);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 4; ++i) {
                id_vector_temp.push_back(builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createUnaryOp(
                        spv::OpConvertUToF, type_float,
                        builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                            packed,
                                            builder.makeUintConstant(10 * i),
                                            i == 3 ? width_a : width_rgb)),
                    i == 3 ? unorm_scale_a : unorm_scale_rgb));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
            case xenos::ColorRenderTargetFormat::
                k_2_10_10_10_FLOAT_AS_16_16_16_16: {
              id_vector_temp.clear();
              // Color.
              spv::Id width_rgb = builder.makeUintConstant(10);
              for (uint32_t i = 0; i < 3; ++i) {
                id_vector_temp.push_back(Convert7e3ToFloat32ForRenderTarget(
                    builder, packed, 10 * i, false,
                    ext_inst_glsl_std_450,
                    UsesScaledUNorm7e3RenderTargets()));
              }
              // Alpha.
              id_vector_temp.push_back(builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                                          packed, builder.makeUintConstant(30),
                                          builder.makeUintConstant(2))),
                  builder.makeFloatConstant(1.0f / 3.0f)));
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_16_16:
            case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
              // All 16 bits per component formats are represented as integers
              // in ownership transfer for safe handling of NaN encodings and
              // -32768 / -32767.
              // TODO(Triang3l): Handle the case when that's not true (no
              // multisampled sampled images, no 16-bit UNORM, no cross-packing
              // 32bpp aliasing on a portability subset device or a 64bpp format
              // where that wouldn't help anyway).
              spv::Id component_offset_width = builder.makeUintConstant(16);
              id_vector_temp.clear();
              for (uint32_t i = 0; i < 2; ++i) {
                id_vector_temp.push_back(builder.createTriOp(
                    spv::OpBitFieldUExtract, type_uint, packed,
                    i ? component_offset_width : builder.makeUintConstant(0),
                    component_offset_width));
              }
              builder.createStore(builder.createCompositeConstruct(
                                      type_fragment_data, id_vector_temp),
                                  output_fragment_data);
            } break;
            case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
              // Float32 is transferred as uint32 to preserve NaN encodings.
              // However, multisampled sampled images are optional in Vulkan,
              // and image views of different formats can't be created
              // separately for sampled image and color attachment usages, so no
              // multisampled integer sampled image support implies no
              // multisampled integer framebuffer attachment support in Xenia.
              spv::Id float32 = packed;
              if (!dest_color_is_uint) {
                float32 =
                    builder.createUnaryOp(spv::OpBitcast, type_float, float32);
              }
              builder.createStore(float32, output_fragment_data);
            } break;
            default:
              // A 64bpp format (handled separately) or an invalid one.
              assert_unhandled_case(dest_color_format);
          }
        }
      } break;
      case TransferOutput::kDepth: {
        if (packed) {
          spv::Id guest_depth24 = packed;
          if (!packed_only_depth) {
            // Extract the depth bits.
            guest_depth24 =
                builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                    guest_depth24, builder.makeUintConstant(8));
          }
          // Load the host float32 depth, check if, when converted to the guest
          // format, it's the same as the guest source, thus up to date, and if
          // it is, write host float32 depth, otherwise do the guest -> host
          // conversion.
          spv::Id host_depth32 = spv::NoResult;
          if (host_depth_source_texture != spv::NoResult) {
            // Convert position and sample index from within the destination
            // tile to within the host depth source tile, like for the guest
            // render target, but for 32bpp -> 32bpp only.
            spv::Id host_depth_source_sample_id = dest_sample_id;
            spv::Id host_depth_source_tile_pixel_x = dest_tile_pixel_x;
            spv::Id host_depth_source_tile_pixel_y = dest_tile_pixel_y;
            if (key.host_depth_source_msaa_samples != key.dest_msaa_samples) {
              if (key.host_depth_source_msaa_samples >=
                  xenos::MsaaSamples::k4X) {
                // 4x -> 1x/2x.
                if (key.dest_msaa_samples == xenos::MsaaSamples::k2X) {
                  // 4x -> 2x.
                  // Horizontal pixels to samples. Vertical sample (1/0 in the
                  // first bit for native 2x or 0/1 in the second bit for 2x as
                  // 4x) to second sample bit.
                  if (msaa_2x_attachments_supported_) {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_tile_pixel_x,
                        builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                            dest_sample_id,
                                            builder.makeUintConstant(1)),
                        builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  } else {
                    host_depth_source_sample_id = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint, dest_sample_id,
                        dest_tile_pixel_x, builder.makeUintConstant(0),
                        builder.makeUintConstant(1));
                  }
                  host_depth_source_tile_pixel_x = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                      builder.makeUintConstant(1));
                } else {
                  // 4x -> 1x.
                  // Pixels to samples.
                  host_depth_source_sample_id = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint,
                      builder.createBinOp(spv::OpBitwiseAnd, type_uint,
                                          dest_tile_pixel_x,
                                          builder.makeUintConstant(1)),
                      dest_tile_pixel_y, builder.makeUintConstant(1),
                      builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_x = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_x,
                      builder.makeUintConstant(1));
                  host_depth_source_tile_pixel_y = builder.createBinOp(
                      spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                      builder.makeUintConstant(1));
                }
              } else {
                // 1x/2x -> 1x/2x/4x (as long as they're different).
                // Only the X part - Y is handled by common code.
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // Horizontal samples to pixels.
                  host_depth_source_tile_pixel_x = builder.createQuadOp(
                      spv::OpBitFieldInsert, type_uint, dest_sample_id,
                      dest_tile_pixel_x, builder.makeUintConstant(1),
                      builder.makeUintConstant(31));
                }
              }
              // Host depth source Y and sample index for 1x/2x AA sources.
              if (key.host_depth_source_msaa_samples <
                  xenos::MsaaSamples::k4X) {
                if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                  // 1x/2x -> 4x.
                  if (key.host_depth_source_msaa_samples ==
                      xenos::MsaaSamples::k2X) {
                    // 2x -> 4x.
                    // Vertical samples (second bit) of 4x destination to
                    // vertical sample (1, 0 for native 2x, or 0, 3 for 2x as
                    // 4x) of 2x source.
                    host_depth_source_sample_id = builder.createBinOp(
                        spv::OpShiftRightLogical, type_uint, dest_sample_id,
                        builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id =
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              host_depth_source_sample_id,
                                              builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id =
                          builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                               host_depth_source_sample_id,
                                               host_depth_source_sample_id,
                                               builder.makeUintConstant(1),
                                               builder.makeUintConstant(1));
                    }
                  } else {
                    // 1x -> 4x.
                    // Vertical samples (second bit) to Y pixels.
                    host_depth_source_tile_pixel_y = builder.createQuadOp(
                        spv::OpBitFieldInsert, type_uint,
                        builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                            dest_sample_id,
                                            builder.makeUintConstant(1)),
                        dest_tile_pixel_y, builder.makeUintConstant(1),
                        builder.makeUintConstant(31));
                  }
                } else {
                  // 1x/2x -> different 1x/2x.
                  if (key.host_depth_source_msaa_samples ==
                      xenos::MsaaSamples::k2X) {
                    // 2x -> 1x.
                    // Vertical pixels of 2x destination to vertical samples (1,
                    // 0 for native 2x, or 0, 3 for 2x as 4x) of 1x source.
                    host_depth_source_sample_id = builder.createBinOp(
                        spv::OpBitwiseAnd, type_uint, dest_tile_pixel_y,
                        builder.makeUintConstant(1));
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_sample_id =
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              host_depth_source_sample_id,
                                              builder.makeUintConstant(1));
                    } else {
                      host_depth_source_sample_id =
                          builder.createQuadOp(spv::OpBitFieldInsert, type_uint,
                                               host_depth_source_sample_id,
                                               host_depth_source_sample_id,
                                               builder.makeUintConstant(1),
                                               builder.makeUintConstant(1));
                    }
                    host_depth_source_tile_pixel_y = builder.createBinOp(
                        spv::OpShiftRightLogical, type_uint, dest_tile_pixel_y,
                        builder.makeUintConstant(1));
                  } else {
                    // 1x -> 2x.
                    // Vertical samples (1/0 in the first bit for native 2x or
                    // 0/1 in the second bit for 2x as 4x) of 2x destination to
                    // vertical pixels of 1x source.
                    if (msaa_2x_attachments_supported_) {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpBitwiseXor, type_uint,
                                              dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    } else {
                      host_depth_source_tile_pixel_y = builder.createQuadOp(
                          spv::OpBitFieldInsert, type_uint,
                          builder.createBinOp(spv::OpShiftRightLogical,
                                              type_uint, dest_sample_id,
                                              builder.makeUintConstant(1)),
                          dest_tile_pixel_y, builder.makeUintConstant(1),
                          builder.makeUintConstant(31));
                    }
                  }
                }
              }
            }
            assert_true(push_constants_member_host_depth_address != UINT32_MAX);
            id_vector_temp.clear();
            id_vector_temp.push_back(builder.makeIntConstant(
                int32_t(push_constants_member_host_depth_address)));
            spv::Id host_depth_address_constant = builder.createLoad(
                builder.createAccessChain(spv::StorageClassPushConstant,
                                          push_constants, id_vector_temp),
                spv::NoPrecision);
            // Transform the destination tile index into the host depth source.
            // After the addition, it may be negative - in which case, the
            // transfer is done across EDRAM addressing wrapping, and
            // xenos::kEdramTileCount must be added to it, but
            // `& (xenos::kEdramTileCount - 1)` handles that regardless of the
            // sign.
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int,
                                              dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(
                                xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(
                                xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
            // Split the host depth source tile index into X and Y tile index
            // within the source image.
            spv::Id host_depth_source_pitch_tiles = builder.createTriOp(
                spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                builder.makeUintConstant(xenos::kEdramPitchTilesBits),
                builder.makeUintConstant(xenos::kEdramPitchTilesBits));
            spv::Id host_depth_source_tile_index_y = builder.createBinOp(
                spv::OpUDiv, type_uint, host_depth_source_tile_index,
                host_depth_source_pitch_tiles);
            spv::Id host_depth_source_tile_index_x = builder.createBinOp(
                spv::OpUMod, type_uint, host_depth_source_tile_index,
                host_depth_source_pitch_tiles);
            // Finally calculate the host depth source texture coordinates.
            spv::Id host_depth_source_pixel_x_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(
                                            tile_width_samples >>
                                            uint32_t(key.source_msaa_samples >=
                                                     xenos::MsaaSamples::k4X)),
                                        host_depth_source_tile_index_x),
                    host_depth_source_tile_pixel_x));
            spv::Id host_depth_source_pixel_y_int = builder.createUnaryOp(
                spv::OpBitcast, type_int,
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(spv::OpIMul, type_uint,
                                        builder.makeUintConstant(
                                            tile_height_samples >>
                                            uint32_t(key.source_msaa_samples >=
                                                     xenos::MsaaSamples::k2X)),
                                        host_depth_source_tile_index_y),
                    host_depth_source_tile_pixel_y));
            // Load the host depth source.
            spv::Builder::TextureParameters
                host_depth_source_texture_parameters = {};
            host_depth_source_texture_parameters.sampler =
                builder.createLoad(host_depth_source_texture, spv::NoPrecision);
            id_vector_temp.clear();
            id_vector_temp.push_back(host_depth_source_pixel_x_int);
            id_vector_temp.push_back(host_depth_source_pixel_y_int);
            host_depth_source_texture_parameters.coords =
                builder.createCompositeConstruct(type_int2, id_vector_temp);
            if (key.host_depth_source_msaa_samples != xenos::MsaaSamples::k1X) {
              host_depth_source_texture_parameters.sample =
                  builder.createUnaryOp(spv::OpBitcast, type_int,
                                        host_depth_source_sample_id);
            } else {
              host_depth_source_texture_parameters.lod =
                  builder.makeIntConstant(0);
            }
            host_depth32 = builder.createCompositeExtract(
                builder.createTextureCall(spv::NoPrecision, type_float4, false,
                                          true, false, false, false,
                                          host_depth_source_texture_parameters,
                                          spv::ImageOperandsMaskNone),
                type_float, 0);
          } else if (host_depth_source_buffer != spv::NoResult) {
            // Get the address in the EDRAM scratch buffer and load from there.
            // The beginning of the buffer is (0, 0) of the destination.
            // 40-sample columns are not swapped for addressing simplicity
            // (because this is used for depth -> depth transfers, where
            // swapping isn't needed).
            // Convert samples to pixels.
            assert_true(key.host_depth_source_msaa_samples ==
                        xenos::MsaaSamples::k1X);
            spv::Id dest_tile_sample_x = dest_tile_pixel_x;
            spv::Id dest_tile_sample_y = dest_tile_pixel_y;
            if (key.dest_msaa_samples >= xenos::MsaaSamples::k2X) {
              if (key.dest_msaa_samples >= xenos::MsaaSamples::k4X) {
                // Horizontal sample index in bit 0.
                dest_tile_sample_x = builder.createQuadOp(
                    spv::OpBitFieldInsert, type_uint, dest_sample_id,
                    dest_tile_pixel_x, builder.makeUintConstant(1),
                    builder.makeUintConstant(31));
              }
              // Vertical sample index as 1 or 0 in bit 0 for true 2x or as 0
              // or 1 in bit 1 for 4x or for 2x emulated as 4x.
              dest_tile_sample_y = builder.createQuadOp(
                  spv::OpBitFieldInsert, type_uint,
                  builder.createBinOp(
                      (key.dest_msaa_samples == xenos::MsaaSamples::k2X &&
                       msaa_2x_attachments_supported_)
                          ? spv::OpBitwiseXor
                          : spv::OpShiftRightLogical,
                      type_uint, dest_sample_id, builder.makeUintConstant(1)),
                  dest_tile_pixel_y, builder.makeUintConstant(1),
                  builder.makeUintConstant(31));
            }
            // Combine the tile sample index and the tile index.
            // The tile index doesn't need to be wrapped, as the host depth is
            // written to the beginning of the buffer, without the base offset.
            spv::Id host_depth_offset = builder.createBinOp(
                spv::OpIAdd, type_uint,
                builder.createBinOp(
                    spv::OpIMul, type_uint,
                    builder.makeUintConstant(tile_width_samples *
                                             tile_height_samples),
                    dest_tile_index),
                builder.createBinOp(
                    spv::OpIAdd, type_uint,
                    builder.createBinOp(
                        spv::OpIMul, type_uint,
                        builder.makeUintConstant(tile_width_samples),
                        dest_tile_sample_y),
                    dest_tile_sample_x));
            id_vector_temp.clear();
            // The only SSBO structure member.
            id_vector_temp.push_back(builder.makeIntConstant(0));
            id_vector_temp.push_back(builder.createUnaryOp(
                spv::OpBitcast, type_int, host_depth_offset));
            // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is
            // generated, it's Uniform.
            host_depth32 = builder.createUnaryOp(
                spv::OpBitcast, type_float,
                builder.createLoad(
                    builder.createAccessChain(spv::StorageClassUniform,
                                              host_depth_source_buffer,
                                              id_vector_temp),
                    spv::NoPrecision));
          }
          spv::Block* depth24_to_depth32_header = builder.getBuildPoint();
          spv::Id depth24_to_depth32_convert_id = spv::NoResult;
          spv::Block* depth24_to_depth32_merge = nullptr;
          spv::Id host_depth24 = spv::NoResult;
          if (host_depth32 != spv::NoResult) {
            // Convert the host depth value to the guest format and check if it
            // matches the value in the currently owning guest render target.
            switch (dest_depth_format) {
              case xenos::DepthRenderTargetFormat::kD24S8: {
                // Round to the nearest even integer. This seems to be the
                // correct conversion, adding +0.5 and rounding towards zero
                // results in red instead of black in the 4D5307E6 clear shader.
                host_depth24 = builder.createUnaryOp(
                    spv::OpConvertFToU, type_uint,
                    builder.createUnaryBuiltinCall(
                        type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                        builder.createBinOp(
                            spv::OpFMul, type_float, host_depth32,
                            builder.makeFloatConstant(float(0xFFFFFF)))));
              } break;
              case xenos::DepthRenderTargetFormat::kD24FS8: {
                host_depth24 = SpirvShaderTranslator::PreClampedDepthTo20e4(
                    builder, host_depth32, depth_float24_round(), true,
                    ext_inst_glsl_std_450);
              } break;
            }
            assert_true(host_depth24 != spv::NoResult);
            // Update the header block pointer after the conversion (to avoid
            // assuming that the conversion doesn't branch).
            depth24_to_depth32_header = builder.getBuildPoint();
            spv::Id host_depth_outdated = builder.createBinOp(
                spv::OpINotEqual, type_bool, guest_depth24, host_depth24);
            spv::Block& depth24_to_depth32_convert_entry =
                builder.makeNewBlock();
            {
              spv::Block& depth24_to_depth32_merge_block =
                  builder.makeNewBlock();
              depth24_to_depth32_merge = &depth24_to_depth32_merge_block;
            }
            builder.createSelectionMerge(depth24_to_depth32_merge,
                                         spv::SelectionControlMaskNone);
            builder.createConditionalBranch(host_depth_outdated,
                                            &depth24_to_depth32_convert_entry,
                                            depth24_to_depth32_merge);
            builder.setBuildPoint(&depth24_to_depth32_convert_entry);
          }
          // Convert the guest 24-bit depth to float32 (in an open conditional
          // if the host depth is also loaded).
          spv::Id guest_depth32 = spv::NoResult;
          switch (dest_depth_format) {
            case xenos::DepthRenderTargetFormat::kD24S8: {
              // Multiplying by 1.0 / 0xFFFFFF produces an incorrect result (for
              // 0xC00000, for instance - which is 2_10_10_10 clear to 0001) -
              // rescale from 0...0xFFFFFF to 0...0x1000000 doing what true
              // float division followed by multiplication does (on x86-64 MSVC
              // with default SSE rounding) - values starting from 0x800000
              // become bigger by 1; then accurately bias the result's exponent.
              guest_depth32 = builder.createBinOp(
                  spv::OpFMul, type_float,
                  builder.createUnaryOp(
                      spv::OpConvertUToF, type_float,
                      builder.createBinOp(
                          spv::OpIAdd, type_uint, guest_depth24,
                          builder.createBinOp(spv::OpShiftRightLogical,
                                              type_uint, guest_depth24,
                                              builder.makeUintConstant(23)))),
                  builder.makeFloatConstant(1.0f / float(1 << 24)));
            } break;
            case xenos::DepthRenderTargetFormat::kD24FS8: {
              guest_depth32 = SpirvShaderTranslator::Depth20e4To32(
                  builder, guest_depth24, 0, true, false,
                  ext_inst_glsl_std_450);
            } break;
          }
          assert_true(guest_depth32 != spv::NoResult);
          spv::Id fragment_depth32 = guest_depth32;
          if (host_depth32 != spv::NoResult) {
            assert_not_null(depth24_to_depth32_merge);
            spv::Id depth24_to_depth32_result_block_id =
                builder.getBuildPoint()->getId();
            builder.createBranch(depth24_to_depth32_merge);
            builder.setBuildPoint(depth24_to_depth32_merge);
            id_vector_temp.clear();
            id_vector_temp.push_back(guest_depth32);
            id_vector_temp.push_back(depth24_to_depth32_result_block_id);
            id_vector_temp.push_back(host_depth32);
            id_vector_temp.push_back(depth24_to_depth32_header->getId());
            fragment_depth32 =
                builder.createOp(spv::OpPhi, type_float, id_vector_temp);
          }
          builder.createStore(fragment_depth32, output_fragment_depth);
          // Unpack the stencil into the stencil reference output if needed and
          // not already written.
          if (!packed_only_depth &&
              output_fragment_stencil_ref != spv::NoResult) {
            builder.createStore(
                builder.createUnaryOp(
                    spv::OpBitcast, type_int,
                    builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                        builder.makeUintConstant(UINT8_MAX))),
                output_fragment_stencil_ref);
          }
        }
      } break;
      case TransferOutput::kStencilBit: {
        if (packed && !cvars::no_discard_stencil_in_transfer_pipelines) {
          // Kill the sample if the needed stencil bit is not set.
          assert_true(push_constants_member_stencil_mask != UINT32_MAX);
          id_vector_temp.clear();
          id_vector_temp.push_back(builder.makeIntConstant(
              int32_t(push_constants_member_stencil_mask)));
          spv::Id stencil_mask_constant = builder.createLoad(
              builder.createAccessChain(spv::StorageClassPushConstant,
                                        push_constants, id_vector_temp),
              spv::NoPrecision);
          SpirvBuilder::IfBuilder stencil_kill_if(
              builder.createBinOp(
                  spv::OpIEqual, type_bool,
                  builder.createBinOp(spv::OpBitwiseAnd, type_uint, packed,
                                      stencil_mask_constant),
                  builder.makeUintConstant(0)),
              spv::SelectionControlMaskNone, builder);
          builder.createNoResultOp(spv::OpKill);
          // OpKill terminates the block.
          stencil_kill_if.makeEndIf(false);
        }
      } break;
    }
  }

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeOriginUpperLeft);
  if (output_fragment_depth != spv::NoResult) {
    builder.addExecutionMode(main_function, spv::ExecutionModeDepthReplacing);
  }
  if (output_fragment_stencil_ref != spv::NoResult) {
    builder.addExecutionMode(main_function,
                             spv::ExecutionModeStencilRefReplacingEXT);
  }
  spv::Instruction* entry_point =
      builder.addEntryPoint(spv::ExecutionModelFragment, main_function, "main");
  for (spv::Id interface_id : main_interface) {
    entry_point->addIdOperand(interface_id);
  }

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the shader module, and store the handle even if creation fails not
  // to try to create it again later.
  VkShaderModule shader_module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (shader_module == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer shader 0x{:08X}",
        key.key);
  }
  transfer_shaders_.emplace(key, shader_module);
  return shader_module;
}

VkPipeline const* VulkanRenderTargetCache::GetTransferPipelines(
    TransferPipelineKey key) {
  auto pipeline_it = transfer_pipelines_.find(key);
  if (pipeline_it != transfer_pipelines_.end()) {
    return pipeline_it->second[0] != VK_NULL_HANDLE ? pipeline_it->second.data()
                                                    : nullptr;
  }

  VkRenderPass render_pass =
      GetHostRenderTargetsRenderPass(key.render_pass_key);
  VkShaderModule fragment_shader_module = GetTransferShader(key.shader_key);
  if (render_pass == VK_NULL_HANDLE ||
      fragment_shader_module == VK_NULL_HANDLE) {
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }

  const TransferModeInfo& mode = kTransferModes[size_t(key.shader_key.mode)];

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties =
      vulkan_device->properties();

  uint32_t dest_sample_count = uint32_t(1)
                               << uint32_t(key.shader_key.dest_msaa_samples);
  bool dest_is_masked_sample =
      dest_sample_count > 1 && !device_properties.sampleRateShading;

  VkPipelineShaderStageCreateInfo shader_stages[2];
  shader_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[0].pNext = nullptr;
  shader_stages[0].flags = 0;
  shader_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  shader_stages[0].module = transfer_passthrough_vertex_shader_;
  shader_stages[0].pName = "main";
  shader_stages[0].pSpecializationInfo = nullptr;
  shader_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  shader_stages[1].pNext = nullptr;
  shader_stages[1].flags = 0;
  shader_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  shader_stages[1].module = fragment_shader_module;
  shader_stages[1].pName = "main";
  shader_stages[1].pSpecializationInfo = nullptr;
  VkSpecializationMapEntry sample_id_specialization_map_entry;
  uint32_t sample_id_specialization_constant;
  VkSpecializationInfo sample_id_specialization_info;
  if (dest_is_masked_sample) {
    sample_id_specialization_map_entry.constantID = 0;
    sample_id_specialization_map_entry.offset = 0;
    sample_id_specialization_map_entry.size = sizeof(uint32_t);
    sample_id_specialization_constant = 0;
    sample_id_specialization_info.mapEntryCount = 1;
    sample_id_specialization_info.pMapEntries =
        &sample_id_specialization_map_entry;
    sample_id_specialization_info.dataSize =
        sizeof(sample_id_specialization_constant);
    sample_id_specialization_info.pData = &sample_id_specialization_constant;
    shader_stages[1].pSpecializationInfo = &sample_id_specialization_info;
  }

  VkVertexInputBindingDescription vertex_input_binding;
  vertex_input_binding.binding = 0;
  vertex_input_binding.stride = sizeof(float) * 2;
  vertex_input_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription vertex_input_attribute;
  vertex_input_attribute.location = 0;
  vertex_input_attribute.binding = 0;
  vertex_input_attribute.format = VK_FORMAT_R32G32_SFLOAT;
  vertex_input_attribute.offset = 0;
  VkPipelineVertexInputStateCreateInfo vertex_input_state;
  vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input_state.pNext = nullptr;
  vertex_input_state.flags = 0;
  vertex_input_state.vertexBindingDescriptionCount = 1;
  vertex_input_state.pVertexBindingDescriptions = &vertex_input_binding;
  vertex_input_state.vertexAttributeDescriptionCount = 1;
  vertex_input_state.pVertexAttributeDescriptions = &vertex_input_attribute;

  VkPipelineInputAssemblyStateCreateInfo input_assembly_state;
  input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly_state.pNext = nullptr;
  input_assembly_state.flags = 0;
  input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  input_assembly_state.primitiveRestartEnable = VK_FALSE;

  // Dynamic, to stay within maxViewportDimensions while preferring a
  // power-of-two factor for converting from pixel coordinates to NDC for exact
  // precision.
  VkPipelineViewportStateCreateInfo viewport_state;
  viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport_state.pNext = nullptr;
  viewport_state.flags = 0;
  viewport_state.viewportCount = 1;
  viewport_state.pViewports = nullptr;
  viewport_state.scissorCount = 1;
  viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo rasterization_state = {};
  rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  rasterization_state.cullMode = VK_CULL_MODE_NONE;
  rasterization_state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization_state.lineWidth = 1.0f;

  // For samples other than the first, will be changed for the pipelines for
  // other samples.
  VkSampleMask sample_mask = UINT32_MAX;
  VkPipelineMultisampleStateCreateInfo multisample_state = {};
  multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample_state.rasterizationSamples =
      (dest_sample_count == 2 && !msaa_2x_attachments_supported_)
          ? VK_SAMPLE_COUNT_4_BIT
          : VkSampleCountFlagBits(dest_sample_count);
  if (dest_sample_count > 1) {
    if (device_properties.sampleRateShading) {
      multisample_state.sampleShadingEnable = VK_TRUE;
      multisample_state.minSampleShading = 1.0f;
      if (dest_sample_count == 2 && !msaa_2x_attachments_supported_) {
        // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
        // supported.
        sample_mask = 0b1001;
      }
    } else {
      sample_mask = 0b1;
    }
    if (sample_mask != UINT32_MAX) {
      multisample_state.pSampleMask = &sample_mask;
    }
  }

  // Whether the depth / stencil state is used depends on the presence of a
  // depth attachment in the render pass - but not making assumptions about
  // whether the render pass contains any specific attachments, so setting up
  // valid depth / stencil state unconditionally.
  VkPipelineDepthStencilStateCreateInfo depth_stencil_state = {};
  depth_stencil_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  if (mode.output == TransferOutput::kDepth) {
    depth_stencil_state.depthTestEnable = VK_TRUE;
    depth_stencil_state.depthWriteEnable = VK_TRUE;
    depth_stencil_state.depthCompareOp = cvars::depth_transfer_not_equal_test
                                             ? VK_COMPARE_OP_NOT_EQUAL
                                             : VK_COMPARE_OP_ALWAYS;
  }
  if ((mode.output == TransferOutput::kDepth &&
       vulkan_device->extensions().ext_EXT_shader_stencil_export) ||
      mode.output == TransferOutput::kStencilBit) {
    depth_stencil_state.stencilTestEnable = VK_TRUE;
    depth_stencil_state.front.failOp = VK_STENCIL_OP_KEEP;
    depth_stencil_state.front.passOp = VK_STENCIL_OP_REPLACE;
    depth_stencil_state.front.depthFailOp = VK_STENCIL_OP_REPLACE;
    // Using ALWAYS, not NOT_EQUAL, so depth writing is unaffected by stencil
    // being different.
    depth_stencil_state.front.compareOp = VK_COMPARE_OP_ALWAYS;
    // Will be dynamic for stencil bit output.
    depth_stencil_state.front.writeMask = UINT8_MAX;
    depth_stencil_state.front.reference = UINT8_MAX;
    depth_stencil_state.back = depth_stencil_state.front;
  }

  // Whether the color blend state is used depends on the presence of color
  // attachments in the render pass - but not making assumptions about whether
  // the render pass contains any specific attachments, so setting up valid
  // color blend state unconditionally.
  VkPipelineColorBlendAttachmentState
      color_blend_attachments[xenos::kMaxColorRenderTargets] = {};
  VkPipelineColorBlendStateCreateInfo color_blend_state = {};
  color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  color_blend_state.attachmentCount =
      32 - xe::lzcnt(key.render_pass_key.depth_and_color_used >> 1);
  color_blend_state.pAttachments = color_blend_attachments;
  if (mode.output == TransferOutput::kColor) {
    assert_true(device_properties.independentBlend);
    color_blend_attachments[key.shader_key.dest_color_rt_index].colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  }

  std::array<VkDynamicState, 3> dynamic_states;
  VkPipelineDynamicStateCreateInfo dynamic_state;
  dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic_state.pNext = nullptr;
  dynamic_state.flags = 0;
  dynamic_state.dynamicStateCount = 0;
  dynamic_state.pDynamicStates = dynamic_states.data();
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_VIEWPORT;
  dynamic_states[dynamic_state.dynamicStateCount++] = VK_DYNAMIC_STATE_SCISSOR;
  if (mode.output == TransferOutput::kStencilBit) {
    dynamic_states[dynamic_state.dynamicStateCount++] =
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
  }

  std::array<VkPipeline, 4> pipelines{};
  VkGraphicsPipelineCreateInfo pipeline_create_info;
  pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipeline_create_info.pNext = nullptr;
  pipeline_create_info.flags = 0;
  if (dest_is_masked_sample) {
    pipeline_create_info.flags |= VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT;
  }
  pipeline_create_info.stageCount = uint32_t(xe::countof(shader_stages));
  pipeline_create_info.pStages = shader_stages;
  pipeline_create_info.pVertexInputState = &vertex_input_state;
  pipeline_create_info.pInputAssemblyState = &input_assembly_state;
  pipeline_create_info.pTessellationState = nullptr;
  pipeline_create_info.pViewportState = &viewport_state;
  pipeline_create_info.pRasterizationState = &rasterization_state;
  pipeline_create_info.pMultisampleState = &multisample_state;
  pipeline_create_info.pDepthStencilState = &depth_stencil_state;
  pipeline_create_info.pColorBlendState = &color_blend_state;
  pipeline_create_info.pDynamicState = &dynamic_state;
  pipeline_create_info.layout =
      transfer_pipeline_layouts_[size_t(mode.pipeline_layout)];
  pipeline_create_info.renderPass = render_pass;
  pipeline_create_info.subpass = 0;
  pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  pipeline_create_info.basePipelineIndex = -1;
  if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                    &pipeline_create_info, nullptr,
                                    &pipelines[0]) != VK_SUCCESS) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create the render target ownership "
        "transfer pipeline for render pass 0x{:08X}, shader 0x{:08X}",
        key.render_pass_key.key, key.shader_key.key);
    transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
    return nullptr;
  }
  if (dest_is_masked_sample) {
    assert_true(multisample_state.pSampleMask == &sample_mask);
    pipeline_create_info.flags = (pipeline_create_info.flags &
                                  ~VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) |
                                 VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    pipeline_create_info.basePipelineHandle = pipelines[0];
    for (uint32_t i = 1; i < dest_sample_count; ++i) {
      // Emulating 2x MSAA as samples 0 and 3 of 4x MSAA when 2x is not
      // supported.
      uint32_t host_sample_index =
          (dest_sample_count == 2 && !msaa_2x_attachments_supported_ && i == 1)
              ? 3
              : i;
      sample_id_specialization_constant = host_sample_index;
      sample_mask = uint32_t(1) << host_sample_index;
      if (dfn.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1,
                                        &pipeline_create_info, nullptr,
                                        &pipelines[i]) != VK_SUCCESS) {
        XELOGE(
            "VulkanRenderTargetCache: Failed to create the render target "
            "ownership transfer pipeline for render pass 0x{:08X}, shader "
            "0x{:08X}, sample {}",
            key.render_pass_key.key, key.shader_key.key, i);
        for (uint32_t j = 0; j < i; ++j) {
          dfn.vkDestroyPipeline(device, pipelines[j], nullptr);
        }
        transfer_pipelines_.emplace(key, std::array<VkPipeline, 4>{});
        return nullptr;
      }
    }
  }
  return transfer_pipelines_.emplace(key, pipelines).first->second.data();
}

void VulkanRenderTargetCache::PerformTransfersAndResolveClears(
    uint32_t render_target_count, RenderTarget* const* render_targets,
    const std::vector<Transfer>* render_target_transfers,
    const uint64_t* render_target_resolve_clear_values,
    const Transfer::Rectangle* resolve_clear_rectangle,
    bool in_current_render_pass) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  uint64_t current_submission = command_processor_.GetCurrentSubmission();
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  bool resolve_clear_needed =
      render_target_resolve_clear_values && resolve_clear_rectangle;
  assert_false(in_current_render_pass && resolve_clear_needed);
  VkClearRect resolve_clear_rect;
  if (resolve_clear_needed) {
    // Assuming the rectangle is already clamped by the setup function from the
    // common render target cache.
    resolve_clear_rect.rect.offset.x =
        int32_t(resolve_clear_rectangle->x_pixels * draw_resolution_scale_x());
    resolve_clear_rect.rect.offset.y =
        int32_t(resolve_clear_rectangle->y_pixels * draw_resolution_scale_y());
    resolve_clear_rect.rect.extent.width =
        resolve_clear_rectangle->width_pixels * draw_resolution_scale_x();
    resolve_clear_rect.rect.extent.height =
        resolve_clear_rectangle->height_pixels * draw_resolution_scale_y();
    resolve_clear_rect.baseArrayLayer = 0;
    resolve_clear_rect.layerCount = 1;
  }

  // Do host depth storing for the depth destination (assuming there can be only
  // one depth destination) where depth destination == host depth source.
  bool host_depth_store_set_up = false;
  for (uint32_t i = 0; !in_current_render_pass && i < render_target_count;
       ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    if (!dest_rt_key.is_depth) {
      continue;
    }
    const std::vector<Transfer>& depth_transfers = render_target_transfers[i];
    for (const Transfer& transfer : depth_transfers) {
      if (transfer.host_depth_source != dest_rt) {
        continue;
      }
      if (!host_depth_store_set_up) {
        // Pipeline.
        command_processor_.BindExternalComputePipeline(
            host_depth_store_pipelines_[size_t(dest_rt_key.msaa_samples)]);
        // Descriptor set bindings.
        VkDescriptorSet host_depth_store_descriptor_sets[] = {
            edram_storage_buffer_descriptor_set_,
            dest_vulkan_rt.GetDescriptorSetTransferSource(),
        };
        command_buffer.CmdVkBindDescriptorSets(
            VK_PIPELINE_BIND_POINT_COMPUTE, host_depth_store_pipeline_layout_,
            0, uint32_t(xe::countof(host_depth_store_descriptor_sets)),
            host_depth_store_descriptor_sets, 0, nullptr);
        // Render target constant.
        HostDepthStoreRenderTargetConstant
            host_depth_store_render_target_constant =
                GetHostDepthStoreRenderTargetConstant(
                    dest_rt_key.pitch_tiles_at_32bpp,
                    msaa_2x_attachments_supported_);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, render_target)),
            sizeof(host_depth_store_render_target_constant),
            &host_depth_store_render_target_constant);
        // Barriers - don't need to try to combine them with the rest of
        // render target transfer barriers now - if this happens, after host
        // depth storing, SHADER_READ -> DEPTH_STENCIL_ATTACHMENT_WRITE will be
        // done anyway even in the best case, so it's not possible to have all
        // the barriers in one place here.
        UseEdramBuffer(EdramBufferUsage::kComputeWrite);
        // Always transitioning both depth and stencil, not storing separate
        // usage flags for depth and stencil.
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            dest_vulkan_rt.current_stage_mask(),
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
            dest_vulkan_rt.current_layout(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                VK_ACCESS_SHADER_READ_BIT,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        host_depth_store_set_up = true;
      }
      Transfer::Rectangle
          transfer_rectangles[Transfer::kMaxRectanglesWithCutout];
      uint32_t transfer_rectangle_count = transfer.GetRectangles(
          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp,
          dest_rt_key.msaa_samples, false, transfer_rectangles,
          resolve_clear_rectangle);
      assert_not_zero(transfer_rectangle_count);
      HostDepthStoreRectangleConstant host_depth_store_rectangle_constant;
      for (uint32_t j = 0; j < transfer_rectangle_count; ++j) {
        uint32_t group_count_x, group_count_y;
        GetHostDepthStoreRectangleInfo(
            transfer_rectangles[j], dest_rt_key.msaa_samples,
            host_depth_store_rectangle_constant, group_count_x, group_count_y);
        command_buffer.CmdVkPushConstants(
            host_depth_store_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
            uint32_t(offsetof(HostDepthStoreConstants, rectangle)),
            sizeof(host_depth_store_rectangle_constant),
            &host_depth_store_rectangle_constant);
        command_processor_.SubmitBarriers(true);
        command_buffer.CmdVkDispatch(group_count_x, group_count_y, 1);
        MarkEdramBufferModified();
      }
    }
    break;
  }

  constexpr VkPipelineStageFlags kSourceStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  constexpr VkAccessFlags kSourceAccessMask = VK_ACCESS_SHADER_READ_BIT;
  constexpr VkImageLayout kSourceLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Try to insert as many barriers as possible in one place, hoping that in the
  // best case (no cross-copying between current render targets), barriers will
  // need to be only inserted here, not between transfers. In case of
  // cross-copying, if the destination use is going to happen before the source
  // use, choose the destination state, otherwise the source state - to match
  // the order in which transfers will actually happen (otherwise there will be
  // just a useless switch back and forth).
  for (uint32_t i = 0; !in_current_render_pass && i < render_target_count;
       ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }
    const std::vector<Transfer>& dest_transfers = render_target_transfers[i];
    if (!resolve_clear_needed && dest_transfers.empty()) {
      continue;
    }
    // Transition the destination, only if not going to be used as a source
    // earlier.
    bool dest_used_previously_as_source = false;
    for (uint32_t j = 0; j < i; ++j) {
      for (const Transfer& previous_transfer : render_target_transfers[j]) {
        if (previous_transfer.source == dest_rt ||
            previous_transfer.host_depth_source == dest_rt) {
          dest_used_previously_as_source = true;
          break;
        }
      }
    }
    if (!dest_used_previously_as_source) {
      auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask,
                                  &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_vulkan_rt.key().is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask,
                              dest_new_layout);
    }
    // Transition the sources, only if not going to be used as destinations
    // earlier.
    for (const Transfer& transfer : dest_transfers) {
      bool source_previously_used_as_dest = false;
      bool host_depth_source_previously_used_as_dest = false;
      for (uint32_t j = 0; j < i; ++j) {
        if (render_target_transfers[j].empty()) {
          continue;
        }
        const RenderTarget* previous_rt = render_targets[j];
        if (transfer.source == previous_rt) {
          source_previously_used_as_dest = true;
        }
        if (transfer.host_depth_source == previous_rt) {
          host_depth_source_previously_used_as_dest = true;
        }
      }
      if (!source_previously_used_as_dest) {
        auto& source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.source);
        command_processor_.PushImageMemoryBarrier(
            source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                source_vulkan_rt.key().is_depth
                    ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                    : VK_IMAGE_ASPECT_COLOR_BIT),
            source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            source_vulkan_rt.current_access_mask(), kSourceAccessMask,
            source_vulkan_rt.current_layout(), kSourceLayout);
        source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask,
                                  kSourceLayout);
      }
      // transfer.host_depth_source == dest_rt means the EDRAM buffer will be
      // used instead, no need to transition.
      if (transfer.host_depth_source && transfer.host_depth_source != dest_rt &&
          !host_depth_source_previously_used_as_dest) {
        auto& host_depth_source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
        command_processor_.PushImageMemoryBarrier(
            host_depth_source_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
            host_depth_source_vulkan_rt.current_stage_mask(), kSourceStageMask,
            host_depth_source_vulkan_rt.current_access_mask(),
            kSourceAccessMask, host_depth_source_vulkan_rt.current_layout(),
            kSourceLayout);
        host_depth_source_vulkan_rt.SetUsage(kSourceStageMask,
                                             kSourceAccessMask, kSourceLayout);
      }
    }
  }
  if (host_depth_store_set_up) {
    // Will be reading copied host depth from the EDRAM buffer.
    UseEdramBuffer(EdramBufferUsage::kFragmentRead);
  }

  // Perform the transfers and clears.

  TransferPipelineLayoutIndex last_transfer_pipeline_layout_index =
      TransferPipelineLayoutIndex::kCount;
  uint32_t transfer_descriptor_sets_bound = 0;
  uint32_t transfer_push_constants_set = 0;
  VkDescriptorSet last_descriptor_set_host_depth_stencil_textures =
      VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_depth_stencil_textures = VK_NULL_HANDLE;
  VkDescriptorSet last_descriptor_set_color_texture = VK_NULL_HANDLE;
  TransferAddressConstant last_host_depth_address_constant;
  TransferAddressConstant last_address_constant;

  for (uint32_t i = 0; i < render_target_count; ++i) {
    RenderTarget* dest_rt = render_targets[i];
    if (!dest_rt) {
      continue;
    }

    const std::vector<Transfer>& current_transfers = render_target_transfers[i];
    if (current_transfers.empty() && !resolve_clear_needed) {
      continue;
    }

    auto& dest_vulkan_rt = *static_cast<VulkanRenderTarget*>(dest_rt);
    RenderTargetKey dest_rt_key = dest_vulkan_rt.key();
    bool android_skip_current_transfers = false;
    bool android_skip_resolve_clear = false;

#if XE_PLATFORM_ANDROID
    const bool android_halo_base_1350_color =
        !dest_rt_key.is_depth && dest_rt_key.base_tiles == 1350 &&
        dest_rt_key.GetPitchTiles() == 15 &&
        dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X;
    // WO26 Probe A follow-up: base-1350's log above never fires for base 675
    // (hardcoded to 1350), so it's structurally blind to how the large
    // 1152x720 4x F16 scene RT at base 675 actually gets populated - widen
    // independently, gated on log_rt_transfers alone so it doesn't touch the
    // existing base-1350 behavior.
    const bool android_halo_base_675_color =
        !dest_rt_key.is_depth && dest_rt_key.base_tiles == 675 &&
        dest_rt_key.GetPitchTiles() == 15;
    if ((android_halo_base_1350_color || android_halo_base_675_color) &&
        !current_transfers.empty()) {
      static uint32_t android_rt_transfer_log_count = 0;
      static uint32_t android_rt_transfer_675_log_count = 0;
      bool android_want_675_transfer_log =
          android_halo_base_675_color &&
          GetAndroidHaloExperiment().log_rt_transfers &&
          android_rt_transfer_675_log_count < 512;
      bool android_want_1350_transfer_log =
          android_halo_base_1350_color &&
          GetAndroidHaloExperiment().log_rt_transfers &&
          android_rt_transfer_log_count < 256;
      if (android_want_1350_transfer_log || android_want_675_transfer_log) {
        for (uint32_t transfer_index = 0;
             transfer_index < uint32_t(current_transfers.size()) &&
             (android_want_1350_transfer_log ||
              android_want_675_transfer_log);
             ++transfer_index) {
          const Transfer& transfer = current_transfers[transfer_index];
          const VulkanRenderTarget* source_vulkan_rt =
              static_cast<const VulkanRenderTarget*>(transfer.source);
          const RenderTargetKey source_rt_key =
              source_vulkan_rt ? source_vulkan_rt->key() : RenderTargetKey();
          const VulkanRenderTarget* host_depth_source_vulkan_rt =
              static_cast<const VulkanRenderTarget*>(
                  transfer.host_depth_source);
          const RenderTargetKey host_depth_source_rt_key =
              host_depth_source_vulkan_rt ? host_depth_source_vulkan_rt->key()
                                          : RenderTargetKey();
          Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
          const uint32_t rectangle_count = transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
              dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(), rectangles,
              resolve_clear_rectangle);
          Transfer::Rectangle first_rectangle = {};
          if (rectangle_count) {
            first_rectangle = rectangles[0];
          }
          XELOGI(
              "Android RT transfer {}: dest_base={} dest_pitch={} dest_fmt={} "
              "dest_msaa={} dest_width={} dest_height={} src_base={} "
              "src_pitch={} src_depth={} src_fmt={} src_msaa={} src_width={} "
              "src_height={} "
              "host_depth_base={} host_depth_msaa={} "
              "tiles=[{}, {}) rects={} first_rect={}x{}+{},{} "
              "resolve_clear={} host_depth_source={} skip_cvar={} blit_cvar={}",
              android_rt_transfer_log_count, uint32_t(dest_rt_key.base_tiles),
              dest_rt_key.GetPitchTiles(), uint32_t(dest_rt_key.resource_format),
              uint32_t(1) << uint32_t(dest_rt_key.msaa_samples),
              dest_rt_key.GetWidth(),
              GetRenderTargetHeight(dest_rt_key.pitch_tiles_at_32bpp,
                                    dest_rt_key.msaa_samples),
              uint32_t(source_rt_key.base_tiles),
              source_rt_key.GetPitchTiles(),
              uint32_t(source_rt_key.is_depth),
              uint32_t(source_rt_key.resource_format),
              uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
              source_rt_key.GetWidth(),
              GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                    source_rt_key.msaa_samples),
              uint32_t(host_depth_source_rt_key.base_tiles),
              uint32_t(1) << uint32_t(host_depth_source_rt_key.msaa_samples),
              transfer.start_tiles, transfer.end_tiles, rectangle_count,
              first_rectangle.width_pixels, first_rectangle.height_pixels,
              first_rectangle.x_pixels, first_rectangle.y_pixels,
              uint32_t(resolve_clear_needed),
              uint32_t(transfer.host_depth_source != nullptr),
              uint32_t(cvars::halo_android_diag_skip_rt_transfers_to_base_1350),
              uint32_t(GetAndroidHaloExperiment().blit_rt_transfers));
          if (android_want_1350_transfer_log) {
            ++android_rt_transfer_log_count;
          }
          if (android_want_675_transfer_log) {
            ++android_rt_transfer_675_log_count;
          }
        }
      }
      // Keep the skip-cvar strictly scoped to base 1350 (its original
      // behavior) - the base-675 widening above is logging-only and must not
      // change transfer behavior for base 675.
      if (android_halo_base_1350_color) {
        android_skip_current_transfers =
            cvars::halo_android_diag_skip_rt_transfers_to_base_1350;
      }
      if (android_skip_current_transfers) {
        static uint32_t android_rt_transfer_skip_log_count = 0;
        if (android_rt_transfer_skip_log_count < 64) {
          XELOGI(
              "Android skipping RT transfers to base 1350: transfers={} "
              "resolve_clear={}",
              uint32_t(current_transfers.size()),
              uint32_t(resolve_clear_needed));
          ++android_rt_transfer_skip_log_count;
        }
        if (!resolve_clear_needed) {
          continue;
        }
      }
    }
    android_skip_resolve_clear =
        android_halo_base_1350_color && resolve_clear_needed &&
        cvars::halo_android_diag_skip_resolve_clear_to_base_1350;

    if (GetAndroidHaloExperiment().menu_initialize_from_base675 &&
        !android_skip_current_transfers && !resolve_clear_needed &&
        !current_transfers.empty() && !dest_rt_key.is_depth &&
        dest_rt_key.base_tiles == 0 &&
        dest_rt_key.pitch_tiles_at_32bpp == 29 &&
        dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
        dest_rt_key.GetColorFormat() ==
            xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
        dest_rt_key.GetWidth() == 2320 && draw_resolution_scale_x() == 1 &&
        draw_resolution_scale_y() == 1) {
      if (android_halo_menu_scene_shadow_valid_ &&
          android_halo_menu_scene_shadow_buffer_ != VK_NULL_HANDLE) {
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_COLOR_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
            dest_vulkan_rt.current_layout(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        AndroidHaloTransitionMenuSceneShadowBuffer(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        command_processor_.SubmitBarriers(true);

        constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
        constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
        std::vector<VkBufferImageCopy> copy_regions;
        copy_regions.reserve(kAndroidHaloMenuScenePitchTiles *
                             kAndroidHaloMenuSceneRows);
        for (uint32_t tile_y = 0; tile_y < kAndroidHaloMenuSceneRows;
             ++tile_y) {
          for (uint32_t tile_x = 0;
               tile_x < kAndroidHaloMenuScenePitchTiles; ++tile_x) {
            VkBufferImageCopy& region = copy_regions.emplace_back();
            region.bufferOffset =
                VkDeviceSize(tile_y * kAndroidHaloMenuScenePitchTiles +
                             tile_x) *
                VkDeviceSize(kAndroidHaloShadowTileBytes);
            region.bufferRowLength = kTileWidth;
            region.bufferImageHeight = kTileHeight;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {int32_t(tile_x * kTileWidth),
                                  int32_t(tile_y * kTileHeight), 0};
            region.imageExtent = {kTileWidth, kTileHeight, 1};
          }
        }
        command_buffer.CmdVkCopyBufferToImage(
            android_halo_menu_scene_shadow_buffer_, dest_vulkan_rt.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            uint32_t(copy_regions.size()), copy_regions.data());
        static uint32_t android_menu_shadow_restore_log_count = 0;
        if (android_menu_shadow_restore_log_count++ < 64) {
          XELOGI(
              "MENU_SCENE_SHADOW_RESTORE dest_base=0 dest_pitch=29 "
              "tiles={} transfers={}",
              uint32_t(copy_regions.size()),
              uint32_t(current_transfers.size()));
        }
        continue;
      }

      VulkanRenderTarget* android_menu_scene_source = nullptr;
      for (const Transfer& transfer : current_transfers) {
        if (transfer.host_depth_source || !transfer.source ||
            transfer.start_tiles >= 1305 || transfer.end_tiles <= 675) {
          continue;
        }
        auto* source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(transfer.source);
        const RenderTargetKey source_rt_key = source_vulkan_rt->key();
        if (!source_rt_key.is_depth && source_rt_key.base_tiles == 675 &&
            source_rt_key.pitch_tiles_at_32bpp == 15 &&
            source_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
            source_rt_key.GetColorFormat() ==
                xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
            source_rt_key.GetWidth() == 1200 &&
            source_vulkan_rt->image() != dest_vulkan_rt.image()) {
          android_menu_scene_source = source_vulkan_rt;
          break;
        }
      }
      if (android_menu_scene_source) {
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_COLOR_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
            dest_vulkan_rt.current_layout(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        command_processor_.PushImageMemoryBarrier(
            android_menu_scene_source->image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_COLOR_BIT),
            android_menu_scene_source->current_stage_mask(),
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            android_menu_scene_source->current_access_mask(),
            VK_ACCESS_TRANSFER_READ_BIT,
            android_menu_scene_source->current_layout(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        android_menu_scene_source->SetUsage(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        command_processor_.SubmitBarriers(true);

        VkImageBlit menu_scene_blit = {};
        menu_scene_blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        menu_scene_blit.srcSubresource.layerCount = 1;
        menu_scene_blit.srcOffsets[1] = {1152, 720, 1};
        menu_scene_blit.dstSubresource = menu_scene_blit.srcSubresource;
        menu_scene_blit.dstOffsets[1] = {1152, 720, 1};
        command_buffer.CmdVkBlitImage(
            android_menu_scene_source->image(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest_vulkan_rt.image(),
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &menu_scene_blit,
            VK_FILTER_NEAREST);
        static uint32_t android_menu_scene_init_log_count = 0;
        if (android_menu_scene_init_log_count++ < 64) {
          XELOGI(
              "MENU_BASE675_INIT source_base=675 source_pitch=15 "
              "dest_base=0 dest_pitch=29 extent=1152x720 transfers={}",
              uint32_t(current_transfers.size()));
        }
        continue;
      }
      // Once the persistent menu target has been seeded, later aliases from
      // small-pitch scratch and depth targets represent the same physical
      // EDRAM bytes but not the same 2D image layout. Importing them through
      // the host-RT transfer shaders creates the observed horizontal bands.
      // Preserve the target and let the following guest draw update it.
      static uint32_t android_menu_alias_skip_log_count = 0;
      if (android_menu_alias_skip_log_count++ < 64) {
        XELOGI(
            "MENU_BASE0_ALIAS_SKIP dest_base=0 dest_pitch=29 transfers={}",
            uint32_t(current_transfers.size()));
      }
      continue;
    }

    if (GetAndroidHaloExperiment().blit_rt_transfers &&
        !android_skip_current_transfers && !resolve_clear_needed &&
        !current_transfers.empty() &&
        !dest_rt_key.is_depth && dest_rt_key.base_tiles == 1350 &&
        dest_rt_key.GetPitchTiles() == 15 &&
        dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
        draw_resolution_scale_x() == 1 && draw_resolution_scale_y() == 1) {
      struct BlitTransferBatch {
        VulkanRenderTarget* source;
        std::vector<VkImageBlit> blits;
      };
      bool can_blit_transfers = true;
      uint32_t blit_rect_count = 0;
      std::vector<BlitTransferBatch> blit_batches;
      blit_batches.reserve(current_transfers.size());
      for (const Transfer& transfer : current_transfers) {
        if (transfer.host_depth_source || !transfer.source) {
          can_blit_transfers = false;
          break;
        }
        auto& source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(transfer.source);
        const RenderTargetKey source_rt_key = source_vulkan_rt.key();
        if (source_rt_key.is_depth ||
            source_rt_key.msaa_samples != xenos::MsaaSamples::k1X ||
            source_rt_key.base_tiles != dest_rt_key.base_tiles ||
            source_rt_key.GetPitchTiles() != dest_rt_key.GetPitchTiles() ||
            source_vulkan_rt.image() == dest_vulkan_rt.image()) {
          can_blit_transfers = false;
          break;
        }
        Transfer::Rectangle rectangles[Transfer::kMaxRectanglesWithCutout];
        const uint32_t rectangle_count = transfer.GetRectangles(
            dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
            dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(), rectangles,
            nullptr);
        if (!rectangle_count) {
          continue;
        }
        BlitTransferBatch& batch = blit_batches.emplace_back();
        batch.source = &source_vulkan_rt;
        batch.blits.reserve(rectangle_count);
        for (uint32_t j = 0; j < rectangle_count; ++j) {
          const Transfer::Rectangle& rectangle = rectangles[j];
          VkImageBlit& blit = batch.blits.emplace_back();
          blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          blit.srcSubresource.mipLevel = 0;
          blit.srcSubresource.baseArrayLayer = 0;
          blit.srcSubresource.layerCount = 1;
          blit.srcOffsets[0] = {int32_t(rectangle.x_pixels),
                                int32_t(rectangle.y_pixels), 0};
          blit.srcOffsets[1] = {
              int32_t(rectangle.x_pixels + rectangle.width_pixels),
              int32_t(rectangle.y_pixels + rectangle.height_pixels), 1};
          blit.dstSubresource = blit.srcSubresource;
          blit.dstOffsets[0] = blit.srcOffsets[0];
          blit.dstOffsets[1] = blit.srcOffsets[1];
        }
        blit_rect_count += rectangle_count;
      }
      if (can_blit_transfers && blit_rect_count) {
        command_processor_.PushImageMemoryBarrier(
            dest_vulkan_rt.image(),
            ui::vulkan::util::InitializeSubresourceRange(
                VK_IMAGE_ASPECT_COLOR_BIT),
            dest_vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
            dest_vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_WRITE_BIT,
            dest_vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dest_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (const BlitTransferBatch& batch : blit_batches) {
          VulkanRenderTarget& source_vulkan_rt = *batch.source;
          command_processor_.PushImageMemoryBarrier(
              source_vulkan_rt.image(),
              ui::vulkan::util::InitializeSubresourceRange(
                  VK_IMAGE_ASPECT_COLOR_BIT),
              source_vulkan_rt.current_stage_mask(),
              VK_PIPELINE_STAGE_TRANSFER_BIT,
              source_vulkan_rt.current_access_mask(),
              VK_ACCESS_TRANSFER_READ_BIT, source_vulkan_rt.current_layout(),
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
          source_vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        command_processor_.SubmitBarriers(true);
        for (const BlitTransferBatch& batch : blit_batches) {
          VulkanRenderTarget& source_vulkan_rt = *batch.source;
          command_buffer.CmdVkBlitImage(
              source_vulkan_rt.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              dest_vulkan_rt.image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              uint32_t(batch.blits.size()), batch.blits.data(),
              VK_FILTER_NEAREST);
        }
        XELOGI(
            "Android blit RT transfers: dest_base={} pitch={} transfers={} "
            "rects={}",
            uint32_t(dest_rt_key.base_tiles), dest_rt_key.GetPitchTiles(),
            uint32_t(current_transfers.size()), blit_rect_count);
        continue;
      }
    }
#endif

    // Late barriers in case there was cross-copying that prevented merging of
    // barriers.
    if (!in_current_render_pass) {
      VkPipelineStageFlags dest_dst_stage_mask;
      VkAccessFlags dest_dst_access_mask;
      VkImageLayout dest_new_layout;
      dest_vulkan_rt.GetDrawUsage(&dest_dst_stage_mask, &dest_dst_access_mask,
                                  &dest_new_layout);
      command_processor_.PushImageMemoryBarrier(
          dest_vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              dest_rt_key.is_depth
                  ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VK_IMAGE_ASPECT_COLOR_BIT),
          dest_vulkan_rt.current_stage_mask(), dest_dst_stage_mask,
          dest_vulkan_rt.current_access_mask(), dest_dst_access_mask,
          dest_vulkan_rt.current_layout(), dest_new_layout);
      dest_vulkan_rt.SetUsage(dest_dst_stage_mask, dest_dst_access_mask,
                              dest_new_layout);
    }

    // Get the objects needed for transfers to the destination.
    // TODO(Triang3l): Reuse the guest render pass for transfers where possible
    // (if the Vulkan format used for drawing is also usable for transfers - for
    // instance, R8G8B8A8_UNORM can be used for both, so the guest pass can be
    // reused, but R16G16B16A16_SFLOAT render targets use R16G16B16A16_UINT for
    // transfers, so the transfer pass has to be separate) to avoid stores and
    // loads on tile-based devices to make this actually applicable. Also
    // overall perform all non-cross-copying transfers for the current
    // framebuffer configuration in a single pass, to load / store only once.
    RenderPassKey transfer_render_pass_key;
    VkRenderPass transfer_render_pass = VK_NULL_HANDLE;
    const Framebuffer* transfer_framebuffer = nullptr;
    if (in_current_render_pass) {
      transfer_render_pass_key = last_update_render_pass_key_;
      transfer_render_pass = last_update_render_pass_;
      transfer_framebuffer = last_update_framebuffer_;
    } else {
      transfer_render_pass_key.msaa_samples = dest_rt_key.msaa_samples;
      if (dest_rt_key.is_depth) {
        transfer_render_pass_key.depth_and_color_used = 0b1;
        transfer_render_pass_key.depth_format = dest_rt_key.GetDepthFormat();
      } else {
        transfer_render_pass_key.depth_and_color_used = 0b1 << 1;
        transfer_render_pass_key.color_0_view_format =
            dest_rt_key.GetColorFormat();
        transfer_render_pass_key.color_rts_use_transfer_formats = 1;
      }
      transfer_render_pass =
          GetHostRenderTargetsRenderPass(transfer_render_pass_key);
      if (transfer_render_pass == VK_NULL_HANDLE) {
        continue;
      }
      const RenderTarget* transfer_framebuffer_render_targets
          [1 + xenos::kMaxColorRenderTargets] = {};
      transfer_framebuffer_render_targets[dest_rt_key.is_depth ? 0 : 1] =
          dest_rt;
      transfer_framebuffer = GetHostRenderTargetsFramebuffer(
          transfer_render_pass_key, dest_rt_key.pitch_tiles_at_32bpp,
          transfer_framebuffer_render_targets);
      if (!transfer_framebuffer) {
        continue;
      }
      // Don't enter the render pass immediately - source barriers may follow.
    }
#if XE_PLATFORM_ANDROID
    const bool android_halo_suppress_transfer_owner =
        IsAndroidHaloCompatActive() &&
        IsAndroidHaloPresentableColorKey(dest_rt_key);
    if (android_halo_suppress_transfer_owner) {
      android_halo_transfer_render_pass_active_ = true;
    }
#endif

    if (!current_transfers.empty() && !android_skip_current_transfers) {
      uint32_t dest_pitch_tiles = dest_rt_key.GetPitchTiles();
      bool dest_is_64bpp = dest_rt_key.Is64bpp();

      // Gather shader keys and sort to reduce pipeline state and binding
      // switches. Also gather stencil rectangles to clear if needed.
      bool need_stencil_bit_draws =
          dest_rt_key.is_depth &&
          !vulkan_device->extensions().ext_EXT_shader_stencil_export;
      current_transfer_invocations_.clear();
      current_transfer_invocations_.reserve(
          current_transfers.size() << uint32_t(need_stencil_bit_draws));
      uint32_t rt_sort_index = 0;
      TransferShaderKey new_transfer_shader_key;
      new_transfer_shader_key.dest_msaa_samples = dest_rt_key.msaa_samples;
      new_transfer_shader_key.dest_color_rt_index =
          dest_rt_key.is_depth || !in_current_render_pass ? 0 : i - 1;
      new_transfer_shader_key.dest_resource_format =
          dest_rt_key.resource_format;
      uint32_t stencil_clear_rectangle_count = 0;
      for (uint32_t j = 0; j <= uint32_t(need_stencil_bit_draws); ++j) {
        // j == 0 - color or depth.
        // j == 1 - stencil bits.
        // Stencil bit writing always requires a different root signature,
        // handle these separately. Stencil never has a host depth source.
        // Clear previously set sort indices.
        for (const Transfer& transfer : current_transfers) {
          auto host_depth_source_vulkan_rt =
              static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(UINT32_MAX);
          }
          assert_not_null(transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(transfer.source);
          source_vulkan_rt.SetTemporarySortIndex(UINT32_MAX);
        }
        for (const Transfer& transfer : current_transfers) {
          assert_not_null(transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(transfer.source);
          VulkanRenderTarget* host_depth_source_vulkan_rt =
              j ? nullptr
                : static_cast<VulkanRenderTarget*>(transfer.host_depth_source);
          if (host_depth_source_vulkan_rt &&
              host_depth_source_vulkan_rt->temporary_sort_index() ==
                  UINT32_MAX) {
            host_depth_source_vulkan_rt->SetTemporarySortIndex(rt_sort_index++);
          }
          if (source_vulkan_rt.temporary_sort_index() == UINT32_MAX) {
            source_vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
          }
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          new_transfer_shader_key.source_msaa_samples =
              source_rt_key.msaa_samples;
          new_transfer_shader_key.source_resource_format =
              source_rt_key.resource_format;
          bool host_depth_source_is_copy =
              host_depth_source_vulkan_rt == &dest_vulkan_rt;
          // The host depth copy buffer has only raw samples.
          new_transfer_shader_key.host_depth_source_msaa_samples =
              (host_depth_source_vulkan_rt && !host_depth_source_is_copy)
                  ? host_depth_source_vulkan_rt->key().msaa_samples
                  : xenos::MsaaSamples::k1X;
          if (j) {
            new_transfer_shader_key.mode =
                source_rt_key.is_depth ? TransferMode::kDepthToStencilBit
                                       : TransferMode::kColorToStencilBit;
            stencil_clear_rectangle_count +=
                transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles,
                                       dest_rt_key.msaa_samples, dest_is_64bpp,
                                       nullptr, resolve_clear_rectangle);
          } else {
            if (dest_rt_key.is_depth) {
              if (host_depth_source_vulkan_rt) {
                if (host_depth_source_is_copy) {
                  new_transfer_shader_key.mode =
                      source_rt_key.is_depth
                          ? TransferMode::kDepthAndHostDepthCopyToDepth
                          : TransferMode::kColorAndHostDepthCopyToDepth;
                } else {
                  new_transfer_shader_key.mode =
                      source_rt_key.is_depth
                          ? TransferMode::kDepthAndHostDepthToDepth
                          : TransferMode::kColorAndHostDepthToDepth;
                }
              } else {
                new_transfer_shader_key.mode =
                    source_rt_key.is_depth ? TransferMode::kDepthToDepth
                                           : TransferMode::kColorToDepth;
              }
            } else {
              new_transfer_shader_key.mode = source_rt_key.is_depth
                                                 ? TransferMode::kDepthToColor
                                                 : TransferMode::kColorToColor;
            }
          }
#if XE_PLATFORM_ANDROID
          new_transfer_shader_key.android_depth_to_color_diag_mode = 0;
          new_transfer_shader_key.android_depth_to_color_sample_mode = 0;
          new_transfer_shader_key.android_value_convert_1010102_to_8888 = 0;
          new_transfer_shader_key.android_value_convert_16bit_to_8888 = 0;
          const bool android_presentable_depth_to_color_alias =
              !j &&
              new_transfer_shader_key.mode == TransferMode::kDepthToColor &&
              source_rt_key.is_depth && !dest_rt_key.is_depth &&
              source_rt_key.msaa_samples == xenos::MsaaSamples::k4X &&
              dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
              source_rt_key.base_tiles == dest_rt_key.base_tiles &&
              source_rt_key.base_tiles == kAndroidHaloShadowBaseTiles &&
              IsAndroidHaloPresentableColorKey(dest_rt_key) &&
              transfer.start_tiles < kAndroidHaloShadowBaseTiles +
                                         kAndroidHaloShadowPitchTiles *
                                             kAndroidHaloShadowRows &&
              transfer.end_tiles > kAndroidHaloShadowBaseTiles;
          const bool android_menu_depth_to_color_alias =
              !j &&
              new_transfer_shader_key.mode == TransferMode::kDepthToColor &&
              source_rt_key.is_depth && !dest_rt_key.is_depth &&
              source_rt_key.msaa_samples == xenos::MsaaSamples::k4X &&
              dest_rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
              source_rt_key.base_tiles == 0 && dest_rt_key.base_tiles == 0 &&
              source_rt_key.pitch_tiles_at_32bpp == 29 &&
              dest_rt_key.pitch_tiles_at_32bpp == 29 &&
              source_rt_key.GetPitchTiles() == 29 &&
              dest_rt_key.GetPitchTiles() == 29 &&
              source_rt_key.GetWidth() == 1160 &&
              dest_rt_key.GetWidth() == 2320 && transfer.start_tiles < 1305 &&
              transfer.end_tiles > 0;
          const bool android_exact_depth_to_color =
              android_presentable_depth_to_color_alias &&
              source_rt_key.GetPitchTiles() == dest_rt_key.GetPitchTiles() &&
              source_rt_key.pitch_tiles_at_32bpp == 15 &&
              source_rt_key.GetWidth() == 600 &&
              dest_rt_key.GetWidth() == 1200 &&
              transfer.start_tiles == 1350 && transfer.end_tiles == 2025;
          if (android_exact_depth_to_color) {
            int32_t depth_to_color_diag_mode =
                cvars::halo_android_diag_depth_to_color_mode;
            if (depth_to_color_diag_mode == 0) {
              if (cvars::halo_android_diag_depth_to_color_pattern) {
                depth_to_color_diag_mode = 1;
              } else if (cvars::halo_android_diag_depth_to_color_zero_stencil) {
                depth_to_color_diag_mode = 4;
              } else if (cvars::halo_android_diag_depth_to_color_sample_mode !=
                         0) {
                depth_to_color_diag_mode = 6;
              }
            }
            if (depth_to_color_diag_mode < 0 ||
                depth_to_color_diag_mode > 10 ||
                depth_to_color_diag_mode == 7) {
              depth_to_color_diag_mode = 0;
            }
            int32_t depth_to_color_sample_mode =
                cvars::halo_android_diag_depth_to_color_sample_mode;
            if (depth_to_color_sample_mode < 0) {
              depth_to_color_sample_mode = 0;
            } else if (depth_to_color_sample_mode > 7) {
              depth_to_color_sample_mode = 7;
            }
            new_transfer_shader_key.android_depth_to_color_diag_mode =
                uint32_t(depth_to_color_diag_mode);
            new_transfer_shader_key.android_depth_to_color_sample_mode =
                uint32_t(depth_to_color_sample_mode);

            if (GetAndroidHaloExperiment().log_rt_transfers) {
              static uint32_t android_depth_to_color_exact_log_count = 0;
              if (android_depth_to_color_exact_log_count < 128) {
                Transfer::Rectangle rectangles
                    [Transfer::kMaxRectanglesWithCutout];
                const uint32_t rectangle_count = transfer.GetRectangles(
                    dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                    dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(),
                    rectangles, resolve_clear_rectangle);
                Transfer::Rectangle first_rectangle = {};
                if (rectangle_count) {
                  first_rectangle = rectangles[0];
                }
                const uint32_t tile_count =
                    transfer.end_tiles - transfer.start_tiles;
                XELOGI(
                    "Android exact DepthToColor diag {}: diag_mode={} "
                    "sample_mode={} src_base={} dest_base={} src_pitch={} "
                    "dest_pitch={} src_fmt={} dest_fmt={} src_msaa={} "
                    "dest_msaa={} src_width={} src_height={} dest_width={} "
                    "dest_height={} tiles=[{}, {}) tile_count={} "
                    "tile_span_bytes=0x{:X} dest_1x_bytes=0x{:X} "
                    "source_4x_bytes=0x{:X} rects={} first_rect={}x{}+{},{} "
                    "host_depth_source={} force_d32s8={}",
                    android_depth_to_color_exact_log_count,
                    new_transfer_shader_key.android_depth_to_color_diag_mode,
                    new_transfer_shader_key.android_depth_to_color_sample_mode,
                    uint32_t(source_rt_key.base_tiles),
                    uint32_t(dest_rt_key.base_tiles),
                    source_rt_key.GetPitchTiles(), dest_rt_key.GetPitchTiles(),
                    uint32_t(source_rt_key.resource_format),
                    uint32_t(dest_rt_key.resource_format),
                    uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                    uint32_t(1) << uint32_t(dest_rt_key.msaa_samples),
                    source_rt_key.GetWidth(),
                    GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                          source_rt_key.msaa_samples),
                    dest_rt_key.GetWidth(),
                    GetRenderTargetHeight(dest_rt_key.pitch_tiles_at_32bpp,
                                          dest_rt_key.msaa_samples),
                    transfer.start_tiles, transfer.end_tiles, tile_count,
                    tile_count * xenos::kEdramTileWidthSamples *
                        xenos::kEdramTileHeightSamples * sizeof(uint32_t),
                    dest_rt_key.GetWidth() *
                        GetRenderTargetHeight(dest_rt_key.pitch_tiles_at_32bpp,
                                              dest_rt_key.msaa_samples) *
                        sizeof(uint32_t),
                    source_rt_key.GetWidth() *
                        GetRenderTargetHeight(
                            source_rt_key.pitch_tiles_at_32bpp,
                            source_rt_key.msaa_samples) *
                        (uint32_t(1) << uint32_t(source_rt_key.msaa_samples)) *
                        sizeof(uint32_t),
                    rectangle_count, first_rectangle.width_pixels,
                    first_rectangle.height_pixels, first_rectangle.x_pixels,
                    first_rectangle.y_pixels,
                    uint32_t(host_depth_source_vulkan_rt != nullptr),
                    uint32_t(cvars::halo_android_diag_force_d32s8_depth_format));
                ++android_depth_to_color_exact_log_count;
              }
            }
          }
          if (android_presentable_depth_to_color_alias &&
              GetAndroidHaloExperiment().skip_depth_to_color_alias &&
              IsAndroidHaloCompatActive() &&
              cvars::halo_android_diag_depth_to_color_mode == 0) {
            AndroidHaloQuarantineDepthToColor(transfer, source_rt_key,
                                              dest_rt_key);
            static uint32_t android_depthcolor_skip_log_count = 0;
            if (android_depthcolor_skip_log_count < 150) {
              XELOGI(
                  "DEPTHCOLOR_SKIP base=1350 src_pitch={} dest_pitch={} "
                  "tiles=[{}, {}) exact={}",
                  source_rt_key.GetPitchTiles(), dest_rt_key.GetPitchTiles(),
                  transfer.start_tiles, transfer.end_tiles,
                  uint32_t(android_exact_depth_to_color));
              ++android_depthcolor_skip_log_count;
            }
            continue;
          }
          if (android_menu_depth_to_color_alias &&
              GetAndroidHaloExperiment().menu_skip_depth_to_color_alias &&
              IsAndroidHaloCompatActive()) {
            static uint32_t android_menu_depthcolor_skip_log_count = 0;
            if (android_menu_depthcolor_skip_log_count++ < 64) {
              XELOGI(
                  "MENU_DEPTHCOLOR_SKIP src_pitch={} dest_pitch={} "
                  "src_width={} dest_width={} tiles=[{}, {})",
                  source_rt_key.GetPitchTiles(), dest_rt_key.GetPitchTiles(),
                  source_rt_key.GetWidth(), dest_rt_key.GetWidth(),
                  transfer.start_tiles, transfer.end_tiles);
            }
            continue;
          }
          if (android_exact_depth_to_color &&
              new_transfer_shader_key.android_depth_to_color_diag_mode == 8) {
            android_depth_to_color_edram_fallback_source_ = &source_vulkan_rt;
            if (GetAndroidHaloExperiment().log_rt_transfers) {
              static uint32_t android_depth_to_color_fallback_skip_log_count =
                  0;
              if (android_depth_to_color_fallback_skip_log_count < 64) {
                XELOGI(
                    "Android DepthToColor EDRAM fallback: skipping fragment "
                    "transfer and scheduling source depth dump for tiles "
                    "[{}, {})",
                    transfer.start_tiles, transfer.end_tiles);
                ++android_depth_to_color_fallback_skip_log_count;
              }
            }
            continue;
          }
          if (android_exact_depth_to_color &&
              new_transfer_shader_key.android_depth_to_color_diag_mode == 9) {
            static uint32_t android_depth_to_color_first_only_count = 0;
            const uint32_t allow_count = std::max<uint32_t>(
                1u, cvars::halo_android_diag_depth_to_color_sample_mode);
            const bool skip_transfer =
                android_depth_to_color_first_only_count >= allow_count;
            if (GetAndroidHaloExperiment().log_rt_transfers &&
                android_depth_to_color_first_only_count < 128) {
              XELOGI(
                  "Android DepthToColor first-only diag: transfer={} "
                  "allow_count={} action={}",
                  android_depth_to_color_first_only_count, allow_count,
                  skip_transfer ? "skip" : "run");
            }
            ++android_depth_to_color_first_only_count;
            if (skip_transfer) {
              continue;
            }
          }
          if (android_exact_depth_to_color &&
              new_transfer_shader_key.android_depth_to_color_diag_mode == 10) {
            static uint32_t android_depth_to_color_skip_first_count = 0;
            const uint32_t skip_count = std::max<uint32_t>(
                1u, cvars::halo_android_diag_depth_to_color_sample_mode);
            const bool skip_transfer =
                android_depth_to_color_skip_first_count < skip_count;
            if (GetAndroidHaloExperiment().log_rt_transfers &&
                android_depth_to_color_skip_first_count < 128) {
              XELOGI(
                  "Android DepthToColor skip-first diag: transfer={} "
                  "skip_count={} action={}",
                  android_depth_to_color_skip_first_count, skip_count,
                  skip_transfer ? "skip" : "run");
            }
            ++android_depth_to_color_skip_first_count;
            if (skip_transfer) {
              continue;
            }
          }
          // Protect Reach's base-675 7e3 scene owner from LDR clobber / steal.
          // t134 blanket both dirs → energetic but cyan. Narrow ldr→hdr only
          // ≈ baseline dull. HOSTDUMP t150: fmt26 resolves often dump from
          // fixed 1010102 while FLOAT still holds HDR — block hdr→ldr steal.
          if (IsAndroidHaloCompatActive() && !j &&
              dest_rt_key.base_tiles == 675 &&
              dest_rt_key.GetPitchTiles() == 15) {
            const AndroidHaloExperiment& scene_675_xfer_exp =
                GetAndroidHaloExperiment();
            const auto is_7e3_fmt =
                [](xenos::ColorRenderTargetFormat format) -> bool {
              return format ==
                         xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
                     format == xenos::ColorRenderTargetFormat::
                                   k_2_10_10_10_FLOAT_AS_16_16_16_16;
            };
            const bool depth_into_scene =
                scene_675_xfer_exp.skip_scene_675_hdr_format_alias &&
                new_transfer_shader_key.mode == TransferMode::kDepthToColor &&
                source_rt_key.is_depth && !dest_rt_key.is_depth;
            const bool ldr_into_hdr =
                scene_675_xfer_exp.skip_scene_675_hdr_format_alias &&
                new_transfer_shader_key.mode == TransferMode::kColorToColor &&
                !source_rt_key.is_depth && !dest_rt_key.is_depth &&
                is_7e3_fmt(dest_rt_key.GetColorFormat()) &&
                !is_7e3_fmt(source_rt_key.GetColorFormat());
            const bool hdr_into_ldr =
                scene_675_xfer_exp.skip_scene_675_hdr_to_ldr &&
                new_transfer_shader_key.mode == TransferMode::kColorToColor &&
                !source_rt_key.is_depth && !dest_rt_key.is_depth &&
                is_7e3_fmt(source_rt_key.GetColorFormat()) &&
                !is_7e3_fmt(dest_rt_key.GetColorFormat());
            if (depth_into_scene || ldr_into_hdr || hdr_into_ldr) {
              static uint32_t android_scene_675_alias_skip_log_count = 0;
              if (android_scene_675_alias_skip_log_count < 128) {
                XELOGI(
                    "SCENE675_XFER_SKIP count={} mode={} src_base={} "
                    "src_pitch={} src_fmt={} src_depth={} src_msaa={} "
                    "dest_base={} dest_pitch={} dest_fmt={} dest_msaa={} "
                    "tiles=[{}, {})",
                    android_scene_675_alias_skip_log_count,
                    depth_into_scene ? "depth_to_color"
                    : ldr_into_hdr   ? "ldr_into_hdr"
                                     : "hdr_into_ldr",
                    uint32_t(source_rt_key.base_tiles),
                    source_rt_key.GetPitchTiles(),
                    uint32_t(source_rt_key.resource_format),
                    uint32_t(source_rt_key.is_depth),
                    uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                    uint32_t(dest_rt_key.base_tiles),
                    dest_rt_key.GetPitchTiles(),
                    uint32_t(dest_rt_key.resource_format),
                    uint32_t(1) << uint32_t(dest_rt_key.msaa_samples),
                    transfer.start_tiles, transfer.end_tiles);
                ++android_scene_675_alias_skip_log_count;
              }
              continue;
            }
          }
          const int32_t skip_color_src_fmt =
              GetAndroidHaloExperiment()
                  .skip_presentable_color_transfer_src_fmt;
          const uint32_t skip_color_src_fmt_mask =
              GetAndroidHaloExperiment()
                  .skip_presentable_color_transfer_src_fmt_mask;
          const int32_t skip_color_dest_fmt =
              GetAndroidHaloExperiment()
                  .skip_presentable_color_transfer_dest_fmt;
          const uint32_t skip_color_src_msaa =
              GetAndroidHaloExperiment()
                  .skip_presentable_color_transfer_src_msaa;
          const uint32_t source_msaa_count =
              uint32_t(1) << uint32_t(source_rt_key.msaa_samples);
          const uint32_t source_format =
              uint32_t(source_rt_key.resource_format);
          const bool skip_color_src_mask_matches =
              source_format < 32 &&
              (skip_color_src_fmt_mask & (uint32_t(1) << source_format));
          const bool skip_color_src_matches =
              (skip_color_src_fmt >= 0 &&
               source_format == uint32_t(skip_color_src_fmt)) ||
              skip_color_src_mask_matches;
          const bool skip_color_dest_matches =
              skip_color_dest_fmt == -2
                  ? true
                  : skip_color_dest_fmt >= 0
                  ? uint32_t(dest_rt_key.resource_format) ==
                        uint32_t(skip_color_dest_fmt)
                  : uint32_t(dest_rt_key.resource_format) ==
                        uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8);
          const bool skip_color_src_msaa_matches =
              skip_color_src_msaa == 0 ||
              skip_color_src_msaa == source_msaa_count;
          if (skip_color_src_matches && android_halo_base_1350_color &&
              IsAndroidHaloCompatActive() && !j &&
              new_transfer_shader_key.mode == TransferMode::kColorToColor &&
              !source_rt_key.is_depth && !dest_rt_key.is_depth &&
              skip_color_dest_matches && skip_color_src_msaa_matches) {
            static uint32_t android_presentable_color_skip_log_count = 0;
            if (android_presentable_color_skip_log_count < 128) {
              Transfer::Rectangle rectangles
                  [Transfer::kMaxRectanglesWithCutout];
              const uint32_t rectangle_count = transfer.GetRectangles(
                  dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                  dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(), rectangles,
                  resolve_clear_rectangle);
              Transfer::Rectangle first_rectangle = {};
              if (rectangle_count) {
                first_rectangle = rectangles[0];
              }
              XELOGI(
                  "COLORXFER_SKIP count={} src_fmt={} source_base={} "
                  "source_pitch={} source_msaa={} source_width={} "
                  "source_height={} dest_fmt={} dest_base={} dest_pitch={} "
                  "dest_msaa={} rects={} first_rect={}x{}+{},{} "
                  "tiles=[{}, {}) src_fmt_filter={} src_fmt_mask=0x{:X} "
                  "dest_filter={} src_msaa_filter={}",
                  android_presentable_color_skip_log_count,
                  source_format,
                  uint32_t(source_rt_key.base_tiles),
                  source_rt_key.GetPitchTiles(),
                  source_msaa_count,
                  source_rt_key.GetWidth(),
                  GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                        source_rt_key.msaa_samples),
                  uint32_t(dest_rt_key.resource_format),
                  uint32_t(dest_rt_key.base_tiles),
                  dest_rt_key.GetPitchTiles(),
                  uint32_t(1) << uint32_t(dest_rt_key.msaa_samples),
                  rectangle_count,
                  first_rectangle.width_pixels, first_rectangle.height_pixels,
                  first_rectangle.x_pixels, first_rectangle.y_pixels,
                  transfer.start_tiles, transfer.end_tiles,
                  skip_color_src_fmt, skip_color_src_fmt_mask,
                  skip_color_dest_fmt, skip_color_src_msaa);
              ++android_presentable_color_skip_log_count;
            }
            continue;
          }
          const xenos::ColorRenderTargetFormat source_color_format =
              xenos::ColorRenderTargetFormat(source_rt_key.resource_format);
          const xenos::ColorRenderTargetFormat dest_color_format =
              xenos::ColorRenderTargetFormat(dest_rt_key.resource_format);
          const bool android_value_convert_1010102 =
              GetAndroidHaloExperiment().value_convert_1010102_to_8888 &&
              android_halo_base_1350_color && IsAndroidHaloCompatActive() &&
              !j &&
              new_transfer_shader_key.mode == TransferMode::kColorToColor &&
              !source_rt_key.is_depth && !dest_rt_key.is_depth &&
              IsAndroidHalo8888ColorFormat(dest_color_format) &&
              IsAndroidHalo1010102ColorFormat(source_color_format);
          if (android_value_convert_1010102) {
            new_transfer_shader_key.android_value_convert_1010102_to_8888 = 1;
            AndroidHaloMarkGameplayPresentContent(360);
            static uint32_t android_value_convert_1010102_arm_log_count = 0;
            if (android_value_convert_1010102_arm_log_count < 32) {
              XELOGI(
                  "HaloCompat value_convert_1010102 armed count={} "
                  "src_base={} src_msaa={} src_fmt={} dest_base={} dest_fmt={}",
                  android_value_convert_1010102_arm_log_count,
                  uint32_t(source_rt_key.base_tiles),
                  uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                  uint32_t(source_color_format),
                  uint32_t(dest_rt_key.base_tiles),
                  uint32_t(dest_color_format));
              ++android_value_convert_1010102_arm_log_count;
            }
            if (GetAndroidHaloExperiment().log_rt_transfers) {
              static uint32_t android_value_convert_1010102_log_count = 0;
              if (android_value_convert_1010102_log_count < 128) {
                Transfer::Rectangle rectangles
                    [Transfer::kMaxRectanglesWithCutout];
                const uint32_t rectangle_count = transfer.GetRectangles(
                    dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                    dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(),
                    rectangles, resolve_clear_rectangle);
                Transfer::Rectangle first_rectangle = {};
                if (rectangle_count) {
                  first_rectangle = rectangles[0];
                }
                XELOGI(
                    "COLORXFER_VALUECONVERT_1010102 count={} source_base={} "
                    "source_pitch={} source_msaa={} source_width={} "
                    "source_height={} dest_base={} dest_pitch={} rects={} "
                    "first_rect={}x{}+{},{} tiles=[{}, {})",
                    android_value_convert_1010102_log_count,
                    uint32_t(source_rt_key.base_tiles),
                    source_rt_key.GetPitchTiles(),
                    uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                    source_rt_key.GetWidth(),
                    GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                          source_rt_key.msaa_samples),
                    uint32_t(dest_rt_key.base_tiles),
                    dest_rt_key.GetPitchTiles(), rectangle_count,
                    first_rectangle.width_pixels, first_rectangle.height_pixels,
                    first_rectangle.x_pixels, first_rectangle.y_pixels,
                    transfer.start_tiles, transfer.end_tiles);
                ++android_value_convert_1010102_log_count;
              }
            }
          }
          const bool android_value_convert_16bit =
              GetAndroidHaloExperiment().value_convert_16bit_to_8888 &&
              android_halo_base_1350_color && IsAndroidHaloCompatActive() &&
              !j &&
              new_transfer_shader_key.mode == TransferMode::kColorToColor &&
              !source_rt_key.is_depth && !dest_rt_key.is_depth &&
              uint32_t(dest_rt_key.resource_format) ==
                  uint32_t(xenos::ColorRenderTargetFormat::k_8_8_8_8) &&
              (uint32_t(source_rt_key.resource_format) ==
                   uint32_t(xenos::ColorRenderTargetFormat::k_16_16_16_16) ||
               uint32_t(source_rt_key.resource_format) ==
                   uint32_t(
                       xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT));
          if (android_value_convert_16bit) {
            new_transfer_shader_key.android_value_convert_16bit_to_8888 = 1;
            if (GetAndroidHaloExperiment().log_rt_transfers) {
              static uint32_t android_value_convert_16bit_log_count = 0;
              if (android_value_convert_16bit_log_count < 128) {
                Transfer::Rectangle rectangles
                    [Transfer::kMaxRectanglesWithCutout];
                const uint32_t rectangle_count = transfer.GetRectangles(
                    dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                    dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(),
                    rectangles, resolve_clear_rectangle);
                Transfer::Rectangle first_rectangle = {};
                if (rectangle_count) {
                  first_rectangle = rectangles[0];
                }
                XELOGI(
                    "COLORXFER_VALUECONVERT_16BIT count={} src_fmt={} "
                    "source_base={} source_pitch={} source_msaa={} "
                    "source_width={} source_height={} dest_base={} "
                    "dest_pitch={} rects={} first_rect={}x{}+{},{} "
                    "tiles=[{}, {})",
                    android_value_convert_16bit_log_count,
                    uint32_t(source_rt_key.resource_format),
                    uint32_t(source_rt_key.base_tiles),
                    source_rt_key.GetPitchTiles(),
                    uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                    source_rt_key.GetWidth(),
                    GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                          source_rt_key.msaa_samples),
                    uint32_t(dest_rt_key.base_tiles),
                    dest_rt_key.GetPitchTiles(), rectangle_count,
                    first_rectangle.width_pixels, first_rectangle.height_pixels,
                    first_rectangle.x_pixels, first_rectangle.y_pixels,
                    transfer.start_tiles, transfer.end_tiles);
                ++android_value_convert_16bit_log_count;
              }
            }
          }
          if (GetAndroidHaloExperiment().log_rt_transfers &&
              android_halo_base_1350_color) {
            static uint32_t android_rt_transfer_mode_log_count = 0;
            if (android_rt_transfer_mode_log_count < 256) {
              XELOGI(
                  "Android RT transfer shader {}: mode={} dest_msaa={} "
                  "src_msaa={} src_depth={} src_fmt={} host_depth_source={} "
                  "source_base={} source_pitch={} dest_fmt={} diag_mode={} "
                  "sample_mode={} value_convert_1010102={} "
                  "value_convert_16bit={} zero_stencil={} pattern={}",
                  android_rt_transfer_mode_log_count,
                  uint32_t(new_transfer_shader_key.mode),
                  uint32_t(1) << uint32_t(
                      new_transfer_shader_key.dest_msaa_samples),
                  uint32_t(1) << uint32_t(
                      new_transfer_shader_key.source_msaa_samples),
                  uint32_t(source_rt_key.is_depth),
                  uint32_t(source_rt_key.resource_format),
                  uint32_t(host_depth_source_vulkan_rt != nullptr),
                  uint32_t(source_rt_key.base_tiles),
                  source_rt_key.GetPitchTiles(),
                  uint32_t(dest_rt_key.resource_format),
                  new_transfer_shader_key.android_depth_to_color_diag_mode,
                  cvars::halo_android_diag_depth_to_color_sample_mode,
                  uint32_t(new_transfer_shader_key
                               .android_value_convert_1010102_to_8888),
                  uint32_t(new_transfer_shader_key
                               .android_value_convert_16bit_to_8888),
                  uint32_t(
                      cvars::halo_android_diag_depth_to_color_zero_stencil),
                  uint32_t(cvars::halo_android_diag_depth_to_color_pattern));
              ++android_rt_transfer_mode_log_count;
            }
          }
          if (GetAndroidHaloExperiment().log_presentable_source_owner &&
              android_halo_base_1350_color && IsAndroidHaloCompatActive()) {
            static uint32_t presentable_source_transfer_log_count = 0;
            if (presentable_source_transfer_log_count < 512) {
              Transfer::Rectangle rectangles
                  [Transfer::kMaxRectanglesWithCutout];
              const uint32_t rectangle_count = transfer.GetRectangles(
                  dest_rt_key.base_tiles, dest_rt_key.GetPitchTiles(),
                  dest_rt_key.msaa_samples, dest_rt_key.Is64bpp(), rectangles,
                  resolve_clear_rectangle);
              Transfer::Rectangle first_rectangle = {};
              if (rectangle_count) {
                first_rectangle = rectangles[0];
              }
              XELOGI(
                  "PRESENTABLE_SOURCE transfer_kind=rt_transfer count={} "
                  "mode={} source_addr=edram_base:{} dest_addr=edram_base:{} "
                  "source_base={} source_pitch={} src_fmt={} src_depth={} "
                  "src_msaa={} src_width={} src_height={} dest_base={} "
                  "dest_pitch={} dest_fmt={} dest_depth={} dest_msaa={} "
                  "dest_width={} dest_height={} rects={} "
                  "first_rect={}x{}+{},{} tiles=[{}, {}) host_depth_source={} "
                  "source_to_1x={} value_convert_1010102={} "
                  "value_convert_16bit={}",
                  presentable_source_transfer_log_count,
                  uint32_t(new_transfer_shader_key.mode),
                  uint32_t(source_rt_key.base_tiles),
                  uint32_t(dest_rt_key.base_tiles),
                  uint32_t(source_rt_key.base_tiles),
                  source_rt_key.GetPitchTiles(),
                  uint32_t(source_rt_key.resource_format),
                  uint32_t(source_rt_key.is_depth),
                  uint32_t(1) << uint32_t(source_rt_key.msaa_samples),
                  source_rt_key.GetWidth(),
                  GetRenderTargetHeight(source_rt_key.pitch_tiles_at_32bpp,
                                        source_rt_key.msaa_samples),
                  uint32_t(dest_rt_key.base_tiles),
                  dest_rt_key.GetPitchTiles(),
                  uint32_t(dest_rt_key.resource_format),
                  uint32_t(dest_rt_key.is_depth),
                  uint32_t(1) << uint32_t(dest_rt_key.msaa_samples),
                  dest_rt_key.GetWidth(),
                  GetRenderTargetHeight(dest_rt_key.pitch_tiles_at_32bpp,
                                        dest_rt_key.msaa_samples),
                  rectangle_count, first_rectangle.width_pixels,
                  first_rectangle.height_pixels, first_rectangle.x_pixels,
                  first_rectangle.y_pixels, transfer.start_tiles,
                  transfer.end_tiles,
                  uint32_t(host_depth_source_vulkan_rt != nullptr),
                  uint32_t(new_transfer_shader_key.source_msaa_samples !=
                           xenos::MsaaSamples::k1X),
                  uint32_t(new_transfer_shader_key
                               .android_value_convert_1010102_to_8888),
                  uint32_t(new_transfer_shader_key
                               .android_value_convert_16bit_to_8888));
              ++presentable_source_transfer_log_count;
            }
          }
          if (android_exact_depth_to_color) {
            static uint32_t android_depthcolor_executed_log_count = 0;
            if (android_depthcolor_executed_log_count < 150) {
              XELOGI("DEPTHCOLOR_EXECUTED base=1350");
              ++android_depthcolor_executed_log_count;
            }
          }
#endif
          current_transfer_invocations_.emplace_back(transfer,
                                                     new_transfer_shader_key);
          if (j) {
            current_transfer_invocations_.back().transfer.host_depth_source =
                nullptr;
          }
        }
      }
      std::sort(current_transfer_invocations_.begin(),
                current_transfer_invocations_.end());

      if (!in_current_render_pass) {
        for (auto it = current_transfer_invocations_.cbegin();
             it != current_transfer_invocations_.cend(); ++it) {
          assert_not_null(it->transfer.source);
          auto& source_vulkan_rt =
              *static_cast<VulkanRenderTarget*>(it->transfer.source);
          command_processor_.PushImageMemoryBarrier(
              source_vulkan_rt.image(),
              ui::vulkan::util::InitializeSubresourceRange(
                  source_vulkan_rt.key().is_depth
                      ? (VK_IMAGE_ASPECT_DEPTH_BIT |
                         VK_IMAGE_ASPECT_STENCIL_BIT)
                      : VK_IMAGE_ASPECT_COLOR_BIT),
              source_vulkan_rt.current_stage_mask(), kSourceStageMask,
              source_vulkan_rt.current_access_mask(), kSourceAccessMask,
              source_vulkan_rt.current_layout(), kSourceLayout);
          source_vulkan_rt.SetUsage(kSourceStageMask, kSourceAccessMask,
                                    kSourceLayout);
          auto host_depth_source_vulkan_rt = static_cast<VulkanRenderTarget*>(
              it->transfer.host_depth_source);
          if (host_depth_source_vulkan_rt) {
            TransferShaderKey transfer_shader_key = it->shader_key;
            if (transfer_shader_key.mode ==
                    TransferMode::kDepthAndHostDepthCopyToDepth ||
                transfer_shader_key.mode ==
                    TransferMode::kColorAndHostDepthCopyToDepth) {
              // Reading copied host depth from the EDRAM buffer.
              UseEdramBuffer(EdramBufferUsage::kFragmentRead);
            } else {
              // Reading host depth from the texture.
              command_processor_.PushImageMemoryBarrier(
                  host_depth_source_vulkan_rt->image(),
                  ui::vulkan::util::InitializeSubresourceRange(
                      VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT),
                  host_depth_source_vulkan_rt->current_stage_mask(),
                  kSourceStageMask,
                  host_depth_source_vulkan_rt->current_access_mask(),
                  kSourceAccessMask,
                  host_depth_source_vulkan_rt->current_layout(),
                  kSourceLayout);
              host_depth_source_vulkan_rt->SetUsage(
                  kSourceStageMask, kSourceAccessMask, kSourceLayout);
            }
          }
        }
      }

      // Perform the transfers for the render target.

      if (!in_current_render_pass) {
        command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
            transfer_render_pass, transfer_framebuffer);
      }

      if (stencil_clear_rectangle_count) {
        VkClearAttachment* stencil_clear_attachment;
        VkClearRect* stencil_clear_rect_write_ptr;
        command_buffer.CmdClearAttachmentsEmplace(1, stencil_clear_attachment,
                                                  stencil_clear_rectangle_count,
                                                  stencil_clear_rect_write_ptr);
        stencil_clear_attachment->aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        stencil_clear_attachment->colorAttachment = 0;
        stencil_clear_attachment->clearValue.depthStencil.depth = 0.0f;
        stencil_clear_attachment->clearValue.depthStencil.stencil = 0;
        for (const Transfer& transfer : current_transfers) {
          Transfer::Rectangle transfer_stencil_clear_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_stencil_clear_rectangle_count =
              transfer.GetRectangles(dest_rt_key.base_tiles, dest_pitch_tiles,
                                     dest_rt_key.msaa_samples, dest_is_64bpp,
                                     transfer_stencil_clear_rectangles,
                                     resolve_clear_rectangle);
          for (uint32_t j = 0; j < transfer_stencil_clear_rectangle_count;
               ++j) {
            const Transfer::Rectangle& stencil_clear_rectangle =
                transfer_stencil_clear_rectangles[j];
            stencil_clear_rect_write_ptr->rect.offset.x = int32_t(
                stencil_clear_rectangle.x_pixels * draw_resolution_scale_x());
            stencil_clear_rect_write_ptr->rect.offset.y = int32_t(
                stencil_clear_rectangle.y_pixels * draw_resolution_scale_y());
            stencil_clear_rect_write_ptr->rect.extent.width =
                stencil_clear_rectangle.width_pixels *
                draw_resolution_scale_x();
            stencil_clear_rect_write_ptr->rect.extent.height =
                stencil_clear_rectangle.height_pixels *
                draw_resolution_scale_y();
            stencil_clear_rect_write_ptr->baseArrayLayer = 0;
            stencil_clear_rect_write_ptr->layerCount = 1;
            ++stencil_clear_rect_write_ptr;
          }
        }
      }

      // Prefer power of two viewports for exact division by simply biasing the
      // exponent.
      VkViewport transfer_viewport;
      transfer_viewport.x = 0.0f;
      transfer_viewport.y = 0.0f;
      transfer_viewport.width =
          float(std::min(xe::next_pow2(transfer_framebuffer->host_extent.width),
                         vulkan_device->properties().maxViewportDimensions[0]));
      transfer_viewport.height = float(
          std::min(xe::next_pow2(transfer_framebuffer->host_extent.height),
                   vulkan_device->properties().maxViewportDimensions[1]));
      transfer_viewport.minDepth = 0.0f;
      transfer_viewport.maxDepth = 1.0f;
      command_processor_.SetViewport(transfer_viewport);
      // GetRectangles returns coordinates in guest pixels, so scale
      // pixels_to_ndc to convert guest pixels to NDC correctly.
      float pixels_to_ndc_x =
          2.0f / transfer_viewport.width * draw_resolution_scale_x();
      float pixels_to_ndc_y =
          2.0f / transfer_viewport.height * draw_resolution_scale_y();
      VkRect2D transfer_scissor;
      transfer_scissor.offset.x = 0;
      transfer_scissor.offset.y = 0;
      transfer_scissor.extent = transfer_framebuffer->host_extent;
      command_processor_.SetScissor(transfer_scissor);

      for (auto it = current_transfer_invocations_.cbegin();
           it != current_transfer_invocations_.cend(); ++it) {
        const TransferInvocation& transfer_invocation_first = *it;
        // Will be merging transfers from the same source into one mesh.
        auto it_merged_first = it, it_merged_last = it;
        uint32_t transfer_rectangle_count =
            transfer_invocation_first.transfer.GetRectangles(
                dest_rt_key.base_tiles, dest_pitch_tiles,
                dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
                resolve_clear_rectangle);
        for (auto it_merge = std::next(it_merged_first);
             it_merge != current_transfer_invocations_.cend(); ++it_merge) {
          if (!transfer_invocation_first.CanBeMergedIntoOneDraw(*it_merge)) {
            break;
          }
          transfer_rectangle_count += it_merge->transfer.GetRectangles(
              dest_rt_key.base_tiles, dest_pitch_tiles,
              dest_rt_key.msaa_samples, dest_is_64bpp, nullptr,
              resolve_clear_rectangle);
          it_merged_last = it_merge;
        }
        assert_not_zero(transfer_rectangle_count);
        // Skip the merged transfers in the subsequent iterations.
        it = it_merged_last;

        assert_not_null(it->transfer.source);
        auto& source_vulkan_rt =
            *static_cast<VulkanRenderTarget*>(it->transfer.source);
        auto host_depth_source_vulkan_rt =
            static_cast<VulkanRenderTarget*>(it->transfer.host_depth_source);
        TransferShaderKey transfer_shader_key = it->shader_key;
        const TransferModeInfo& transfer_mode_info =
            kTransferModes[size_t(transfer_shader_key.mode)];
        TransferPipelineLayoutIndex transfer_pipeline_layout_index =
            transfer_mode_info.pipeline_layout;
        const TransferPipelineLayoutInfo& transfer_pipeline_layout_info =
            kTransferPipelineLayoutInfos[size_t(
                transfer_pipeline_layout_index)];
        uint32_t transfer_sample_pipeline_count =
            vulkan_device->properties().sampleRateShading
                ? 1
                : uint32_t(1) << uint32_t(dest_rt_key.msaa_samples);
        bool transfer_is_stencil_bit =
            (transfer_pipeline_layout_info.used_push_constant_dwords &
             kTransferUsedPushConstantDwordStencilMaskBit) != 0;

        uint32_t transfer_vertex_count = 6 * transfer_rectangle_count;
        VkBuffer transfer_vertex_buffer;
        VkDeviceSize transfer_vertex_buffer_offset;
        float* transfer_rectangle_write_ptr =
            reinterpret_cast<float*>(transfer_vertex_buffer_pool_->Request(
                current_submission, sizeof(float) * 2 * transfer_vertex_count,
                sizeof(float), transfer_vertex_buffer,
                transfer_vertex_buffer_offset));
        if (!transfer_rectangle_write_ptr) {
          continue;
        }
        for (auto it_merged = it_merged_first; it_merged <= it_merged_last;
             ++it_merged) {
          Transfer::Rectangle transfer_invocation_rectangles
              [Transfer::kMaxRectanglesWithCutout];
          uint32_t transfer_invocation_rectangle_count =
              it_merged->transfer.GetRectangles(
                  dest_rt_key.base_tiles, dest_pitch_tiles,
                  dest_rt_key.msaa_samples, dest_is_64bpp,
                  transfer_invocation_rectangles, resolve_clear_rectangle);
          assert_not_zero(transfer_invocation_rectangle_count);
          for (uint32_t j = 0; j < transfer_invocation_rectangle_count; ++j) {
            const Transfer::Rectangle& transfer_rectangle =
                transfer_invocation_rectangles[j];
            float transfer_rectangle_x0 =
                -1.0f + transfer_rectangle.x_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y0 =
                -1.0f + transfer_rectangle.y_pixels * pixels_to_ndc_y;
            float transfer_rectangle_x1 =
                transfer_rectangle_x0 +
                transfer_rectangle.width_pixels * pixels_to_ndc_x;
            float transfer_rectangle_y1 =
                transfer_rectangle_y0 +
                transfer_rectangle.height_pixels * pixels_to_ndc_y;
            // O-*
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            // *-*
            // |/
            // O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            // *-O
            // |/
            // *
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   O
            //  /|
            // *-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y0;
            //   *
            //  /|
            // O-*
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x0;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
            //   *
            //  /|
            // *-O
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_x1;
            *(transfer_rectangle_write_ptr++) = transfer_rectangle_y1;
          }
        }
        command_buffer.CmdVkBindVertexBuffers(0, 1, &transfer_vertex_buffer,
                                              &transfer_vertex_buffer_offset);

        const VkPipeline* transfer_pipelines = GetTransferPipelines(
            TransferPipelineKey(transfer_render_pass_key, transfer_shader_key));
        if (!transfer_pipelines) {
          continue;
        }
        command_processor_.BindExternalGraphicsPipeline(transfer_pipelines[0]);
        if (last_transfer_pipeline_layout_index !=
            transfer_pipeline_layout_index) {
          last_transfer_pipeline_layout_index = transfer_pipeline_layout_index;
          transfer_descriptor_sets_bound = 0;
          transfer_push_constants_set = 0;
        }

        // Invalidate outdated bindings.
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          VkDescriptorSet descriptor_set_host_depth_stencil_textures =
              host_depth_source_vulkan_rt->GetDescriptorSetTransferSource();
          if (last_descriptor_set_host_depth_stencil_textures !=
              descriptor_set_host_depth_stencil_textures) {
            last_descriptor_set_host_depth_stencil_textures =
                descriptor_set_host_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          VkDescriptorSet descriptor_set_depth_stencil_textures =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_depth_stencil_textures !=
              descriptor_set_depth_stencil_textures) {
            last_descriptor_set_depth_stencil_textures =
                descriptor_set_depth_stencil_textures;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetDepthStencilTexturesBit;
          }
        }
        if (transfer_pipeline_layout_info.used_descriptor_sets &
            kTransferUsedDescriptorSetColorTextureBit) {
          VkDescriptorSet descriptor_set_color_texture =
              source_vulkan_rt.GetDescriptorSetTransferSource();
          if (last_descriptor_set_color_texture !=
              descriptor_set_color_texture) {
            last_descriptor_set_color_texture = descriptor_set_color_texture;
            transfer_descriptor_sets_bound &=
                ~kTransferUsedDescriptorSetColorTextureBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          assert_not_null(host_depth_source_vulkan_rt);
          RenderTargetKey host_depth_source_rt_key =
              host_depth_source_vulkan_rt->key();
          TransferAddressConstant host_depth_address_constant;
          host_depth_address_constant.dest_pitch = dest_pitch_tiles;
          host_depth_address_constant.source_pitch =
              host_depth_source_rt_key.GetPitchTiles();
          host_depth_address_constant.source_to_dest =
              int32_t(dest_rt_key.base_tiles) -
              int32_t(host_depth_source_rt_key.base_tiles);
          if (last_host_depth_address_constant != host_depth_address_constant) {
            last_host_depth_address_constant = host_depth_address_constant;
            transfer_push_constants_set &=
                ~kTransferUsedPushConstantDwordHostDepthAddressBit;
          }
        }
        if (transfer_pipeline_layout_info.used_push_constant_dwords &
            kTransferUsedPushConstantDwordAddressBit) {
          RenderTargetKey source_rt_key = source_vulkan_rt.key();
          TransferAddressConstant address_constant;
          address_constant.dest_pitch = dest_pitch_tiles;
          address_constant.source_pitch = source_rt_key.GetPitchTiles();
          address_constant.source_to_dest = int32_t(dest_rt_key.base_tiles) -
                                            int32_t(source_rt_key.base_tiles);
          if (last_address_constant != address_constant) {
            last_address_constant = address_constant;
            transfer_push_constants_set &=
                ~kTransferUsedPushConstantDwordAddressBit;
          }
        }

        // Apply the new bindings.
        // TODO(Triang3l): Merge binding updates into spans.
        VkPipelineLayout transfer_pipeline_layout =
            transfer_pipeline_layouts_[size_t(transfer_pipeline_layout_index)];
        uint32_t transfer_descriptor_sets_unbound =
            transfer_pipeline_layout_info.used_descriptor_sets &
            ~transfer_descriptor_sets_bound;
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthBufferBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetHostDepthBufferBit - 1)),
              1, &edram_storage_buffer_descriptor_set_, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetHostDepthBufferBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetHostDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(
                  transfer_pipeline_layout_info.used_descriptor_sets &
                  (kTransferUsedDescriptorSetHostDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_host_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetHostDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetDepthStencilTexturesBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(
                  transfer_pipeline_layout_info.used_descriptor_sets &
                  (kTransferUsedDescriptorSetDepthStencilTexturesBit - 1)),
              1, &last_descriptor_set_depth_stencil_textures, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetDepthStencilTexturesBit;
        }
        if (transfer_descriptor_sets_unbound &
            kTransferUsedDescriptorSetColorTextureBit) {
          command_buffer.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, transfer_pipeline_layout,
              xe::bit_count(transfer_pipeline_layout_info.used_descriptor_sets &
                            (kTransferUsedDescriptorSetColorTextureBit - 1)),
              1, &last_descriptor_set_color_texture, 0, nullptr);
          transfer_descriptor_sets_bound |=
              kTransferUsedDescriptorSetColorTextureBit;
        }
        uint32_t transfer_push_constants_unset =
            transfer_pipeline_layout_info.used_push_constant_dwords &
            ~transfer_push_constants_set;
        if (transfer_push_constants_unset &
            kTransferUsedPushConstantDwordHostDepthAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  xe::bit_count(
                      transfer_pipeline_layout_info.used_push_constant_dwords &
                      (kTransferUsedPushConstantDwordHostDepthAddressBit - 1)),
              sizeof(uint32_t), &last_host_depth_address_constant);
          transfer_push_constants_set |=
              kTransferUsedPushConstantDwordHostDepthAddressBit;
        }
        if (transfer_push_constants_unset &
            kTransferUsedPushConstantDwordAddressBit) {
          command_buffer.CmdVkPushConstants(
              transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
              sizeof(uint32_t) *
                  xe::bit_count(
                      transfer_pipeline_layout_info.used_push_constant_dwords &
                      (kTransferUsedPushConstantDwordAddressBit - 1)),
              sizeof(uint32_t), &last_address_constant);
          transfer_push_constants_set |=
              kTransferUsedPushConstantDwordAddressBit;
        }

        for (uint32_t j = 0; j < transfer_sample_pipeline_count; ++j) {
          if (j) {
            command_processor_.BindExternalGraphicsPipeline(
                transfer_pipelines[j]);
          }
          for (uint32_t k = 0; k < uint32_t(transfer_is_stencil_bit ? 8 : 1);
               ++k) {
            if (transfer_is_stencil_bit) {
              uint32_t transfer_stencil_bit = uint32_t(1) << k;
              command_buffer.CmdVkPushConstants(
                  transfer_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                  sizeof(uint32_t) *
                      xe::bit_count(
                          transfer_pipeline_layout_info
                              .used_push_constant_dwords &
                          (kTransferUsedPushConstantDwordStencilMaskBit - 1)),
                  sizeof(uint32_t), &transfer_stencil_bit);
              command_buffer.CmdVkSetStencilWriteMask(
                  VK_STENCIL_FACE_FRONT_AND_BACK, transfer_stencil_bit);
            }
            command_buffer.CmdVkDraw(transfer_vertex_count, 1, 0, 0);
          }
        }
      }
    }

    // Perform the clear.
    if (resolve_clear_needed && !android_skip_resolve_clear) {
      if (!in_current_render_pass) {
        command_processor_.SubmitBarriersAndEnterRenderTargetCacheRenderPass(
            transfer_render_pass, transfer_framebuffer);
      }
      VkClearAttachment resolve_clear_attachment;
      resolve_clear_attachment.colorAttachment = 0;
      std::memset(&resolve_clear_attachment.clearValue, 0,
                  sizeof(resolve_clear_attachment.clearValue));
      uint64_t clear_value = render_target_resolve_clear_values[i];
      if (dest_rt_key.is_depth) {
        resolve_clear_attachment.aspectMask =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        uint32_t depth_guest_clear_value =
            (uint32_t(clear_value) >> 8) & 0xFFFFFF;
        switch (dest_rt_key.GetDepthFormat()) {
          case xenos::DepthRenderTargetFormat::kD24S8:
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::UNorm24To32(depth_guest_clear_value);
            break;
          case xenos::DepthRenderTargetFormat::kD24FS8:
            // Taking [0, 2) -> [0, 1) remapping into account.
            resolve_clear_attachment.clearValue.depthStencil.depth =
                xenos::Float20e4To32(depth_guest_clear_value) * 0.5f;
            break;
        }
        resolve_clear_attachment.clearValue.depthStencil.stencil =
            uint32_t(clear_value) & 0xFF;
      } else {
        resolve_clear_attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        switch (dest_rt_key.GetColorFormat()) {
          case xenos::ColorRenderTargetFormat::k_8_8_8_8:
          case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
            for (uint32_t j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 8)) & 0xFF) * (1.0f / 0xFF);
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10:
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  ((clear_value >> (j * 10)) & 0x3FF) * (1.0f / 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
          case xenos::ColorRenderTargetFormat::
              k_2_10_10_10_FLOAT_AS_16_16_16_16: {
            for (uint32_t j = 0; j < 3; ++j) {
              resolve_clear_attachment.clearValue.color.float32[j] =
                  xenos::Float7e3To32((clear_value >> (j * 10)) & 0x3FF);
            }
            resolve_clear_attachment.clearValue.color.float32[3] =
                ((clear_value >> 30) & 0x3) * (1.0f / 0x3);
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            // TODO(Triang3l): Handle cases of unsupported multisampled 16_UINT
            // and completely unsupported 16_UNORM.
            for (uint32_t j = 0; j < 2; ++j) {
              resolve_clear_attachment.clearValue.color.uint32[j] =
                  uint32_t(clear_value >> (j * 16)) & 0xFFFF;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_16_16_16_16:
          case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
            // Using uint for transfers and clears of both. Disregarding the
            // current -32...32 vs. -1...1 settings for consistency with color
            // clear via depth aliasing.
            // TODO(Triang3l): Handle cases of unsupported multisampled 16_UINT
            // and completely unsupported 16_UNORM.
            for (uint32_t j = 0; j < 4; ++j) {
              resolve_clear_attachment.clearValue.color.uint32[j] =
                  uint32_t(clear_value >> (j * 16)) & 0xFFFF;
            }
          } break;
          case xenos::ColorRenderTargetFormat::k_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            resolve_clear_attachment.clearValue.color.uint32[0] =
                uint32_t(clear_value);
          } break;
          case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
            // Using uint for proper denormal and NaN handling.
            resolve_clear_attachment.clearValue.color.uint32[0] =
                uint32_t(clear_value);
            resolve_clear_attachment.clearValue.color.uint32[1] =
                uint32_t(clear_value >> 32);
          } break;
        }
      }
      command_buffer.CmdVkClearAttachments(1, &resolve_clear_attachment, 1,
                                           &resolve_clear_rect);
    } else if (android_skip_resolve_clear) {
      static uint32_t android_resolve_clear_skip_log_count = 0;
      if (android_resolve_clear_skip_log_count < 64) {
        XELOGI(
            "Android skipping resolve clear to base 1350: clear_value=0x{:016X} "
            "rect={}x{}+{},{}",
            render_target_resolve_clear_values
                ? render_target_resolve_clear_values[i]
                : uint64_t(0),
            resolve_clear_rectangle ? resolve_clear_rectangle->width_pixels : 0,
            resolve_clear_rectangle ? resolve_clear_rectangle->height_pixels : 0,
            resolve_clear_rectangle ? resolve_clear_rectangle->x_pixels : 0,
            resolve_clear_rectangle ? resolve_clear_rectangle->y_pixels : 0);
        ++android_resolve_clear_skip_log_count;
      }
    }
#if XE_PLATFORM_ANDROID
    if (android_halo_suppress_transfer_owner) {
      android_halo_transfer_render_pass_active_ = false;
    }
#endif
  }
}

VkPipeline VulkanRenderTargetCache::GetDumpPipeline(DumpPipelineKey key) {
  auto pipeline_it = dump_pipelines_.find(key);
  if (pipeline_it != dump_pipelines_.end()) {
    return pipeline_it->second;
  }

  std::vector<spv::Id> id_vector_temp;

  SpirvBuilder builder(spv::Spv_1_0,
                       (SpirvShaderTranslator::kSpirvMagicToolId << 16) | 1,
                       nullptr);
  spv::Id ext_inst_glsl_std_450 = builder.import("GLSL.std.450");
  builder.addCapability(spv::CapabilityShader);
  builder.setMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
  builder.setSource(spv::SourceLanguageUnknown, 0);

  spv::Id type_void = builder.makeVoidType();
  spv::Id type_int = builder.makeIntType(32);
  spv::Id type_int2 = builder.makeVectorType(type_int, 2);
  spv::Id type_uint = builder.makeUintType(32);
  spv::Id type_uint2 = builder.makeVectorType(type_uint, 2);
  spv::Id type_uint3 = builder.makeVectorType(type_uint, 3);
  spv::Id type_float = builder.makeFloatType(32);
  spv::Id type_float4 = builder.makeVectorType(type_float, 4);

  // Bindings.
  // EDRAM buffer.
  bool format_is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                              key.GetColorFormat());
  id_vector_temp.clear();
  id_vector_temp.push_back(
      builder.makeRuntimeArray(format_is_64bpp ? type_uint2 : type_uint));
  // Storage buffers have std430 packing, no padding to 4-component vectors.
  builder.addDecoration(id_vector_temp.back(), spv::DecorationArrayStride,
                        sizeof(uint32_t) << uint32_t(format_is_64bpp));
  spv::Id type_edram = builder.makeStructType(id_vector_temp, "XeEdram");
  builder.addMemberName(type_edram, 0, "edram");
  builder.addMemberDecoration(type_edram, 0, spv::DecorationNonReadable);
  builder.addMemberDecoration(type_edram, 0, spv::DecorationOffset, 0);
  // Block since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // BufferBlock.
  builder.addDecoration(type_edram, spv::DecorationBufferBlock);
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  spv::Id edram_buffer = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniform, type_edram, "xe_edram");
  builder.addDecoration(edram_buffer, spv::DecorationDescriptorSet,
                        kDumpDescriptorSetEdram);
  builder.addDecoration(edram_buffer, spv::DecorationBinding, 0);
  // Color or depth source.
  bool source_is_multisampled = key.msaa_samples != xenos::MsaaSamples::k1X;
  bool source_is_uint;
  if (key.is_depth) {
    source_is_uint = false;
  } else {
    GetColorOwnershipTransferVulkanFormat(key.GetColorFormat(),
                                          &source_is_uint);
  }
  spv::Id source_component_type = source_is_uint ? type_uint : type_float;
  spv::Id source_texture = builder.createVariable(
      spv::NoPrecision, spv::StorageClassUniformConstant,
      builder.makeImageType(source_component_type, spv::Dim2D, false, false,
                            source_is_multisampled, 1, spv::ImageFormatUnknown),
      "xe_edram_dump_source");
  builder.addDecoration(source_texture, spv::DecorationDescriptorSet,
                        kDumpDescriptorSetSource);
  builder.addDecoration(source_texture, spv::DecorationBinding, 0);
  // Stencil source.
  spv::Id source_stencil_texture = spv::NoResult;
  if (key.is_depth) {
    source_stencil_texture = builder.createVariable(
        spv::NoPrecision, spv::StorageClassUniformConstant,
        builder.makeImageType(type_uint, spv::Dim2D, false, false,
                              source_is_multisampled, 1,
                              spv::ImageFormatUnknown),
        "xe_edram_dump_stencil");
    builder.addDecoration(source_stencil_texture, spv::DecorationDescriptorSet,
                          kDumpDescriptorSetSource);
    builder.addDecoration(source_stencil_texture, spv::DecorationBinding, 1);
  }
  // Push constants.
  id_vector_temp.clear();
  id_vector_temp.reserve(kDumpPushConstantCount);
  for (uint32_t i = 0; i < kDumpPushConstantCount; ++i) {
    id_vector_temp.push_back(type_uint);
  }
  spv::Id type_push_constants =
      builder.makeStructType(id_vector_temp, "XeEdramDumpPushConstants");
  builder.addMemberName(type_push_constants, kDumpPushConstantPitches,
                        "pitches");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantPitches,
                              spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantPitches));
  builder.addMemberName(type_push_constants, kDumpPushConstantOffsets,
                        "offsets");
  builder.addMemberDecoration(type_push_constants, kDumpPushConstantOffsets,
                              spv::DecorationOffset,
                              int(sizeof(uint32_t) * kDumpPushConstantOffsets));
  builder.addDecoration(type_push_constants, spv::DecorationBlock);
  spv::Id push_constants = builder.createVariable(
      spv::NoPrecision, spv::StorageClassPushConstant, type_push_constants,
      "xe_edram_dump_push_constants");

  // gl_GlobalInvocationID input.
  spv::Id input_global_invocation_id =
      builder.createVariable(spv::NoPrecision, spv::StorageClassInput,
                             type_uint3, "gl_GlobalInvocationID");
  builder.addDecoration(input_global_invocation_id, spv::DecorationBuiltIn,
                        static_cast<int>(spv::BuiltIn::GlobalInvocationId));

  // Begin the main function.
  std::vector<spv::Id> main_param_types;
  std::vector<std::vector<spv::Decoration>> main_precisions;
  spv::Block* main_entry;
  spv::Function* main_function =
      builder.makeFunctionEntry(spv::NoPrecision, type_void, "main",
                                main_param_types, main_precisions, &main_entry);

  // For now, as the exact addressing in 64bpp render targets relatively to
  // 32bpp is unknown, treating 64bpp tiles as storing 40x16 samples rather than
  // 80x16 for simplicity of addressing into the texture.

  // Split the destination sample index into the 32bpp tile and the
  // 32bpp-tile-relative sample index.
  // Note that division by non-power-of-two constants will include a 4-cycle
  // 32*32 multiplication on AMD, even though so many bits are not needed for
  // the sample position - however, if an OpUnreachable path is inserted for the
  // case when the position has upper bits set, for some reason, the code for it
  // is not eliminated when compiling the shader for AMD via RenderDoc on
  // Windows, as of June 2022.
  spv::Id global_invocation_id =
      builder.createLoad(input_global_invocation_id, spv::NoPrecision);
  spv::Id rectangle_sample_x =
      builder.createCompositeExtract(global_invocation_id, type_uint, 0);
  // Diagnostic A/B (edram_64bpp_tile_height_halved): the true 64bpp EDRAM
  // tile addressing is undocumented here (see the comment above); by default
  // this halves tile WIDTH (40x16) for 64bpp, but the total-byte-capacity
  // constraint is equally satisfied by halving HEIGHT instead (80x8). Keep
  // both dimensions consistent - exactly one of the two is ever halved.
  const bool android_halve_64bpp_height =
      format_is_64bpp &&
      GetAndroidHaloExperiment().edram_64bpp_tile_height_halved;
  const bool android_halve_64bpp_width =
      format_is_64bpp && !android_halve_64bpp_height;
  uint32_t tile_width =
      (xenos::kEdramTileWidthSamples >> uint32_t(android_halve_64bpp_width)) *
      draw_resolution_scale_x();
  spv::Id const_tile_width = builder.makeUintConstant(tile_width);
  spv::Id rectangle_tile_index_x = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id tile_sample_x = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_x, const_tile_width);
  spv::Id rectangle_sample_y =
      builder.createCompositeExtract(global_invocation_id, type_uint, 1);
  uint32_t tile_height =
      (xenos::kEdramTileHeightSamples >>
       uint32_t(android_halve_64bpp_height)) *
      draw_resolution_scale_y();
  spv::Id const_tile_height = builder.makeUintConstant(tile_height);
  spv::Id rectangle_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, rectangle_sample_y, const_tile_height);
  spv::Id tile_sample_y = builder.createBinOp(
      spv::OpUMod, type_uint, rectangle_sample_y, const_tile_height);

  // Get the tile index in the EDRAM relative to the dump rectangle base tile.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantPitches));
  spv::Id pitches_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_uint_0 = builder.makeUintConstant(0);
  spv::Id const_edram_pitch_tiles_bits =
      builder.makeUintConstant(xenos::kEdramPitchTilesBits);
  spv::Id rectangle_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(spv::OpBitFieldUExtract, type_uint,
                              pitches_constant, const_uint_0,
                              const_edram_pitch_tiles_bits),
          rectangle_tile_index_y),
      rectangle_tile_index_x);
  // Add the base tile in the dispatch to the dispatch-local tile index, not
  // wrapping yet so in case of a wraparound, the address relative to the base
  // in the image after subtraction of the base won't be negative.
  id_vector_temp.clear();
  id_vector_temp.push_back(builder.makeIntConstant(kDumpPushConstantOffsets));
  spv::Id offsets_constant = builder.createLoad(
      builder.createAccessChain(spv::StorageClassPushConstant, push_constants,
                                id_vector_temp),
      spv::NoPrecision);
  spv::Id const_edram_base_tiles_bits_plus_1 =
      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);
  spv::Id edram_tile_index_non_wrapped = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createTriOp(spv::OpBitFieldUExtract, type_uint, offsets_constant,
                          const_uint_0, const_edram_base_tiles_bits_plus_1),
      rectangle_tile_index);

  // Combine the tile sample index and the tile index, wrapping the tile
  // addressing, into the EDRAM sample index.
  spv::Id edram_sample_address = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.makeUintConstant(tile_width * tile_height),
          builder.createBinOp(
              spv::OpBitwiseAnd, type_uint, edram_tile_index_non_wrapped,
              builder.makeUintConstant(xenos::kEdramTileCount - 1))),
      builder.createBinOp(spv::OpIAdd, type_uint,
                          builder.createBinOp(spv::OpIMul, type_uint,
                                              const_tile_width, tile_sample_y),
                          tile_sample_x));
  if (key.is_depth) {
    // Swap 40-sample columns in the depth buffer in the destination address to
    // get the final address of the sample in the EDRAM.
    uint32_t tile_width_half = tile_width >> 1;
    edram_sample_address = builder.createUnaryOp(
        spv::OpBitcast, type_uint,
        builder.createBinOp(
            spv::OpIAdd, type_int,
            builder.createUnaryOp(spv::OpBitcast, type_int,
                                  edram_sample_address),
            builder.createTriOp(
                spv::OpSelect, type_int,
                builder.createBinOp(spv::OpULessThan, builder.makeBoolType(),
                                    tile_sample_x,
                                    builder.makeUintConstant(tile_width_half)),
                builder.makeIntConstant(int32_t(tile_width_half)),
                builder.makeIntConstant(-int32_t(tile_width_half)))));
  }

  // Get the linear tile index within the source texture.
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpISub, type_uint, edram_tile_index_non_wrapped,
      builder.createTriOp(
          spv::OpBitFieldUExtract, type_uint, offsets_constant,
          const_edram_base_tiles_bits_plus_1,
          builder.makeUintConstant(xenos::kEdramBaseTilesBits)));
  // Split the linear tile index in the source texture into X and Y in tiles.
  spv::Id source_pitch_tiles = builder.createTriOp(
      spv::OpBitFieldUExtract, type_uint, pitches_constant,
      const_edram_pitch_tiles_bits, const_edram_pitch_tiles_bits);
  spv::Id source_tile_index_y = builder.createBinOp(
      spv::OpUDiv, type_uint, source_tile_index, source_pitch_tiles);
  spv::Id source_tile_index_x = builder.createBinOp(
      spv::OpUMod, type_uint, source_tile_index, source_pitch_tiles);
  // Combine the source tile offset and the sample index within the tile.
  spv::Id source_sample_x = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_width,
                          source_tile_index_x),
      tile_sample_x);
  spv::Id source_sample_y = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(spv::OpIMul, type_uint, const_tile_height,
                          source_tile_index_y),
      tile_sample_y);
  // Get the source pixel coordinate and the sample index within the pixel.
  spv::Id source_pixel_x = source_sample_x, source_pixel_y = source_sample_y;
  spv::Id source_sample_id = spv::NoResult;
  if (source_is_multisampled) {
    spv::Id const_uint_1 = builder.makeUintConstant(1);
    source_pixel_y = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                         source_sample_y, const_uint_1);
    if (key.msaa_samples >= xenos::MsaaSamples::k4X) {
      source_pixel_x = builder.createBinOp(spv::OpShiftRightLogical, type_uint,
                                           source_sample_x, const_uint_1);
      if (key.source_to_1x) {
        // Collapse the sample quad: every EDRAM sample slot of the pixel
        // receives host sample 0, so a 1x-MSAA resolve read of this span
        // returns one shaded value per source pixel (no sample interleaving).
        source_sample_id = const_uint_0;
      } else {
        // 4x MSAA source texture sample index - bit 0 for horizontal, bit 1
        // for vertical.
        source_sample_id = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint,
            builder.createBinOp(spv::OpBitwiseAnd, type_uint, source_sample_x,
                                const_uint_1),
            source_sample_y, const_uint_1, const_uint_1);
      }
    } else {
      if (key.source_to_1x) {
        // Collapse the sample pair: every EDRAM sample slot of the pixel
        // receives guest sample 0, so a 1x-MSAA resolve read of this span
        // returns one shaded value per source pixel.
        source_sample_id = builder.makeUintConstant(
            draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                0, msaa_2x_attachments_supported_));
      } else {
        // 2x MSAA source texture sample index - convert from the guest to
        // the Vulkan standard sample locations.
        source_sample_id = builder.createTriOp(
            spv::OpSelect, type_uint,
            builder.createBinOp(
                spv::OpINotEqual, builder.makeBoolType(),
                builder.createBinOp(spv::OpBitwiseAnd, type_uint,
                                    source_sample_y, const_uint_1),
                const_uint_0),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    1, msaa_2x_attachments_supported_)),
            builder.makeUintConstant(
                draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                    0, msaa_2x_attachments_supported_)));
      }
    }
  }
#if XE_PLATFORM_ANDROID
  // WO26 Probe B: override which host MSAA sample the dump fetch reads for
  // multisampled sources, to test whether B/A are alive at a sample other
  // than whatever the normal collapse/selection logic above picked (for our
  // 64bpp source_to_1x case, that's always host sample 0).
  if (source_is_multisampled &&
      GetAndroidHaloExperiment().dump_msaa_sample_index_override >= 0) {
    source_sample_id = builder.makeUintConstant(uint32_t(
        GetAndroidHaloExperiment().dump_msaa_sample_index_override));
  }
#endif

  // Load the source, and pack the value into one or two 32-bit integers.
  spv::Id packed[2] = {};
  spv::Builder::TextureParameters source_texture_parameters = {};
  spv::Id source_vec4 = spv::NoResult;
  const bool android_dump_7e3_pattern =
      GetAndroidHaloExperiment().dump_7e3_pattern && !key.is_depth &&
      (key.GetColorFormat() ==
           xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
       key.GetColorFormat() == xenos::ColorRenderTargetFormat::
                                   k_2_10_10_10_FLOAT_AS_16_16_16_16);
  if ((cvars::halo_android_diag_dump_shader_pattern && !key.is_depth &&
       key.GetColorFormat() ==
           xenos::ColorRenderTargetFormat::k_8_8_8_8) ||
      android_dump_7e3_pattern) {
    spv::Id source_x_float = builder.createUnaryOp(
        spv::OpConvertUToF, type_float, source_pixel_x);
    spv::Id source_y_float = builder.createUnaryOp(
        spv::OpConvertUToF, type_float, source_pixel_y);
    spv::Id source_r = builder.createBinOp(
        spv::OpFDiv, type_float, source_x_float,
        builder.makeFloatConstant(float(1152 - 1)));
    spv::Id source_g = builder.createBinOp(
        spv::OpFDiv, type_float, source_y_float,
        builder.makeFloatConstant(float(720 - 1)));
    if (android_dump_7e3_pattern) {
      const spv::Id hdr_scale = builder.makeFloatConstant(31.875f);
      source_r = builder.createBinOp(spv::OpFMul, type_float, source_r,
                                     hdr_scale);
      source_g = builder.createBinOp(spv::OpFMul, type_float, source_g,
                                     hdr_scale);
#if XE_PLATFORM_ANDROID
      if (UsesScaledUNorm7e3RenderTargets()) {
        const spv::Id host_scale =
            builder.makeFloatConstant(1.0f / 31.875f);
        source_r = builder.createBinOp(spv::OpFMul, type_float, source_r,
                                       host_scale);
        source_g = builder.createBinOp(spv::OpFMul, type_float, source_g,
                                       host_scale);
      }
#endif
    }
    spv::Id source_tile_x = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_x,
        builder.makeUintConstant(xenos::kEdramTileWidthSamples));
    spv::Id source_tile_y = builder.createBinOp(
        spv::OpUDiv, type_uint, source_pixel_y,
        builder.makeUintConstant(xenos::kEdramTileHeightSamples));
    spv::Id source_checker = builder.createBinOp(
        spv::OpBitwiseAnd, type_uint,
        builder.createBinOp(spv::OpBitwiseXor, type_uint, source_tile_x,
                            source_tile_y),
        builder.makeUintConstant(1));
    spv::Id source_b = builder.createTriOp(
        spv::OpSelect, type_float,
        builder.createBinOp(spv::OpINotEqual, builder.makeBoolType(),
                            source_checker, const_uint_0),
        builder.makeFloatConstant(
            android_dump_7e3_pattern &&
                    UsesScaledUNorm7e3RenderTargets()
                ? 28.0f / 31.875f
                : android_dump_7e3_pattern ? 28.0f : 1.0f),
        builder.makeFloatConstant(
            android_dump_7e3_pattern &&
                    UsesScaledUNorm7e3RenderTargets()
                ? 4.0f / 31.875f
                : android_dump_7e3_pattern ? 4.0f : 0.125f));
    id_vector_temp.clear();
    id_vector_temp.push_back(source_r);
    id_vector_temp.push_back(source_g);
    id_vector_temp.push_back(source_b);
    id_vector_temp.push_back(builder.makeFloatConstant(1.0f));
    source_vec4 = builder.createCompositeConstruct(type_float4, id_vector_temp);
  } else {
    source_texture_parameters.sampler =
        builder.createLoad(source_texture, spv::NoPrecision);
    id_vector_temp.clear();
    id_vector_temp.push_back(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_x));
    id_vector_temp.push_back(
        builder.createUnaryOp(spv::OpBitcast, type_int, source_pixel_y));
    source_texture_parameters.coords =
        builder.createCompositeConstruct(type_int2, id_vector_temp);
    // Device A/B 2026-06-10: averaging the sample quad produced a WORSE image
    // than sample 0 - for spans whose samples hold four distinct full-res
    // pixels (transfer round-trips), averaging blurs unrelated pixels.
    // Keep the sample-0 collapse; averaging retained only as dead code path.
    constexpr bool source_average_to_1x = false;
    if (source_average_to_1x) {
      const uint32_t source_sample_count =
          key.msaa_samples >= xenos::MsaaSamples::k4X ? 4 : 2;
      spv::Id source_sum = spv::NoResult;
      for (uint32_t i = 0; i < source_sample_count; ++i) {
        const uint32_t source_host_sample =
            key.msaa_samples >= xenos::MsaaSamples::k4X
                ? i
                : draw_util::GetD3D10SampleIndexForGuest2xMSAA(
                      i, msaa_2x_attachments_supported_);
        source_texture_parameters.sample =
            builder.makeIntConstant(int32_t(source_host_sample));
        spv::Id source_sample_vec4 = builder.createTextureCall(
            spv::NoPrecision, type_float4, false, true, false, false, false,
            source_texture_parameters, spv::ImageOperandsMaskNone);
        source_sum = i ? builder.createBinOp(spv::OpFAdd, type_float4,
                                             source_sum, source_sample_vec4)
                       : source_sample_vec4;
      }
      source_vec4 = builder.createBinOp(
          spv::OpVectorTimesScalar, type_float4, source_sum,
          builder.makeFloatConstant(1.0f / float(source_sample_count)));
    } else {
      if (source_is_multisampled) {
        source_texture_parameters.sample =
            builder.createUnaryOp(spv::OpBitcast, type_int, source_sample_id);
      } else {
        source_texture_parameters.lod = builder.makeIntConstant(0);
      }
      source_vec4 = builder.createTextureCall(
          spv::NoPrecision, builder.makeVectorType(source_component_type, 4),
          false, true, false, false, false, source_texture_parameters,
          spv::ImageOperandsMaskNone);
    }
  }
  if (key.is_depth) {
    source_texture_parameters.sampler =
        builder.createLoad(source_stencil_texture, spv::NoPrecision);
    spv::Id source_stencil = builder.createCompositeExtract(
        builder.createTextureCall(
            spv::NoPrecision, builder.makeVectorType(type_uint, 4), false, true,
            false, false, false, source_texture_parameters,
            spv::ImageOperandsMaskNone),
        type_uint, 0);
    spv::Id source_depth32 =
        builder.createCompositeExtract(source_vec4, type_float, 0);
    switch (key.GetDepthFormat()) {
      case xenos::DepthRenderTargetFormat::kD24S8: {
        // Round to the nearest even integer. This seems to be the correct
        // conversion, adding +0.5 and rounding towards zero results in red
        // instead of black in the 4D5307E6 clear shader.
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createUnaryBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450RoundEven,
                builder.createBinOp(
                    spv::OpFMul, type_float, source_depth32,
                    builder.makeFloatConstant(float(0xFFFFFF)))));
      } break;
      case xenos::DepthRenderTargetFormat::kD24FS8: {
        packed[0] = SpirvShaderTranslator::PreClampedDepthTo20e4(
            builder, source_depth32, depth_float24_round(), true,
            ext_inst_glsl_std_450);
      } break;
    }
    packed[0] = builder.createQuadOp(
        spv::OpBitFieldInsert, type_uint, source_stencil, packed[0],
        builder.makeUintConstant(8), builder.makeUintConstant(24));
  } else {
    xenos::ColorRenderTargetFormat dump_pack_format = key.GetColorFormat();
#if XE_PLATFORM_ANDROID
    if (key.source_to_1x && !format_is_64bpp && key.android_force_8888_repack) {
      // Explicit presentation experiment. Native dump mode preserves the
      // owner's format, including 10-bit formats, for guest reinterpretation.
      dump_pack_format = xenos::ColorRenderTargetFormat::k_8_8_8_8;
      if (GetAndroidHaloExperiment().repack_16_16_to_8888 &&
          key.GetColorFormat() == xenos::ColorRenderTargetFormat::k_16_16) {
        // k_16_16 is stored as signed fixed-point -32...32. Convert the raw
        // 16-bit transfer-view components to visible float color before the
        // normal 8888 pack below.
        const spv::Id const_uint_16 = builder.makeUintConstant(16);
        const spv::Id fixed_scale =
            builder.makeFloatConstant(32.0f / 32767.0f);
        id_vector_temp.clear();
        for (uint32_t i = 0; i < 2; ++i) {
          spv::Id raw_component =
              builder.createCompositeExtract(source_vec4, type_uint, i);
          spv::Id signed_shifted = builder.createUnaryOp(
              spv::OpBitcast, type_int,
              builder.createBinOp(spv::OpShiftLeftLogical, type_uint,
                                  raw_component, const_uint_16));
          spv::Id signed_component = builder.createBinOp(
              spv::OpShiftRightArithmetic, type_int, signed_shifted,
              const_uint_16);
          id_vector_temp.push_back(builder.createBinOp(
              spv::OpFMul, type_float,
              builder.createUnaryOp(spv::OpConvertSToF, type_float,
                                    signed_component),
              fixed_scale));
        }
        id_vector_temp.push_back(builder.makeFloatConstant(0.0f));
        id_vector_temp.push_back(builder.makeFloatConstant(1.0f));
        source_vec4 =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
      }
      // Shader behavior must be determined by its cache key. These keys are
      // constructed in this process; there are no legacy keys to recover.
      if (key.android_normalize_7e3 && !UsesScaledUNorm7e3RenderTargets() &&
          (key.GetColorFormat() ==
               xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
           key.GetColorFormat() == xenos::ColorRenderTargetFormat::
                                       k_2_10_10_10_FLOAT_AS_16_16_16_16)) {
        const uint32_t curve = uint32_t(key.android_normalize_7e3_curve);
        id_vector_temp.clear();
        const spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
        const spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
        const spv::Id hdr_to_unorm_scale =
            builder.makeFloatConstant(1.0f / 31.875f);
        // Soft-knee constant for curve 3: c/(c+k) after linear scale. k=0.35
        // keeps ~0.22 of a unit-midtone (vs 0.5 for pure Reinhard) while
        // rolling off values that would otherwise clip to pure white.
        const spv::Id soft_knee = builder.makeFloatConstant(0.35f);
        for (uint32_t i = 0; i < 3; ++i) {
          spv::Id component = builder.createCompositeExtract(
              source_vec4, type_float, i);
          if (curve == 2) {
            // Display-space diagnostic shoulder. Unlike the linear mapping,
            // this assigns useful 8-bit precision to values around 0..2 while
            // retaining highlights above 1 instead of clipping them.
            component = builder.createBinBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450NMax, component,
                const_float_0);
            component = builder.createBinOp(
                spv::OpFDiv, type_float, component,
                builder.createBinOp(spv::OpFAdd, type_float, component,
                                    const_float_1));
          } else {
            component = builder.createBinOp(
                spv::OpFMul, type_float, component, hdr_to_unorm_scale);
            component = builder.createBinBuiltinCall(
                type_float, ext_inst_glsl_std_450, GLSLstd450NMax, component,
                const_float_0);
            if (curve == 1) {
              // Compand before the unavoidable 8-bit quantization. Reach's
              // visible scene values around 0..2 then occupy roughly 0..64
              // instead of only 0..16 codes.
              component = builder.createTriBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
                  component, const_float_0, const_float_1);
              component = builder.createUnaryBuiltinCall(
                  type_float, ext_inst_glsl_std_450, GLSLstd450Sqrt,
                  component);
            } else if (curve == 3) {
              // Soft knee after linear scale: preserves dark/mid foliage and
              // armor while compressing HDR emissives that used to clip white.
              component = builder.createBinOp(
                  spv::OpFDiv, type_float, component,
                  builder.createBinOp(spv::OpFAdd, type_float, component,
                                      soft_knee));
            }
          }
          id_vector_temp.push_back(component);
        }
        id_vector_temp.push_back(
            builder.createCompositeExtract(source_vec4, type_float, 3));
        source_vec4 =
            builder.createCompositeConstruct(type_float4, id_vector_temp);
        static uint32_t android_normalize_7e3_rgba8_repack_log_count = 0;
        if (android_normalize_7e3_rgba8_repack_log_count++ < 16) {
          XELOGI(
              "HaloCompat normalized 7e3 RGBA8 dump repack: src_fmt={} "
              "rgb_scale=1/31.875 curve={}",
              uint32_t(key.GetColorFormat()), curve);
        }
      }
      // 7e3-float owners can exceed [0, 1]; saturate so values don't wrap in
      // the 8-bit BitFieldInsert. No-op for unorm owners.
      spv::Id const_float_0 = builder.makeFloatConstant(0.0f);
      spv::Id const_float_1 = builder.makeFloatConstant(1.0f);
      id_vector_temp.clear();
      for (uint32_t i = 0; i < 4; ++i) {
        id_vector_temp.push_back(const_float_0);
      }
      spv::Id const_float4_0 =
          builder.makeCompositeConstant(type_float4, id_vector_temp);
      id_vector_temp.clear();
      for (uint32_t i = 0; i < 4; ++i) {
        id_vector_temp.push_back(const_float_1);
      }
      spv::Id const_float4_1 =
          builder.makeCompositeConstant(type_float4, id_vector_temp);
      source_vec4 = builder.createTriBuiltinCall(
          type_float4, ext_inst_glsl_std_450, GLSLstd450NClamp, source_vec4,
          const_float4_0, const_float4_1);
    }
#endif
    switch (dump_pack_format) {
      case xenos::ColorRenderTargetFormat::k_8_8_8_8:
      case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale = builder.makeFloatConstant(255.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createCompositeExtract(source_vec4, type_float, 0),
                    unorm_scale),
                unorm_round_offset));
        spv::Id component_width = builder.makeUintConstant(8);
        for (uint32_t i = 1; i < 4; ++i) {
          uint32_t source_component = i;
#if XE_PLATFORM_ANDROID
          // Previously: source_to_1x + 1010102 forced byte1=alpha (killed green)
          // for a 0xAC2 present hack. Keep real G; present swizzle handles
          // residual channel order.
          if (key.android_writer_gb_fix) {
            if (i == 1) {
              source_component = 2;
            } else if (i == 2) {
              source_component = 1;
            }
          }
#endif
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(spv::OpFMul, type_float,
                                          builder.createCompositeExtract(
                                              source_vec4, type_float,
                                              source_component),
                                          unorm_scale),
                      unorm_round_offset)),
              builder.makeUintConstant(8 * i), component_width);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: {
        spv::Id unorm_round_offset = builder.makeFloatConstant(0.5f);
        spv::Id unorm_scale_rgb = builder.makeFloatConstant(1023.0f);
        packed[0] = builder.createUnaryOp(
            spv::OpConvertFToU, type_uint,
            builder.createBinOp(
                spv::OpFAdd, type_float,
                builder.createBinOp(
                    spv::OpFMul, type_float,
                    builder.createCompositeExtract(source_vec4, type_float, 0),
                    unorm_scale_rgb),
                unorm_round_offset));
        spv::Id width_rgb = builder.makeUintConstant(10);
        spv::Id unorm_scale_a = builder.makeFloatConstant(3.0f);
        spv::Id width_a = builder.makeUintConstant(2);
        for (uint32_t i = 1; i < 4; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              builder.createUnaryOp(
                  spv::OpConvertFToU, type_uint,
                  builder.createBinOp(
                      spv::OpFAdd, type_float,
                      builder.createBinOp(
                          spv::OpFMul, type_float,
                          builder.createCompositeExtract(source_vec4,
                                                         type_float, i),
                          i == 3 ? unorm_scale_a : unorm_scale_rgb),
                      unorm_round_offset)),
              builder.makeUintConstant(10 * i), i == 3 ? width_a : width_rgb);
        }
      } break;
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
      case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16: {
        // Float16 has a wider range for both color and alpha, also NaNs - clamp
        // and convert.
        packed[0] = ConvertFloat32To7e3ForRenderTarget(
            builder, builder.createCompositeExtract(source_vec4, type_float, 0),
            ext_inst_glsl_std_450, UsesScaledUNorm7e3RenderTargets());
        spv::Id width_rgb = builder.makeUintConstant(10);
        for (uint32_t i = 1; i < 3; ++i) {
          packed[0] = builder.createQuadOp(
              spv::OpBitFieldInsert, type_uint, packed[0],
              ConvertFloat32To7e3ForRenderTarget(
                  builder,
                  builder.createCompositeExtract(source_vec4, type_float, i),
                  ext_inst_glsl_std_450,
                  UsesScaledUNorm7e3RenderTargets()),
              builder.makeUintConstant(10 * i), width_rgb);
        }
        // Saturate and convert the alpha.
        spv::Id alpha_saturated = builder.createTriBuiltinCall(
            type_float, ext_inst_glsl_std_450, GLSLstd450NClamp,
            builder.createCompositeExtract(source_vec4, type_float, 3),
            builder.makeFloatConstant(0.0f), builder.makeFloatConstant(1.0f));
        packed[0] = builder.createQuadOp(
            spv::OpBitFieldInsert, type_uint, packed[0],
            builder.createUnaryOp(
                spv::OpConvertFToU, type_uint,
                builder.createBinOp(
                    spv::OpFAdd, type_float,
                    builder.createBinOp(spv::OpFMul, type_float,
                                        alpha_saturated,
                                        builder.makeFloatConstant(3.0f)),
                    builder.makeFloatConstant(0.5f))),
            builder.makeUintConstant(30), builder.makeUintConstant(2));
      } break;
      case xenos::ColorRenderTargetFormat::k_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16:
      case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
      case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: {
        // All 64bpp formats, and all 16 bits per component formats, are
        // represented as integers in ownership transfer for safe handling of
        // NaN encodings and -32768 / -32767.
        // TODO(Triang3l): Handle the case when that's not true (no multisampled
        // sampled images, no 16-bit UNORM, no cross-packing 32bpp aliasing on a
        // portability subset device or a 64bpp format where that wouldn't help
        // anyway).
        spv::Id component_offset_width = builder.makeUintConstant(16);
        // WO25: when native_float_view_for_16bpc_transfer forced a float
        // fetch (source_is_uint=false) for this format, the OpImageFetch on
        // the SFLOAT view already upconverted each 16-bit half to a 32-bit
        // float - a raw bitcast back to uint would NOT reproduce the
        // original 16-bit pattern (it would produce a 32-bit float's bit
        // pattern, garbage once treated as two packed 16-bit fields).
        // GLSLstd450PackHalf2x16 on a float2 correctly re-quantizes both
        // components to f16 and packs them into the same low16/high16 uint32
        // layout OpBitFieldInsert produces below - same semantic intent
        // (preserve "is NaN" and finite magnitude) via the float pipeline
        // instead of a raw-bit reinterpreting view.
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          if (source_is_uint) {
            packed[i] = builder.createQuadOp(
                spv::OpBitFieldInsert, type_uint,
                builder.createCompositeExtract(source_vec4, type_uint, 2 * i),
                builder.createCompositeExtract(source_vec4, type_uint,
                                               2 * i + 1),
                component_offset_width, component_offset_width);
          } else {
            id_vector_temp.clear();
            id_vector_temp.push_back(
                builder.createCompositeExtract(source_vec4, type_float, 2 * i));
            id_vector_temp.push_back(builder.createCompositeExtract(
                source_vec4, type_float, 2 * i + 1));
            spv::Id component_pair = builder.createCompositeConstruct(
                builder.makeVectorType(type_float, 2), id_vector_temp);
            packed[i] = builder.createUnaryBuiltinCall(
                type_uint, ext_inst_glsl_std_450, GLSLstd450PackHalf2x16,
                component_pair);
          }
        }
      } break;
      // Float32 is transferred as uint32 to preserve NaN encodings. However,
      // multisampled sampled image support is optional in Vulkan.
      case xenos::ColorRenderTargetFormat::k_32_FLOAT:
      case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: {
        for (uint32_t i = 0; i <= uint32_t(format_is_64bpp); ++i) {
          spv::Id& packed_ref = packed[i];
          packed_ref = builder.createCompositeExtract(source_vec4,
                                                      source_component_type, i);
          if (!source_is_uint) {
            packed_ref =
                builder.createUnaryOp(spv::OpBitcast, type_uint, packed_ref);
          }
        }
      } break;
    }
  }

  // Write the packed value to the EDRAM buffer.
  spv::Id store_value = packed[0];
  if (format_is_64bpp) {
    id_vector_temp.clear();
    id_vector_temp.push_back(packed[0]);
    id_vector_temp.push_back(packed[1]);
    store_value = builder.createCompositeConstruct(type_uint2, id_vector_temp);
  }
  id_vector_temp.clear();
  // The only SSBO structure member.
  id_vector_temp.push_back(builder.makeIntConstant(0));
  id_vector_temp.push_back(
      builder.createUnaryOp(spv::OpBitcast, type_int, edram_sample_address));
  // StorageBuffer since SPIR-V 1.3, but since SPIR-V 1.0 is generated, it's
  // Uniform.
  builder.createStore(store_value,
                      builder.createAccessChain(spv::StorageClassUniform,
                                                edram_buffer, id_vector_temp));

  // End the main function and make it the entry point.
  builder.leaveFunction();
  builder.addExecutionMode(main_function, spv::ExecutionModeLocalSize,
                           kDumpSamplesPerGroupX, kDumpSamplesPerGroupY, 1);
  spv::Instruction* entry_point = builder.addEntryPoint(
      spv::ExecutionModelGLCompute, main_function, "main");
  // Bindings only need to be added to the entry point's interface starting with
  // SPIR-V 1.4 - emitting 1.0 here, so only inputs / outputs.
  entry_point->addIdOperand(input_global_invocation_id);

  // Serialize the shader code.
  std::vector<unsigned int> shader_code;
  builder.dump(shader_code);

  // Create the pipeline, and store the handle even if creation fails not to try
  // to create it again later.
  VkPipeline pipeline = ui::vulkan::util::CreateComputePipeline(
      command_processor_.GetVulkanDevice(),
      key.is_depth ? dump_pipeline_layout_depth_ : dump_pipeline_layout_color_,
      reinterpret_cast<const uint32_t*>(shader_code.data()),
      sizeof(uint32_t) * shader_code.size());
  if (pipeline == VK_NULL_HANDLE) {
    XELOGE(
        "VulkanRenderTargetCache: Failed to create a render target dumping "
        "pipeline for {}-sample render targets with format {}",
        UINT32_C(1) << uint32_t(key.msaa_samples),
        key.is_depth
            ? xenos::GetDepthRenderTargetFormatName(key.GetDepthFormat())
            : xenos::GetColorRenderTargetFormatName(key.GetColorFormat()));
  }
  dump_pipelines_.emplace(key, pipeline);
  return pipeline;
}

#if XE_PLATFORM_ANDROID
namespace {

// IEEE 754 binary16 → float (Vulkan R16G16B16A16_SFLOAT host contents).
float AndroidIeeeHalfToFloat(uint16_t bits) {
  const uint32_t sign = (uint32_t(bits) & 0x8000u) << 16;
  const uint32_t exp = (uint32_t(bits) >> 10) & 0x1Fu;
  const uint32_t mant = uint32_t(bits) & 0x3FFu;
  uint32_t fbits;
  if (exp == 0) {
    if (mant == 0) {
      fbits = sign;
    } else {
      uint32_t m = mant;
      uint32_t e = 127 - 15 + 1;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        --e;
      }
      m &= 0x3FFu;
      fbits = sign | (e << 23) | (m << 13);
    }
  } else if (exp == 31) {
    fbits = sign | 0x7F800000u | (mant << 13);
  } else {
    fbits = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
  }
  float value;
  std::memcpy(&value, &fbits, sizeof(value));
  return value;
}

}  // namespace

void VulkanRenderTargetCache::AndroidHaloMaybeLogHost675Stats(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch, uint32_t resolve_dest, uint32_t resolve_dest_fmt) {
  static uint32_t host_675_stats_seen = 0;
  static uint32_t host_675_stats_log_count = 0;
  if (!GetAndroidHaloExperiment().dump_host_675_stats ||
      dump_base != 675 || resolve_dest != UINT32_C(0x02354000) ||
      resolve_dest_fmt != uint32_t(xenos::ColorFormat::k_16_16_16_16)) {
    return;
  }
  // fmt26@675 resolves burst early; subsample so cinematic (~f1800) and
  // gameplay (~f2100) are covered. Skip first 200, then log every 25th.
  ++host_675_stats_seen;
  if (host_675_stats_seen <= 200 || (host_675_stats_seen % 25) != 0 ||
      host_675_stats_log_count >= 24) {
    return;
  }

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
  if (dump_rectangles_.empty()) {
    XELOGI(
        "HOSTDUMP_675 n={} frame={} dest=0x02354000 dest_fmt=26 dump_base=675 "
        "rectangles=0",
        host_675_stats_log_count, command_processor_.GetCurrentSubmission());
    ++host_675_stats_log_count;
    return;
  }

  VulkanRenderTarget* vulkan_rt = nullptr;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto* candidate =
        static_cast<VulkanRenderTarget*>(rectangle.render_target);
    if (!candidate->key().is_depth) {
      vulkan_rt = candidate;
      break;
    }
  }
  if (!vulkan_rt) {
    XELOGI(
        "HOSTDUMP_675 n={} frame={} dest=0x02354000 dest_fmt=26 dump_base=675 "
        "no_color_rt",
        host_675_stats_log_count, command_processor_.GetCurrentSubmission());
    ++host_675_stats_log_count;
    return;
  }

  const RenderTargetKey rt_key = vulkan_rt->key();
  const VkFormat vk_format = GetColorVulkanFormat(rt_key.GetColorFormat());
  const bool is_f16 = vk_format == VK_FORMAT_R16G16B16A16_SFLOAT;
  const bool is_f32 = vk_format == VK_FORMAT_R32G32B32A32_SFLOAT;
  if (!is_f16 && !is_f32) {
    XELOGI(
        "HOSTDUMP_675 n={} frame={} rt={}/{}/{}/{}x vk_fmt={} unsupported "
        "(want SFLOAT16/32)",
        host_675_stats_log_count, command_processor_.GetCurrentSubmission(),
        uint32_t(rt_key.base_tiles), rt_key.GetPitchTiles(),
        rt_key.GetFormatName(), uint32_t(1) << uint32_t(rt_key.msaa_samples),
        uint32_t(vk_format));
    ++host_675_stats_log_count;
    return;
  }

  const uint32_t image_width = rt_key.GetWidth() * draw_resolution_scale_x();
  const uint32_t image_height =
      GetRenderTargetHeight(rt_key.pitch_tiles_at_32bpp, rt_key.msaa_samples) *
      draw_resolution_scale_y();
  if (!image_width || !image_height) {
    return;
  }

  // Reach scene resolves only use the top 1152x720 of the EDRAM surface; the
  // host RT is taller (~2192). Center the crop in the *visible* rect or the
  // probe reads empty padding below the game view.
  constexpr uint32_t kVisibleW = 1152;
  constexpr uint32_t kVisibleH = 720;
  constexpr uint32_t kCropW = 160;
  constexpr uint32_t kCropH = 90;
  const uint32_t visible_w = std::min(kVisibleW, image_width);
  const uint32_t visible_h = std::min(kVisibleH, image_height);
  const uint32_t copy_w = std::min(kCropW, visible_w);
  const uint32_t copy_h = std::min(kCropH, visible_h);
  const int32_t copy_x = int32_t((visible_w - copy_w) / 2);
  const int32_t copy_y = int32_t((visible_h - copy_h) / 2);
  const uint32_t bpp = is_f32 ? 16u : 8u;
  const VkDeviceSize buffer_size =
      VkDeviceSize(copy_w) * VkDeviceSize(copy_h) * VkDeviceSize(bpp);

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, buffer_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer,
          readback_memory)) {
    XELOGW("HOSTDUMP_675: failed to create readback buffer size={}",
           uint64_t(buffer_size));
    ++host_675_stats_log_count;
    return;
  }

  if (rt_key.msaa_samples != xenos::MsaaSamples::k1X) {
    XELOGI(
        "HOSTDUMP_675 n={} frame={} rt={}/{}/{}/{}x vk_fmt={} skip_msaa "
        "(1x-only probe)",
        host_675_stats_log_count, command_processor_.GetCurrentSubmission(),
        uint32_t(rt_key.base_tiles), rt_key.GetPitchTiles(),
        rt_key.GetFormatName(), uint32_t(1) << uint32_t(rt_key.msaa_samples),
        uint32_t(vk_format));
    ++host_675_stats_log_count;
    return;
  }

  command_processor_.PushImageMemoryBarrier(
      vulkan_rt->image(),
      ui::vulkan::util::InitializeSubresourceRange(VK_IMAGE_ASPECT_COLOR_BIT),
      vulkan_rt->current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
      vulkan_rt->current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT,
      vulkan_rt->current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vulkan_rt->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  command_processor_.SubmitBarriers(true);

  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  VkBufferImageCopy copy_region = {};
  copy_region.bufferOffset = 0;
  copy_region.bufferRowLength = 0;
  copy_region.bufferImageHeight = 0;
  copy_region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  copy_region.imageSubresource.mipLevel = 0;
  copy_region.imageSubresource.baseArrayLayer = 0;
  copy_region.imageSubresource.layerCount = 1;
  copy_region.imageOffset = {copy_x, copy_y, 0};
  copy_region.imageExtent = {copy_w, copy_h, 1};
  command_buffer.CmdVkCopyImageToBuffer(
      vulkan_rt->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      readback_buffer, 1, &copy_region);

  if (!command_processor_.AndroidAwaitQueueAndResumeGuestSubmission()) {
    XELOGW("HOSTDUMP_675: await/resume failed");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    ++host_675_stats_log_count;
    return;
  }

  void* mapped = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, buffer_size, 0, &mapped) !=
          VK_SUCCESS ||
      !mapped) {
    XELOGW("HOSTDUMP_675: map failed");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    ++host_675_stats_log_count;
    return;
  }

  uint32_t nonblack = 0;
  uint32_t hdr_count = 0;
  float min_r = 1.0e30f, min_g = 1.0e30f, min_b = 1.0e30f;
  float max_r = -1.0e30f, max_g = -1.0e30f, max_b = -1.0e30f;
  double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0;

  // Subsample every 8th pixel of the crop to keep CPU work light.
  constexpr uint32_t kStride = 8;
  uint32_t sample_count = 0;
  if (is_f16) {
    const uint16_t* pixels = reinterpret_cast<const uint16_t*>(mapped);
    for (uint32_t y = 0; y < copy_h; y += kStride) {
      for (uint32_t x = 0; x < copy_w; x += kStride) {
        const uint32_t idx = (y * copy_w + x) * 4;
        const float r = AndroidIeeeHalfToFloat(pixels[idx + 0]);
        const float g = AndroidIeeeHalfToFloat(pixels[idx + 1]);
        const float b = AndroidIeeeHalfToFloat(pixels[idx + 2]);
        const bool finite =
            (r == r) && (g == g) && (b == b) &&
            r < 1.0e20f && g < 1.0e20f && b < 1.0e20f &&
            r > -1.0e20f && g > -1.0e20f && b > -1.0e20f;
        if (!finite) {
          continue;
        }
        ++sample_count;
        if (r != 0.0f || g != 0.0f || b != 0.0f) {
          ++nonblack;
        }
        if (r > 1.0f || g > 1.0f || b > 1.0f) {
          ++hdr_count;
        }
        min_r = std::min(min_r, r);
        min_g = std::min(min_g, g);
        min_b = std::min(min_b, b);
        max_r = std::max(max_r, r);
        max_g = std::max(max_g, g);
        max_b = std::max(max_b, b);
        sum_r += double(r);
        sum_g += double(g);
        sum_b += double(b);
      }
    }
  } else {
    const float* pixels = reinterpret_cast<const float*>(mapped);
    for (uint32_t y = 0; y < copy_h; y += kStride) {
      for (uint32_t x = 0; x < copy_w; x += kStride) {
        const uint32_t idx = (y * copy_w + x) * 4;
        const float r = pixels[idx + 0];
        const float g = pixels[idx + 1];
        const float b = pixels[idx + 2];
        const bool finite =
            (r == r) && (g == g) && (b == b) &&
            r < 1.0e20f && g < 1.0e20f && b < 1.0e20f &&
            r > -1.0e20f && g > -1.0e20f && b > -1.0e20f;
        if (!finite) {
          continue;
        }
        ++sample_count;
        if (r != 0.0f || g != 0.0f || b != 0.0f) {
          ++nonblack;
        }
        if (r > 1.0f || g > 1.0f || b > 1.0f) {
          ++hdr_count;
        }
        min_r = std::min(min_r, r);
        min_g = std::min(min_g, g);
        min_b = std::min(min_b, b);
        max_r = std::max(max_r, r);
        max_g = std::max(max_g, g);
        max_b = std::max(max_b, b);
        sum_r += double(r);
        sum_g += double(g);
        sum_b += double(b);
      }
    }
  }

  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  // AndroidAwaitQueueAndResumeGuestSubmission already reopened the submission.

  const float mean_r =
      sample_count ? float(sum_r / double(sample_count)) : 0.0f;
  const float mean_g =
      sample_count ? float(sum_g / double(sample_count)) : 0.0f;
  const float mean_b =
      sample_count ? float(sum_b / double(sample_count)) : 0.0f;
  const float hdr_pct =
      sample_count ? (100.0f * float(hdr_count) / float(sample_count)) : 0.0f;
  if (sample_count == 0) {
    min_r = min_g = min_b = 0.0f;
    max_r = max_g = max_b = 0.0f;
  }

  XELOGI(
      "HOSTDUMP_675 n={} frame={} rt={}/{}/{}/{}x vk_fmt={} crop={}x{}@{},{} "
      "samples={} nonblack={} min_rgb=({:.5g},{:.5g},{:.5g}) "
      "max_rgb=({:.5g},{:.5g},{:.5g}) mean_rgb=({:.5g},{:.5g},{:.5g}) "
      "hdr_pct={:.2f}",
      host_675_stats_log_count, command_processor_.GetCurrentSubmission(),
      uint32_t(rt_key.base_tiles), rt_key.GetPitchTiles(),
      rt_key.GetFormatName(), uint32_t(1) << uint32_t(rt_key.msaa_samples),
      uint32_t(vk_format), copy_w, copy_h, copy_x, copy_y, sample_count,
      nonblack, min_r, min_g, min_b, max_r, max_g, max_b, mean_r, mean_g,
      mean_b, hdr_pct);
  ++host_675_stats_log_count;
}
#endif  // XE_PLATFORM_ANDROID

void VulkanRenderTargetCache::DumpRenderTargets(uint32_t dump_base,
                                                uint32_t dump_row_length_used,
                                                uint32_t dump_rows,
                                                uint32_t dump_pitch) {
  assert_true(GetPath() == Path::kHostRenderTargets);

  GetResolveCopyRectanglesToDump(dump_base, dump_row_length_used, dump_rows,
                                 dump_pitch, dump_rectangles_);
#if XE_PLATFORM_ANDROID
  if (android_prefer_7e3_dump_675_ && dump_base == 675 && dump_pitch == 15 &&
      dump_row_length_used && dump_rows) {
    const auto is_7e3_owner = [](const RenderTargetKey& key) -> bool {
      if (key.is_depth) {
        return false;
      }
      const auto fmt = key.GetColorFormat();
      return fmt == xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
             fmt == xenos::ColorRenderTargetFormat::
                       k_2_10_10_10_FLOAT_AS_16_16_16_16;
    };
    bool owner_is_7e3 = !dump_rectangles_.empty();
    for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
      if (!is_7e3_owner(rectangle.render_target->key())) {
        owner_is_7e3 = false;
        break;
      }
    }
    if (!owner_is_7e3) {
      RenderTargetKey float_key;
      float_key.base_tiles = 675;
      float_key.pitch_tiles_at_32bpp = 15;
      float_key.msaa_samples = xenos::MsaaSamples::k1X;
      float_key.is_depth = 0;
      float_key.resource_format =
          uint32_t(xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT);
      RenderTarget* float_rt = FindRenderTarget(float_key);
      if (!float_rt) {
        // The owner check accepts the AS_16_16_16_16 aliasing of the same 7e3
        // data, so the lookup must too - the game switches between both keys
        // for the same physical target.
        float_key.resource_format = uint32_t(
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16);
        float_rt = FindRenderTarget(float_key);
      }
      if (!float_rt) {
        // No live 7e3 RT (typically right after a memory-pressure cache
        // clear deleted it - the redirect's premise is that it is NOT the
        // ownership-map owner, so ClearCache picks exactly it). The dump
        // falls back to the LDR owner and colors go dull until the next HDR
        // pass recreates the RT; log so disarm windows are visible.
        static uint32_t android_prefer_7e3_dump_miss_log_count = 0;
        if (android_prefer_7e3_dump_miss_log_count < 64) {
          XELOGW("SCENE675_DUMP_PREFER_7E3 miss={}: no live 7e3 RT, dumping "
                 "current owner",
                 android_prefer_7e3_dump_miss_log_count);
          ++android_prefer_7e3_dump_miss_log_count;
        }
      }
      if (float_rt) {
        const uint32_t prev_fmt =
            dump_rectangles_.empty()
                ? UINT32_MAX
                : uint32_t(dump_rectangles_.front()
                               .render_target->key()
                               .resource_format);
        dump_rectangles_.clear();
        dump_rectangles_.emplace_back(float_rt, 0, dump_rows, 0,
                                      dump_row_length_used);
        static uint32_t android_prefer_7e3_dump_log_count = 0;
        static uint32_t android_prefer_7e3_dump_apply_count = 0;
        ++android_prefer_7e3_dump_apply_count;
        if (android_prefer_7e3_dump_log_count < 64) {
          XELOGI(
              "SCENE675_DUMP_PREFER_7E3 count={} applied={} prev_owner_fmt={} "
              "dump_rows={} row_len={}",
              android_prefer_7e3_dump_log_count,
              android_prefer_7e3_dump_apply_count, prev_fmt, dump_rows,
              dump_row_length_used);
          ++android_prefer_7e3_dump_log_count;
        }
      }
    }
  }
#endif
  if (dump_rectangles_.empty()) {
#if XE_PLATFORM_ANDROID
    if (cvars::halo_android_diag_log_resolve_constants && dump_base == 1350 &&
        dump_row_length_used == 15 && dump_rows == 45 && dump_pitch == 15) {
      XELOGI(
          "Android Halo dump rectangles: none dump_base={} rows={} row_len={} "
          "pitch={}",
          dump_base, dump_rows, dump_row_length_used, dump_pitch);
    }
#endif
    return;
  }
#if XE_PLATFORM_ANDROID
  const bool android_halo_dump_span =
      dump_base == 1350 && dump_row_length_used == 15 && dump_rows == 45 &&
      dump_pitch == 15;
  const bool android_halo_menu_dump_span =
      dump_base == 0 && dump_row_length_used == 15 && dump_rows == 45 &&
      dump_pitch == 29;
  if (android_halo_dump_span &&
      android_depth_to_color_edram_fallback_source_) {
    VulkanRenderTarget* fallback_source =
        android_depth_to_color_edram_fallback_source_;
    android_depth_to_color_edram_fallback_source_ = nullptr;
    const RenderTargetKey fallback_key = fallback_source->key();
    if (fallback_key.is_depth &&
        fallback_key.msaa_samples == xenos::MsaaSamples::k4X &&
        fallback_key.base_tiles == 1350 && fallback_key.GetPitchTiles() == 15 &&
        fallback_key.GetWidth() == 600) {
      dump_rectangles_.clear();
      dump_rectangles_.emplace_back(fallback_source, 0, 45, 0, 15);
      XELOGI(
          "Android DepthToColor EDRAM fallback: dumping source 4x depth RT "
          "directly to EDRAM base={} row_len={} rows={} pitch={}",
          dump_base, dump_row_length_used, dump_rows, dump_pitch);
    } else {
      XELOGW(
          "Android DepthToColor EDRAM fallback ignored: source depth RT no "
          "longer matches base=1350 pitch=15 4x width=600");
    }
  }
  const bool android_log_halo_dump =
      cvars::halo_android_diag_log_resolve_constants && android_halo_dump_span;
  if (android_log_halo_dump) {
    XELOGI(
        "Android Halo dump rectangles: count={} dump_base={} rows={} "
        "row_len={} pitch={}",
        uint32_t(dump_rectangles_.size()), dump_base, dump_rows,
        dump_row_length_used, dump_pitch);
    uint32_t rectangle_index = 0;
    for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
      const auto& vulkan_rt =
          *static_cast<const VulkanRenderTarget*>(rectangle.render_target);
      const RenderTargetKey rt_key = vulkan_rt.key();
      ResolveCopyDumpRectangle::Dispatch
          dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
      const uint32_t dispatch_count = rectangle.GetDispatches(
          dump_pitch, dump_row_length_used, dispatches);
      XELOGI(
          "Android Halo dump rectangle {}: rt_base={} rt_pitch={} rt_width={} "
          "msaa={} depth={} format={} row_first={} rows={} first_start={} "
          "last_end={} dispatch_count={}",
          rectangle_index, uint32_t(rt_key.base_tiles),
          rt_key.GetPitchTiles(), rt_key.GetWidth(),
          uint32_t(1) << uint32_t(rt_key.msaa_samples), uint32_t(rt_key.is_depth),
          rt_key.GetFormatName(), rectangle.row_first, rectangle.rows,
          rectangle.row_first_start, rectangle.row_last_end, dispatch_count);
      for (uint32_t i = 0; i < dispatch_count; ++i) {
        const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
        XELOGI(
            "Android Halo dump dispatch {}.{}: first_tile={} offset={} "
            "width_tiles={} height_tiles={} group_scale={}x{}",
            rectangle_index, i, dump_base + dispatch.offset, dispatch.offset,
            dispatch.width_tiles, dispatch.height_tiles,
            draw_resolution_scale_x(), draw_resolution_scale_y());
      }
      ++rectangle_index;
    }
  }
  AndroidHaloCaptureMenuSceneShadow(dump_base, dump_row_length_used,
                                    dump_rows, dump_pitch);
  const bool android_halo_menu_transfer_dump =
      android_halo_menu_dump_span &&
      GetAndroidHaloExperiment().menu_base0_transfer_dump;
  if (((cvars::halo_android_diag_transfer_dump_8bpp &&
        android_halo_dump_span) ||
       android_halo_menu_transfer_dump) &&
      dump_rectangles_.size() == 1) {
    const ResolveCopyDumpRectangle& rectangle = dump_rectangles_.front();
    auto& vulkan_rt =
        *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    const RenderTargetKey rt_key = vulkan_rt.key();
    const xenos::ColorRenderTargetFormat color_format =
        rt_key.is_depth ? xenos::ColorRenderTargetFormat::k_8_8_8_8
                        : rt_key.GetColorFormat();
    const bool transfer_dump_color_format =
        !rt_key.is_depth &&
        (color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
         color_format == xenos::ColorRenderTargetFormat::k_2_10_10_10);
    const bool transfer_dump_presentable_supported =
        transfer_dump_color_format &&
        rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
        rt_key.base_tiles == 1350 && rt_key.GetPitchTiles() == 15 &&
        rt_key.GetWidth() == 1200 && rectangle.row_first == 0 &&
        rectangle.rows == 45 && rectangle.row_first_start == 0 &&
        rectangle.row_last_end == 15 && draw_resolution_scale_x() == 1 &&
        draw_resolution_scale_y() == 1;
    const bool transfer_dump_menu_supported =
        android_halo_menu_transfer_dump && transfer_dump_color_format &&
        color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
        rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
        rt_key.base_tiles == 0 && rt_key.GetPitchTiles() == 29 &&
        rt_key.GetWidth() == 2320 && rectangle.row_first == 0 &&
        rectangle.rows == 45 && rectangle.row_first_start == 0 &&
        rectangle.row_last_end == 15 && draw_resolution_scale_x() == 1 &&
        draw_resolution_scale_y() == 1;
    const bool transfer_dump_supported =
        transfer_dump_presentable_supported || transfer_dump_menu_supported;
    if (transfer_dump_supported) {
      constexpr uint32_t kTileWidth = xenos::kEdramTileWidthSamples;
      constexpr uint32_t kTileHeight = xenos::kEdramTileHeightSamples;
      constexpr uint32_t kTileBytes = kTileWidth * kTileHeight * 4;
      command_processor_.PushImageMemoryBarrier(
          vulkan_rt.image(),
          ui::vulkan::util::InitializeSubresourceRange(
              VK_IMAGE_ASPECT_COLOR_BIT),
          vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_TRANSFER_BIT,
          vulkan_rt.current_access_mask(), VK_ACCESS_TRANSFER_READ_BIT,
          vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      vulkan_rt.SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
      UseEdramBuffer(EdramBufferUsage::kTransferWrite);
      command_processor_.SubmitBarriers(true);

      std::vector<VkBufferImageCopy> copy_regions;
      const uint32_t copy_columns =
          transfer_dump_menu_supported ? dump_row_length_used : dump_pitch;
      copy_regions.reserve(size_t(copy_columns) * size_t(dump_rows));
      for (uint32_t tile_y = 0; tile_y < dump_rows; ++tile_y) {
        for (uint32_t tile_x = 0; tile_x < copy_columns; ++tile_x) {
          VkBufferImageCopy& region = copy_regions.emplace_back();
          region.bufferOffset =
              VkDeviceSize(dump_base + tile_y * dump_pitch + tile_x) *
              VkDeviceSize(kTileBytes);
          region.bufferRowLength = kTileWidth;
          region.bufferImageHeight = kTileHeight;
          region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          region.imageSubresource.mipLevel = 0;
          region.imageSubresource.baseArrayLayer = 0;
          region.imageSubresource.layerCount = 1;
          region.imageOffset = {int32_t(tile_x * kTileWidth),
                                int32_t(tile_y * kTileHeight), 0};
          region.imageExtent = {kTileWidth, kTileHeight, 1};
        }
      }
      command_processor_.deferred_command_buffer().CmdVkCopyImageToBuffer(
          vulkan_rt.image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          edram_buffer_, uint32_t(copy_regions.size()), copy_regions.data());
      XELOGI(
          "Android transfer dump 32bpp: menu={} copied {} tiles from {} RT "
          "image to EDRAM base={} pitch={} rows={} columns={}",
          uint32_t(transfer_dump_menu_supported),
          uint32_t(copy_regions.size()), rt_key.GetFormatName(), dump_base,
          dump_pitch, dump_rows, copy_columns);
      return;
    }
    if (android_log_halo_dump) {
      XELOGI(
          "Android transfer dump 32bpp skipped: rt_base={} rt_pitch={} "
          "rt_width={} msaa={} depth={} format={} row_first={} rows={} "
          "first_start={} last_end={} scale={}x{}",
          uint32_t(rt_key.base_tiles), rt_key.GetPitchTiles(),
          rt_key.GetWidth(), uint32_t(1) << uint32_t(rt_key.msaa_samples),
          uint32_t(rt_key.is_depth), rt_key.GetFormatName(),
          rectangle.row_first, rectangle.rows, rectangle.row_first_start,
          rectangle.row_last_end, draw_resolution_scale_x(),
          draw_resolution_scale_y());
    }
  }
#endif

  // Clear previously set temporary indices.
  AndroidHaloExecutePendingDumpRectangles(dump_base, dump_row_length_used,
                                          dump_rows, dump_pitch);
#if XE_PLATFORM_ANDROID
  if (android_halo_dump_span) {
    AndroidHaloCapturePresentableShadow(dump_base, dump_row_length_used,
                                        dump_rows, dump_pitch);
  }
#endif
}

void VulkanRenderTargetCache::ExecutePendingDumpRectanglesToEdram(
    uint32_t dump_base, uint32_t dump_row_length_used, uint32_t dump_rows,
    uint32_t dump_pitch) {
  if (dump_rectangles_.empty()) {
    return;
  }

  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    static_cast<VulkanRenderTarget*>(rectangle.render_target)
        ->SetTemporarySortIndex(UINT32_MAX);
  }
  UseEdramBuffer(EdramBufferUsage::kComputeWrite);
  dump_invocations_.clear();
  dump_invocations_.reserve(dump_rectangles_.size());
  uint32_t rt_sort_index = 0;
  for (const ResolveCopyDumpRectangle& rectangle : dump_rectangles_) {
    auto& vulkan_rt =
        *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    command_processor_.PushImageMemoryBarrier(
        vulkan_rt.image(),
        ui::vulkan::util::InitializeSubresourceRange(
            rt_key.is_depth
                ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                : VK_IMAGE_ASPECT_COLOR_BIT),
        vulkan_rt.current_stage_mask(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        vulkan_rt.current_access_mask(), VK_ACCESS_SHADER_READ_BIT,
        vulkan_rt.current_layout(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vulkan_rt.SetUsage(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_ACCESS_SHADER_READ_BIT,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (vulkan_rt.temporary_sort_index() == UINT32_MAX) {
      vulkan_rt.SetTemporarySortIndex(rt_sort_index++);
    }
    DumpPipelineKey pipeline_key;
    pipeline_key.msaa_samples = rt_key.msaa_samples;
    pipeline_key.resource_format = rt_key.resource_format;
    pipeline_key.is_depth = rt_key.is_depth;
#if XE_PLATFORM_ANDROID
    // When a span is dumped from an MSAA color owner and then read back by a
    // 1x guest resolve (samples-as-pixels aliasing), the sample-layout
    // mismatch interleaves host samples into row/column stripes. Collapse to
    // sample 0 so the 1x read returns a clean (upscaled) image. Only owners
    // whose dump source is loaded as float color can be repacked as the 8888
    // read format; integer-view sources (k_16_16 family as 32bpp, k_32_FLOAT)
    // are excluded. 64bpp owners pass raw bits through and need no repack.
    const xenos::ColorRenderTargetFormat android_owner_format =
        rt_key.is_depth ? xenos::ColorRenderTargetFormat::k_8_8_8_8
                        : rt_key.GetColorFormat();
    const bool android_repack_fixed_16_16_to_8888 =
        GetAndroidHaloExperiment().repack_16_16_to_8888 &&
        android_owner_format == xenos::ColorRenderTargetFormat::k_16_16;
    const bool android_owner_collapsible =
        rt_key.Is64bpp() ||
        android_owner_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 ||
        android_owner_format ==
            xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA ||
        android_owner_format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
        android_owner_format ==
            xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10 ||
        android_owner_format ==
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
        android_owner_format ==
            xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16 ||
        android_repack_fixed_16_16_to_8888;
    // The bit is also needed for 1x owners whose format differs from the 8888
    // the guest reads (e.g. a 1x k_2_10_10_10 owner would otherwise dump
    // 10-bit words that scramble as 8888 - green channel destroyed). For a 1x
    // 8888-family owner the repack is an identity, so skip to avoid pipeline
    // churn.
    // WO27: a 1x 64bpp owner must NOT take the source_to_1x collapse/repack
    // path - there are no samples to collapse and the guest reads raw 64bpp
    // bits, so the mainline raw dump is bit-exact by definition, while the
    // collapse shader's 64bpp reassembly is unvalidated and demonstrably ran
    // for 1x fmt7 owners (WO22 "source_to_1x rt_base=1350 fmt=7 msaa=1").
    // legacy_collapse_64bpp_1x=1 restores the old behavior for A/B.
    const bool android_owner_1x_64bpp_raw =
        rt_key.Is64bpp() &&
        rt_key.msaa_samples == xenos::MsaaSamples::k1X &&
        !GetAndroidHaloExperiment().legacy_collapse_64bpp_1x;
    const bool android_owner_needs_repack =
        !android_owner_1x_64bpp_raw &&
        (rt_key.msaa_samples != xenos::MsaaSamples::k1X ||
         (android_owner_format != xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
          android_owner_format !=
              xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA));
    // Keep source_to_1x active in repack_mode=0 so the sample-collapse path
    // still runs, but let GetDumpPipeline pack in the owner's native format.
    pipeline_key.source_to_1x =
        !rt_key.is_depth &&
        (android_halo_force_8888_repack_ ||
         (android_owner_collapsible &&
          rt_key.Is64bpp() == android_resolve_read_64bpp_ &&
          android_owner_needs_repack && android_resolve_read_msaa_1x_ &&
          GetAndroidHaloExperiment().collapse_msaa_resolve));
    pipeline_key.android_writer_gb_fix =
        !rt_key.is_depth && GetAndroidHaloExperiment().writer_gb_fix &&
        (android_halo_force_writer_gb_fix_ ||
         (pipeline_key.source_to_1x &&
          GetAndroidHaloExperiment().repack_mode != 0));
    pipeline_key.android_force_8888_repack =
        pipeline_key.source_to_1x && !rt_key.Is64bpp() &&
        (android_halo_force_8888_repack_ ||
         GetAndroidHaloExperiment().repack_mode != 0);
#if XE_PLATFORM_ANDROID
    {
      const AndroidHaloExperiment& normalize_experiment =
          GetAndroidHaloExperiment();
      const bool is_7e3_owner =
          !rt_key.is_depth &&
          (android_owner_format ==
               xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
           android_owner_format == xenos::ColorRenderTargetFormat::
                                       k_2_10_10_10_FLOAT_AS_16_16_16_16);
      pipeline_key.android_normalize_7e3 =
          pipeline_key.android_force_8888_repack && is_7e3_owner &&
          normalize_experiment.normalize_7e3_to_rgba8_repack &&
          !UsesScaledUNorm7e3RenderTargets();
      // Only bake the curve when normalize is active so inactive keys stay
      // shared across curve experiment flips that don't affect this owner.
      pipeline_key.android_normalize_7e3_curve =
          pipeline_key.android_normalize_7e3
              ? std::min(
                    normalize_experiment.normalize_7e3_to_rgba8_repack_curve,
                    uint32_t(3))
              : 0;
    }
#endif
    if (pipeline_key.source_to_1x) {
      if (android_owner_format ==
              xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
          android_owner_format ==
              xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10 ||
          android_owner_format ==
              xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT ||
          android_owner_format ==
              xenos::ColorRenderTargetFormat::
                  k_2_10_10_10_FLOAT_AS_16_16_16_16) {
        AndroidHaloMarkGameplayPresentContent(360);
      }
      static uint32_t android_source_to_1x_log_count = 0;
      if (android_source_to_1x_log_count < 128) {
        XELOGI(
            "HaloCompat dump source_to_1x count={} rt_base={} rt_pitch={} "
            "rt_width={} fmt={} msaa={} repack_mode={} "
            "repack_16_16_to_8888={}",
            android_source_to_1x_log_count, uint32_t(rt_key.base_tiles),
            rt_key.GetPitchTiles(), rt_key.GetWidth(),
            uint32_t(android_owner_format),
            uint32_t(1) << uint32_t(rt_key.msaa_samples),
            GetAndroidHaloExperiment().repack_mode,
            uint32_t(GetAndroidHaloExperiment().repack_16_16_to_8888));
        ++android_source_to_1x_log_count;
      }
    }
    // WO27: census of which owner render targets actually get dumped for the
    // watched tile window - closes the loop between OWNSNAP (who owns tiles)
    // and what the resolve read actually pulls from. Dedupe by owner key +
    // path flags, log power-of-two count milestones.
    if (GetAndroidHaloExperiment().log_ownership_snapshot) {
      const vulkan::AndroidHaloExperiment& android_experiment_dumpinv =
          GetAndroidHaloExperiment();
      const uint32_t owner_start_tiles = uint32_t(rt_key.base_tiles);
      const uint32_t owner_end_tiles =
          owner_start_tiles +
          rt_key.GetPitchTiles() * 48;  // generous row bound for overlap test
      if (owner_start_tiles <
              android_experiment_dumpinv.ownership_watch_end_tiles &&
          owner_end_tiles >
              android_experiment_dumpinv.ownership_watch_start_tiles) {
        static std::unordered_map<uint64_t, uint32_t> dumpinv_counts;
        uint64_t dumpinv_key =
            (uint64_t(rt_key.key) << 8) |
            (uint64_t(pipeline_key.source_to_1x) << 1) |
            uint64_t(pipeline_key.android_writer_gb_fix);
        uint32_t& dumpinv_count = dumpinv_counts[dumpinv_key];
        ++dumpinv_count;
        if (dumpinv_count == 1 || !(dumpinv_count & (dumpinv_count - 1))) {
          XELOGI(
              "DUMPINV count={} owner_key=0x{:08X} owner_base={} "
              "owner_pitch32={} owner_msaa={} owner_depth={} owner_fmt={} "
              "owner_64bpp={} source_to_1x={} writer_gb_fix={}",
              dumpinv_count, rt_key.key, uint32_t(rt_key.base_tiles),
              uint32_t(rt_key.pitch_tiles_at_32bpp),
              uint32_t(1) << uint32_t(rt_key.msaa_samples),
              uint32_t(rt_key.is_depth), uint32_t(rt_key.resource_format),
              uint32_t(rt_key.Is64bpp()),
              uint32_t(pipeline_key.source_to_1x),
              uint32_t(pipeline_key.android_writer_gb_fix));
        }
      }
    }
#endif
    dump_invocations_.emplace_back(rectangle, pipeline_key);
  }

  std::sort(dump_invocations_.begin(), dump_invocations_.end());

  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();
  bool edram_buffer_bound = false;
  VkDescriptorSet last_source_descriptor_set = VK_NULL_HANDLE;
  DumpPitches last_pitches;
  DumpOffsets last_offsets;
  bool pitches_bound = false, offsets_bound = false;
  for (const DumpInvocation& invocation : dump_invocations_) {
    const ResolveCopyDumpRectangle& rectangle = invocation.rectangle;
    auto& vulkan_rt =
        *static_cast<VulkanRenderTarget*>(rectangle.render_target);
    RenderTargetKey rt_key = vulkan_rt.key();
    DumpPipelineKey pipeline_key = invocation.pipeline_key;
    VkPipeline pipeline = GetDumpPipeline(pipeline_key);
    if (!pipeline) {
      continue;
    }
    command_processor_.BindExternalComputePipeline(pipeline);

    VkPipelineLayout pipeline_layout = rt_key.is_depth
                                           ? dump_pipeline_layout_depth_
                                           : dump_pipeline_layout_color_;

    if (!edram_buffer_bound) {
      edram_buffer_bound = true;
      command_buffer.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
          kDumpDescriptorSetEdram, 1, &edram_storage_buffer_descriptor_set_, 0,
          nullptr);
    }

    VkDescriptorSet source_descriptor_set =
        vulkan_rt.GetDescriptorSetTransferSource();
    if (last_source_descriptor_set != source_descriptor_set) {
      last_source_descriptor_set = source_descriptor_set;
      command_buffer.CmdVkBindDescriptorSets(
          VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
          kDumpDescriptorSetSource, 1, &source_descriptor_set, 0, nullptr);
    }

    DumpPitches pitches;
    pitches.dest_pitch = dump_pitch;
    pitches.source_pitch = rt_key.GetPitchTiles();
    if (last_pitches != pitches) {
      last_pitches = pitches;
      pitches_bound = false;
    }
    if (!pitches_bound) {
      pitches_bound = true;
      command_buffer.CmdVkPushConstants(
          pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
          sizeof(uint32_t) * kDumpPushConstantPitches, sizeof(last_pitches),
          &last_pitches);
    }

    DumpOffsets offsets;
    offsets.source_base_tiles = rt_key.base_tiles;
    ResolveCopyDumpRectangle::Dispatch
        dispatches[ResolveCopyDumpRectangle::kMaxDispatches];
    uint32_t dispatch_count =
        rectangle.GetDispatches(dump_pitch, dump_row_length_used, dispatches);
    for (uint32_t i = 0; i < dispatch_count; ++i) {
      const ResolveCopyDumpRectangle::Dispatch& dispatch = dispatches[i];
      offsets.dispatch_first_tile = dump_base + dispatch.offset;
      if (last_offsets != offsets) {
        last_offsets = offsets;
        offsets_bound = false;
      }
      if (!offsets_bound) {
        offsets_bound = true;
        command_buffer.CmdVkPushConstants(
            pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
            sizeof(uint32_t) * kDumpPushConstantOffsets, sizeof(last_offsets),
            &last_offsets);
      }
      command_processor_.SubmitBarriers(true);
      // Must stay consistent with GetDumpPipeline's tile_width/tile_height
      // (edram_64bpp_tile_height_halved flips which dimension is halved for
      // 64bpp - see the comment there).
      const bool android_dispatch_halve_height =
          rt_key.Is64bpp() &&
          GetAndroidHaloExperiment().edram_64bpp_tile_height_halved;
      const bool android_dispatch_halve_width =
          rt_key.Is64bpp() && !android_dispatch_halve_height;
      command_buffer.CmdVkDispatch(
          (draw_resolution_scale_x() *
               (xenos::kEdramTileWidthSamples >>
                uint32_t(android_dispatch_halve_width)) *
               dispatch.width_tiles +
           (kDumpSamplesPerGroupX - 1)) /
              kDumpSamplesPerGroupX,
          (draw_resolution_scale_y() *
               (xenos::kEdramTileHeightSamples >>
                uint32_t(android_dispatch_halve_height)) *
               dispatch.height_tiles +
           (kDumpSamplesPerGroupY - 1)) /
              kDumpSamplesPerGroupY,
          1);
    }
    MarkEdramBufferModified();
  }
}

void VulkanRenderTargetCache::GetLastUpdateRenderingAttachments(
    VkRenderingAttachmentInfo* color_attachments,
    uint32_t* color_attachment_count_out,
    VkRenderingAttachmentInfo* depth_attachment,
    VkRenderingAttachmentInfo* stencil_attachment) const {
  RenderPassKey key = last_update_render_pass_key_;
  const RenderTarget* const* rts = last_update_accumulated_render_targets();

  std::memset(depth_attachment, 0, sizeof(VkRenderingAttachmentInfo));
  std::memset(stencil_attachment, 0, sizeof(VkRenderingAttachmentInfo));

  if ((key.depth_and_color_used & 0b1) && rts[0]) {
    const auto* vulkan_rt = static_cast<const VulkanRenderTarget*>(rts[0]);
    depth_attachment->sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment->imageView = vulkan_rt->view_depth_stencil();
    depth_attachment->imageLayout = VulkanRenderTarget::kDepthDrawLayout;
    depth_attachment->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depth_attachment->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    *stencil_attachment = *depth_attachment;
  }

  uint32_t color_attachment_count = 0;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    VkRenderingAttachmentInfo& color_attachment = color_attachments[i];
    std::memset(&color_attachment, 0, sizeof(VkRenderingAttachmentInfo));
    if ((key.depth_and_color_used & (1 << (1 + i))) && rts[1 + i]) {
      const auto* vulkan_rt =
          static_cast<const VulkanRenderTarget*>(rts[1 + i]);
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.imageView = vulkan_rt->view_depth_color();
      color_attachment.imageLayout = VulkanRenderTarget::kColorDrawLayout;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment_count = i + 1;
    }
  }
  *color_attachment_count_out = color_attachment_count;
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
