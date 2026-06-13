/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_UI_VULKAN_SPIRV_TOOLS_CONTEXT_H_
#define XENIA_UI_VULKAN_SPIRV_TOOLS_CONTEXT_H_

#include <cstdint>
#include <string>
#include <vector>

#include "xenia/base/platform.h"

#if XE_PLATFORM_ANDROID
using spv_result_t = int32_t;
using spv_context = void*;
enum spv_target_env {
  SPV_ENV_UNIVERSAL_1_0 = 0,
};
constexpr spv_result_t SPV_SUCCESS = 0;
constexpr spv_result_t SPV_ERROR_INVALID_BINARY = -1;
constexpr spv_result_t SPV_UNSUPPORTED = 1;
#else
#include <spirv-tools/libspirv.h>
#include <spirv-tools/optimizer.hpp>
#endif  // XE_PLATFORM_ANDROID

namespace xe {
namespace ui {
namespace vulkan {

class SpirvToolsContext {
 public:
  SpirvToolsContext() {}
  SpirvToolsContext(const SpirvToolsContext& context) = delete;
  SpirvToolsContext& operator=(const SpirvToolsContext& context) = delete;
  ~SpirvToolsContext() { Shutdown(); }
  bool Initialize(unsigned int spirv_version);
  void Shutdown();

  spv_result_t Validate(const uint32_t* words, size_t num_words,
                        std::string* error) const;

  // Optimizes SPIR-V code. Returns SPV_SUCCESS on successful optimization.
  // The optimized binary is returned in optimized_words.
  spv_result_t Optimize(const uint32_t* words, size_t num_words,
                        std::vector<uint32_t>& optimized_words,
                        bool performance_passes = true);

 private:
  spv_context context_ = nullptr;
  spv_target_env target_env_ = SPV_ENV_UNIVERSAL_1_0;
};

}  // namespace vulkan
}  // namespace ui
}  // namespace xe

#endif  // XENIA_UI_VULKAN_SPIRV_TOOLS_CONTEXT_H_
