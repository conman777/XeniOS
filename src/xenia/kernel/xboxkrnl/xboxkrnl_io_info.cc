/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/info/file.h"
#include "xenia/kernel/info/volume.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/kernel/xfile.h"
#include "xenia/vfs/device.h"
#include "xenia/vfs/devices/disc_image_entry.h"
#include "xenia/xbox.h"

DECLARE_bool(halo_android_diagnostics);
DECLARE_bool(halo_android_io_verbose);
DECLARE_uint32(halo_android_io_summary_ms);

namespace xe {
namespace kernel {
namespace xboxkrnl {

namespace {

bool ShouldLogHaloIoInfoPath(std::string_view path) {
  return xe::utf8::find_first_of_case(path, "maps") != std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "mainmenu") !=
             std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "bink") != std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "cache") !=
             std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "webcache") !=
             std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "harddisk") !=
             std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "partition") !=
             std::string_view::npos;
}

struct HaloFilePositionStats {
  uint64_t calls = 0;
  uint64_t backward_seeks = 0;
  uint64_t forward_seeks = 0;
  uint64_t max_position = 0;
  uint64_t last_position = 0;
  uint32_t last_status = 0;
  uint32_t last_emit_ms = 0;
  uint64_t emitted_calls = 0;
};

std::mutex& HaloFilePositionStatsMutex() {
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, HaloFilePositionStats>&
HaloFilePositionStatsByPath() {
  static std::unordered_map<std::string, HaloFilePositionStats> stats;
  return stats;
}

uint32_t HaloIoInfoSummaryIntervalMs() {
  return cvars::halo_android_io_summary_ms ? cvars::halo_android_io_summary_ms
                                           : 5000;
}

void TrackHaloFilePosition(std::string_view path, X_STATUS result,
                           uint64_t old_position, uint64_t new_position) {
  if (!cvars::halo_android_diagnostics || !ShouldLogHaloIoInfoPath(path)) {
    return;
  }

  std::lock_guard<std::mutex> lock(HaloFilePositionStatsMutex());
  auto& stats = HaloFilePositionStatsByPath()[std::string(path)];
  ++stats.calls;
  if (new_position < old_position) {
    ++stats.backward_seeks;
  } else if (new_position > old_position) {
    ++stats.forward_seeks;
  }
  stats.max_position = std::max(stats.max_position, new_position);
  stats.last_position = new_position;
  stats.last_status = static_cast<uint32_t>(result);

  uint32_t uptime_ms = Clock::QueryGuestUptimeMillis();
  if (!stats.last_emit_ms) {
    stats.last_emit_ms = uptime_ms;
    return;
  }
  if (uptime_ms - stats.last_emit_ms < HaloIoInfoSummaryIntervalMs()) {
    return;
  }

  XELOGI(
      "HaloReach FilePositionSummary path='{}' dt_ms={} calls={} (+{}) "
      "forward={} backward={} max_position=0x{:X} last_position=0x{:X} "
      "last_status=0x{:08X}",
      path, uptime_ms - stats.last_emit_ms, stats.calls,
      stats.calls - stats.emitted_calls, stats.forward_seeks,
      stats.backward_seeks, stats.max_position, stats.last_position,
      stats.last_status);
  stats.last_emit_ms = uptime_ms;
  stats.emitted_calls = stats.calls;
}

const char* FileInformationClassName(uint32_t info_class) {
  switch (info_class) {
    case XFileBasicInformation:
      return "Basic";
    case XFileDispositionInformation:
      return "Disposition";
    case XFilePositionInformation:
      return "Position";
    case XFileAllocationInformation:
      return "Allocation";
    case XFileEndOfFileInformation:
      return "EndOfFile";
    case XFileNetworkOpenInformation:
      return "NetworkOpen";
    case XFileSectorInformation:
      return "Sector";
    default:
      return "Other";
  }
}

}  // namespace

uint32_t GetQueryFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileInternalInformation:
      return sizeof(X_FILE_INTERNAL_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileXctdCompressionInformation:
      return sizeof(X_FILE_XCTD_COMPRESSION_INFORMATION);
    case XFileNetworkOpenInformation:
      return sizeof(X_FILE_NETWORK_OPEN_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    case XFileModeInformation:
    case XFileAlignmentInformation:
    case XFileSectorInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileNameInformation:
    case XFileAllocationInformation:
      return 8;
    case XFileBasicInformation:
      return 40;
    default:
      return 0;
  }
}

dword_result_t NtQueryInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetQueryFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;
  uint64_t log_value0 = 0;
  uint64_t log_value1 = 0;
  const char* log_value0_name = "value0";
  const char* log_value1_name = "value1";

  switch (info_class) {
    case XFileInternalInformation: {
      // Internal unique file pointer. Not sure why anyone would want this.
      // TODO(benvanik): use pointer to fs::entry?
      auto info = info_ptr.as<X_FILE_INTERNAL_INFORMATION*>();
      info->index_number = xe::memory::hash_combine(0, file->path());
      out_length = sizeof(*info);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      info->current_byte_offset = file->position();
      log_value0 = info->current_byte_offset;
      log_value0_name = "position";
      out_length = sizeof(*info);
      break;
    }
    case XFileAlignmentInformation: {
      // Requested by XMountUtilityDrive XAM-task
      auto info = info_ptr.as<uint32_t*>();
      *info = 0;  // FILE_BYTE_ALIGNMENT?
      out_length = sizeof(*info);
      break;
    }
    case XFileSectorInformation: {
      auto info = info_ptr.as<uint32_t*>();
      const size_t bytes_per_sector =
          std::max<size_t>(1, file->device()->bytes_per_sector());
      if (auto* disc_entry =
              dynamic_cast<xe::vfs::DiscImageEntry*>(file->entry())) {
        *info = static_cast<uint32_t>(disc_entry->data_offset() /
                                      bytes_per_sector);
      } else {
        *info = static_cast<uint32_t>(bytes_per_sector);
      }
      log_value0 = *info;
      log_value0_name = "sector";
      out_length = sizeof(uint32_t);
      break;
    }
    case XFileXctdCompressionInformation: {
      XELOGE(
          "NtQueryInformationFile(XFileXctdCompressionInformation) "
          "unimplemented");
      // Files that are XCTD compressed begin with the magic 0x0FF512ED but we
      // shouldn't detect this that way. There's probably a flag somewhere
      // (attributes?) that defines if it's compressed or not.
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    };
    case XFileNetworkOpenInformation: {
      // Make sure we're working with up-to-date information, just in case the
      // file size has changed via something other than NtSetInfoFile
      // (eg. seems NtWriteFile might extend the file in some cases)
      file->entry()->update();

      auto info = info_ptr.as<X_FILE_NETWORK_OPEN_INFORMATION*>();
      info->creation_time = file->entry()->create_timestamp();
      info->last_access_time = file->entry()->access_timestamp();
      info->last_write_time = file->entry()->write_timestamp();
      info->change_time = file->entry()->write_timestamp();
      info->allocation_size = file->entry()->allocation_size();
      info->end_of_file = file->entry()->size();
      info->attributes = file->entry()->attributes();
      log_value0 = info->end_of_file;
      log_value0_name = "eof";
      log_value1 = info->allocation_size;
      log_value1_name = "allocation";
      out_length = sizeof(*info);
      break;
    }
    default: {
      // Unsupported, for now.
      assert_always();
      status = X_STATUS_INVALID_PARAMETER;
      out_length = 0;
      break;
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  if (ShouldLogHaloIoInfoPath(file->entry()->absolute_path())) {
    XELOGI(
        "HaloReach NtQueryInformationFile path='{}' class={}({}) "
        "result=0x{:08X} info=0x{:X} {}=0x{:X} {}=0x{:X} pos=0x{:X} "
        "size=0x{:X}",
        file->entry()->absolute_path(), static_cast<uint32_t>(info_class),
        FileInformationClassName(info_class), static_cast<uint32_t>(status),
        out_length, log_value0_name, log_value0, log_value1_name, log_value1,
        file->position(), file->entry()->size());
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryInformationFile, kFileSystem, kImplemented);

uint32_t GetSetFileInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileRenameInformation:
      return sizeof(X_FILE_RENAME_INFORMATION);
    case XFileDispositionInformation:
      return sizeof(X_FILE_DISPOSITION_INFORMATION);
    case XFilePositionInformation:
      return sizeof(X_FILE_POSITION_INFORMATION);
    case XFileCompletionInformation:
      return sizeof(X_FILE_COMPLETION_INFORMATION);
    case XFileAllocationInformation:
      return sizeof(X_FILE_ALLOCATION_INFORMATION);
    case XFileEndOfFileInformation:
      return sizeof(X_FILE_END_OF_FILE_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    case XFileModeInformation:
    case XFileIoPriorityInformation:
      return 4;
    case XFileMountPartitionInformation:
      return 8;
    case XFileLinkInformation:
      return 16;
    case XFileBasicInformation:
      return 40;
    case XFileMountPartitionsInformation:
      return 152;
    default:
      return 0;
  }
}

dword_result_t NtSetInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetSetFileInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  const bool mutates_vfs =
      info_class == XFileBasicInformation ||
      info_class == XFileRenameInformation ||
      info_class == XFileDispositionInformation ||
      info_class == XFileAllocationInformation ||
      info_class == XFileEndOfFileInformation;
  ReversibleAdmissionGate::Lease kernel_completion_admission;
  ReversibleAdmissionGate::Lease vfs_write_admission;
  if (mutates_vfs) {
    kernel_completion_admission =
        kernel_state()->AcquireSaveStateKernelDispatchTimerAdmission();
    vfs_write_admission =
        kernel_state()->file_system()->AcquireSaveStateGuestWriteAdmission();
  }

  X_STATUS result = X_STATUS_SUCCESS;
  uint32_t out_length;
  uint64_t log_value0 = 0;
  uint64_t log_value1 = 0;
  const char* log_value0_name = "value0";
  const char* log_value1_name = "value1";
  const uint64_t position_before = file->position();

  switch (info_class) {
    case XFileBasicInformation: {
      auto info = info_ptr.as<X_FILE_BASIC_INFORMATION*>();
      log_value0 = info->attributes;
      log_value0_name = "attributes";

      bool basic_result = true;
      if (info->creation_time) {
        basic_result &= file->entry()->SetCreateTimestamp(info->creation_time);
      }

      if (info->last_access_time) {
        basic_result &=
            file->entry()->SetAccessTimestamp(info->last_access_time);
      }

      if (info->last_write_time) {
        basic_result &= file->entry()->SetWriteTimestamp(info->last_write_time);
      }

      basic_result &= file->entry()->SetAttributes(info->attributes);
      if (!basic_result) {
        result = X_STATUS_UNSUCCESSFUL;
      }

      out_length = sizeof(*info);
      break;
    }
    case XFileRenameInformation: {
      auto info = info_ptr.as<X_FILE_RENAME_INFORMATION*>();
      // Compute path, possibly attrs relative.
      std::filesystem::path target_path =
          util::TranslateAnsiPath(kernel_memory(), &info->ansi_string);

      // Place IsValidPath in path from where it can be accessed everywhere
      if (!IsValidPath(target_path.string(), false)) {
        return X_STATUS_OBJECT_NAME_INVALID;
      }

      if (!target_path.has_filename()) {
        return X_STATUS_INVALID_PARAMETER;
      }

      file->Rename(target_path);
      out_length = sizeof(*info);
      break;
    }
    case XFileDispositionInformation: {
      auto info = info_ptr.as<X_FILE_DISPOSITION_INFORMATION*>();
      bool delete_on_close = info->delete_file ? true : false;
      log_value0 = info->delete_file;
      log_value0_name = "delete";
      if (delete_on_close && !file->entry()->parent()) {
        result = X_STATUS_ACCESS_DENIED;
        out_length = 0;
        XELOGW("NtSetInformationFile ignoring delete-on-close for root path {}",
               file->path());
        break;
      }
      file->entry()->SetForDeletion(static_cast<bool>(info->delete_file));
      out_length = 0;
      XELOGW("NtSetInformationFile set deleting flag for {} on close to: {}",
             file->name(), delete_on_close);
      break;
    }
    case XFilePositionInformation: {
      auto info = info_ptr.as<X_FILE_POSITION_INFORMATION*>();
      log_value0 = info->current_byte_offset;
      log_value0_name = "position";
      file->set_position(info->current_byte_offset);
      out_length = sizeof(*info);
      break;
    }
    case XFileAllocationInformation: {
      auto info = info_ptr.as<X_FILE_ALLOCATION_INFORMATION*>();
      log_value0 = info->allocation_size;
      log_value0_name = "allocation";
      result = file->SetLength(info->allocation_size);
      out_length = sizeof(*info);

      // Update the files vfs::Entry information
      file->entry()->update();
      break;
    }
    case XFileEndOfFileInformation: {
      auto info = info_ptr.as<X_FILE_END_OF_FILE_INFORMATION*>();
      log_value0 = info->end_of_file;
      log_value0_name = "eof";
      result = file->SetLength(info->end_of_file);
      out_length = sizeof(*info);

      // Update the files vfs::Entry information
      file->entry()->update();
      break;
    }
    case XFileCompletionInformation: {
      // Info contains IO Completion handle and completion key
      auto info = info_ptr.as<X_FILE_COMPLETION_INFORMATION*>();
      auto handle = uint32_t(info->handle);
      auto key = uint32_t(info->key);
      log_value0 = handle;
      log_value0_name = "completion_handle";
      log_value1 = key;
      log_value1_name = "completion_key";
      out_length = sizeof(*info);
      auto port =
          kernel_state()->object_table()->LookupObject<XIOCompletion>(handle);
      if (!port) {
        result = X_STATUS_INVALID_HANDLE;
      } else {
        file->RegisterIOCompletionPort(key, port);
      }
      break;
    }
    default:
      // Unsupported, for now.
      assert_always();
      out_length = 0;
      break;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = out_length;
  }

  if (info_class == XFilePositionInformation) {
    TrackHaloFilePosition(file->absolute_path(), result, position_before,
                          file->position());
  }

  if (ShouldLogHaloIoInfoPath(file->absolute_path()) &&
      (cvars::halo_android_io_verbose || XFAILED(result) ||
       info_class != XFilePositionInformation)) {
    XELOGI(
        "HaloReach NtSetInformationFile path='{}' class={}({}) "
        "result=0x{:08X} info=0x{:X} {}=0x{:X} {}=0x{:X} "
        "pos=0x{:X}->0x{:X} size=0x{:X}",
        file->absolute_path(), static_cast<uint32_t>(info_class),
        FileInformationClassName(info_class), static_cast<uint32_t>(result),
        out_length, log_value0_name, log_value0, log_value1_name, log_value1,
        position_before, file->position(), file->entry()->size());
  }

  return result;
}
DECLARE_XBOXKRNL_EXPORT2(NtSetInformationFile, kFileSystem, kImplemented,
                         kHighFrequency);

uint32_t GetQueryVolumeInfoMinimumLength(uint32_t info_class) {
  switch (info_class) {
    case XFileFsVolumeInformation:
      return sizeof(X_FILE_FS_VOLUME_INFORMATION);
    case XFileFsSizeInformation:
      return sizeof(X_FILE_FS_SIZE_INFORMATION);
    case XFileFsDeviceInformation:
      return sizeof(X_FILE_FS_DEVICE_INFORMATION);
    case XFileFsAttributeInformation:
      return sizeof(X_FILE_FS_ATTRIBUTE_INFORMATION);
    // TODO(gibbed): structures to get the size of.
    default:
      XELOGW("Unimplemented Info Class: 0x{:08x}", info_class);
      return 0;
  }
}

dword_result_t NtQueryVolumeInformationFile_entry(
    dword_t file_handle, pointer_t<X_IO_STATUS_BLOCK> io_status_block_ptr,
    lpvoid_t info_ptr, dword_t info_length, dword_t info_class) {
  uint32_t minimum_length = GetQueryVolumeInfoMinimumLength(info_class);
  if (!minimum_length) {
    return X_STATUS_INVALID_INFO_CLASS;
  }

  if (info_length < minimum_length) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  auto file = kernel_state()->object_table()->LookupObject<XFile>(file_handle);
  if (!file) {
    return X_STATUS_INVALID_HANDLE;
  }

  info_ptr.Zero(info_length);

  X_STATUS status = X_STATUS_SUCCESS;
  uint32_t out_length;

  switch (info_class) {
    case XFileFsVolumeInformation: {
      auto info = info_ptr.as<X_FILE_FS_VOLUME_INFORMATION*>();
      info->creation_time = 0;
      info->serial_number = 0;  // set for FATX, but we don't do that currently
      info->supports_objects = 0;
      info->label_length = 0;
      out_length = offsetof(X_FILE_FS_VOLUME_INFORMATION, label);
      break;
    }
    case XFileFsSizeInformation: {
      auto device = file->device();
      auto info = info_ptr.as<X_FILE_FS_SIZE_INFORMATION*>();
      info->total_allocation_units = device->total_allocation_units();
      info->available_allocation_units = device->available_allocation_units();
      info->sectors_per_allocation_unit = device->sectors_per_allocation_unit();
      info->bytes_per_sector = device->bytes_per_sector();
      // TODO(gibbed): sanity check, XCTD userland code seems to require this.
      assert_true(info->bytes_per_sector == 0x200);
      out_length = sizeof(*info);
      break;
    }
    case XFileFsAttributeInformation: {
      auto device = file->device();
      const auto& name = device->name();
      auto info = info_ptr.as<X_FILE_FS_ATTRIBUTE_INFORMATION*>();
      info->attributes = device->attributes();
      info->component_name_max_length = device->component_name_max_length();
      info->name_length = uint32_t(name.size());
      if (info_length >= 12 + name.size()) {
        std::memcpy(info->name, name.data(), name.size());
        out_length =
            offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name) + info->name_length;
      } else {
        status = X_STATUS_BUFFER_OVERFLOW;
        out_length = offsetof(X_FILE_FS_ATTRIBUTE_INFORMATION, name);
      }
      break;
    }
    case XFileFsDeviceInformation: {
      auto info = info_ptr.as<X_FILE_FS_DEVICE_INFORMATION*>();
      auto file_device = file->device();
      XELOGW("Stub XFileFsDeviceInformation!");
      info->device_type =
          FILE_DEVICE_UNKNOWN;  // 415608D8 checks for FILE_DEVICE_EHSTOR;
      info->characteristics = 0;
      out_length = sizeof(X_FILE_FS_DEVICE_INFORMATION);
      break;
    }
    default: {
      assert_always();
      out_length = 0;
      break;
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = status;
    io_status_block_ptr->information = out_length;
  }

  if (ShouldLogHaloIoInfoPath(file->entry()->absolute_path())) {
    XELOGI(
        "HaloReach NtQueryVolumeInformationFile path='{}' class={} "
        "result=0x{:08X} info=0x{:X}",
        file->entry()->absolute_path(), static_cast<uint32_t>(info_class),
        static_cast<uint32_t>(status), out_length);
  }

  return status;
}
DECLARE_XBOXKRNL_EXPORT1(NtQueryVolumeInformationFile, kFileSystem,
                         kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(IoInfo);
