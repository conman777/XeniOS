/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/host_path_file.h"

#include <cerrno>
#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/vfs/devices/host_path_entry.h"

namespace xe {
namespace vfs {

HostPathFile::HostPathFile(
    uint32_t file_access, HostPathEntry* entry,
    std::unique_ptr<xe::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {}

HostPathFile::~HostPathFile() = default;

void HostPathFile::Destroy() {
  delete this;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  if (!(file_access_ &
        (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Read(byte_offset, buffer.data(), buffer.size(),
                         out_bytes_read)) {
    return X_STATUS_SUCCESS;
  } else {
    const int error_code = errno;
    XELOGW(
        "HostPathFile::ReadSync failed path='{}' offset=0x{:X} length=0x{:X} "
        "buffer=0x{:X} errno={} ({})",
        entry()->absolute_path(), byte_offset, buffer.size(),
        reinterpret_cast<uintptr_t>(buffer.data()), error_code,
        std::strerror(error_code));
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer,
                                 size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                        FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(),
                          out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    const int error_code = errno;
    XELOGW(
        "HostPathFile::WriteSync failed path='{}' offset=0x{:X} length=0x{:X} "
        "buffer=0x{:X} errno={} ({})",
        entry()->absolute_path(), byte_offset, buffer.size(),
        reinterpret_cast<uintptr_t>(buffer.data()), error_code,
        std::strerror(error_code));
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace vfs
}  // namespace xe
