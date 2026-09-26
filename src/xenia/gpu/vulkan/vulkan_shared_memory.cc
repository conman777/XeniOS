/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/vulkan/vulkan_shared_memory.h"

#include <cstring>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/gpu/vulkan/deferred_command_buffer.h"
#include "xenia/gpu/vulkan/vulkan_command_processor.h"
#include "xenia/ui/vulkan/vulkan_util.h"

DECLARE_bool(gpu_allow_invalid_upload_range);

// Render pass splitting is expensive on mobile (tiled) GPUs.
#if XE_PLATFORM_ANDROID
static constexpr bool kVulkanTiledGpuDefault = true;
#else
static constexpr bool kVulkanTiledGpuDefault = false;
#endif

DEFINE_bool(vulkan_shared_memory_texel_buffer, false,
            "Read shared memory in shaders (vertex fetch and similar) through "
            "one R32_UINT uniform texel buffer covering all of it, instead of "
            "storage buffers. Needs maxTexelBufferElements of 128M.",
            "Vulkan");
DEFINE_bool(vulkan_hoist_shared_memory_uploads, kVulkanTiledGpuDefault,
            "Record CPU-to-GPU shared memory uploads needed by a draw before "
            "the open render pass instead of ending it, when no draw already "
            "in the pass read the uploaded range. Avoids splitting render "
            "passes, which is expensive on mobile GPUs.",
            "Vulkan");
DEFINE_bool(vulkan_shared_memory_skip_waw_barriers, kVulkanTiledGpuDefault,
            "With vulkan_shared_memory_hazard_barriers, don't order guest draws "
            "whose memexport streams overlap each other (unless something reads "
            "the data in between). Xenia sees each stream as its whole buffer, "
            "while draws usually export to different elements of it.",
            "Vulkan");
DEFINE_bool(vulkan_shared_memory_hazard_barriers, kVulkanTiledGpuDefault,
            "Between guest draws, insert shared memory barriers only when a "
            "draw reads or writes a range memexported since the last barrier, "
            "or memexports to a range read since then, rather than around "
            "every memexport draw. Barriers end render passes.",
            "Vulkan");

DEFINE_bool(vulkan_sparse_shared_memory, true,
            "Enable sparse binding for shared memory emulation. Disabling it "
            "increases video memory usage - a 512 MB buffer is created - but "
            "allows graphics debuggers that don't support sparse binding to "
            "work.",
            "Vulkan");

namespace xe {
namespace gpu {
namespace vulkan {

VulkanSharedMemory::VulkanSharedMemory(
    VulkanCommandProcessor& command_processor, Memory& memory,
    TraceWriter& trace_writer,
    VkPipelineStageFlags guest_shader_pipeline_stages)
    : SharedMemory(memory),
      command_processor_(command_processor),
      trace_writer_(trace_writer),
      guest_shader_pipeline_stages_(guest_shader_pipeline_stages) {}

VulkanSharedMemory::~VulkanSharedMemory() { Shutdown(true); }

bool VulkanSharedMemory::Initialize() {
  if (!InitializeCommon()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const VkBufferCreateFlags sparse_flags =
      VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
      VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;

  // Try to create a sparse buffer.
  VkBufferCreateInfo buffer_create_info;
  buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_create_info.pNext = nullptr;
  buffer_create_info.flags = sparse_flags;
  buffer_create_info.size = kBufferSize;
  buffer_create_info.usage =
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (cvars::vulkan_shared_memory_texel_buffer) {
    buffer_create_info.usage |= VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  }
  buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  buffer_create_info.queueFamilyIndexCount = 0;
  buffer_create_info.pQueueFamilyIndices = nullptr;
  if (cvars::vulkan_sparse_shared_memory &&
      vulkan_device->properties().sparseResidencyBuffer) {
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) ==
        VK_SUCCESS) {
      VkMemoryRequirements buffer_memory_requirements;
      dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                        &buffer_memory_requirements);
      if (xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                   vulkan_device->memory_types().device_local,
                               &buffer_memory_type_)) {
        uint32_t allocation_size_log2;
        xe::bit_scan_forward(
            std::max(uint64_t(buffer_memory_requirements.alignment),
                     uint64_t(1)),
            &allocation_size_log2);
        if (allocation_size_log2 < kBufferSizeLog2) {
          // Maximum of 1024 allocations in the worst case for all of the
          // buffer because of the overall 4096 allocation count limit on
          // Windows drivers.
          InitializeSparseHostGpuMemory(
              std::max(allocation_size_log2,
                       std::max(kHostGpuMemoryOptimalSparseAllocationLog2,
                                kBufferSizeLog2 - uint32_t(10))));
        } else {
          // Shouldn't happen on any real platform, but no point allocating the
          // buffer sparsely.
          dfn.vkDestroyBuffer(device, buffer_, nullptr);
          buffer_ = VK_NULL_HANDLE;
        }
      } else {
        XELOGE(
            "Shared memory: Failed to get a device-local Vulkan memory type "
            "for the sparse buffer");
        dfn.vkDestroyBuffer(device, buffer_, nullptr);
        buffer_ = VK_NULL_HANDLE;
      }
    } else {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan sparse buffer",
             kBufferSize >> 20);
    }
  }

  // Create a non-sparse buffer if there were issues with the sparse buffer.
  if (buffer_ == VK_NULL_HANDLE) {
    XELOGGPU(
        "Vulkan sparse binding is not used for shared memory emulation - video "
        "memory usage may increase significantly because a full {} MB buffer "
        "will be created",
        kBufferSize >> 20);
    buffer_create_info.flags &= ~sparse_flags;
    if (dfn.vkCreateBuffer(device, &buffer_create_info, nullptr, &buffer_) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to create the {} MB Vulkan buffer",
             kBufferSize >> 20);
      Shutdown();
      return false;
    }
    VkMemoryRequirements buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, buffer_,
                                      &buffer_memory_requirements);
    if (!xe::bit_scan_forward(buffer_memory_requirements.memoryTypeBits &
                                  vulkan_device->memory_types().device_local,
                              &buffer_memory_type_)) {
      XELOGE(
          "Shared memory: Failed to get a device-local Vulkan memory type for "
          "the buffer");
      Shutdown();
      return false;
    }
    VkMemoryAllocateInfo buffer_memory_allocate_info;
    VkMemoryAllocateInfo* buffer_memory_allocate_info_last =
        &buffer_memory_allocate_info;
    buffer_memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    buffer_memory_allocate_info.pNext = nullptr;
    buffer_memory_allocate_info.allocationSize =
        buffer_memory_requirements.size;
    buffer_memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
    VkMemoryDedicatedAllocateInfo buffer_memory_dedicated_allocate_info;
    if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
      buffer_memory_allocate_info_last->pNext =
          &buffer_memory_dedicated_allocate_info;
      buffer_memory_allocate_info_last =
          reinterpret_cast<VkMemoryAllocateInfo*>(
              &buffer_memory_dedicated_allocate_info);
      buffer_memory_dedicated_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
      buffer_memory_dedicated_allocate_info.pNext = nullptr;
      buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
      buffer_memory_dedicated_allocate_info.buffer = buffer_;
    }
    VkDeviceMemory buffer_memory;
    if (dfn.vkAllocateMemory(device, &buffer_memory_allocate_info, nullptr,
                             &buffer_memory) != VK_SUCCESS) {
      XELOGE(
          "Shared memory: Failed to allocate {} MB of memory for the Vulkan "
          "buffer",
          kBufferSize >> 20);
      Shutdown();
      return false;
    }
    buffer_memory_.push_back(buffer_memory);
    if (dfn.vkBindBufferMemory(device, buffer_, buffer_memory, 0) !=
        VK_SUCCESS) {
      XELOGE("Shared memory: Failed to bind memory to the Vulkan buffer");
      Shutdown();
      return false;
    }
  }

  // The first usage will likely be uploading.
  last_usage_ = Usage::kTransferDestination;
  last_written_range_ = std::make_pair<uint32_t, uint32_t>(0, 0);

  if (cvars::vulkan_shared_memory_texel_buffer) {
    VkBufferViewCreateInfo view_create_info = {};
    view_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
    view_create_info.buffer = buffer_;
    view_create_info.format = VK_FORMAT_R32_UINT;
    view_create_info.offset = 0;
    view_create_info.range = kBufferSize;
    if (dfn.vkCreateBufferView(device, &view_create_info, nullptr,
                               &texel_buffer_view_) != VK_SUCCESS) {
      XELOGW(
          "Shared memory: Failed to create the texel buffer view, using "
          "storage buffers for shader reads");
      texel_buffer_view_ = VK_NULL_HANDLE;
      OVERRIDE_bool(vulkan_shared_memory_texel_buffer, false);
    } else {
      XELOGI("Shared memory: shader reads use an R32_UINT texel buffer");
    }
  }

  upload_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      xe::align(ui::vulkan::VulkanUploadBufferPool::kDefaultPageSize,
                size_t(1) << page_size_log2()));

  return true;
}

void VulkanSharedMemory::Shutdown(bool from_destructor) {
  ResetTraceDownload();

  upload_buffer_pool_.reset();

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                         texel_buffer_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, buffer_);
  for (VkDeviceMemory memory : buffer_memory_) {
    dfn.vkFreeMemory(device, memory, nullptr);
  }
  buffer_memory_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void VulkanSharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void VulkanSharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void VulkanSharedMemory::EndSubmission() { upload_buffer_pool_->FlushWrites(); }

void VulkanSharedMemory::ResetBarrierHazardTracking() {
  reads_since_barrier_.clear();
  reads_since_barrier_overflow_ = false;
  unsynced_writes_.clear();
  unsynced_writes_overflow_ = false;
}

bool VulkanSharedMemory::OverlapsUnsyncedWrites(uint32_t start,
                                                uint32_t end) const {
  if (unsynced_writes_overflow_) {
    return true;
  }
  for (const std::pair<uint32_t, uint32_t>& write : unsynced_writes_) {
    if (write.first < end && start < write.second) {
      return true;
    }
  }
  return false;
}

void VulkanSharedMemory::AddUnsyncedWrites(
    std::pair<uint32_t, uint32_t> written_range,
    const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges) {
  if (!written_range.second) {
    return;
  }
  if (exact_written_ranges && !exact_written_ranges->empty()) {
    for (const std::pair<uint32_t, uint32_t>& range : *exact_written_ranges) {
      if (unsynced_writes_.size() >= kMaxPassReads) {
        unsynced_writes_overflow_ = true;
        return;
      }
      unsynced_writes_.emplace_back(range.first, range.first + range.second);
    }
    return;
  }
  if (unsynced_writes_.size() >= kMaxPassReads) {
    unsynced_writes_overflow_ = true;
    return;
  }
  unsynced_writes_.emplace_back(written_range.first,
                                written_range.first + written_range.second);
}

bool VulkanSharedMemory::DrawHasSharedMemoryHazard(
    std::pair<uint32_t, uint32_t> written_range,
    const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges)
    const {
  auto is_own_stream = [&](const std::pair<uint32_t, uint32_t>& read) {
    if (!written_range.second) {
      return false;
    }
    if (exact_written_ranges && !exact_written_ranges->empty()) {
      for (const std::pair<uint32_t, uint32_t>& range :
           *exact_written_ranges) {
        if (read.first == range.first &&
            read.second == range.first + range.second) {
          return true;
        }
      }
      return false;
    }
    return read.first >= written_range.first &&
           read.second <= written_range.first + written_range.second;
  };
  if (pending_pass_reads_overflow_) {
    return true;
  }
  const uint32_t write_start = written_range.first;
  const uint32_t write_end = written_range.first + written_range.second;
  // Read-after-write and write-after-write: anything this draw requested
  // (vertex and index buffers, its own memexport streams) against writes not
  // yet made visible.
  DeferredCommandBuffer::WorkStats& stats = DeferredCommandBuffer::work_stats();
  for (const std::pair<uint32_t, uint32_t>& read : pending_pass_reads_) {
    if (cvars::vulkan_shared_memory_skip_waw_barriers && is_own_stream(read)) {
      continue;
    }
    if (OverlapsUnsyncedWrites(read.first, read.second)) {
      // Own export stream overlapping earlier exports: write-after-write.
      ++(is_own_stream(read) ? stats.draw_use_war : stats.draw_use_raw);
      static uint32_t hazard_log_count = 0;
      if (hazard_log_count < 24) {
        ++hazard_log_count;
        std::string writes;
        for (const std::pair<uint32_t, uint32_t>& write : unsynced_writes_) {
          if (write.first < read.second && read.first < write.second) {
            writes += fmt::format(" [{:08X},{:08X})", write.first,
                                  write.second);
          }
        }
        XELOGI(
            "SharedMemHazard own={} read=[{:08X},{:08X}) unsynced={} "
            "overlapping:{}",
            is_own_stream(read), read.first, read.second,
            unsynced_writes_.size(), writes);
      }
      return true;
    }
  }
  if (!written_range.second) {
    return false;
  }
  // Write-after-read: earlier readers of the range this draw exports to. The
  // draw's own requests inside its export range are its streams.
  if (reads_since_barrier_overflow_) {
    ++stats.draw_use_war;
    return true;
  }
  for (const std::pair<uint32_t, uint32_t>& read : reads_since_barrier_) {
    if (read.first < write_end && write_start < read.second) {
      ++stats.draw_use_war;
      return true;
    }
  }
  for (const std::pair<uint32_t, uint32_t>& read : pending_pass_reads_) {
    if (!is_own_stream(read) && read.first < write_end &&
        write_start < read.second) {
      ++stats.draw_use_war;
      return true;
    }
  }
  return false;
}

void VulkanSharedMemory::Use(
    Usage usage, std::pair<uint32_t, uint32_t> written_range, bool from_draw,
    const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges) {
  UseImpl(usage, written_range, from_draw, exact_written_ranges);
  // A draw's memexport streams are requested like reads, but are writes -
  // tracked as unsynchronized writes, not as reads of later hazard checks.
  if (from_draw && written_range.second && exact_written_ranges &&
      !pending_pass_reads_overflow_) {
    pending_pass_reads_.erase(
        std::remove_if(
            pending_pass_reads_.begin(), pending_pass_reads_.end(),
            [&](const std::pair<uint32_t, uint32_t>& read) {
              for (const std::pair<uint32_t, uint32_t>& range :
                   *exact_written_ranges) {
                if (read.first == range.first &&
                    read.second == range.first + range.second) {
                  return true;
                }
              }
              return false;
            }),
        pending_pass_reads_.end());
  }
}

void VulkanSharedMemory::UseImpl(
    Usage usage, std::pair<uint32_t, uint32_t> written_range, bool from_draw,
    const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges) {
  written_range.first = std::min(written_range.first, kBufferSize);
  written_range.second =
      std::min(written_range.second, kBufferSize - written_range.first);
  assert_true(usage != Usage::kRead || !written_range.second);
  if (from_draw && cvars::vulkan_shared_memory_hazard_barriers &&
      cvars::vulkan_hoist_shared_memory_uploads &&
      (last_usage_ == Usage::kRead ||
       last_usage_ == Usage::kGuestDrawReadWrite) &&
      (usage == Usage::kRead || usage == Usage::kGuestDrawReadWrite) &&
      !DrawHasSharedMemoryHazard(written_range, exact_written_ranges)) {
    if (written_range.second) {
      // Stay in (or enter) the read-write state without a barrier; a later
      // barrier covers the union of the unsynchronized writes.
      last_usage_ = Usage::kGuestDrawReadWrite;
      AddUnsyncedWrites(written_range, exact_written_ranges);
      if (last_written_range_.second) {
        uint32_t start = std::min(last_written_range_.first,
                                  written_range.first);
        uint32_t end = std::max(
            last_written_range_.first + last_written_range_.second,
            written_range.first + written_range.second);
        last_written_range_ = std::make_pair(start, end - start);
      } else {
        last_written_range_ = written_range;
      }
    }
    ++DeferredCommandBuffer::work_stats().draw_use_skipped;
    return;
  }
  if (from_draw && !((last_usage_ == Usage::kRead ||
                       last_usage_ == Usage::kGuestDrawReadWrite) &&
                      (usage == Usage::kRead ||
                       usage == Usage::kGuestDrawReadWrite))) {
    ++DeferredCommandBuffer::work_stats().draw_use_ineligible;
  }
  if (last_usage_ != usage || last_written_range_.second) {
    VkPipelineStageFlags src_stage_mask, dst_stage_mask;
    VkAccessFlags src_access_mask, dst_access_mask;
    GetUsageMasks(last_usage_, src_stage_mask, src_access_mask);
    GetUsageMasks(usage, dst_stage_mask, dst_access_mask);
    VkDeviceSize offset, size;
    if (last_usage_ == usage) {
      // Committing the previous write, while not changing the access mask
      // (passing false as whether to skip the barrier if no masks are changed
      // for this reason).
      offset = VkDeviceSize(last_written_range_.first);
      size = VkDeviceSize(last_written_range_.second);
    } else {
      // Changing the stage and access mask - all preceding writes must be
      // available not only to the source stage, but to the destination as well.
      offset = 0;
      size = VK_WHOLE_SIZE;
      last_usage_ = usage;
    }
    command_processor_.PushBufferMemoryBarrier(
        buffer_, offset, size, src_stage_mask, dst_stage_mask, src_access_mask,
        dst_access_mask, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        false);
    ResetBarrierHazardTracking();
  }
  last_written_range_ = written_range;
  AddUnsyncedWrites(written_range, exact_written_ranges);
}

bool VulkanSharedMemory::InitializeTraceSubmitDownloads(bool* capture_success) {
  if (capture_success) {
    *capture_success = false;
  }
  ResetTraceDownload();
  if (!PrepareForTraceDownload()) {
    XELOGE("Shared memory: Failed to prepare GPU-written memory for capture");
    ResetTraceDownload();
    return false;
  }
  uint32_t download_page_count = trace_download_page_count();
  if (!download_page_count) {
    if (capture_success) {
      *capture_success = true;
    }
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          command_processor_.GetVulkanDevice(),
          download_page_count << page_size_log2(),
          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, trace_download_buffer_,
          trace_download_buffer_memory_)) {
    XELOGE(
        "Shared memory: Failed to create a {} KB GPU-written memory download "
        "buffer for frame tracing",
        download_page_count << page_size_log2() >> 10);
    ResetTraceDownload();
    return false;
  }

  Use(Usage::kRead);
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  command_processor_.InsertDebugMarker(
      "Trace Download: %u KB, %zu ranges",
      download_page_count << page_size_log2() >> 10,
      trace_download_ranges().size());

  size_t download_range_count = trace_download_ranges().size();
  VkBufferCopy* download_regions = command_buffer.CmdCopyBufferEmplace(
      buffer_, trace_download_buffer_, uint32_t(download_range_count));
  VkDeviceSize download_buffer_offset = 0;
  for (size_t i = 0; i < download_range_count; ++i) {
    VkBufferCopy& download_region = download_regions[i];
    const std::pair<uint32_t, uint32_t>& download_range =
        trace_download_ranges()[i];
    download_region.srcOffset = download_range.first;
    download_region.dstOffset = download_buffer_offset;
    download_region.size = download_range.second;
    download_buffer_offset += download_range.second;
  }

  command_processor_.PushBufferMemoryBarrier(
      trace_download_buffer_, 0, VK_WHOLE_SIZE, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT);

  if (capture_success) {
    *capture_success = true;
  }
  return true;
}

bool VulkanSharedMemory::InitializeTraceCompleteDownloads(
    bool copy_to_guest_memory) {
  if (!trace_download_buffer_memory_) {
    return true;
  }
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  void* download_mapping = nullptr;
  bool success = false;
  if (dfn.vkMapMemory(device, trace_download_buffer_memory_, 0, VK_WHOLE_SIZE,
                      0, &download_mapping) == VK_SUCCESS) {
    success = true;
    uint32_t download_buffer_offset = 0;
    for (const auto& download_range : trace_download_ranges()) {
      const uint8_t* source =
          static_cast<const uint8_t*>(download_mapping) + download_buffer_offset;
      if (copy_to_guest_memory) {
        std::memcpy(memory().TranslatePhysical(download_range.first), source,
                    download_range.second);
      } else {
        trace_writer_.WriteMemoryRead(download_range.first, download_range.second,
                                     source);
      }
      download_buffer_offset += download_range.second;
    }
    dfn.vkUnmapMemory(device, trace_download_buffer_memory_);
  } else {
    XELOGE(
        "Shared memory: Failed to map the GPU-written memory download buffer "
        "for frame tracing");
  }
  ResetTraceDownload();
  return success;
}

bool VulkanSharedMemory::AllocateSparseHostGpuMemoryRange(
    uint32_t offset_allocations, uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkMemoryAllocateInfo memory_allocate_info;
  memory_allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  memory_allocate_info.pNext = nullptr;
  memory_allocate_info.allocationSize =
      length_allocations << host_gpu_memory_sparse_granularity_log2();
  memory_allocate_info.memoryTypeIndex = buffer_memory_type_;
  VkDeviceMemory memory;
  if (dfn.vkAllocateMemory(device, &memory_allocate_info, nullptr, &memory) !=
      VK_SUCCESS) {
    XELOGE("Shared memory: Failed to allocate sparse buffer memory");
    return false;
  }
  buffer_memory_.push_back(memory);

  VkSparseMemoryBind bind;
  bind.resourceOffset = offset_allocations
                        << host_gpu_memory_sparse_granularity_log2();
  bind.size = memory_allocate_info.allocationSize;
  bind.memory = memory;
  bind.memoryOffset = 0;
  bind.flags = 0;
  VkPipelineStageFlags bind_wait_stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
  if (vulkan_device->properties().tessellationShader) {
    bind_wait_stage_mask |=
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
  }
  command_processor_.SparseBindBuffer(buffer_, 1, &bind, bind_wait_stage_mask);

  return true;
}

void VulkanSharedMemory::OnRangeRequested(uint32_t start, uint32_t length) {
  if (!cvars::vulkan_hoist_shared_memory_uploads) {
    return;
  }
  if (pending_pass_reads_.size() >= kMaxPassReads) {
    pending_pass_reads_overflow_ = true;
    return;
  }
  pending_pass_reads_.emplace_back(start, start + length);
}

void VulkanSharedMemory::CommitPendingPassReads() {
  if (!cvars::vulkan_hoist_shared_memory_uploads) {
    return;
  }
  uint64_t serial =
      command_processor_.deferred_command_buffer().render_pass_serial();
  if (serial != pass_reads_serial_) {
    pass_reads_serial_ = serial;
    pass_reads_.clear();
    pass_reads_overflow_ = false;
  }
  if (pending_pass_reads_overflow_ ||
      pass_reads_.size() + pending_pass_reads_.size() > kMaxPassReads) {
    pass_reads_overflow_ = true;
  } else {
    pass_reads_.insert(pass_reads_.end(), pending_pass_reads_.begin(),
                       pending_pass_reads_.end());
  }
  if (pending_pass_reads_overflow_ ||
      reads_since_barrier_.size() + pending_pass_reads_.size() >
          kMaxPassReads) {
    reads_since_barrier_overflow_ = true;
  } else {
    reads_since_barrier_.insert(reads_since_barrier_.end(),
                                pending_pass_reads_.begin(),
                                pending_pass_reads_.end());
  }
  pending_pass_reads_.clear();
  pending_pass_reads_overflow_ = false;
}

bool VulkanSharedMemory::UploadConflictsWithPassReads(uint32_t start,
                                                      uint32_t end) const {
  if (pass_reads_serial_ !=
      command_processor_.deferred_command_buffer().render_pass_serial()) {
    // No draw recorded in this render pass yet.
    return false;
  }
  if (pass_reads_overflow_) {
    return true;
  }
  for (const std::pair<uint32_t, uint32_t>& read : pass_reads_) {
    if (read.first < end && start < read.second) {
      return true;
    }
  }
  return false;
}

bool VulkanSharedMemory::UploadRanges(
    const std::pair<uint32_t, uint32_t>* upload_page_ranges,
    uint32_t num_upload_ranges) {
  if (!num_upload_ranges) {
    return true;
  }

  auto& range_front = upload_page_ranges[0];
  auto& range_back = upload_page_ranges[num_upload_ranges - 1];
  const uint32_t upload_span_start = range_front.first << page_size_log2();
  const uint32_t upload_span_end = (range_back.first + range_back.second)
                                   << page_size_log2();

  DeferredCommandBuffer& command_buffer =
      command_processor_.deferred_command_buffer();

  // Inside a render pass where the buffer is only being read, the upload can
  // go before the pass (with its own barriers) if no draw already in the pass
  // read the range - otherwise those draws would see the new data.
  const bool hoist = cvars::vulkan_hoist_shared_memory_uploads &&
                     command_buffer.can_hoist_before_render_pass() &&
                     ((last_usage_ == Usage::kRead &&
                       !last_written_range_.second) ||
                      (last_usage_ == Usage::kGuestDrawReadWrite &&
                       !OverlapsUnsyncedWrites(upload_span_start,
                                               upload_span_end))) &&
                     !UploadConflictsWithPassReads(upload_span_start,
                                                   upload_span_end);
  VkPipelineStageFlags read_stage_mask = 0;
  VkAccessFlags read_access_mask = 0;
  if (hoist) {
    GetUsageMasks(last_usage_, read_stage_mask, read_access_mask);
  } else {
    // upload_page_ranges are sorted, use them to determine the range for the
    // ordering barrier.
    Use(Usage::kTransferDestination,
        std::make_pair(upload_span_start,
                       upload_span_end - upload_span_start));
    // Submit barriers (may end render pass) before pushing debug marker so
    // EndRenderPass is not inside the SharedMem Upload marker.
    command_processor_.SubmitBarriers(true);

    // Calculate total upload size for debug marker.
    uint32_t total_upload_bytes = upload_span_end - upload_span_start;
    command_processor_.PushDebugMarker(
        "UploadRanges (SharedMem): 0x%08X-0x%08X (%u KB, %u ranges)",
        upload_span_start, upload_span_end, total_upload_bytes / 1024,
        num_upload_ranges);
  }
  uint64_t submission_current = command_processor_.GetCurrentSubmission();
  bool successful = true;
  upload_regions_.clear();
  VkBuffer upload_buffer_previous = VK_NULL_HANDLE;

  // for (auto upload_range : upload_page_ranges) {
  for (unsigned int i = 0; i < num_upload_ranges; ++i) {
    uint32_t upload_range_start = upload_page_ranges[i].first;
    uint32_t upload_range_length = upload_page_ranges[i].second;
    trace_writer_.WriteMemoryRead(upload_range_start << page_size_log2(),
                                  upload_range_length << page_size_log2());

    if (upload_range_length > 0 && !cvars::gpu_allow_invalid_upload_range) {
      const uint32_t range_start_addr = upload_range_start << page_size_log2();
      const uint32_t upload_range_last_page =
          upload_range_start + upload_range_length - 1;
      const uint32_t range_end_addr = upload_range_last_page
                                      << page_size_log2();

      const memory::PageAccess start_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_start_addr,
                                                       range_start_addr);
      const memory::PageAccess end_access =
          memory().GetPhysicalHeap()->QueryRangeAccess(range_end_addr,
                                                       range_end_addr);
      if (start_access == xe::memory::PageAccess::kNoAccess ||
          end_access == xe::memory::PageAccess::kNoAccess) {
        XELOGE(
            "Vulkan shared memory: Invalid upload range {:08X} length {:08X}",
            upload_range_start, upload_range_length);
        successful = false;
        break;
      }
    }

    while (upload_range_length) {
      VkBuffer upload_buffer;
      VkDeviceSize upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          submission_current, upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), upload_buffer, upload_buffer_offset,
          upload_buffer_size);
      if (upload_buffer_mapping == nullptr) {
        XELOGE("Shared memory: Failed to get a Vulkan upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(),
                     uint32_t(upload_buffer_size), false);
      // Guest memory may have deferred (resolve readback) data not copied in
      // yet.
      memory().ProvideDeferredPhysicalMemoryWrites(
          upload_range_start << page_size_log2(),
          uint32_t(upload_buffer_size));

      if (upload_buffer_size < (1ULL << 32) && upload_buffer_size > 8192) {
        memory::vastcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            static_cast<uint32_t>(upload_buffer_size));
        swcache::WriteFence();
      } else {
        std::memcpy(
            upload_buffer_mapping,
            memory().TranslatePhysical(upload_range_start << page_size_log2()),
            upload_buffer_size);
      }
      if (hoist) {
        VkBufferCopy hoisted_region;
        hoisted_region.srcOffset = upload_buffer_offset;
        hoisted_region.dstOffset =
            VkDeviceSize(upload_range_start << page_size_log2());
        hoisted_region.size = upload_buffer_size;
        command_buffer.HoistBufferCopyBeforeRenderPass(
            upload_buffer, buffer_, hoisted_region, read_stage_mask,
            read_access_mask, read_stage_mask, read_access_mask);
      } else {
        if (upload_buffer_previous != upload_buffer &&
            !upload_regions_.empty()) {
          assert_true(upload_buffer_previous != VK_NULL_HANDLE);
          command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                         uint32_t(upload_regions_.size()),
                                         upload_regions_.data());
          upload_regions_.clear();
        }
        upload_buffer_previous = upload_buffer;
        VkBufferCopy& upload_region = upload_regions_.emplace_back();
        upload_region.srcOffset = upload_buffer_offset;
        upload_region.dstOffset =
            VkDeviceSize(upload_range_start << page_size_log2());
        upload_region.size = upload_buffer_size;
      }
      uint32_t upload_buffer_pages =
          uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (!upload_regions_.empty()) {
    assert_true(upload_buffer_previous != VK_NULL_HANDLE);
    command_buffer.CmdVkCopyBuffer(upload_buffer_previous, buffer_,
                                   uint32_t(upload_regions_.size()),
                                   upload_regions_.data());
    upload_regions_.clear();
  }
  // Hoisted copies are recorded before the render pass when it ends.
  if (!hoist) {
    command_processor_.PopDebugMarker();
  }
  return successful;
}

void VulkanSharedMemory::GetUsageMasks(Usage usage,
                                       VkPipelineStageFlags& stage_mask,
                                       VkAccessFlags& access_mask) const {
  switch (usage) {
    case Usage::kComputeWrite:
      stage_mask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
      access_mask = VK_ACCESS_SHADER_WRITE_BIT;
      return;
    case Usage::kTransferDestination:
      stage_mask = VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask = VK_ACCESS_TRANSFER_WRITE_BIT;
      return;
    default:
      break;
  }
  stage_mask =
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | guest_shader_pipeline_stages_;
  access_mask = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
  switch (usage) {
    case Usage::kRead:
      stage_mask |=
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
      access_mask |= VK_ACCESS_TRANSFER_READ_BIT;
      break;
    case Usage::kGuestDrawReadWrite:
      access_mask |= VK_ACCESS_SHADER_WRITE_BIT;
      break;
    default:
      assert_unhandled_case(usage);
  }
}

void VulkanSharedMemory::ResetTraceDownload() {
  const ui::vulkan::VulkanDevice* const vulkan_device =
      command_processor_.GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device,
                                         trace_download_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         trace_download_buffer_memory_);
  ReleaseTraceDownloadRanges();
}

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe
