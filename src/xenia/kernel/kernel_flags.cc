/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"

DEFINE_bool(headless, false,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Kernel");
DEFINE_bool(halo_android_diagnostics, true,
            "Emit low-rate Android/Halo progress diagnostics.", "Android");
DEFINE_bool(halo_android_io_verbose, false,
            "Emit per-call Android/Halo file IO diagnostics. Leave disabled "
            "for performance runs.",
            "Android");
DEFINE_bool(halo_android_thread_verbose, false,
            "Emit full per-thread Android/Halo samples instead of compact "
            "thread summaries.",
            "Android");
DEFINE_uint32(halo_android_io_summary_ms, 5000,
              "Interval for aggregated Android/Halo IO summaries.", "Android");
DEFINE_uint32(halo_android_thread_sample_ms, 5000,
              "Interval for Android/Halo guest thread samples.", "Android");
DEFINE_uint32(halo_android_gpu_summary_ms, 5000,
              "Interval for Android/Halo Vulkan swap summaries.", "Android");
