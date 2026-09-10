/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/save_state.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace xe::save_state {
namespace {

template <typename T>
void WriteLittleEndian(std::vector<uint8_t>& output, size_t offset, T value) {
  for (size_t index = 0; index < sizeof(T); ++index) {
    output[offset + index] =
        uint8_t((uint64_t(value) >> (index * 8)) & UINT64_C(0xFF));
  }
}

template <typename T>
T ReadLittleEndian(const uint8_t* data, size_t offset) {
  uint64_t value = 0;
  for (size_t index = 0; index < sizeof(T); ++index) {
    value |= uint64_t(data[offset + index]) << (index * 8);
  }
  return T(value);
}

FormatValidation Error(FormatError error, std::string message) {
  return {error, std::move(message)};
}

bool IsKnownSection(uint32_t section_id) {
  return std::find_if(
             kRequiredSectionIds.begin(), kRequiredSectionIds.end(),
             [section_id](SectionId required_id) {
               return uint32_t(required_id) == section_id;
             }) != kRequiredSectionIds.end();
}

bool AddWouldOverflow(uint64_t first, uint64_t second) {
  return second > std::numeric_limits<uint64_t>::max() - first;
}

}  // namespace

bool HasDigest(const Sha256Digest& digest) {
  return std::any_of(digest.begin(), digest.end(),
                     [](uint8_t byte) { return byte != 0; });
}

bool SlotDescriptor::IsValid() const {
  return generation != 0 && process_session_id != 0 && title_id != 0 &&
         HasDigest(payload_sha256);
}

FormatValidation ValidateContainerDirectory(
    const ContainerDirectory& directory, uint64_t file_size) {
  const ContainerHeader& header = directory.header;
  if (header.magic != kContainerMagic) {
    return Error(FormatError::kInvalidMagic,
                 "Save-state container magic does not match.");
  }
  if (header.version_major != kContainerVersionMajor ||
      header.version_minor > kContainerVersionMinor) {
    return Error(FormatError::kUnsupportedVersion,
                 "Save-state container version is unsupported.");
  }
  if (header.header_size != kContainerHeaderSize) {
    return Error(FormatError::kInvalidHeaderSize,
                 "Save-state header size is invalid.");
  }
  if (header.section_entry_size != kSectionEntrySize) {
    return Error(FormatError::kInvalidSectionEntrySize,
                 "Save-state section entry size is invalid.");
  }
  if (header.flags != kContainerFlagSameProcessOnly) {
    return Error(FormatError::kUnsupportedFlags,
                 "Save-state container flags are unsupported.");
  }
  if (header.section_count != directory.sections.size() ||
      header.section_count > kMaximumSectionCount) {
    return Error(FormatError::kInvalidSectionCount,
                 "Save-state section count is invalid.");
  }
  if (header.title_id == 0) {
    return Error(FormatError::kInvalidTitle,
                 "Save-state title identity is missing.");
  }
  if (header.process_session_id == 0) {
    return Error(FormatError::kInvalidProcessSession,
                 "Save-state process session identity is missing.");
  }
  if (!HasDigest(header.identity_sha256) ||
      !HasDigest(header.payload_sha256)) {
    return Error(FormatError::kMissingDigest,
                 "Save-state container digest is missing.");
  }

  const uint64_t directory_size =
      uint64_t(kContainerHeaderSize) +
      uint64_t(header.section_count) * uint64_t(kSectionEntrySize);
  if (header.total_size != file_size || header.total_size < directory_size) {
    return Error(FormatError::kInvalidTotalSize,
                 "Save-state total size is invalid.");
  }

  std::vector<uint32_t> section_ids;
  section_ids.reserve(directory.sections.size());
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  ranges.reserve(directory.sections.size());
  for (const SectionEntry& section : directory.sections) {
    const uint32_t section_id = uint32_t(section.id);
    if (std::find(section_ids.begin(), section_ids.end(), section_id) !=
        section_ids.end()) {
      return Error(FormatError::kDuplicateSection,
                   "Save-state contains a duplicate section.");
    }
    section_ids.push_back(section_id);

    if (!IsKnownSection(section_id) &&
        (section.flags & kSectionFlagRequired) != 0) {
      return Error(FormatError::kUnsupportedRequiredSection,
                   "Save-state contains an unknown required section.");
    }
    if ((section.flags & ~kSectionFlagRequired) != 0) {
      return Error(FormatError::kInvalidSectionFlags,
                   "Save-state section flags are invalid.");
    }
    if (section.version == 0) {
      return Error(FormatError::kInvalidSectionVersion,
                   "Save-state section version is invalid.");
    }
    if (!HasDigest(section.payload_sha256)) {
      return Error(FormatError::kMissingDigest,
                   "Save-state section digest is missing.");
    }
    if (section.offset < directory_size || (section.offset & 7) != 0 ||
        section.size == 0 || AddWouldOverflow(section.offset, section.size) ||
        section.offset + section.size > header.total_size) {
      return Error(FormatError::kInvalidSectionRange,
                   "Save-state section range is invalid.");
    }
    ranges.emplace_back(section.offset, section.offset + section.size);
  }

  for (SectionId required_id : kRequiredSectionIds) {
    auto found = std::find_if(
        directory.sections.begin(), directory.sections.end(),
        [required_id](const SectionEntry& section) {
          return section.id == required_id &&
                 (section.flags & kSectionFlagRequired) != 0;
        });
    if (found == directory.sections.end()) {
      return Error(FormatError::kMissingRequiredSection,
                   "Save-state is missing a required subsystem section.");
    }
  }

  std::sort(ranges.begin(), ranges.end());
  for (size_t index = 1; index < ranges.size(); ++index) {
    if (ranges[index].first < ranges[index - 1].second) {
      return Error(FormatError::kOverlappingSections,
                   "Save-state sections overlap.");
    }
  }

  return {};
}

FormatValidation EncodeContainerDirectory(
    const ContainerDirectory& directory, std::vector<uint8_t>* encoded) {
  if (!encoded) {
    return Error(FormatError::kTruncatedDirectory,
                 "Save-state output directory is missing.");
  }
  FormatValidation validation =
      ValidateContainerDirectory(directory, directory.header.total_size);
  if (!validation.ok()) {
    return validation;
  }

  const size_t directory_size =
      size_t(kContainerHeaderSize) +
      directory.sections.size() * size_t(kSectionEntrySize);
  encoded->assign(directory_size, 0);
  std::vector<uint8_t>& output = *encoded;
  const ContainerHeader& header = directory.header;
  WriteLittleEndian(output, 0, header.magic);
  WriteLittleEndian(output, 4, header.version_major);
  WriteLittleEndian(output, 6, header.version_minor);
  WriteLittleEndian(output, 8, header.header_size);
  WriteLittleEndian(output, 12, header.section_entry_size);
  WriteLittleEndian(output, 16, header.flags);
  WriteLittleEndian(output, 20, header.section_count);
  WriteLittleEndian(output, 24, header.title_id);
  WriteLittleEndian(output, 32, header.process_session_id);
  WriteLittleEndian(output, 40, header.total_size);
  std::copy(header.identity_sha256.begin(), header.identity_sha256.end(),
            output.begin() + 48);
  std::copy(header.payload_sha256.begin(), header.payload_sha256.end(),
            output.begin() + 80);

  for (size_t index = 0; index < directory.sections.size(); ++index) {
    const SectionEntry& section = directory.sections[index];
    const size_t offset =
        size_t(kContainerHeaderSize) + index * size_t(kSectionEntrySize);
    WriteLittleEndian(output, offset + 0, uint32_t(section.id));
    WriteLittleEndian(output, offset + 4, section.version);
    WriteLittleEndian(output, offset + 8, section.flags);
    WriteLittleEndian(output, offset + 16, section.offset);
    WriteLittleEndian(output, offset + 24, section.size);
    std::copy(section.payload_sha256.begin(), section.payload_sha256.end(),
              output.begin() + offset + 32);
  }
  return {};
}

FormatValidation DecodeContainerDirectory(
    const uint8_t* data, size_t data_size, uint64_t file_size,
    ContainerDirectory* directory) {
  if (!data || !directory || data_size < kContainerHeaderSize) {
    return Error(FormatError::kTruncatedDirectory,
                 "Save-state directory is truncated.");
  }

  ContainerDirectory decoded;
  ContainerHeader& header = decoded.header;
  header.magic = ReadLittleEndian<uint32_t>(data, 0);
  header.version_major = ReadLittleEndian<uint16_t>(data, 4);
  header.version_minor = ReadLittleEndian<uint16_t>(data, 6);
  header.header_size = ReadLittleEndian<uint32_t>(data, 8);
  header.section_entry_size = ReadLittleEndian<uint32_t>(data, 12);
  header.flags = ReadLittleEndian<uint32_t>(data, 16);
  header.section_count = ReadLittleEndian<uint32_t>(data, 20);
  header.title_id = ReadLittleEndian<uint32_t>(data, 24);
  header.process_session_id = ReadLittleEndian<uint64_t>(data, 32);
  header.total_size = ReadLittleEndian<uint64_t>(data, 40);
  std::copy(data + 48, data + 80, header.identity_sha256.begin());
  std::copy(data + 80, data + 112, header.payload_sha256.begin());

  if (header.section_count > kMaximumSectionCount) {
    return Error(FormatError::kInvalidSectionCount,
                 "Save-state section count is invalid.");
  }
  const uint64_t directory_size =
      uint64_t(kContainerHeaderSize) +
      uint64_t(header.section_count) * uint64_t(kSectionEntrySize);
  if (directory_size > data_size) {
    return Error(FormatError::kTruncatedDirectory,
                 "Save-state section directory is truncated.");
  }

  decoded.sections.reserve(header.section_count);
  for (uint32_t index = 0; index < header.section_count; ++index) {
    const size_t offset =
        size_t(kContainerHeaderSize) + size_t(index) * kSectionEntrySize;
    SectionEntry section;
    section.id = SectionId(ReadLittleEndian<uint32_t>(data, offset + 0));
    section.version = ReadLittleEndian<uint32_t>(data, offset + 4);
    section.flags = ReadLittleEndian<uint32_t>(data, offset + 8);
    section.offset = ReadLittleEndian<uint64_t>(data, offset + 16);
    section.size = ReadLittleEndian<uint64_t>(data, offset + 24);
    std::copy(data + offset + 32, data + offset + 64,
              section.payload_sha256.begin());
    decoded.sections.push_back(section);
  }

  FormatValidation validation =
      ValidateContainerDirectory(decoded, file_size);
  if (!validation.ok()) {
    return validation;
  }
  *directory = std::move(decoded);
  return {};
}

Coordinator::Coordinator(uint64_t process_session_id, uint32_t title_id)
    : process_session_id_(process_session_id), title_id_(title_id) {}

bool Coordinator::IsActive(Phase phase) {
  return phase == Phase::kWaitingForQuiescentBoundary ||
         phase == Phase::kQuiesced || phase == Phase::kTransferringState ||
         phase == Phase::kResuming;
}

bool Coordinator::IsCurrentRequest(uint64_t request_id) const {
  return request_id != 0 && status_.request_id == request_id;
}

BeginResult Coordinator::Begin(Operation operation) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (process_session_id_ == 0 || title_id_ == 0) {
    return {0, CoordinatorError::kInvalidConfiguration};
  }
  if (IsActive(status_.phase)) {
    return {0, CoordinatorError::kBusy};
  }
  if (operation == Operation::kRestore && !slot_.has_value()) {
    return {0, CoordinatorError::kNoSlot};
  }

  uint64_t request_id = next_request_id_++;
  if (request_id == 0) {
    request_id = next_request_id_++;
  }
  status_ = {};
  status_.request_id = request_id;
  status_.operation = operation;
  status_.phase = Phase::kWaitingForQuiescentBoundary;
  return {request_id, CoordinatorError::kNone};
}

bool Coordinator::Transition(uint64_t request_id, Phase expected, Phase next) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsCurrentRequest(request_id) || status_.phase != expected) {
    return false;
  }
  status_.phase = next;
  return true;
}

bool Coordinator::MarkQuiesced(uint64_t request_id) {
  return Transition(request_id, Phase::kWaitingForQuiescentBoundary,
                    Phase::kQuiesced);
}

bool Coordinator::MarkTransferStarted(uint64_t request_id) {
  return Transition(request_id, Phase::kQuiesced,
                    Phase::kTransferringState);
}

bool Coordinator::MarkResuming(uint64_t request_id) {
  return Transition(request_id, Phase::kTransferringState, Phase::kResuming);
}

bool Coordinator::CommitSave(uint64_t request_id,
                             const SlotDescriptor& slot) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsCurrentRequest(request_id) || status_.operation != Operation::kSave ||
      status_.phase != Phase::kResuming || !slot.IsValid() ||
      slot.process_session_id != process_session_id_ ||
      slot.title_id != title_id_) {
    return false;
  }
  slot_ = slot;
  status_.phase = Phase::kSucceeded;
  return true;
}

bool Coordinator::CompleteRestore(uint64_t request_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsCurrentRequest(request_id) ||
      status_.operation != Operation::kRestore ||
      status_.phase != Phase::kResuming || !slot_.has_value()) {
    return false;
  }
  status_.phase = Phase::kSucceeded;
  return true;
}

bool Coordinator::Fail(uint64_t request_id, CoordinatorError error,
                       std::string message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsCurrentRequest(request_id) || !IsActive(status_.phase) ||
      error == CoordinatorError::kNone) {
    return false;
  }
  status_.phase = Phase::kFailed;
  status_.error = error;
  status_.message = std::move(message);
  return true;
}

CoordinatorStatus Coordinator::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return status_;
}

std::optional<SlotDescriptor> Coordinator::slot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return slot_;
}

QuiescenceLease::QuiescenceLease(QuiescenceProvider& provider)
    : provider_(provider) {}

QuiescenceLease::~QuiescenceLease() { Release(); }

QuiescenceResult QuiescenceLease::Enter(Operation operation,
                                        uint64_t request_id,
                                        std::string* error_message) {
  if (active_) {
    return QuiescenceResult::kBusy;
  }
  QuiescenceResult result =
      provider_.Enter(operation, request_id, error_message);
  if (result == QuiescenceResult::kReached) {
    request_id_ = request_id;
    active_ = true;
  }
  return result;
}

void QuiescenceLease::Release() noexcept {
  if (!active_) {
    return;
  }
  const uint64_t request_id = request_id_;
  request_id_ = 0;
  active_ = false;
  provider_.Leave(request_id);
}

}  // namespace xe::save_state
