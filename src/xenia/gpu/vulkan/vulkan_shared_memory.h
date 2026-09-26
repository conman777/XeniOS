/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
#define XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/gpu/shared_memory.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/memory.h"
#include "xenia/ui/vulkan/vulkan_upload_buffer_pool.h"

namespace xe {
namespace gpu {
namespace vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, Memory& memory,
                     TraceWriter& trace_writer,
                     VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;
  void ClearUploadBufferCache() { upload_buffer_pool_->ClearCache(); }
  size_t upload_buffer_memory_usage() const {
    return upload_buffer_pool_->GetMemoryUsage();
  }

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage {
    // Index buffer, vfetch, compute read, transfer source.
    kRead,
    // Index buffer, vfetch, memexport.
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  // Inserts a pipeline barrier for the target usage, also ensuring consecutive
  // read-write accesses are ordered with each other. from_draw: called for a
  // guest draw whose shared memory reads were all requested with RequestRange
  // since the previous draw, which allows skipping barriers between draws
  // that don't touch each other's memexported ranges
  // (vulkan_shared_memory_hazard_barriers).
  // exact_written_ranges: (start, length) of each memexport stream within
  // written_range, for precise hazard tracking.
  void Use(Usage usage, std::pair<uint32_t, uint32_t> written_range = {},
           bool from_draw = false,
           const std::vector<std::pair<uint32_t, uint32_t>>*
               exact_written_ranges = nullptr);

  VkBuffer buffer() const { return buffer_; }
  // R32_UINT view of the whole buffer for shader reads through the texture
  // path (vulkan_shared_memory_texel_buffer), or VK_NULL_HANDLE.
  VkBufferView texel_buffer_view() const { return texel_buffer_view_; }
  Memory& guest_memory_for_diagnostics() const { return memory(); }

  // Returns true if any downloads were submitted. capture_success, if supplied,
  // distinguishes an empty GPU-owned range set from an allocation failure.
  bool InitializeTraceSubmitDownloads(bool* capture_success = nullptr);
  // Save-state callers must keep all guest producers paused until completion.
  bool InitializeTraceCompleteDownloads(bool copy_to_guest_memory = false);

  // Attributes the ranges requested since the previous draw to the draw just
  // recorded in the current render pass (for hoisting uploads before it).
  void CommitPendingPassReads();

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::pair<uint32_t, uint32_t>* upload_page_ranges,
                    uint32_t num_ranges) override;
  void OnRangeRequested(uint32_t start, uint32_t length) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;

  VulkanCommandProcessor& command_processor_;
  TraceWriter& trace_writer_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkBufferView texel_buffer_view_ = VK_NULL_HANDLE;
  uint32_t buffer_memory_type_;
  // Single for non-sparse, every allocation so far for sparse.
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<uint32_t, uint32_t> last_written_range_;

  // Byte ranges [start, end) read by draws in the current render pass, and
  // requested since the last draw. Beyond the limit, the pass is treated as
  // reading everything.
  static constexpr size_t kMaxPassReads = 1024;
  bool UploadConflictsWithPassReads(uint32_t start, uint32_t end) const;

  // Ranges read and written since the last barrier on the buffer, for
  // hazard-based barriers between guest draws.
  bool DrawHasSharedMemoryHazard(
      std::pair<uint32_t, uint32_t> written_range,
      const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges)
      const;
  void UseImpl(Usage usage, std::pair<uint32_t, uint32_t> written_range,
               bool from_draw,
               const std::vector<std::pair<uint32_t, uint32_t>>*
                   exact_written_ranges);
  void AddUnsyncedWrites(
      std::pair<uint32_t, uint32_t> written_range,
      const std::vector<std::pair<uint32_t, uint32_t>>* exact_written_ranges);
  bool OverlapsUnsyncedWrites(uint32_t start, uint32_t end) const;
  void ResetBarrierHazardTracking();
  std::vector<std::pair<uint32_t, uint32_t>> reads_since_barrier_;
  bool reads_since_barrier_overflow_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> unsynced_writes_;
  bool unsynced_writes_overflow_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> pending_pass_reads_;
  bool pending_pass_reads_overflow_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> pass_reads_;
  bool pass_reads_overflow_ = false;
  uint64_t pass_reads_serial_ = UINT64_MAX;

  std::unique_ptr<ui::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;

  // Created temporarily, only for downloading.
  VkBuffer trace_download_buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory trace_download_buffer_memory_ = VK_NULL_HANDLE;
  void ResetTraceDownload();
};

}  // namespace vulkan
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_VULKAN_VULKAN_SHARED_MEMORY_H_
