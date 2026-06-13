/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_file.h"

#include <string_view>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/vfs/devices/disc_image_device.h"
#include "xenia/vfs/devices/disc_image_entry.h"

DECLARE_bool(halo_android_io_verbose);

namespace xe {
namespace vfs {

namespace {

bool ShouldLogHaloDiscRead(std::string_view path) {
  return xe::utf8::find_first_of_case(path, "maps") != std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "mainmenu") !=
             std::string_view::npos ||
         xe::utf8::find_first_of_case(path, "bink") != std::string_view::npos;
}

}  // namespace

DiscImageFile::DiscImageFile(uint32_t file_access, DiscImageEntry* entry)
    : File(file_access, entry), entry_(entry) {}

DiscImageFile::~DiscImageFile() = default;

void DiscImageFile::Destroy() { delete this; }

X_STATUS DiscImageFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_read) {
  const bool log_halo_read =
      cvars::halo_android_io_verbose &&
      ShouldLogHaloDiscRead(entry_->absolute_path());
  if (byte_offset >= entry_->size()) {
    if (log_halo_read) {
      XELOGI(
          "HaloReach VFS disc read EOF path='{}' offset=0x{:X} request=0x{:X} "
          "size=0x{:X}",
          entry_->absolute_path(), byte_offset, buffer.size(), entry_->size());
    }
    return X_STATUS_END_OF_FILE;
  }

  if (entry_->mmap() && entry_->data_offset() >= entry_->mmap()->size()) {
    xe::FatalError("This ISO image is corrupted and cannot be played.");
    return X_STATUS_END_OF_FILE;
  }

  size_t real_offset = entry_->data_offset() + byte_offset;
  size_t real_length =
      std::min(buffer.size(), entry_->data_size() - byte_offset);
  if (entry_->mmap()) {
    std::memcpy(buffer.data(), entry_->mmap()->data() + real_offset,
                real_length);
  } else {
    auto* device = static_cast<DiscImageDevice*>(entry_->device());
    if (!device ||
        !device->ReadImage(real_offset, buffer.data(), real_length)) {
      if (log_halo_read) {
        XELOGI(
            "HaloReach VFS disc read failed path='{}' offset=0x{:X} "
            "request=0x{:X} real_offset=0x{:X} real_length=0x{:X}",
            entry_->absolute_path(), byte_offset, buffer.size(), real_offset,
            real_length);
      }
      return X_STATUS_UNSUCCESSFUL;
    }
  }
  if (log_halo_read &&
      (byte_offset < 0x10000 || real_length != buffer.size() ||
       real_length == 0)) {
    XELOGI(
        "HaloReach VFS disc read path='{}' offset=0x{:X} request=0x{:X} "
        "bytes=0x{:X} real_offset=0x{:X} file_size=0x{:X}",
        entry_->absolute_path(), byte_offset, buffer.size(), real_length,
        real_offset, entry_->size());
  }
  *out_bytes_read = real_length;
  return X_STATUS_SUCCESS;
}

}  // namespace vfs
}  // namespace xe
