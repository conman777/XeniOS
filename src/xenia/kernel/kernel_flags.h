/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_KERNEL_FLAGS_H_
#define XENIA_KERNEL_KERNEL_FLAGS_H_
#include "xenia/base/cvar.h"

DECLARE_bool(headless);
DECLARE_bool(log_high_frequency_kernel_calls);
DECLARE_bool(halo_android_diagnostics);
DECLARE_bool(halo_android_io_verbose);
DECLARE_bool(halo_android_thread_verbose);
DECLARE_uint32(halo_android_io_summary_ms);
DECLARE_uint32(halo_android_thread_sample_ms);
DECLARE_uint32(halo_android_gpu_summary_ms);

#endif  // XENIA_KERNEL_KERNEL_FLAGS_H_
