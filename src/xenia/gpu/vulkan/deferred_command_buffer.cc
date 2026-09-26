/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/deferred_command_buffer.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/math.h"
#include "xenia/base/profiling.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_instance.h"

DEFINE_bool(vulkan_shrink_render_area, false,
            "Shrink each host render pass's render area to the union of its "
            "draw scissors. Attachments are loaded and stored, so results are "
            "unchanged; tiled GPUs skip the unused part of each render target.",
            "Vulkan");

DEFINE_bool(vulkan_gpu_timing, false,
            "Diagnostic: write GPU timestamps around every render pass and log "
            "where GPU time goes with each performance summary.",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

void DeferredCommandBuffer::CollectGpuTiming(uint32_t slot) {
  uint32_t count = gpu_timing_query_counts_[slot];
  gpu_timing_query_counts_[slot] = 0;
  if (count < 2 || !gpu_timing_pools_[slot]) {
    return;
  }
  const ui::vulkan::VulkanDevice* vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  std::vector<uint64_t> ts(count);
  if (dfn.vkGetQueryPoolResults(vulkan_device->device(),
                                gpu_timing_pools_[slot], 0, count,
                                sizeof(uint64_t) * count, ts.data(),
                                sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  // Layout: [start, (pass begin, pass end)..., end].
  const double to_ms = gpu_timing_period_ns_ / 1e6;
  GpuTiming& timing = gpu_timing();
  double total_ms = double(ts[count - 1] - ts[0]) * to_ms;
  double in_pass_ms = 0.0;
  const std::vector<uint32_t>& pass_draws = gpu_timing_pass_draws_[slot];
  const std::vector<VkPipeline>& dispatch_pipelines =
      gpu_timing_dispatch_pipelines_[slot];
  size_t dispatch_index = 0;
  const std::vector<TimedPass>& timed_passes = gpu_timing_passes_[slot];
  size_t timed_pass_index = 0;
  for (size_t i = 0; i < pass_draws.size() && 2 * i + 2 < count; ++i) {
    double pass_ms = double(ts[2 * i + 2] - ts[2 * i + 1]) * to_ms;
    uint32_t draws = pass_draws[i];
    if (draws == kGpuTimingSegmentDispatch) {
      timing.dispatch_ms += pass_ms;
      ++timing.dispatches;
      if (dispatch_index < dispatch_pipelines.size()) {
        DispatchTiming& pipeline_timing =
            dispatch_timing()[dispatch_pipelines[dispatch_index++]];
        pipeline_timing.ms += pass_ms;
        ++pipeline_timing.count;
      }
      continue;
    }
    if (draws == kGpuTimingSegmentCopy) {
      timing.copy_ms += pass_ms;
      ++timing.copies;
      continue;
    }
    in_pass_ms += pass_ms;
    if (timed_pass_index < timed_passes.size()) {
      const TimedPass& timed_pass = timed_passes[timed_pass_index++];
      auto name_it = render_pass_names().find(timed_pass.render_pass);
      DispatchTiming& pass_entry = pass_timing()[fmt::format(
          "{}@{}x{}",
          name_it != render_pass_names().end()
              ? name_it->second
              : fmt::format("{:X}", uint64_t(timed_pass.render_pass)),
          timed_pass.width, timed_pass.height)];
      pass_entry.ms += pass_ms;
      ++pass_entry.count;
      pass_entry.draws += draws;
    }
    size_t bucket = draws == 0     ? 0
                    : draws == 1   ? 1
                    : draws <= 5   ? 2
                    : draws <= 20  ? 3
                    : draws <= 100 ? 4
                                   : 5;
    timing.bucket_ms[bucket] += pass_ms;
    ++timing.bucket_passes[bucket];
  }
  timing.total_ms += total_ms;
  timing.glue_ms += total_ms - in_pass_ms;
  ++timing.submissions;
}

DeferredCommandBuffer::DeferredCommandBuffer(
    const VulkanCommandProcessor& command_processor, size_t initial_size)
    : command_processor_(command_processor) {
  command_stream_.reserve(initial_size / sizeof(uintmax_t));
}

void DeferredCommandBuffer::Reset() {
  assert_false(hoisting_);
  assert_true(hoisted_copies_.empty());
  hoisted_copies_.clear();
  command_stream_.clear();
  render_pass_args_offset_ = SIZE_MAX;
  ++render_pass_serial_;
  current_scissor_valid_ = false;
}

void DeferredCommandBuffer::HoistBufferCopyBeforeRenderPass(
    VkBuffer src_buffer, VkBuffer dst_buffer, const VkBufferCopy& region,
    VkPipelineStageFlags before_stage_mask, VkAccessFlags before_access_mask,
    VkPipelineStageFlags after_stage_mask, VkAccessFlags after_access_mask) {
  assert_true(can_hoist_before_render_pass());
  assert_true(hoisted_copies_.empty() ||
              hoisted_copies_dst_buffer_ == dst_buffer);
  hoisted_copies_dst_buffer_ = dst_buffer;
  hoisted_copies_.push_back({src_buffer, region});
  hoisted_before_stage_mask_ |= before_stage_mask;
  hoisted_before_access_mask_ |= before_access_mask;
  hoisted_after_stage_mask_ |= after_stage_mask;
  hoisted_after_access_mask_ |= after_access_mask;
  ++work_stats().hoisted_uploads;
}

void DeferredCommandBuffer::FlushHoistedBufferCopies() {
  if (hoisted_copies_.empty()) {
    return;
  }
  assert_true(render_pass_args_offset_ != SIZE_MAX);
  // Record into a separate stream, then insert it before the pass.
  hoisting_ = true;
  hoist_saved_stream_.swap(command_stream_);
  command_stream_.clear();

  VkBufferMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = hoisted_copies_dst_buffer_;
  barrier.offset = 0;
  barrier.size = VK_WHOLE_SIZE;
  barrier.srcAccessMask = hoisted_before_access_mask_;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  CmdVkPipelineBarrier(hoisted_before_stage_mask_,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                       &barrier, 0, nullptr);
  // One copy command per source buffer, keeping the order of the copies to
  // the same destination.
  size_t first = 0;
  while (first < hoisted_copies_.size()) {
    VkBuffer src_buffer = hoisted_copies_[first].src_buffer;
    hoisted_copy_regions_.clear();
    size_t next_first = hoisted_copies_.size();
    for (size_t i = first; i < hoisted_copies_.size(); ++i) {
      if (hoisted_copies_[i].src_buffer == VK_NULL_HANDLE) {
        continue;
      }
      if (hoisted_copies_[i].src_buffer == src_buffer) {
        hoisted_copy_regions_.push_back(hoisted_copies_[i].region);
        hoisted_copies_[i].src_buffer = VK_NULL_HANDLE;
      } else if (next_first == hoisted_copies_.size()) {
        next_first = i;
      }
    }
    CmdVkCopyBuffer(src_buffer, hoisted_copies_dst_buffer_,
                    uint32_t(hoisted_copy_regions_.size()),
                    hoisted_copy_regions_.data());
    first = next_first;
  }
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = hoisted_after_access_mask_;
  CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                       hoisted_after_stage_mask_, 0, 0, nullptr, 1, &barrier,
                       0, nullptr);

  hoisting_ = false;
  command_stream_.swap(hoist_saved_stream_);
  size_t insert_at = render_pass_args_offset_ - kCommandHeaderSizeElements;
  command_stream_.insert(command_stream_.begin() + insert_at,
                         hoist_saved_stream_.begin(),
                         hoist_saved_stream_.end());
  render_pass_args_offset_ += hoist_saved_stream_.size();
  hoist_saved_stream_.clear();

  hoisted_copies_.clear();
  hoisted_copies_dst_buffer_ = VK_NULL_HANDLE;
  hoisted_before_stage_mask_ = 0;
  hoisted_before_access_mask_ = 0;
  hoisted_after_stage_mask_ = 0;
  hoisted_after_access_mask_ = 0;
}

void DeferredCommandBuffer::ShrinkRenderPassArea() {
  size_t args_offset = render_pass_args_offset_;
  render_pass_args_offset_ = SIZE_MAX;
  if (args_offset == SIZE_MAX || !cvars::vulkan_shrink_render_area ||
      render_pass_unbounded_ || !render_pass_has_bounds_) {
    return;
  }
  VkRect2D& area = reinterpret_cast<ArgsVkBeginRenderPass*>(
                       command_stream_.data() + args_offset)
                       ->render_area;
  int32_t x0 = std::max(area.offset.x, render_pass_bounds_[0]);
  int32_t y0 = std::max(area.offset.y, render_pass_bounds_[1]);
  int32_t x1 = std::min(area.offset.x + int32_t(area.extent.width),
                        render_pass_bounds_[2]);
  int32_t y1 = std::min(area.offset.y + int32_t(area.extent.height),
                        render_pass_bounds_[3]);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  area.offset.x = x0;
  area.offset.y = y0;
  area.extent.width = uint32_t(x1 - x0);
  area.extent.height = uint32_t(y1 - y0);
}

void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const ui::vulkan::VulkanDevice::Functions& dfn =
      command_processor_.GetVulkanDevice()->functions();
  const uintmax_t* stream = command_stream_.data();
  size_t stream_remaining = command_stream_.size();
  WorkStats& work_stats = DeferredCommandBuffer::work_stats();

  VkQueryPool timing_pool = VK_NULL_HANDLE;
  uint32_t timing_slot = 0;
  uint32_t timing_pass_draws = 0;
  if (cvars::vulkan_gpu_timing) {
    timing_slot = gpu_timing_ring_index_;
    gpu_timing_ring_index_ = (timing_slot + 1) % kGpuTimingRing;
    // The slot was last used kGpuTimingRing submissions ago.
    CollectGpuTiming(timing_slot);
    const ui::vulkan::VulkanDevice* vulkan_device =
        command_processor_.GetVulkanDevice();
    if (!gpu_timing_pools_[timing_slot]) {
      VkQueryPoolCreateInfo pool_info = {};
      pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      pool_info.queryCount = kGpuTimingMaxQueries;
      dfn.vkCreateQueryPool(vulkan_device->device(), &pool_info, nullptr,
                            &gpu_timing_pools_[timing_slot]);
      if (gpu_timing_period_ns_ == 0.0) {
        VkPhysicalDeviceProperties properties;
        vulkan_device->vulkan_instance()
            ->functions()
            .vkGetPhysicalDeviceProperties(vulkan_device->physical_device(),
                                           &properties);
        gpu_timing_period_ns_ = properties.limits.timestampPeriod;
      }
    }
    timing_pool = gpu_timing_pools_[timing_slot];
    if (timing_pool) {
      dfn.vkCmdResetQueryPool(command_buffer, timing_pool, 0,
                              kGpuTimingMaxQueries);
      dfn.vkCmdWriteTimestamp(command_buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              timing_pool, 0);
      gpu_timing_query_counts_[timing_slot] = 1;
      gpu_timing_pass_draws_[timing_slot].clear();
      gpu_timing_dispatch_pipelines_[timing_slot].clear();
      gpu_timing_passes_[timing_slot].clear();
    }
  }
  // A pass begin only gets a timestamp if room for its end and the final
  // timestamp remains, so the pairs never misalign.
  auto write_pass_begin_timestamp = [&]() {
    uint32_t& query = gpu_timing_query_counts_[timing_slot];
    if (timing_pool && query + 2 < kGpuTimingMaxQueries) {
      dfn.vkCmdWriteTimestamp(command_buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              timing_pool, query++);
      return true;
    }
    return false;
  };
  bool timing_in_pass = false;
  // Outside render passes, dispatches and copies are timed individually.
  uint32_t timing_segment = 0;
  bool timing_in_segment = false;
  VkPipeline timing_compute_pipeline = VK_NULL_HANDLE;

  while (stream_remaining) {
    const CommandHeader& header =
        *reinterpret_cast<const CommandHeader*>(stream);
    stream += kCommandHeaderSizeElements;
    stream_remaining -= kCommandHeaderSizeElements;

    switch (header.command) {
      case Command::kVkBeginRenderPass: {
        const VkRect2D& area =
            reinterpret_cast<const ArgsVkBeginRenderPass*>(stream)
                ->render_area;
        timing_in_pass = write_pass_begin_timestamp();
        timing_pass_draws = 0;
        if (timing_in_pass) {
          const auto& pass_args =
              *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
          gpu_timing_passes_[timing_slot].push_back(
              {pass_args.render_pass, area.extent.width, area.extent.height});
        }
        ++work_stats.render_passes;
        work_stats.render_pass_pixels +=
            uint64_t(area.extent.width) * area.extent.height;
      } break;
      case Command::kVkBeginRendering:
        ++work_stats.render_passes;
        break;
      case Command::kVkDraw:
      case Command::kVkDrawIndexed:
        ++work_stats.draws;
        ++timing_pass_draws;
        break;
      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        if (args.pipeline_bind_point == VK_PIPELINE_BIND_POINT_COMPUTE) {
          timing_compute_pipeline = args.pipeline;
        }
      } break;
      case Command::kVkDispatch:
        ++work_stats.dispatches;
        if (!timing_in_pass) {
          timing_in_segment = write_pass_begin_timestamp();
          timing_segment = kGpuTimingSegmentDispatch;
          if (timing_in_segment) {
            gpu_timing_dispatch_pipelines_[timing_slot].push_back(
                timing_compute_pipeline);
          }
        }
        break;
      case Command::kVkPipelineBarrier:
        ++work_stats.barriers;
        break;
      case Command::kVkCopyBuffer:
      case Command::kVkCopyBufferToImage:
      case Command::kVkCopyImageToBuffer:
      case Command::kVkBlitImage:
        ++work_stats.copies;
        if (!timing_in_pass) {
          timing_in_segment = write_pass_begin_timestamp();
          timing_segment = kGpuTimingSegmentCopy;
        }
        break;
      case Command::kVkClearAttachments:
      case Command::kVkClearColorImage:
        ++work_stats.clears;
        break;
      default:
        break;
    }

    switch (header.command) {
      case Command::kVkBeginRenderPass: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRenderPass*>(stream);
        size_t offset_bytes = sizeof(ArgsVkBeginRenderPass);
        VkRenderPassBeginInfo render_pass_begin_info;
        render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        render_pass_begin_info.pNext = nullptr;
        render_pass_begin_info.renderPass = args.render_pass;
        render_pass_begin_info.framebuffer = args.framebuffer;
        render_pass_begin_info.renderArea = args.render_area;
        render_pass_begin_info.clearValueCount = args.clear_value_count;
        if (render_pass_begin_info.clearValueCount) {
          offset_bytes = xe::align(offset_bytes, alignof(VkClearValue));
          render_pass_begin_info.pClearValues =
              reinterpret_cast<const VkClearValue*>(
                  reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes +=
              sizeof(VkClearValue) * render_pass_begin_info.clearValueCount;
        } else {
          render_pass_begin_info.pClearValues = nullptr;
        }
        dfn.vkCmdBeginRenderPass(command_buffer, &render_pass_begin_info,
                                 args.contents);
      } break;

      case Command::kVkBindDescriptorSets: {
        auto& args = *reinterpret_cast<const ArgsVkBindDescriptorSets*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkBindDescriptorSets),
                                        alignof(VkDescriptorSet));
        const VkDescriptorSet* descriptor_sets =
            reinterpret_cast<const VkDescriptorSet*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes += sizeof(VkDescriptorSet) * args.descriptor_set_count;
        const uint32_t* dynamic_offsets = nullptr;
        if (args.dynamic_offset_count) {
          offset_bytes = xe::align(offset_bytes, alignof(uint32_t));
          dynamic_offsets = reinterpret_cast<const uint32_t*>(
              reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
          offset_bytes += sizeof(uint32_t) * args.dynamic_offset_count;
        }
        dfn.vkCmdBindDescriptorSets(command_buffer, args.pipeline_bind_point,
                                    args.layout, args.first_set,
                                    args.descriptor_set_count, descriptor_sets,
                                    args.dynamic_offset_count, dynamic_offsets);
      } break;

      case Command::kVkBindIndexBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkBindIndexBuffer*>(stream);
        dfn.vkCmdBindIndexBuffer(command_buffer, args.buffer, args.offset,
                                 args.index_type);
      } break;

      case Command::kVkBindPipeline: {
        auto& args = *reinterpret_cast<const ArgsVkBindPipeline*>(stream);
        dfn.vkCmdBindPipeline(command_buffer, args.pipeline_bind_point,
                              args.pipeline);
      } break;

      case Command::kVkBindVertexBuffers: {
        auto& args = *reinterpret_cast<const ArgsVkBindVertexBuffers*>(stream);
        size_t offset_bytes =
            xe::align(sizeof(ArgsVkBindVertexBuffers), alignof(VkBuffer));
        const VkBuffer* buffers = reinterpret_cast<const VkBuffer*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes =
            xe::align(offset_bytes + sizeof(VkBuffer) * args.binding_count,
                      alignof(VkDeviceSize));
        const VkDeviceSize* offsets = reinterpret_cast<const VkDeviceSize*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdBindVertexBuffers(command_buffer, args.first_binding,
                                   args.binding_count, buffers, offsets);
      } break;
      case Command::kVkBeginQuery: {
        auto& args = *reinterpret_cast<const ArgsVkBeginQuery*>(stream);
        dfn.vkCmdBeginQuery(command_buffer, args.query_pool, args.query,
                            args.flags);
      } break;
      case Command::kVkEndQuery: {
        auto& args = *reinterpret_cast<const ArgsVkEndQuery*>(stream);
        dfn.vkCmdEndQuery(command_buffer, args.query_pool, args.query);
      } break;
      case Command::kVkCopyQueryPoolResults: {
        auto& args =
            *reinterpret_cast<const ArgsVkCopyQueryPoolResults*>(stream);
        dfn.vkCmdCopyQueryPoolResults(
            command_buffer, args.query_pool, args.first_query, args.query_count,
            args.dst_buffer, args.dst_offset, args.stride, args.flags);
      } break;
      case Command::kVkResetQueryPool: {
        auto& args = *reinterpret_cast<const ArgsVkResetQueryPool*>(stream);
        dfn.vkCmdResetQueryPool(command_buffer, args.query_pool,
                                args.first_query, args.query_count);
      } break;

      case Command::kVkClearAttachments: {
        auto& args = *reinterpret_cast<const ArgsVkClearAttachments*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkClearAttachments),
                                        alignof(VkClearAttachment));
        const VkClearAttachment* attachments =
            reinterpret_cast<const VkClearAttachment*>(
                reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        offset_bytes = xe::align(
            offset_bytes + sizeof(VkClearAttachment) * args.attachment_count,
            alignof(VkClearRect));
        const VkClearRect* rects = reinterpret_cast<const VkClearRect*>(
            reinterpret_cast<const uint8_t*>(stream) + offset_bytes);
        dfn.vkCmdClearAttachments(command_buffer, args.attachment_count,
                                  attachments, args.rect_count, rects);
      } break;

      case Command::kVkClearColorImage: {
        auto& args = *reinterpret_cast<const ArgsVkClearColorImage*>(stream);
        dfn.vkCmdClearColorImage(
            command_buffer, args.image, args.image_layout, &args.color,
            args.range_count,
            reinterpret_cast<const VkImageSubresourceRange*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkClearColorImage),
                          alignof(VkImageSubresourceRange))));
      } break;

      case Command::kVkCopyBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBuffer*>(stream);
        dfn.vkCmdCopyBuffer(
            command_buffer, args.src_buffer, args.dst_buffer, args.region_count,
            reinterpret_cast<const VkBufferCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBuffer), alignof(VkBufferCopy))));
      } break;

      case Command::kVkCopyBufferToImage: {
        auto& args = *reinterpret_cast<const ArgsVkCopyBufferToImage*>(stream);
        dfn.vkCmdCopyBufferToImage(
            command_buffer, args.src_buffer, args.dst_image,
            args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyBufferToImage),
                          alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkCopyImageToBuffer: {
        auto& args = *reinterpret_cast<const ArgsVkCopyImageToBuffer*>(stream);
        dfn.vkCmdCopyImageToBuffer(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_buffer, args.region_count,
            reinterpret_cast<const VkBufferImageCopy*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkCopyImageToBuffer),
                          alignof(VkBufferImageCopy))));
      } break;

      case Command::kVkBlitImage: {
        auto& args = *reinterpret_cast<const ArgsVkBlitImage*>(stream);
        dfn.vkCmdBlitImage(
            command_buffer, args.src_image, args.src_image_layout,
            args.dst_image, args.dst_image_layout, args.region_count,
            reinterpret_cast<const VkImageBlit*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkBlitImage), alignof(VkImageBlit))),
            args.filter);
      } break;

      case Command::kVkDispatch: {
        auto& args = *reinterpret_cast<const ArgsVkDispatch*>(stream);
        dfn.vkCmdDispatch(command_buffer, args.group_count_x,
                          args.group_count_y, args.group_count_z);
      } break;

      case Command::kVkDraw: {
        auto& args = *reinterpret_cast<const ArgsVkDraw*>(stream);
        dfn.vkCmdDraw(command_buffer, args.vertex_count, args.instance_count,
                      args.first_vertex, args.first_instance);
      } break;

      case Command::kVkDrawIndexed: {
        auto& args = *reinterpret_cast<const ArgsVkDrawIndexed*>(stream);
        dfn.vkCmdDrawIndexed(command_buffer, args.index_count,
                             args.instance_count, args.first_index,
                             args.vertex_offset, args.first_instance);
      } break;

      case Command::kVkEndRenderPass:
        dfn.vkCmdEndRenderPass(command_buffer);
        break;

      case Command::kVkBeginRendering: {
        auto& args = *reinterpret_cast<const ArgsVkBeginRendering*>(stream);
        size_t offset_bytes = xe::align(sizeof(ArgsVkBeginRendering),
                                        alignof(VkRenderingAttachmentInfo));
        const VkRenderingAttachmentInfo* color_attachments =
            args.color_attachment_count
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        offset_bytes +=
            sizeof(VkRenderingAttachmentInfo) * args.color_attachment_count;
        const VkRenderingAttachmentInfo* depth_attachment =
            args.has_depth_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        if (args.has_depth_attachment) {
          offset_bytes += sizeof(VkRenderingAttachmentInfo);
        }
        const VkRenderingAttachmentInfo* stencil_attachment =
            args.has_stencil_attachment
                ? reinterpret_cast<const VkRenderingAttachmentInfo*>(
                      reinterpret_cast<const uint8_t*>(stream) + offset_bytes)
                : nullptr;
        VkRenderingInfo rendering_info = {};
        rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering_info.flags = args.flags;
        rendering_info.renderArea = args.render_area;
        rendering_info.layerCount = args.layer_count;
        rendering_info.viewMask = args.view_mask;
        rendering_info.colorAttachmentCount = args.color_attachment_count;
        rendering_info.pColorAttachments = color_attachments;
        rendering_info.pDepthAttachment = depth_attachment;
        rendering_info.pStencilAttachment = stencil_attachment;
        dfn.vkCmdBeginRendering(command_buffer, &rendering_info);
      } break;

      case Command::kVkEndRendering:
        dfn.vkCmdEndRendering(command_buffer);
        break;

      case Command::kVkPipelineBarrier: {
        auto& args = *reinterpret_cast<const ArgsVkPipelineBarrier*>(stream);
        size_t barrier_offset_bytes = sizeof(ArgsVkPipelineBarrier);
        const VkMemoryBarrier* memory_barriers = nullptr;
        if (args.memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkMemoryBarrier));
          memory_barriers = reinterpret_cast<const VkMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkMemoryBarrier) * args.memory_barrier_count;
        }
        const VkBufferMemoryBarrier* buffer_memory_barriers = nullptr;
        if (args.buffer_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkBufferMemoryBarrier));
          buffer_memory_barriers =
              reinterpret_cast<const VkBufferMemoryBarrier*>(
                  reinterpret_cast<const uint8_t*>(stream) +
                  barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkBufferMemoryBarrier) * args.buffer_memory_barrier_count;
        }
        const VkImageMemoryBarrier* image_memory_barriers = nullptr;
        if (args.image_memory_barrier_count) {
          barrier_offset_bytes =
              xe::align(barrier_offset_bytes, alignof(VkImageMemoryBarrier));
          image_memory_barriers = reinterpret_cast<const VkImageMemoryBarrier*>(
              reinterpret_cast<const uint8_t*>(stream) + barrier_offset_bytes);
          barrier_offset_bytes +=
              sizeof(VkImageMemoryBarrier) * args.image_memory_barrier_count;
        }
        dfn.vkCmdPipelineBarrier(
            command_buffer, args.src_stage_mask, args.dst_stage_mask,
            args.dependency_flags, args.memory_barrier_count, memory_barriers,
            args.buffer_memory_barrier_count, buffer_memory_barriers,
            args.image_memory_barrier_count, image_memory_barriers);
      } break;

      case Command::kVkPushConstants: {
        auto& args = *reinterpret_cast<const ArgsVkPushConstants*>(stream);
        dfn.vkCmdPushConstants(command_buffer, args.layout, args.stage_flags,
                               args.offset, args.size,
                               reinterpret_cast<const uint8_t*>(stream) +
                                   sizeof(ArgsVkPushConstants));
      } break;

      case Command::kVkSetBlendConstants: {
        auto& args = *reinterpret_cast<const ArgsVkSetBlendConstants*>(stream);
        dfn.vkCmdSetBlendConstants(command_buffer, args.blend_constants);
      } break;

      case Command::kVkSetDepthBias: {
        auto& args = *reinterpret_cast<const ArgsVkSetDepthBias*>(stream);
        dfn.vkCmdSetDepthBias(command_buffer, args.depth_bias_constant_factor,
                              args.depth_bias_clamp,
                              args.depth_bias_slope_factor);
      } break;

      case Command::kVkSetScissor: {
        auto& args = *reinterpret_cast<const ArgsVkSetScissor*>(stream);
        dfn.vkCmdSetScissor(
            command_buffer, args.first_scissor, args.scissor_count,
            reinterpret_cast<const VkRect2D*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetScissor), alignof(VkRect2D))));
      } break;

      case Command::kVkSetStencilCompareMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilCompareMask(command_buffer, args.face_mask,
                                       args.mask_reference);
      } break;

      case Command::kVkSetStencilReference: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilReference(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetStencilWriteMask: {
        auto& args =
            *reinterpret_cast<const ArgsSetStencilMaskReference*>(stream);
        dfn.vkCmdSetStencilWriteMask(command_buffer, args.face_mask,
                                     args.mask_reference);
      } break;

      case Command::kVkSetViewport: {
        auto& args = *reinterpret_cast<const ArgsVkSetViewport*>(stream);
        dfn.vkCmdSetViewport(
            command_buffer, args.first_viewport, args.viewport_count,
            reinterpret_cast<const VkViewport*>(
                reinterpret_cast<const uint8_t*>(stream) +
                xe::align(sizeof(ArgsVkSetViewport), alignof(VkViewport))));
      } break;

      case Command::kVkBeginDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdBeginDebugUtilsLabelEXT) {
          auto& args = *reinterpret_cast<const ArgsVkDebugUtilsLabel*>(stream);
          const char* label_name = reinterpret_cast<const char*>(
              reinterpret_cast<const uint8_t*>(stream) +
              sizeof(ArgsVkDebugUtilsLabel));
          VkDebugUtilsLabelEXT label_info = {};
          label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
          label_info.pLabelName = label_name;
          ifn.vkCmdBeginDebugUtilsLabelEXT(command_buffer, &label_info);
        }
      } break;

      case Command::kVkEndDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdEndDebugUtilsLabelEXT) {
          ifn.vkCmdEndDebugUtilsLabelEXT(command_buffer);
        }
      } break;

      case Command::kVkInsertDebugUtilsLabelEXT: {
        const ui::vulkan::VulkanInstance::Functions& ifn =
            command_processor_.GetVulkanDevice()
                ->vulkan_instance()
                ->functions();
        if (ifn.vkCmdInsertDebugUtilsLabelEXT) {
          auto& args = *reinterpret_cast<const ArgsVkDebugUtilsLabel*>(stream);
          const char* label_name = reinterpret_cast<const char*>(
              reinterpret_cast<const uint8_t*>(stream) +
              sizeof(ArgsVkDebugUtilsLabel));
          VkDebugUtilsLabelEXT label_info = {};
          label_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
          label_info.pLabelName = label_name;
          ifn.vkCmdInsertDebugUtilsLabelEXT(command_buffer, &label_info);
        }
      } break;

      default:
        assert_unhandled_case(header.command);
        break;
    }

    if (timing_in_segment) {
      uint32_t& query = gpu_timing_query_counts_[timing_slot];
      dfn.vkCmdWriteTimestamp(command_buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              timing_pool, query++);
      gpu_timing_pass_draws_[timing_slot].push_back(timing_segment);
      timing_in_segment = false;
    }
    if (timing_in_pass && header.command == Command::kVkEndRenderPass) {
      uint32_t& query = gpu_timing_query_counts_[timing_slot];
      dfn.vkCmdWriteTimestamp(command_buffer,
                              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                              timing_pool, query++);
      gpu_timing_pass_draws_[timing_slot].push_back(timing_pass_draws);
      timing_in_pass = false;
    }

    stream += header.arguments_size_elements;
    stream_remaining -= header.arguments_size_elements;
  }

  if (timing_pool) {
    uint32_t& query = gpu_timing_query_counts_[timing_slot];
    dfn.vkCmdWriteTimestamp(command_buffer,
                            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timing_pool,
                            query++);
  }
}

void DeferredCommandBuffer::CmdVkPipelineBarrier(
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkDependencyFlags dependency_flags, uint32_t memory_barrier_count,
    const VkMemoryBarrier* memory_barriers,
    uint32_t buffer_memory_barrier_count,
    const VkBufferMemoryBarrier* buffer_memory_barriers,
    uint32_t image_memory_barrier_count,
    const VkImageMemoryBarrier* image_memory_barriers) {
  size_t arguments_size = sizeof(ArgsVkPipelineBarrier);
  size_t memory_barriers_offset = 0;
  if (memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkMemoryBarrier));
    memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkMemoryBarrier) * memory_barrier_count;
  }
  size_t buffer_memory_barriers_offset = 0;
  if (buffer_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkBufferMemoryBarrier));
    buffer_memory_barriers_offset = arguments_size;
    arguments_size +=
        sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count;
  }
  size_t image_memory_barriers_offset = 0;
  if (image_memory_barrier_count) {
    arguments_size = xe::align(arguments_size, alignof(VkImageMemoryBarrier));
    image_memory_barriers_offset = arguments_size;
    arguments_size += sizeof(VkImageMemoryBarrier) * image_memory_barrier_count;
  }
  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
      WriteCommand(Command::kVkPipelineBarrier, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkPipelineBarrier*>(args_ptr);
  args.src_stage_mask = src_stage_mask;
  args.dst_stage_mask = dst_stage_mask;
  args.dependency_flags = dependency_flags;
  args.memory_barrier_count = memory_barrier_count;
  args.buffer_memory_barrier_count = buffer_memory_barrier_count;
  args.image_memory_barrier_count = image_memory_barrier_count;
  if (memory_barrier_count) {
    std::memcpy(args_ptr + memory_barriers_offset, memory_barriers,
                sizeof(VkMemoryBarrier) * memory_barrier_count);
  }
  if (buffer_memory_barrier_count) {
    std::memcpy(args_ptr + buffer_memory_barriers_offset,
                buffer_memory_barriers,
                sizeof(VkBufferMemoryBarrier) * buffer_memory_barrier_count);
  }
  if (image_memory_barrier_count) {
    std::memcpy(args_ptr + image_memory_barriers_offset, image_memory_barriers,
                sizeof(VkImageMemoryBarrier) * image_memory_barrier_count);
  }
}

void DeferredCommandBuffer::CmdVkBeginRendering(
    const VkRenderingInfo* rendering_info) {
  assert_null(rendering_info->pNext);

  size_t arguments_size = xe::align(sizeof(ArgsVkBeginRendering),
                                    alignof(VkRenderingAttachmentInfo));
  size_t color_attachments_offset = arguments_size;
  arguments_size +=
      sizeof(VkRenderingAttachmentInfo) * rendering_info->colorAttachmentCount;
  size_t depth_attachment_offset = arguments_size;
  if (rendering_info->pDepthAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }
  size_t stencil_attachment_offset = arguments_size;
  if (rendering_info->pStencilAttachment) {
    arguments_size += sizeof(VkRenderingAttachmentInfo);
  }

  uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
      WriteCommand(Command::kVkBeginRendering, arguments_size));
  auto& args = *reinterpret_cast<ArgsVkBeginRendering*>(args_ptr);
  args.flags = rendering_info->flags;
  args.render_area = rendering_info->renderArea;
  args.layer_count = rendering_info->layerCount;
  args.view_mask = rendering_info->viewMask;
  args.color_attachment_count = rendering_info->colorAttachmentCount;
  args.has_depth_attachment = rendering_info->pDepthAttachment != nullptr;
  args.has_stencil_attachment = rendering_info->pStencilAttachment != nullptr;

  if (rendering_info->colorAttachmentCount) {
    std::memcpy(args_ptr + color_attachments_offset,
                rendering_info->pColorAttachments,
                sizeof(VkRenderingAttachmentInfo) *
                    rendering_info->colorAttachmentCount);
  }
  if (rendering_info->pDepthAttachment) {
    std::memcpy(args_ptr + depth_attachment_offset,
                rendering_info->pDepthAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
  if (rendering_info->pStencilAttachment) {
    std::memcpy(args_ptr + stencil_attachment_offset,
                rendering_info->pStencilAttachment,
                sizeof(VkRenderingAttachmentInfo));
  }
}

void* DeferredCommandBuffer::WriteCommand(Command command,
                                          size_t arguments_size_bytes) {
  size_t arguments_size_elements =
      (arguments_size_bytes + sizeof(uintmax_t) - 1) / sizeof(uintmax_t);
  size_t offset = command_stream_.size();
  command_stream_.resize(offset + kCommandHeaderSizeElements +
                         arguments_size_elements);
  CommandHeader& header =
      *reinterpret_cast<CommandHeader*>(command_stream_.data() + offset);
  header.command = command;
  header.arguments_size_elements = uint32_t(arguments_size_elements);
  return command_stream_.data() + (offset + kCommandHeaderSizeElements);
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
