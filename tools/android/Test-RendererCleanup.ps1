param(
  [string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path,
  [string]$Compiler = 'g++',
  [string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'xenios-renderer-cleanup-tests')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$render = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/xenia/gpu/vulkan/vulkan_render_target_cache.cc'))
$start = $render.IndexOf('xenos::ColorRenderTargetFormat dump_pack_format = key.GetColorFormat();')
$end = $render.IndexOf('if (GetAndroidHaloExperiment().repack_16_16_to_8888', $start)
if ($start -lt 0 -or $end -lt $start) { throw 'Cannot locate actual dump-format selection' }
$selection = $render.Substring($start, $end - $start)
# The extracted prefix ends inside the RGBA8 conversion branch. Close it and
# its Android guard, then return the selected format for independent checks.
$selection += "`n    }`n#endif`n return dump_pack_format;"
$test = @'
#include <cstdio>
#include <cstdint>
#define XE_PLATFORM_ANDROID 1
namespace xenos {
enum class ColorRenderTargetFormat {
  k_8_8_8_8, k_8_8_8_8_GAMMA, k_2_10_10_10,
  k_2_10_10_10_AS_10_10_10_10, k_2_10_10_10_FLOAT,
  k_2_10_10_10_FLOAT_AS_16_16_16_16, k_16_16_16_16
};
}
using Format = xenos::ColorRenderTargetFormat;
struct Experiment { unsigned repack_mode = 0; } experiment;
const Experiment& GetAndroidHaloExperiment() { return experiment; }
struct Key {
  Format format;
  bool source_to_1x, android_force_8888_repack;
  Format GetColorFormat() const { return format; }
};
Format SelectFormat(Key key, bool format_is_64bpp) {
@@SELECTION@@
}
int main() {
  const Format native_formats[] = {
    Format::k_2_10_10_10, Format::k_2_10_10_10_AS_10_10_10_10,
    Format::k_2_10_10_10_FLOAT,
    Format::k_2_10_10_10_FLOAT_AS_16_16_16_16
  };
  for (Format format : native_formats) {
    if (SelectFormat({format, true, false}, false) != format) {
      std::puts("FAIL: native dump mode silently converts a 10-bit format to RGBA8");
      return 1;
    }
    if (SelectFormat({format, true, true}, false) != Format::k_8_8_8_8 ||
        SelectFormat({format, false, true}, false) != format) {
      std::puts("FAIL: explicit conversion must require sample-collapse mode");
      return 1;
    }
  }
  if (SelectFormat({Format::k_16_16_16_16, true, true}, true) !=
      Format::k_16_16_16_16) return 1;
  // A cached key must produce the same shader even if live configuration changes.
  experiment.repack_mode = 1;
  if (SelectFormat({Format::k_2_10_10_10_FLOAT, true, false}, false) !=
      Format::k_2_10_10_10_FLOAT) {
    std::puts("FAIL: live configuration changes shader behavior outside its cache key");
    return 1;
  }
  std::puts("PASS: native formats remain native; explicit RGBA8 conversion is bounded; dump key determines format independently of live configuration");
}
'@
$test = $test.Replace('@@SELECTION@@', $selection)
$cpp = Join-Path $OutputDirectory 'dump-format-test.cc'
$exe = Join-Path $OutputDirectory 'dump-format-test.exe'
[IO.File]::WriteAllText($cpp, $test)
& $Compiler -std=c++17 -Wall -Wextra $cpp -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Dump-format test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Dump-format regression failed' }

function Read-Function([string]$Text, [string]$Name) {
  $start = $Text.IndexOf($Name)
  if ($start -lt 0) { throw "Missing source function: $Name" }
  $open = $Text.IndexOf('{', $start)
  $depth = 1
  $end = $open + 1
  while ($depth -gt 0 -and $end -lt $Text.Length) {
    if ($Text[$end] -eq '{') { ++$depth }
    if ($Text[$end] -eq '}') { --$depth }
    ++$end
  }
  if ($depth -ne 0) { throw "Unterminated source function: $Name" }
  return $Text.Substring($start, $end - $start)
}
$processor = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/xenia/gpu/vulkan/vulkan_command_processor.cc'))
$baseProcessor = [IO.File]::ReadAllText((Join-Path $SourceRoot 'src/xenia/gpu/command_processor.cc'))
$baseRestore = Read-Function $baseProcessor 'bool CommandProcessor::Restore('
if ($baseRestore -notmatch 'return RestoreSaveStateEdramSnapshot\(') {
  throw 'Live restore still discards the checked EDRAM restore result'
}
$restore = Read-Function $processor 'bool VulkanCommandProcessor::RestoreSaveStateEdramSnapshot('
# Replace only the clock source so a timeout test takes milliseconds, not 5 s.
$restore = $restore.Replace('std::chrono::steady_clock::now()', 'TestNow()')
$release = Read-Function $processor 'auto destroy_download = [&]()'
$release += '; destroy_download();'
$capToken = $render.IndexOf('!device_properties.fragmentStoresAndAtomics')
$capStart = $render.LastIndexOf('    if (', $capToken) + '    if ('.Length
$capEnd = $render.IndexOf(') {', $capToken)
if ($capToken -lt 0 -or $capEnd -lt $capStart) { throw 'Missing FSI capability check' }
$capabilityCheck = $render.Substring($capStart, $capEnd - $capStart)
$test = @'
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>
#include <utility>
#define XELOGE(...) ((void)0)
using VkDevice = int;
using VkBuffer = int;
using VkDeviceMemory = int;
constexpr int VK_NULL_HANDLE = 0, VK_BUFFER_USAGE_TRANSFER_SRC_BIT = 1;
constexpr int VK_WHOLE_SIZE = -1, VK_SUCCESS = 0;
namespace xenos { constexpr unsigned kEdramSizeBytes = 16; }
enum class Fault { None, Allocate, Map, Begin, Submit, Timeout, Lost };
Fault fault;
uint64_t completed;
int freed_buffer, freed_memory;
std::array<uint8_t, xenos::kEdramSizeBytes> mapping;
auto TestNow() {
  static auto time = std::chrono::steady_clock::time_point{};
  time += std::chrono::seconds(6);
  return time;
}
namespace ui::vulkan {
struct VulkanDevice {
  struct Functions {
    int vkMapMemory(int, int, int, int, int, void** data) const {
      *data = mapping.data(); return fault == Fault::Map ? -1 : VK_SUCCESS;
    }
    void vkUnmapMemory(int, int) const {}
    void vkDestroyBuffer(int, int, void*) const { ++freed_buffer; }
    void vkFreeMemory(int, int, void*) const { ++freed_memory; }
  } fn;
  const Functions& functions() const { return fn; }
  int device() const { return 1; }
  bool IsLost() const { return fault == Fault::Lost; }
};
namespace util {
enum class MemoryPurpose { kUpload };
bool CreateDedicatedAllocationBuffer(VulkanDevice*, unsigned, int,
                                    MemoryPurpose, int& buffer, int& memory) {
  if (fault == Fault::Allocate) return false;
  buffer = 1; memory = 2; return true;
}
}
}
struct RenderTargets {
  bool scaled = false;
  bool IsDrawResolutionScaled() const { return scaled; }
  void ClearCache(const char*) {}
  void SaveStateSubmitEdramUpload(int) {}
};
struct Timeline {
  uint64_t UpdateAndGetCompletedSubmission() {
    if (fault != Fault::Timeout && fault != Fault::Lost) completed = 7;
    return completed;
  }
};
struct VulkanCommandProcessor {
  bool device_lost_ = false;
  RenderTargets targets;
  RenderTargets* render_target_cache_ = &targets;
  ui::vulkan::VulkanDevice device;
  Timeline completion_timeline_;
  std::deque<std::pair<uint64_t, int>> destroy_buffers_, destroy_memory_;
  auto* GetVulkanDevice() { return &device; }
  bool BeginSubmission(bool) { return fault != Fault::Begin; }
  bool EndSubmission(bool) { return fault != Fault::Submit; }
  uint64_t GetCurrentSubmission() const { return 7; }
  uint64_t GetCompletedSubmission() const { return completed; }
  void CheckSubmissionCompletionAndDeviceLoss(uint64_t) {
    if (!destroy_buffers_.empty() && destroy_buffers_.front().first <= completed) {
      ++freed_buffer; destroy_buffers_.pop_front();
      ++freed_memory; destroy_memory_.pop_front();
    }
  }
  bool RestoreSaveStateEdramSnapshot(const void* snapshot);
  void ReleaseCaptureBuffer(uint64_t target) {
    const auto& dfn = device.functions();
    const VkDevice device = 1;
    const VkBuffer download_buffer = 1;
    const VkDeviceMemory download_memory = 2;
    @@RELEASE@@
  }
};
@@RESTORE@@
struct Properties {
  bool fragmentShaderSampleInterlock = false, fragmentShaderPixelInterlock = false;
  bool fragmentStoresAndAtomics = true, sampleRateShading = true;
  bool standardSampleLocations = true;
  unsigned maxPerStageDescriptorStorageBuffers = 8;
};
bool MustFallBack(Properties device_properties, unsigned shared_memory_binding_count) {
  [[maybe_unused]] const bool force_fsi_experiment = true;
  return @@CAPABILITY@@;
}
int main() {
  std::array<uint8_t, xenos::kEdramSizeBytes> snapshot;
  snapshot.fill(42);
  for (Fault mode : {Fault::None, Fault::Allocate, Fault::Map, Fault::Begin,
                     Fault::Submit, Fault::Timeout, Fault::Lost}) {
    fault = mode; completed = 0; freed_buffer = freed_memory = 0;
    VulkanCommandProcessor cp;
    assert(cp.RestoreSaveStateEdramSnapshot(snapshot.data()) == (mode == Fault::None));
    if (mode == Fault::None) {
      assert(mapping == snapshot && freed_buffer == 1 && freed_memory == 1);
    } else if (mode == Fault::Submit || mode == Fault::Timeout || mode == Fault::Lost) {
      assert(freed_buffer == 0 && freed_memory == 0);
      assert(cp.destroy_buffers_.size() == 1 && cp.destroy_memory_.size() == 1);
      completed = 7;
      cp.CheckSubmissionCompletionAndDeviceLoss(0);
      assert(freed_buffer == 1 && freed_memory == 1);
    } else if (mode == Fault::Map || mode == Fault::Begin) {
      assert(freed_buffer == 1 && freed_memory == 1);
    }
  }
  for (bool already_complete : {false, true}) {
    completed = already_complete ? 7 : 0; freed_buffer = freed_memory = 0;
    VulkanCommandProcessor cp;
    cp.ReleaseCaptureBuffer(7);
    assert(freed_buffer == int(already_complete));
    assert(cp.destroy_buffers_.empty() == already_complete);
  }
  fault = Fault::None;
  VulkanCommandProcessor cp;
  assert(!cp.RestoreSaveStateEdramSnapshot(nullptr));
  cp.targets.scaled = true;
  assert(!cp.RestoreSaveStateEdramSnapshot(snapshot.data()));
  Properties caps;
  assert(MustFallBack(caps, 2));
  caps.fragmentShaderPixelInterlock = true;
  assert(!MustFallBack(caps, 2));
  caps.fragmentShaderPixelInterlock = false; caps.fragmentShaderSampleInterlock = true;
  assert(!MustFallBack(caps, 2));
  assert(MustFallBack(caps, 8));
  caps.fragmentStoresAndAtomics = false;
  assert(MustFallBack(caps, 2));
  std::puts("PASS: restore failures propagate; pending upload/readback allocations survive timeouts; completed allocations retire; FSI cannot bypass device capabilities");
}
'@
$test = $test.Replace('@@RESTORE@@', $restore).Replace('@@RELEASE@@', $release).Replace('@@CAPABILITY@@', $capabilityCheck)
$cpp = Join-Path $OutputDirectory 'restore-lifecycle-test.cc'
$exe = Join-Path $OutputDirectory 'restore-lifecycle-test.exe'
[IO.File]::WriteAllText($cpp, $test)
& $Compiler -std=c++17 -Wall -Wextra $cpp -o $exe
if ($LASTEXITCODE -ne 0) { throw 'Restore lifecycle test compilation failed' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'Restore lifecycle regression failed' }
