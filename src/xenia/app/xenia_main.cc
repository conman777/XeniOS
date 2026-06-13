/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#if !XE_PLATFORM_ANDROID
#include "xenia/app/discord/discord_presence.h"
#endif
#include "xenia/app/emulator_window.h"
#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/debug/gdb/gdbstub.h"
#include "xenia/debug/ui/debug_window.h"
#include "xenia/emulator.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_listener.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/devices/host_path_device.h"

// Available audio systems:
#include "xenia/apu/nop/nop_audio_system.h"
#if XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
#include "xenia/apu/alsa/alsa_audio_system.h"
#endif  // XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
#if !XE_PLATFORM_ANDROID
#include "xenia/apu/sdl/sdl_audio_system.h"
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#endif  // XE_PLATFORM_WIN32

// Available graphics systems:
#include "xenia/gpu/null/null_graphics_system.h"
#if !XE_PLATFORM_APPLE
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#if XE_PLATFORM_ANDROID
#include "xenia/gpu/vulkan/android_halo_experiment.h"
#endif
#endif  // !XE_PLATFORM_APPLE
#if XE_PLATFORM_WIN32
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif  // XE_PLATFORM_WIN32
#if XE_PLATFORM_APPLE
#include "xenia/gpu/metal/metal_graphics_system.h"
#endif  // XE_PLATFORM_APPLE

// Available input drivers:
#include "xenia/hid/nop/nop_hid.h"
#if !XE_PLATFORM_ANDROID
#include "xenia/hid/sdl/sdl_hid.h"
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
#include "xenia/hid/winkey/winkey_hid.h"
#include "xenia/hid/xinput/xinput_hid.h"
#endif  // XE_PLATFORM_WIN32

#if XE_PLATFORM_WIN32
#define APU_OPTIONS "[xaudio2, sdl, nop]"
#define GPU_OPTIONS "[d3d12, vulkan, null]"
#define HID_OPTIONS "[sdl, winkey, xinput, nop]"
DEFINE_string(apu, "xaudio2", "Audio system. Use: " APU_OPTIONS, "APU");
DEFINE_string(gpu, "d3d12", "Graphics system. Use: " GPU_OPTIONS, "GPU");
DEFINE_string(hid, "sdl", "Input system. Use: " HID_OPTIONS, "HID");
#elif XE_PLATFORM_ANDROID
#define APU_OPTIONS "[nop]"
#define GPU_OPTIONS "[vulkan, null]"
#define HID_OPTIONS "[nop]"
DEFINE_string(apu, "nop", "Audio system. Use: " APU_OPTIONS, "APU");
DEFINE_string(gpu, "vulkan", "Graphics system. Use: " GPU_OPTIONS, "GPU");
DEFINE_string(hid, "nop", "Input system. Use: " HID_OPTIONS, "HID");
#elif XE_PLATFORM_LINUX
#define APU_OPTIONS "[alsa, sdl, nop]"
#define GPU_OPTIONS "[vulkan, null]"
#define HID_OPTIONS "[sdl, nop]"
DEFINE_string(apu, "alsa", "Audio system. Use: " APU_OPTIONS, "APU");
DEFINE_string(gpu, "vulkan", "Graphics system. Use: " GPU_OPTIONS, "GPU");
DEFINE_string(hid, "sdl", "Input system. Use: " HID_OPTIONS, "HID");
#else
#define APU_OPTIONS "[sdl, nop]"
#define HID_OPTIONS "[sdl, nop]"
DEFINE_string(apu, "sdl", "Audio system. Use: " APU_OPTIONS, "APU");
#if XE_PLATFORM_APPLE
DEFINE_string(gpu, "metal", "Graphics system. Use: [metal, null]", "GPU");
#else
DEFINE_string(gpu, "vulkan", "Graphics system. Use: [vulkan, null]", "GPU");
#endif
DEFINE_string(hid, "sdl", "Input system. Use: " HID_OPTIONS, "HID");
#endif

DEFINE_path(
    storage_root, "",
    "Root path for persistent internal data storage (config, etc.), or empty "
    "to use the path preferred for the OS, such as the documents folder, or "
    "the emulator executable directory if portable.txt is present in it.",
    "Storage");
DEFINE_path(
    content_root, "",
    "Root path for guest content storage (saves, etc.), or empty to use the "
    "content folder under the storage root.",
    "Storage");
DEFINE_path(
    cache_root, "",
    "Root path for files used to speed up certain parts of the emulator or the "
    "game. These files may be persistent, but they can be deleted without "
    "major side effects such as progress loss. If empty, the cache folder "
    "under the storage root, or, if available, the cache directory preferred "
    "for the OS, will be used.",
    "Storage");

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");

DEFINE_bool(mount_cache, true, "Enable cache mount", "Storage");
UPDATE_from_bool(mount_cache, 2024, 8, 31, 20, false);

DECLARE_bool(force_mount_devkit);

DECLARE_path(target);  // Defined in windowed_app_main_qt.cc
DEFINE_transient_bool(portable, false,
                      "Specifies if Xenia should run in portable mode.",
                      "General");

DECLARE_uint32(window_size_ui_x);
DECLARE_uint32(window_size_ui_y);

DEFINE_CVar(window_size_game_x, 0,
            "Game window width in pixels (0 = use internal resolution). "
            "Command-line only.",
            "Display", true, uint32_t);
DEFINE_CVar(window_size_game_y, 0,
            "Game window height in pixels (0 = use internal resolution). "
            "Command-line only.",
            "Display", true, uint32_t);

DECLARE_bool(debug);
DEFINE_int32(
    gdbport, 0,
    "Port for GDBStub debugger to listen on, requires --debug (0 = disable)",
    "General");

#if XE_PLATFORM_ANDROID
DEFINE_bool(discord, false, "Enable Discord rich presence", "General");
#else
DEFINE_bool(discord, true, "Enable Discord rich presence", "General");
#endif

DECLARE_bool(widescreen);

DECLARE_uint32(launch_flags);
DECLARE_string(launch_data);
#if XE_PLATFORM_ANDROID
DECLARE_bool(async_shader_compilation);
DECLARE_int32(vulkan_pipeline_creation_threads);
DECLARE_string(xma_decoder);
DECLARE_uint64(framerate_limit);
DECLARE_string(render_target_path);
DECLARE_string(render_target_path_vulkan);
DECLARE_string(readback_resolve);
DECLARE_bool(readback_memexport);
DECLARE_bool(readback_memexport_fast);
DECLARE_bool(guest_display_refresh_cap);
DECLARE_bool(halo_android_compat_presentable_color_shadow);
DECLARE_bool(halo_android_compat_skip_depth_to_color_alias);
DECLARE_bool(halo_android_compat_direct_presentable_resolve);
DECLARE_bool(halo_android_compat_linear_to_tiled_frontbuffer);
DECLARE_bool(halo_android_gpu_frame_dumps);
DECLARE_bool(mrt_edram_used_range_clamp_to_min);
DECLARE_bool(native_2x_msaa);
DECLARE_bool(vulkan_dynamic_rendering);
DECLARE_bool(vulkan_sparse_shared_memory);
DECLARE_bool(tiled_shared_memory);
DECLARE_string(postprocess_antialiasing);
DECLARE_string(postprocess_scaling_and_sharpening);
DECLARE_bool(postprocess_dither);
#endif

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64 == 1
DEFINE_bool(enable_rdrand_ntdll_patch, false,
            "Hot-patches ntdll at the start of the process to not use rdrand "
            "as part of the RNG for heap randomization. Can reduce CPU usage "
            "significantly, but is untested on all Windows versions.",
            "Win32");

// begin ntdll rdrand patch
#include <psapi.h>

static void write_process_memory(HANDLE process, uintptr_t offset,
                                 unsigned size, const unsigned char* bvals) {
  if (!WriteProcessMemory(process, (void*)offset, bvals, size, nullptr)) {
    DWORD error = GetLastError();
    XELOGE(
        "RDRAND patch: Failed to write to process memory at 0x{:X} (error: {})",
        offset, error);
  }
}

static constexpr unsigned char pattern_cmp_processorfeature_28_[] = {
    0x80, 0x3C, 0x25, 0x90,
    0x02, 0xFE, 0x7F, 0x00};  // cmp     byte ptr ds:7FFE0290h, 0
static constexpr unsigned char pattern_replacement[] = {
    0x48, 0x39, 0xe4,             // cmp rsp, rsp = always Z
    0x0F, 0x1F, 0x44, 0x00, 0x00  // 5byte nop
};

static void do_ntdll_rdrand_patch() {
  HMODULE ntdll_handle = GetModuleHandleA("ntdll.dll");
  if (!ntdll_handle) {
    XELOGE("RDRAND patch: Failed to get ntdll.dll handle");
    return;
  }

  MODULEINFO modinfo;
  if (!GetModuleInformation(GetCurrentProcess(), ntdll_handle, &modinfo,
                            sizeof(MODULEINFO))) {
    XELOGE("RDRAND patch: Failed to get ntdll.dll module information");
    return;
  }

  std::vector<uintptr_t> possible_places{};
  unsigned char* strt = (unsigned char*)modinfo.lpBaseOfDll;

  for (unsigned i = 0; i < modinfo.SizeOfImage; ++i) {
    for (unsigned j = 0; j < sizeof(pattern_cmp_processorfeature_28_); ++j) {
      if (strt[i + j] != pattern_cmp_processorfeature_28_[j]) {
        goto miss;
      }
    }
    possible_places.push_back((uintptr_t)(&strt[i]));
  miss:;
  }

  if (possible_places.empty()) {
    XELOGW(
        "RDRAND patch: Pattern not found in ntdll.dll (Windows version may be "
        "incompatible)");
  } else {
    for (auto&& place : possible_places) {
      write_process_memory(GetCurrentProcess(), place,
                           sizeof(pattern_replacement), pattern_replacement);
    }
    XELOGI("RDRAND patch: Successfully applied to {} location(s)",
           possible_places.size());
  }
}
// end ntdll rdrand patch
#endif

namespace xe {
namespace app {

class EmulatorApp final : public xe::ui::WindowedApp {
 public:
  static std::unique_ptr<xe::ui::WindowedApp> Create(
      xe::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<xe::ui::WindowedApp>(new EmulatorApp(app_context));
  }

  ~EmulatorApp();

  bool OnInitialize() override;

 protected:
  void OnDestroy() override;

 private:
  template <typename T, typename... Args>
  class Factory {
   private:
    struct Creator {
      std::string name;
      std::function<bool()> is_available;
      std::function<std::unique_ptr<T>(Args...)> instantiate;
    };

    std::vector<Creator> creators_;

   public:
    void Add(const std::string_view name, std::function<bool()> is_available,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      creators_.push_back({std::string(name), is_available, instantiate});
    }

    void Add(const std::string_view name,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      auto always_available = []() { return true; };
      Add(name, always_available, instantiate);
    }

    template <typename DT>
    void Add(const std::string_view name) {
      Add(name, DT::IsAvailable, [](Args... args) {
        return std::make_unique<DT>(std::forward<Args>(args)...);
      });
    }

    std::unique_ptr<T> Create(const std::string_view name, Args... args) {
      if (!name.empty() && name != "any") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });
        if (it != creators_.cend() && (*it).is_available()) {
          return (*it).instantiate(std::forward<Args>(args)...);
        }
        return nullptr;
      } else {
        for (const auto& creator : creators_) {
          if (!creator.is_available()) continue;
          auto instance = creator.instantiate(std::forward<Args>(args)...);
          if (!instance) continue;
          return instance;
        }
        return nullptr;
      }
    }

    std::vector<std::unique_ptr<T>> CreateAll(const std::string_view name,
                                              Args... args) {
      std::vector<std::unique_ptr<T>> instances;

      if (name != "winkey") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });

        if (it != creators_.cend() && (*it).is_available()) {
          auto instance = (*it).instantiate(std::forward<Args>(args)...);
          if (instance) {
            instances.emplace_back(std::move(instance));
          }
        }
      }

      auto it = std::find_if(
          creators_.cbegin(), creators_.cend(),
          [&name](const auto& f) { return f.name.compare("winkey") == 0; });
      if (it != creators_.cend() && (*it).is_available()) {
        auto instance = (*it).instantiate(std::forward<Args>(args)...);
        if (instance) {
          instances.emplace_back(std::move(instance));
        }
      }
      return instances;
    }
  };

  class DebugWindowClosedListener final : public xe::ui::WindowListener {
   public:
    explicit DebugWindowClosedListener(EmulatorApp& emulator_app)
        : emulator_app_(emulator_app) {}

    void OnClosing(xe::ui::UIEvent& e) override;

   private:
    EmulatorApp& emulator_app_;
  };

  explicit EmulatorApp(xe::ui::WindowedAppContext& app_context);

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(
      cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(
      ui::Window* window);

  void EmulatorThread(bool is_game_process);
  void ShutdownEmulatorThreadFromUIThread();

  DebugWindowClosedListener debug_window_closed_listener_;

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<EmulatorWindow> emulator_window_;

  // Created on demand, used by the emulator.
  std::unique_ptr<xe::debug::ui::DebugWindow> debug_window_;
#if XE_PLATFORM_WIN32
  std::unique_ptr<xe::debug::gdb::GDBStub> debug_gdbstub_;
#endif

  // Refreshing the emulator - placed after its dependencies.
  std::atomic<bool> emulator_thread_quit_requested_;
  std::unique_ptr<xe::threading::Event> emulator_thread_event_;
  std::thread emulator_thread_;
};

void EmulatorApp::DebugWindowClosedListener::OnClosing(xe::ui::UIEvent& e) {
  EmulatorApp* emulator_app = &emulator_app_;
  emulator_app->emulator_->processor()->set_debug_listener(nullptr);
  emulator_app->debug_window_.reset();
}

EmulatorApp::EmulatorApp(xe::ui::WindowedAppContext& app_context)
    : xe::ui::WindowedApp(app_context, "xenia", "[Path to .iso/.xex]"),
      debug_window_closed_listener_(*this) {
  AddPositionalOption("target");
}

#if XE_PLATFORM_ANDROID
namespace {

template <typename T>
void OverrideAndroidConfigVar(const char* name, T value) {
  if (!cvar::ConfigVars) {
    return;
  }
  auto it = cvar::ConfigVars->find(name);
  if (it == cvar::ConfigVars->end()) {
    return;
  }
  auto* config_var = dynamic_cast<cvar::ConfigVar<T>*>(it->second);
  if (config_var) {
    config_var->SetCommandLineValue(std::move(value));
  }
}

std::string TrimAndroidProfileToken(const std::string& token) {
  size_t first = 0;
  while (first < token.size() &&
         std::isspace(static_cast<unsigned char>(token[first]))) {
    ++first;
  }
  size_t last = token.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(token[last - 1]))) {
    --last;
  }
  return token.substr(first, last - first);
}

std::string UnquoteAndroidProfileValue(std::string value) {
  value = TrimAndroidProfileToken(value);
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

bool ParseAndroidProfileBool(std::string value, bool& parsed_value) {
  value = UnquoteAndroidProfileValue(value);
  for (char& c : value) {
    c = static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  }
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    parsed_value = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    parsed_value = false;
    return true;
  }
  return false;
}

bool ParseAndroidProfileInt32(std::string value, int32_t& parsed_value) {
  value = UnquoteAndroidProfileValue(value);
  char* end = nullptr;
  errno = 0;
  long parsed_long = std::strtol(value.c_str(), &end, 0);
  if (errno || end == value.c_str() || *end) {
    return false;
  }
  parsed_value = static_cast<int32_t>(parsed_long);
  return true;
}

bool ParseAndroidProfileUint32(std::string value, uint32_t& parsed_value) {
  value = UnquoteAndroidProfileValue(value);
  char* end = nullptr;
  errno = 0;
  unsigned long parsed_long = std::strtoul(value.c_str(), &end, 0);
  if (errno || end == value.c_str() || *end) {
    return false;
  }
  parsed_value = static_cast<uint32_t>(parsed_long);
  return true;
}

bool ParseAndroidProfileUint64(std::string value, uint64_t& parsed_value) {
  value = UnquoteAndroidProfileValue(value);
  char* end = nullptr;
  errno = 0;
  unsigned long long parsed_long = std::strtoull(value.c_str(), &end, 0);
  if (errno || end == value.c_str() || *end) {
    return false;
  }
  parsed_value = static_cast<uint64_t>(parsed_long);
  return true;
}

bool ApplyAndroidProfileOverride(const std::string& name,
                                 const std::string& value) {
  if (name == "render_target_path" || name == "render_target_path_vulkan" ||
      name == "readback_resolve" || name == "xma_decoder" ||
      name == "postprocess_antialiasing" ||
      name == "postprocess_scaling_and_sharpening") {
    OverrideAndroidConfigVar<std::string>(
        name.c_str(), UnquoteAndroidProfileValue(value));
    return true;
  }

  if (name == "async_shader_compilation" ||
      name == "vulkan_dynamic_rendering" ||
      name == "vulkan_sparse_shared_memory" ||
      name == "tiled_shared_memory" || name == "postprocess_dither" ||
      name == "mrt_edram_used_range_clamp_to_min" ||
      name == "native_2x_msaa" || name == "halo_android_gpu_frame_dumps" ||
      name == "readback_memexport" || name == "readback_memexport_fast" ||
      name == "halo_android_diag_direct_fb_fill_linear" ||
      name == "halo_android_diag_direct_fb_fill_tiled" ||
      name == "halo_android_diag_linear_to_tiled_frontbuffer" ||
      name == "halo_android_diag_strict_barriers" ||
      name == "halo_android_diag_log_resolve_constants" ||
      name == "halo_android_diag_force_full_32bpp_non80" ||
      name == "halo_android_diag_synthetic_edram_fill" ||
      name == "halo_android_diag_dump_shader_pattern" ||
      name == "halo_android_diag_transfer_dump_8bpp" ||
      name == "halo_android_diag_force_1010102_rt_as_rgba8" ||
      name == "halo_android_diag_blit_rt_transfers" ||
      name == "halo_android_diag_force_null_textures" ||
      name == "halo_android_diag_log_texture_bindings" ||
      name == "halo_android_diag_dump_resolve_images" ||
      name == "halo_android_diag_log_draws" ||
      name == "halo_android_diag_skip_draws_to_base_1350" ||
      name == "halo_android_diag_log_rt_transfers" ||
      name == "halo_android_diag_skip_rt_transfers_to_base_1350" ||
      name == "halo_android_diag_skip_resolve_clear_to_base_1350" ||
      name == "halo_android_diag_depth_to_color_pattern" ||
      name == "halo_android_diag_depth_to_color_zero_stencil" ||
      name == "halo_android_diag_force_d32s8_depth_format" ||
      name == "halo_android_compat_presentable_color_shadow" ||
      name == "halo_android_compat_skip_depth_to_color_alias" ||
      name == "halo_android_compat_direct_presentable_resolve" ||
      name == "halo_android_compat_linear_to_tiled_frontbuffer" ||
      name == "halo_android_disable_high_4k_physical_routing") {
    bool parsed_value = false;
    if (!ParseAndroidProfileBool(value, parsed_value)) {
      return false;
    }
    OverrideAndroidConfigVar<bool>(name.c_str(), parsed_value);
    return true;
  }

    if (name == "vulkan_pipeline_creation_threads" ||
        name == "halo_android_diag_depth_to_color_sample_mode" ||
        name == "halo_android_diag_depth_to_color_mode") {
    int32_t parsed_value = 0;
    if (!ParseAndroidProfileInt32(value, parsed_value)) {
      return false;
    }
    OverrideAndroidConfigVar<int32_t>(name.c_str(), parsed_value);
    return true;
  }

  if (name == "halo_android_gpu_summary_ms" ||
      name == "halo_android_diag_swap_base_page_override") {
    uint32_t parsed_value = 0;
    if (!ParseAndroidProfileUint32(value, parsed_value)) {
      return false;
    }
    OverrideAndroidConfigVar<uint32_t>(name.c_str(), parsed_value);
    return true;
  }

  if (name == "framerate_limit") {
    uint64_t parsed_value = 0;
    if (!ParseAndroidProfileUint64(value, parsed_value)) {
      return false;
    }
    OverrideAndroidConfigVar<uint64_t>(name.c_str(), parsed_value);
    return true;
  }

  return false;
}

void ApplyAndroidCompatPresentationDefaults() {
  OverrideAndroidConfigVar<bool>(
      "halo_android_compat_presentable_color_shadow", true);
  OverrideAndroidConfigVar<bool>(
      "halo_android_compat_skip_depth_to_color_alias", true);
  OverrideAndroidConfigVar<bool>(
      "halo_android_compat_direct_presentable_resolve", true);
  OverrideAndroidConfigVar<bool>(
      "halo_android_compat_linear_to_tiled_frontbuffer", true);
  // Per-draw / per-binding logging served its diagnostic purpose; the volume
  // (thousands of logd lines/sec during mission load) contributes to
  // device-wide unresponsiveness and watchdog kills. Off by default.
  OverrideAndroidConfigVar<bool>("halo_android_diag_log_draws", false);
  OverrideAndroidConfigVar<bool>("halo_android_diag_log_texture_bindings",
                                 false);
  OverrideAndroidConfigVar<bool>("halo_android_gpu_frame_dumps", false);
  OverrideAndroidConfigVar<bool>("readback_memexport", true);
  OverrideAndroidConfigVar<bool>("readback_memexport_fast", true);
}

void ApplyAndroidProfileFileOverrides(
    const std::filesystem::path& storage_root) {
  const std::array<std::filesystem::path, 3> profile_paths = {
      storage_root / "xenios_android_profile.txt",
      std::filesystem::path(
          "/sdcard/Android/data/jp.xenios.emulator.github.debug/files/"
          "xenios_android_profile.txt"),
      std::filesystem::path(
          "/sdcard/Android/data/jp.xenios.emulator.github/files/"
          "xenios_android_profile.txt"),
  };
  for (const std::filesystem::path& profile_path : profile_paths) {
    std::ifstream profile_file(profile_path);
    if (!profile_file) {
      continue;
    }
    XELOGI("Android profile overrides: loading {}", profile_path);

    std::string line;
    uint32_t line_number = 0;
    while (std::getline(profile_file, line)) {
      ++line_number;
      size_t comment = line.find('#');
      if (comment != std::string::npos) {
        line.resize(comment);
      }
      size_t equals = line.find('=');
      if (equals == std::string::npos) {
        continue;
      }
      std::string name = TrimAndroidProfileToken(line.substr(0, equals));
      std::string value = TrimAndroidProfileToken(line.substr(equals + 1));
      if (name.empty()) {
        continue;
      }
      if (ApplyAndroidProfileOverride(name, value)) {
        XELOGI("Android profile override {}={}", name,
               UnquoteAndroidProfileValue(value));
      } else {
        XELOGW("Android profile override ignored at {}:{}: {}={}",
               profile_path, line_number, name, value);
      }
    }
    return;
  }
}

void EnsureAndroidDefaultProfile(xe::Emulator* emulator) {
  if (!emulator || !emulator->kernel_state() ||
      !emulator->kernel_state()->xam_state()) {
    return;
  }
  auto* profile_manager =
      emulator->kernel_state()->xam_state()->profile_manager();
  if (!profile_manager) {
    return;
  }

  profile_manager->ReloadProfiles();
  if (!profile_manager->GetAccounts()->empty()) {
    if (!profile_manager->IsAnyProfileSignedIn()) {
      profile_manager->Login(profile_manager->GetAccounts()->begin()->first, 0,
                             true);
    }
    return;
  }

  constexpr char kDefaultAndroidGamertag[] = "AndroidPlayer";
  if (!xe::kernel::xam::ProfileManager::IsGamertagValid(
          kDefaultAndroidGamertag)) {
    return;
  }

  if (profile_manager->CreateProfile(kDefaultAndroidGamertag, true)) {
    XELOGI("Android: created default offline profile {}",
           kDefaultAndroidGamertag);
  }
}

void EnsureAndroidEmptyUpdateMount(xe::Emulator* emulator) {
  if (!emulator) {
    return;
  }
  auto* fs = emulator->file_system();
  if (!fs) {
    return;
  }

  std::string resolved_path;
  if (fs->FindSymbolicLink("UPDATE:", resolved_path)) {
    return;
  }

  const std::filesystem::path update_root =
      emulator->storage_root() / "update_empty";
  std::error_code ec;
  std::filesystem::create_directories(update_root, ec);

  auto update_device = std::make_unique<xe::vfs::HostPathDevice>(
      "\\UPDATE", update_root, false);
  if (!update_device->Initialize()) {
    XELOGW("Android: unable to initialize empty UPDATE mount at {}",
           update_root);
    return;
  }
  if (!fs->RegisterDevice(std::move(update_device))) {
    XELOGW("Android: unable to register empty UPDATE device");
    return;
  }
  fs->RegisterSymbolicLink("UPDATE:", "\\UPDATE");
}

}  // namespace
#endif

EmulatorApp::~EmulatorApp() {
  // Should be shut down from OnDestroy if OnInitialize has ever been done, but
  // for the most safety as a running thread may be destroyed only after
  // joining.
  ShutdownEmulatorThreadFromUIThread();
}

std::unique_ptr<apu::AudioSystem> EmulatorApp::CreateAudioSystem(
    cpu::Processor* processor) {
  Factory<apu::AudioSystem, cpu::Processor*> factory;
#if XE_PLATFORM_WIN32
  factory.Add<apu::xaudio2::XAudio2AudioSystem>("xaudio2");
#endif  // XE_PLATFORM_WIN32
#if XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
  factory.Add<apu::alsa::ALSAAudioSystem>("alsa");
#endif  // XE_PLATFORM_LINUX && !XE_PLATFORM_ANDROID
#if !XE_PLATFORM_ANDROID
  factory.Add<apu::sdl::SDLAudioSystem>("sdl");
#endif  // !XE_PLATFORM_ANDROID
  factory.Add<apu::nop::NopAudioSystem>("nop");
  return factory.Create(cvars::apu, processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorApp::CreateGraphicsSystem() {
  // While Vulkan is supported by a large variety of operating systems (Windows,
  // GNU/Linux, Android, also via the MoltenVK translation layer on top of Metal
  // on macOS and iOS), please don't remove platform-specific GPU backends from
  // Xenia.
  //
  // Regardless of the operating system, having multiple options provides more
  // stability to users. In case of driver issues, users may try switching
  // between the available backends. For example, in June 2022, on Nvidia Ampere
  // (RTX 30xx), Xenia had synchronization issues that resulted in flickering,
  // most prominently in 4D5307E6, on Direct3D 12 - but the same issue was not
  // reproducible in the Vulkan backend, however, it used ImageSampleExplicitLod
  // with explicit gradients for cubemaps, which triggered a different driver
  // bug on Nvidia (every 1 out of 2x2 pixels receiving junk).
  //
  // Specifically on Microsoft platforms, there are a few reasons why supporting
  // Direct3D 12 is desirable rather than limiting Xenia to Vulkan only:
  // - Wider hardware support for Direct3D 12 on x86 Windows desktops.
  //   Direct3D 12 requires the minimum of Nvidia Fermi, or, with a pre-2021
  //   driver version, Intel HD Graphics 4200. Vulkan, however, is supported
  //   only starting with Nvidia Kepler and a much more recent Intel UHD
  //   Graphics generation.
  // - Wider hardware support on other kinds of Microsoft devices. The Xbox One
  //   and the Xbox Series X|S only support Direct3D as the GPU API in their UWP
  //   runtime, and only version 12 can be granted expanded resource access.
  //   Qualcomm, as of June 2022, also doesn't provide a Vulkan implementation
  //   for their Arm-based Windows devices, while Direct3D 12 is available.
  //   - Both older Intel GPUs and the Xbox One apparently, as well as earlier
  //     Windows 10 versions, also require Shader Model 5.1 DXBC shaders rather
  //     than Shader Model 6 DXIL ones, so a DXBC shader translator should be
  //     available in Xenia too, a DXIL one doesn't fully replace it.
  // - As of June 2022, AMD also refuses to implement the
  //   VK_EXT_fragment_shader_interlock Vulkan extension in their drivers, as
  //   well as its OpenGL counterpart, which is heavily utilized for accurate
  //   support of Xenos render target formats that don't have PC equivalents
  //   (8_8_8_8_GAMMA, 2_10_10_10_FLOAT, 16_16 and 16_16_16_16 with -32 to 32
  //   range, D24FS8) with correct blending. Direct3D 12, however, requires
  //   support for similar functionality (rasterizer-ordered views) on the
  //   feature level 12_1, and the AMD driver implements it on Direct3D, as well
  //   as raster order groups in their Metal driver.
  //
  // Additionally, different host GPU APIs receive feature support at different
  // paces. VK_EXT_fragment_shader_interlock first appeared in 2019, for
  // instance, while Xenia had been taking advantage of rasterizer-ordered views
  // on Direct3D 12 for over half a year at that point (they have existed in
  // Direct3D 12 since the first version).
  //
  // MoltenVK on top Metal also has its flaws and limitations. Metal, for
  // instance, as of June 2022, doesn't provide a switch for primitive restart,
  // while Vulkan does - so MoltenVK is not completely transparent to Xenia,
  // many of its issues that may be not very obvious (unlike when the Metal API
  // is used directly) should be taken into account in Xenia. Also, as of June
  // 2022, MoltenVK translates SPIR-V shaders into the C++-based Metal Shading
  // Language rather than AIR directly, which likely massively increases
  // pipeline object creation time - and Xenia translates shaders and creates
  // pipelines when they're first actually used for a draw command by the game,
  // thus it can't precompile anything that hasn't ever been encountered before
  // there's already no time to waste.
  //
  // Very old hardware (Direct3D 10 level) is also not supported by most Vulkan
  // drivers. However, in the future, Xenia may be ported to it using the
  // Direct3D 11 API with the feature level 10_1 or 10_0. OpenGL, however, had
  // been lagging behind Direct3D prior to versions 4.x, and didn't receive
  // compute shaders until a 4.2 extension (while 4.2 already corresponds
  // roughly to Direct3D 11 features) - and replacing Xenia compute shaders with
  // transform feedback / stream output is not always trivial (in particular,
  // will need to rely on GL_ARB_transform_feedback3 for skipping over memory
  // locations that shouldn't be overwritten).
  //
  // For maintainability, as much implementation code as possible should be
  // placed in `xe::gpu` and shared between the backends rather than duplicated
  // between them.
  const std::string gpu_implementation_name = cvars::gpu;
  if (gpu_implementation_name == "null") {
    return std::make_unique<gpu::null::NullGraphicsSystem>();
  }
  Factory<gpu::GraphicsSystem> factory;
#if XE_PLATFORM_WIN32
  factory.Add<gpu::d3d12::D3D12GraphicsSystem>("d3d12");
#endif  // XE_PLATFORM_WIN32
#if XE_PLATFORM_APPLE
  factory.Add<gpu::metal::MetalGraphicsSystem>("metal");
#else
  factory.Add<gpu::vulkan::VulkanGraphicsSystem>("vulkan");
#endif  // XE_PLATFORM_APPLE
  std::unique_ptr<gpu::GraphicsSystem> gpu_implementation =
      factory.Create(gpu_implementation_name);
  if (!gpu_implementation) {
    xe::FatalError(
        "Unable to initialize the graphics subsystem.\n"
        "\n"
#if XE_PLATFORM_ANDROID
        "The GPU must support at least Vulkan 1.0 with the 'independentBlend' "
        "feature.\n"
        "\n"
#else
#if XE_PLATFORM_WIN32
        "For Direct3D 12, at least Windows 10 is required, and the GPU must be "
        "compatible with Direct3D 12 feature level 11_0.\n"
        "\n"
#endif  // XE_PLATFORM_WIN32
        "For Vulkan, the Vulkan runtime must be installed, and the GPU must "
        "support at least Vulkan 1.0. The Vulkan runtime can be downloaded at "
        "https://vulkan.lunarg.com/sdk/home.\n"
        "\n"
        "Also, ensure that you have the latest driver installed for your GPU.\n"
        "\n"
#endif  // XE_PLATFORM_ANDROID
        "See https://xenios.jp/faq for more information and the system "
        "requirements.");
  }
  return gpu_implementation;
}

std::vector<std::unique_ptr<hid::InputDriver>> EmulatorApp::CreateInputDrivers(
    ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  auto add_driver_if_ready = [&drivers](std::unique_ptr<hid::InputDriver> driver)
                                 -> bool {
    if (!driver) {
      return false;
    }
    if (XFAILED(driver->Setup())) {
      return false;
    }
    drivers.emplace_back(std::move(driver));
    return true;
  };
  if (cvars::hid.compare("nop") == 0) {
    add_driver_if_ready(
        xe::hid::nop::Create(window, EmulatorWindow::kZOrderHidInput));
  } else {
    Factory<hid::InputDriver, ui::Window*, size_t> factory;
#if XE_PLATFORM_WIN32
    factory.Add("xinput", xe::hid::xinput::Create);
#endif  // XE_PLATFORM_WIN32
#if !XE_PLATFORM_ANDROID
    factory.Add("sdl", xe::hid::sdl::Create);
#endif  // !XE_PLATFORM_ANDROID
#if XE_PLATFORM_WIN32
    // WinKey input driver should always be the last input driver added!
    factory.Add("winkey", xe::hid::winkey::Create);
#endif  // XE_PLATFORM_WIN32
    for (auto& driver : factory.CreateAll(cvars::hid, window,
                                          EmulatorWindow::kZOrderHidInput)) {
      add_driver_if_ready(std::move(driver));
    }
    if (drivers.empty()) {
      // Fallback to nop if none created.
      add_driver_if_ready(
          xe::hid::nop::Create(window, EmulatorWindow::kZOrderHidInput));
    }
  }
  return drivers;
}

bool EmulatorApp::OnInitialize() {
  Profiler::Initialize();
  Profiler::ThreadEnter("Main");

  // Figure out where internal files and content should go.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe::filesystem::GetExecutableFolder();
    if (!cvars::portable &&
        !std::filesystem::exists(storage_root / "portable.txt")) {
      storage_root = xe::filesystem::GetUserFolder();
#if defined(XE_PLATFORM_WIN32) || defined(XE_PLATFORM_LINUX)
      storage_root = storage_root / "Xenia";
#else
      // TODO(Triang3l): Point to the app's external storage "files" directory
      // on Android.
#warning Unhandled platform for the data root.
      storage_root = storage_root / "Xenia";
#endif
    }
  }
  storage_root = std::filesystem::absolute(storage_root);
  XELOGI("Storage root: {}", storage_root);

  config::SetupConfig(storage_root);

  // Load game-specific config if a target is specified.
  if (!cvars::target.empty()) {
    config::LoadGameConfigForFile(cvars::target);
  }

#if XE_PLATFORM_ANDROID
  // Saved config files can override Android launch/default values. Keep the
  // mobile profile deterministic for compatibility and battery/thermal limits.
  OVERRIDE_bool(discord, false);
  // Async compilation keeps the frame loop alive through FSI's much larger
  // pipeline compilations (sync stalls there starve the watchdog into killing
  // the app). Off on the FBO path where sync compiles are short and
  // placeholders would muddy diagnostics.
  if (xe::gpu::vulkan::GetAndroidHaloExperiment().force_fsi) {
    OverrideAndroidConfigVar<bool>("async_shader_compilation", true);
    OverrideAndroidConfigVar<int32_t>("vulkan_pipeline_creation_threads", 2);
  } else {
    OverrideAndroidConfigVar<bool>("async_shader_compilation", false);
    OverrideAndroidConfigVar<int32_t>("vulkan_pipeline_creation_threads", 1);
  }
  OverrideAndroidConfigVar<std::string>("xma_decoder", "old");
  OverrideAndroidConfigVar<uint64_t>("framerate_limit", 30);
  OverrideAndroidConfigVar<std::string>("render_target_path", "performance");
  OverrideAndroidConfigVar<std::string>("render_target_path_vulkan", "");
  // readback_resolve=full stalls the GPU on every resolve (the dominant cost
  // at ~8 resolves/frame) but keeps CPU-visible guest memory authoritative
  // for the diagnostics dumps. Experiment-switchable for perf A/B runs.
  if (xe::gpu::vulkan::GetAndroidHaloExperiment().readback_resolve_full) {
    OverrideAndroidConfigVar<std::string>("readback_resolve", "full");
  }
  if (xe::gpu::vulkan::GetAndroidHaloExperiment().vblank_uncapped) {
    OverrideAndroidConfigVar<bool>("guest_display_refresh_cap", false);
  }
  if (xe::gpu::vulkan::GetAndroidHaloExperiment().quiet_logs) {
    OverrideAndroidConfigVar<int32_t>("log_level", 1);
  } else {
    OverrideAndroidConfigVar<int32_t>("log_level", 2);
  }
  OverrideAndroidConfigVar<bool>("vulkan_dynamic_rendering", false);
  OverrideAndroidConfigVar<bool>("vulkan_sparse_shared_memory", true);
  OverrideAndroidConfigVar<bool>("tiled_shared_memory", true);
  OverrideAndroidConfigVar<std::string>("postprocess_antialiasing", "");
  OverrideAndroidConfigVar<std::string>("postprocess_scaling_and_sharpening",
                                        "");
  OverrideAndroidConfigVar<bool>("postprocess_dither", false);
  ApplyAndroidCompatPresentationDefaults();
  ApplyAndroidProfileFileOverrides(storage_root);
  XELOGI(
      "Android forced profile: discord={} async_shader_compilation={} "
      "vulkan_pipeline_creation_threads={} xma_decoder={} framerate_limit={} "
      "render_target_path={} render_target_path_vulkan={} readback_resolve={} "
      "guest_display_refresh_cap={} "
      "readback_memexport={} readback_memexport_fast={} "
      "halo_android_compat_presentable_color_shadow={} "
      "halo_android_compat_skip_depth_to_color_alias={} "
      "halo_android_compat_direct_presentable_resolve={} "
      "halo_android_compat_linear_to_tiled_frontbuffer={} "
      "vulkan_dynamic_rendering={} vulkan_sparse_shared_memory={} "
      "tiled_shared_memory={} mrt_edram_used_range_clamp_to_min={} "
      "native_2x_msaa={} postprocess_antialiasing={} "
      "postprocess_scaling_and_sharpening={} postprocess_dither={}",
      cvars::discord, cvars::async_shader_compilation,
      cvars::vulkan_pipeline_creation_threads, cvars::xma_decoder,
      cvars::framerate_limit, cvars::render_target_path,
      cvars::render_target_path_vulkan, cvars::readback_resolve,
      cvars::guest_display_refresh_cap,
      cvars::readback_memexport, cvars::readback_memexport_fast,
      cvars::halo_android_compat_presentable_color_shadow,
      cvars::halo_android_compat_skip_depth_to_color_alias,
      cvars::halo_android_compat_direct_presentable_resolve,
      cvars::halo_android_compat_linear_to_tiled_frontbuffer,
      cvars::vulkan_dynamic_rendering, cvars::vulkan_sparse_shared_memory,
      cvars::tiled_shared_memory, cvars::mrt_edram_used_range_clamp_to_min,
      cvars::native_2x_msaa, cvars::postprocess_antialiasing,
      cvars::postprocess_scaling_and_sharpening, cvars::postprocess_dither);
#endif

#if XE_ARCH_AMD64 == 1
  amd64::InitFeatureFlags();
#endif

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!content_root.is_absolute()) {
      content_root = storage_root / content_root;
    }
  }
  content_root = std::filesystem::absolute(content_root);
  XELOGI("Content root: {}", content_root);

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
    cache_root = storage_root / "cache_host";
    // TODO(Triang3l): Point to the app's external storage "cache" directory on
    // Android.
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!cache_root.is_absolute()) {
      cache_root = storage_root / cache_root;
    }
  }
  cache_root = std::filesystem::absolute(cache_root);
  XELOGI("Host cache root: {}", cache_root);

  // Create the emulator but don't initialize so we can setup the window.
  emulator_ =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // Check if this is a game process (has target) or UI process
  bool is_game_process = !cvars::target.empty();

#if XE_PLATFORM_WIN32 && XE_ARCH_AMD64 == 1
  // Apply ntdll rdrand patch for game process only
  if (is_game_process && cvars::enable_rdrand_ntdll_patch) {
    do_ntdll_rdrand_patch();
  }
#endif

  // Initialize Discord rich presence only for game process
  #if !XE_PLATFORM_ANDROID
  if (is_game_process && cvars::discord) {
    discord::DiscordPresence::Initialize();
    discord::DiscordPresence::NotPlaying();
  }
  #endif

  // Determine window size based on process type
  uint32_t window_width, window_height;
  if (is_game_process) {
    // Game process - use internal resolution or command-line override
    auto res = xe::gpu::GraphicsSystem::GetInternalDisplayResolution();
    window_width = res.first;
    window_height = res.second;

    // Override with command-line args if set (transient cvars)
    if (cvars::window_size_game_x != 0) {
      window_width = cvars::window_size_game_x;
    }
    if (cvars::window_size_game_y != 0) {
      window_height = cvars::window_size_game_y;
    }
  } else {
    // UI process - use persistent cvars (defaults to 950x750)
    window_width = cvars::window_size_ui_x;
    window_height = cvars::window_size_ui_y;
  }

  // Main emulator display window.
  emulator_window_ =
      EmulatorWindow::Create(emulator_.get(), app_context(), window_width,
                             window_height, is_game_process);
  if (!emulator_window_) {
    XELOGE("Failed to create the main emulator window");
    return false;
  }

  // Setup the emulator and run its loop in a separate thread.
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  assert_not_null(emulator_thread_event_);
  emulator_thread_ =
      std::thread(&EmulatorApp::EmulatorThread, this, is_game_process);

  return true;
}

void EmulatorApp::OnDestroy() {
  ShutdownEmulatorThreadFromUIThread();

  #if !XE_PLATFORM_ANDROID
  if (cvars::discord) {
    discord::DiscordPresence::Shutdown();
  }
  #endif

  Profiler::Dump();
  // The profiler needs to shut down before the graphics context.
  Profiler::Shutdown();

  // TODO(DrChat): Remove this code and do a proper exit.
  XELOGI("Cheap-skate exit!");
  xe::FlushLog();
  std::quick_exit(EXIT_SUCCESS);
}

void EmulatorApp::EmulatorThread(bool is_game_process) {
  assert_not_null(emulator_thread_event_);

  xe::threading::set_name("Emulator");
  Profiler::ThreadEnter("Emulator");

  // UI process: Minimal setup for profiles/GPD only
  if (!is_game_process) {
    // Initialize just enough for profiles: kernel state with XAM module
    X_STATUS result = emulator_->Setup(emulator_window_->window(), nullptr,
                                       false, nullptr, nullptr, nullptr);
    if (XFAILED(result)) {
      XELOGE("Failed to setup minimal emulator for UI: {:08X}", result);
      app_context().RequestDeferredQuit();
      return;
    }

    // Notify that the UI is ready to be shown
    app_context().CallInUIThread(
        [this]() { emulator_window_->OnEmulatorInitialized(); });

    // Keep the thread alive for UI process
    while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
      xe::threading::Wait(emulator_thread_event_.get(), false);
    }
    return;
  }

  // Game process: Full emulator setup with graphics, audio, and input
  X_STATUS result = emulator_->Setup(
      emulator_window_->window(), emulator_window_->imgui_drawer(), true,
      CreateAudioSystem, CreateGraphicsSystem, CreateInputDrivers);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    app_context().RequestDeferredQuit();
    return;
  }

  // Setup graphics presenter painting for game process
  app_context().CallInUIThread(
      [this]() { emulator_window_->SetupGraphicsSystemPresenterPainting(); });

  const auto fs = emulator_->file_system();

  if (cvars::mount_scratch) {
    auto scratch_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\SCRATCH", emulator_->storage_root() / "scratch", false);
    if (!scratch_device->Initialize()) {
      XELOGE("Unable to scan scratch path");
    } else {
      if (!fs->RegisterDevice(std::move(scratch_device))) {
        XELOGE("Unable to register scratch path");
      } else {
        fs->RegisterSymbolicLink("scratch:", "\\SCRATCH");
      }
    }
  }

  if (cvars::mount_cache) {
    auto cache0_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE0", emulator_->storage_root() / "cache0", false);
    if (!cache0_device->Initialize()) {
      XELOGE("Unable to scan cache0 path");
    } else {
      if (!fs->RegisterDevice(std::move(cache0_device))) {
        XELOGE("Unable to register cache0 path");
      } else {
        fs->RegisterSymbolicLink("cache0:", "\\CACHE0");
      }
    }

    auto cache1_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE1", emulator_->storage_root() / "cache1", false);
    if (!cache1_device->Initialize()) {
      XELOGE("Unable to scan cache1 path");
    } else {
      if (!fs->RegisterDevice(std::move(cache1_device))) {
        XELOGE("Unable to register cache1 path");
      } else {
        fs->RegisterSymbolicLink("cache1:", "\\CACHE1");
      }
    }

    // Some (older?) games try accessing cache:\ too
    // NOTE: this must be registered _after_ the cache0/cache1 devices, due to
    // substring/start_with logic inside VirtualFileSystem::ResolvePath, else
    // accesses to those devices will go here instead
    auto cache_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\CACHE", emulator_->storage_root() / "cache", false);
    if (!cache_device->Initialize()) {
      XELOGE("Unable to scan cache path");
    } else {
      if (!fs->RegisterDevice(std::move(cache_device))) {
        XELOGE("Unable to register cache path");
      } else {
        fs->RegisterSymbolicLink("cache:", "\\CACHE");
      }
    }
  }

  if (cvars::force_mount_devkit) {
    auto devkit_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\DEVKIT", "devkit", false);

    if (!devkit_device->Initialize()) {
      XELOGE("Unable to scan devkit path");
    }

    if (!fs->RegisterDevice(std::move(devkit_device))) {
      XELOGE("Unable to register devkit path");
    }

    fs->RegisterSymbolicLink("DEVKIT:", "\\DEVKIT");
    fs->RegisterSymbolicLink("e:", "\\DEVKIT");
  }

#if XE_PLATFORM_ANDROID
  EnsureAndroidDefaultProfile(emulator_.get());
  EnsureAndroidEmptyUpdateMount(emulator_.get());
#endif

  // Set a debug handler.
  // This will respond to debugging requests so we can open the debug UI.
  if (cvars::debug) {
    if (cvars::gdbport > 0) {
#if XE_PLATFORM_WIN32
      emulator_->processor()->set_debug_listener_request_handler(
          [this](xe::cpu::Processor* processor) {
            if (debug_gdbstub_) {
              return debug_gdbstub_.get();
            }
            debug_gdbstub_ = xe::debug::gdb::GDBStub::Create(emulator_.get(),
                                                             cvars::gdbport);
            return debug_gdbstub_.get();
          });
      emulator_->processor()->ShowDebugger();
#endif
    }
#if !XE_PLATFORM_ANDROID
    else {
      emulator_->processor()->set_debug_listener_request_handler(
          [this](xe::cpu::Processor* processor) {
            if (debug_window_) {
              return debug_window_.get();
            }
            app_context().CallInUIThreadSynchronous([this]() {
              debug_window_ = xe::debug::ui::DebugWindow::Create(
                  emulator_.get(), app_context());
              debug_window_->window()->AddListener(
                  &debug_window_closed_listener_);
            });
            // If failed to enqueue the UI thread call, this will just be null.
            return debug_window_.get();
          });
    }
#endif
  }

  emulator_->on_launch.AddListener([&](auto title_id, const auto& game_title) {
#if !XE_PLATFORM_ANDROID
    if (cvars::discord) {
      discord::DiscordPresence::PlayingTitle(
          game_title.empty() ? "Unknown Title" : std::string(game_title));
    }
#endif
    app_context().CallInUIThread([this]() { emulator_window_->UpdateTitle(); });
    emulator_thread_event_->Set();
  });

  emulator_->on_shader_storage_initialization.AddListener(
      [this](bool initializing) {
        app_context().CallInUIThread([this, initializing]() {
          emulator_window_->SetInitializingShaderStorage(initializing);
        });
      });

  emulator_->on_patch_apply.AddListener([this]() {
    app_context().CallInUIThread([this]() { emulator_window_->UpdateTitle(); });
  });

  emulator_->on_terminate.AddListener([]() {
#if !XE_PLATFORM_ANDROID
    if (cvars::discord) {
      discord::DiscordPresence::NotPlaying();
    }
#endif
  });

  // Enable emulator input now that the emulator is properly loaded.
  app_context().CallInUIThread(
      [this]() { emulator_window_->OnEmulatorInitialized(); });

  // Grab path from the flag or unnamed argument.
  std::filesystem::path path;
  if (!cvars::target.empty()) {
    path = cvars::target;
  }

  // Set up launch data BEFORE LaunchPath — LaunchPath starts the game thread,
  // and the game may query XamLoaderGetLaunchData during early init.
  auto xam = emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
      "xam.xex");

  if (xam && (cvars::launch_flags != 0 || !cvars::launch_data.empty())) {
    auto& loader_data = xam->loader_data();
    loader_data.launch_data_present = true;
    loader_data.launch_flags = cvars::launch_flags;

    // Decode hex-encoded launch_data
    if (!cvars::launch_data.empty()) {
      loader_data.launch_data.clear();
      const std::string& hex = cvars::launch_data;
      for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        loader_data.launch_data.push_back(byte);
      }
    }
  }

  if (!path.empty()) {
    // Normalize the path and make absolute.
    auto abs_path = std::filesystem::absolute(path);

    // Store the host path in loader_data for title-to-title launches
    // (must be set before LaunchPath so the game sees it immediately)
    if (xam) {
      xam->loader_data().host_path = xe::path_to_utf8(abs_path);
    }

#if XE_PLATFORM_APPLE
    // macOS: Run through UI thread for Metal backend requirements.
    result = app_context().CallInUIThread(
        [this, abs_path]() { return emulator_window_->RunTitle(abs_path); });
#else
    // TODO(has207): Add archive format check like in RunTitle?
    result = emulator_->LaunchPath(abs_path);
#endif
    if (XFAILED(result)) {
#if XE_PLATFORM_ANDROID
      XELOGE("Failed to launch target: {:08X}", result);
      app_context().RequestDeferredQuit();
#else
      xe::FatalError(fmt::format("Failed to launch target: {:08X}", result));
      app_context().RequestDeferredQuit();
#endif
      return;
    }
  }

  // Now, we're going to use this thread to drive events related to emulation.
  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    xe::threading::Wait(emulator_thread_event_.get(), false);
    emulator_->WaitUntilExit();
  }
}

void EmulatorApp::ShutdownEmulatorThreadFromUIThread() {
  // TODO(Triang3l): Proper shutdown of the emulator (relying on std::quick_exit
  // for now) - currently WaitUntilExit loops forever otherwise (plus possibly
  // lots of other things not shutting down correctly now). Some parts of the
  // code call the regular std::exit, which seems to be calling destructors (at
  // least on Linux), so the entire join is currently commented out.
#if 0
  // Same thread as the one created it, to make sure there's zero possibility of
  // a race with the creation of the emulator thread.
  assert_true(app_context().IsInUIThread());
  emulator_thread_quit_requested_.store(true, std::memory_order_relaxed);
  if (!emulator_thread_.joinable()) {
    return;
  }
  emulator_thread_event_->Set();
  emulator_thread_.join();
#endif
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorApp::Create);
