param(
  [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
  [string]$Compiler = 'g++',
  [string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'xenios-draw-pass-transfer-tests')
)
$ErrorActionPreference = 'Stop'
$source = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/xenia/gpu/vulkan/vulkan_render_target_cache.cc'))
$start = $source.IndexOf('bool VulkanRenderTargetCache::CanQueueDrawPassTransfers(')
$end = $source.IndexOf('bool VulkanRenderTargetCache::PreflightPendingDrawPassTransfers(', $start)
if ($start -lt 0 -or $end -lt $start) { throw 'Cannot locate transfer eligibility function' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
# Execute the actual eligibility function with small GPU-object substitutes.
# This tests the scheduling decision, not Vulkan execution or colour accuracy.
$test = @'
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>
#define XE_PLATFORM_ANDROID 0
namespace cvars { bool vulkan_transfer_in_draw_pass = false; }
using VkFormat = int;
namespace xenos {
constexpr uint32_t kMaxColorRenderTargets = 4;
enum class ColorRenderTargetFormat { k_8_8_8_8, k_8_8_8_8_GAMMA,
                                    k_2_10_10_10_FLOAT, k_16_16_FLOAT };
enum class MsaaSamples { k1X, k2X, k4X };
}
using Format = xenos::ColorRenderTargetFormat;
struct RenderTargetKey {
  bool is_depth = false;
  unsigned base_tiles = 0, resource_format = 0, pitch = 29;
  xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
  Format format = Format::k_8_8_8_8;
  Format GetColorFormat() const { return format; }
  unsigned GetPitchTiles() const { return pitch; }
};
struct RenderTarget { RenderTargetKey key_; RenderTargetKey key() const { return key_; } };
struct VulkanRenderTarget : RenderTarget {
  unsigned draw_view = 1, transfer_view = 1, depth_view = 1;
  unsigned view_color_transfer() const { return transfer_view; }
  unsigned view_depth_color() const { return draw_view; }
  unsigned view_depth_stencil() const { return depth_view; }
};
struct Transfer { RenderTarget* source; RenderTarget* host_depth_source = nullptr; };
struct TransferRectanglePlan {};
struct VulkanRenderTargetCache {
  std::array<std::vector<Transfer>, 5> transfers;
  bool have_rectangles = true;
  const std::vector<Transfer>* last_update_transfers() const { return transfers.data(); }
  VkFormat GetColorVulkanFormat(Format f) const { return int(f) + 1; }
  VkFormat GetColorOwnershipTransferVulkanFormat(Format f, bool* integer) const {
    *integer = f == Format::k_16_16_FLOAT;
    return int(f) + (*integer ? 2 : 1);
  }
  bool BuildTransferRectanglePlans(RenderTargetKey, const std::vector<Transfer>&,
                                    std::vector<TransferRectanglePlan>&) const {
    return have_rectangles;
  }
  bool CanQueueDrawPassTransfers(uint32_t, RenderTarget* const*,
                                 const std::vector<Transfer>&) const;
};
@@FUNCTION@@
int main() {
  VulkanRenderTargetCache cache;
  VulkanRenderTarget dest, source, other;
  std::array<RenderTarget*, 5> active = {nullptr, &dest, nullptr, nullptr, nullptr};
  cache.transfers[1] = {{&source}};
  unsigned checks = 0;
  auto check = [&](bool expected, const char* name) {
    ++checks;
    bool actual = cache.CanQueueDrawPassTransfers(1, active.data(), cache.transfers[1]);
    if (actual != expected) { std::printf("FAIL: %s\n", name); return false; }
    return true;
  };
  if (!check(true, "legacy RGBA8 is eligible")) return 1;
  dest.key_.format = Format::k_2_10_10_10_FLOAT;
  if (!check(false, "legacy mode keeps 7e3 standalone")) return 1;
  cvars::vulkan_transfer_in_draw_pass = true;
  if (!check(true, "matching 7e3 views can join the draw pass")) return 1;
  dest.key_.msaa_samples = xenos::MsaaSamples::k4X;
  if (!check(true, "compatible multisample colour can join")) return 1;
  dest.key_.format = Format::k_16_16_FLOAT;
  if (!check(false, "integer transfer view cannot join float draw attachment")) return 1;
  dest.key_.format = Format::k_8_8_8_8;
  dest.transfer_view = 2;
  if (!check(false, "different attachment views retain fallback")) return 1;
  dest.transfer_view = 1;
  dest.key_.is_depth = true;
  if (!check(false, "depth destination retains fallback")) return 1;
  dest.key_.is_depth = false;
  active[2] = &source;
  if (!check(false, "source cannot be another active attachment")) return 1;
  active[2] = nullptr;
  cache.transfers[1][0].source = &dest;
  if (!check(false, "self transfer retains fallback")) return 1;
  cache.transfers[1][0].source = nullptr;
  if (!check(false, "missing source retains fallback")) return 1;
  cache.transfers[1][0].source = &source;
  cache.transfers[1][0].host_depth_source = &other;
  if (!check(false, "host depth reconstruction retains fallback")) return 1;
  cache.transfers[1][0].host_depth_source = nullptr;
  source.key_.is_depth = true; source.depth_view = 0;
  if (!check(false, "depth source requires a sampled view")) return 1;
  source.key_.is_depth = false;
  cache.have_rectangles = false;
  if (!check(false, "missing rectangles retain fallback")) return 1;
  cache.have_rectangles = true;
  cache.transfers[2] = {{&dest}};
  if (!check(false, "standalone transfer must see the newly transferred destination")) return 1;
  cache.transfers[2] = {{&other, &dest}};
  if (!check(false, "standalone host depth reader must retain original ordering")) return 1;
  cache.transfers[2] = {{&other}};
  if (!check(true, "unrelated standalone transfers do not prevent batching")) return 1;
  if (cache.CanQueueDrawPassTransfers(0, active.data(), cache.transfers[1]) ||
      cache.CanQueueDrawPassTransfers(5, active.data(), cache.transfers[1]) ||
      cache.CanQueueDrawPassTransfers(1, nullptr, cache.transfers[1])) return 1;
  std::printf("PASS: %u transfer compatibility/ordering checks and invalid-index guards\n", checks);
}
'@
$cpp = Join-Path $OutputDirectory 'draw-pass-transfer-test.cc'
$exe = Join-Path $OutputDirectory 'draw-pass-transfer-test.exe'
[IO.File]::WriteAllText($cpp, $test.Replace('@@FUNCTION@@', $source.Substring($start, $end - $start)))
& $Compiler -std=c++17 -Wall -Wextra $cpp -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Transfer test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Draw-pass transfer eligibility regression failed' }

function Read-Prefix([string]$Text, [string]$Name, [string]$EndToken) {
  $start = $Text.IndexOf($Name)
  $open = $Text.IndexOf('{', $start)
  $end = $Text.IndexOf($EndToken, $open)
  if ($start -lt 0 -or $open -lt 0 -or $end -lt $open) { throw "Missing boundary: $Name" }
  return $Text.Substring($open + 1, $end - $open - 1)
}
$processor = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/xenia/gpu/vulkan/vulkan_command_processor.cc'))
$download = Read-Prefix $source 'bool VulkanRenderTargetCache::SaveStateSubmitEdramDownload(' '  if (GetPath()'
$resolve = Read-Prefix $source 'bool VulkanRenderTargetCache::Resolve(' '  bool draw_resolution_scaled'
$submit = Read-Prefix $processor 'bool VulkanCommandProcessor::EndSubmission(' '  ui::vulkan::VulkanDevice'
$shutdown = Read-Prefix $source 'void VulkanRenderTargetCache::Shutdown(' '  const ui::vulkan::VulkanDevice'
# Execute actual reader/submission prefixes and queue draining, with later GPU
# work represented by a content read. A skipped draw must not leave stale data.
$clearStart = $source.IndexOf('void VulkanRenderTargetCache::ClearPendingDrawPassTransfers(')
$clearEnd = $source.IndexOf('bool VulkanRenderTargetCache::BuildTransferRectanglePlans(', $clearStart)
$flushStart = $source.IndexOf('bool VulkanRenderTargetCache::FlushPendingDrawPassTransfers(')
$flushEnd = $source.IndexOf('VkRenderPass VulkanRenderTargetCache::GetHostRenderTargetsRenderPass(', $flushStart)
if ($clearStart -lt 0 -or $clearEnd -lt $clearStart -or $flushStart -lt 0 -or $flushEnd -lt $flushStart) {
  throw 'Cannot locate queue draining functions'
}
$test = @'
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
namespace xenos { constexpr unsigned kMaxColorRenderTargets = 4; }
constexpr unsigned VK_NULL_HANDLE = 0;
struct VulkanRenderTargetCache {
  std::array<std::vector<int>, 5> pending_draw_pass_transfers_;
  std::array<void*, 5> pending_draw_pass_render_targets_ = {};
  unsigned pending_draw_pass_transfer_mask_ = 0, encoded = 0, content = 0, saved = 0;
  bool scaled = false;
  bool HasPendingDrawPassTransfers() { return pending_draw_pass_transfer_mask_ != 0; }
  bool IsDrawResolutionScaled() { return scaled; }
  void Queue() {
    pending_draw_pass_transfers_[1] = {1};
    pending_draw_pass_render_targets_[1] = this;
    pending_draw_pass_transfer_mask_ = 2;
  }
  void PerformTransfersAndResolveClears(unsigned, void* const*, const std::vector<int>* transfers) {
    assert(transfers[1].size() == 1); ++encoded; ++content;
  }
  void ClearPendingDrawPassTransfers();
  bool FlushPendingDrawPassTransfers();
  bool Download(unsigned destination) { @@DOWNLOAD@@ saved = content; return true; }
  bool Resolve(uint32_t& written_address_out, uint32_t& written_length_out) {
    @@RESOLVE@@ saved = content; return true;
  }
  void Shutdown() { @@SHUTDOWN@@ }
};
@@CLEAR@@
@@FLUSH@@
struct VulkanCommandProcessor {
  VulkanRenderTargetCache* render_target_cache_;
  bool submission_open_ = true;
  bool EndSubmission(bool is_swap) { (void)is_swap; @@SUBMIT@@ return true; }
};
int main() {
  VulkanRenderTargetCache cache;
  VulkanCommandProcessor processor{&cache};
  assert(cache.FlushPendingDrawPassTransfers() && !cache.encoded);
  cache.Queue();
  assert(cache.Download(1) && cache.saved == 1 && !cache.HasPendingDrawPassTransfers());
  assert(cache.pending_draw_pass_transfers_[1].empty() && !cache.pending_draw_pass_render_targets_[1]);
  cache.Queue(); uint32_t address = 7, length = 9;
  assert(cache.Resolve(address, length) && cache.saved == 2 && !address && !length);
  cache.Queue();
  assert(processor.EndSubmission(false) && cache.content == 3 && !cache.HasPendingDrawPassTransfers());
  assert(processor.EndSubmission(true) && cache.content == 3);
  cache.Queue(); processor.submission_open_ = false;
  assert(processor.EndSubmission(false) && cache.HasPendingDrawPassTransfers() && cache.content == 3);
  assert(!cache.Download(VK_NULL_HANDLE) && cache.HasPendingDrawPassTransfers());
  cache.scaled = true;
  assert(!cache.Download(1) && cache.HasPendingDrawPassTransfers());
  cache.Shutdown();
  assert(!cache.HasPendingDrawPassTransfers() && cache.content == 3);
  assert(cache.pending_draw_pass_transfers_[1].empty() && !cache.pending_draw_pass_render_targets_[1]);
  std::puts("PASS: skipped-draw transfers complete before resolve, save and submission; shutdown discards references");
}
'@
$test = $test.Replace('@@DOWNLOAD@@', $download).Replace('@@RESOLVE@@', $resolve).Replace('@@SUBMIT@@', $submit).Replace('@@SHUTDOWN@@', $shutdown)
$test = $test.Replace('@@CLEAR@@', $source.Substring($clearStart, $clearEnd - $clearStart)).Replace('@@FLUSH@@', $source.Substring($flushStart, $flushEnd - $flushStart))
$cpp = Join-Path $OutputDirectory 'draw-pass-boundary-test.cc'
$exe = Join-Path $OutputDirectory 'draw-pass-boundary-test.exe'
[IO.File]::WriteAllText($cpp, $test)
& $Compiler -std=c++17 -Wall -Wextra $cpp -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Transfer boundary test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Draw-pass transfer boundary regression failed' }
