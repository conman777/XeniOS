/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_SAVE_STATE_H_
#define XENIA_SAVE_STATE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace xe::save_state {

constexpr uint32_t MakeLittleEndianFourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
         (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

constexpr uint32_t kContainerMagic =
    MakeLittleEndianFourCC('X', 'S', 'S', '1');
constexpr uint16_t kContainerVersionMajor = 1;
constexpr uint16_t kContainerVersionMinor = 0;
constexpr uint32_t kContainerHeaderSize = 112;
constexpr uint32_t kSectionEntrySize = 64;
constexpr uint32_t kMaximumSectionCount = 64;
constexpr uint32_t kContainerFlagSameProcessOnly = 1u << 0;
constexpr uint32_t kSectionFlagRequired = 1u << 0;

using Sha256Digest = std::array<uint8_t, 32>;

enum class SectionId : uint32_t {
  kIdentity = MakeLittleEndianFourCC('I', 'D', 'E', 'N'),
  kClock = MakeLittleEndianFourCC('T', 'I', 'M', 'E'),
  kProcessor = MakeLittleEndianFourCC('C', 'P', 'U', ' '),
  kMemory = MakeLittleEndianFourCC('M', 'E', 'M', ' '),
  kKernel = MakeLittleEndianFourCC('K', 'R', 'N', 'L'),
  kGraphics = MakeLittleEndianFourCC('G', 'P', 'U', ' '),
  kAudio = MakeLittleEndianFourCC('A', 'P', 'U', ' '),
  kInput = MakeLittleEndianFourCC('H', 'I', 'D', ' '),
  kVfs = MakeLittleEndianFourCC('V', 'F', 'S', ' '),
};

inline constexpr std::array<SectionId, 9> kRequiredSectionIds = {
    SectionId::kIdentity, SectionId::kClock,    SectionId::kProcessor,
    SectionId::kMemory,   SectionId::kKernel,   SectionId::kGraphics,
    SectionId::kAudio,    SectionId::kInput,    SectionId::kVfs,
};

struct ContainerHeader {
  uint32_t magic = kContainerMagic;
  uint16_t version_major = kContainerVersionMajor;
  uint16_t version_minor = kContainerVersionMinor;
  uint32_t header_size = kContainerHeaderSize;
  uint32_t section_entry_size = kSectionEntrySize;
  uint32_t flags = kContainerFlagSameProcessOnly;
  uint32_t section_count = 0;
  uint32_t title_id = 0;
  uint64_t process_session_id = 0;
  uint64_t total_size = 0;
  Sha256Digest identity_sha256 = {};
  Sha256Digest payload_sha256 = {};
};

struct SectionEntry {
  SectionId id = SectionId::kIdentity;
  uint32_t version = 0;
  uint32_t flags = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  Sha256Digest payload_sha256 = {};
};

struct ContainerDirectory {
  ContainerHeader header;
  std::vector<SectionEntry> sections;
};

enum class FormatError {
  kNone,
  kTruncatedDirectory,
  kInvalidMagic,
  kUnsupportedVersion,
  kInvalidHeaderSize,
  kInvalidSectionEntrySize,
  kUnsupportedFlags,
  kInvalidSectionCount,
  kInvalidTitle,
  kInvalidProcessSession,
  kInvalidTotalSize,
  kMissingDigest,
  kDuplicateSection,
  kMissingRequiredSection,
  kUnsupportedRequiredSection,
  kInvalidSectionVersion,
  kInvalidSectionFlags,
  kInvalidSectionRange,
  kOverlappingSections,
};

struct FormatValidation {
  FormatError error = FormatError::kNone;
  std::string message;

  bool ok() const { return error == FormatError::kNone; }
};

bool HasDigest(const Sha256Digest& digest);

FormatValidation ValidateContainerDirectory(
    const ContainerDirectory& directory, uint64_t file_size);

// Encodes only the fixed header and section directory. Section payloads are
// produced by future subsystem serializers and are deliberately out of scope
// for the Phase 1 coordinator.
FormatValidation EncodeContainerDirectory(
    const ContainerDirectory& directory, std::vector<uint8_t>* encoded);

// Decodes and validates the fixed header and section directory. data_size may
// be smaller than file_size, but it must contain the complete directory.
FormatValidation DecodeContainerDirectory(
    const uint8_t* data, size_t data_size, uint64_t file_size,
    ContainerDirectory* directory);

enum class Operation {
  kSave,
  kRestore,
};

enum class Phase {
  kIdle,
  kWaitingForQuiescentBoundary,
  kQuiesced,
  kTransferringState,
  kResuming,
  kSucceeded,
  kFailed,
};

enum class CoordinatorError {
  kNone,
  kBusy,
  kNoSlot,
  kInvalidConfiguration,
  kStaleRequest,
  kInvalidTransition,
  kInvalidSlot,
  kBoundaryUnsupported,
  kBoundaryFailed,
  kSerializationFailed,
  kValidationFailed,
  kRestoreFailed,
  kCancelled,
};

struct SlotDescriptor {
  uint64_t generation = 0;
  uint64_t process_session_id = 0;
  uint32_t title_id = 0;
  Sha256Digest payload_sha256 = {};

  bool IsValid() const;
};

struct CoordinatorStatus {
  uint64_t request_id = 0;
  Operation operation = Operation::kSave;
  Phase phase = Phase::kIdle;
  CoordinatorError error = CoordinatorError::kNone;
  std::string message;
};

struct BeginResult {
  uint64_t request_id = 0;
  CoordinatorError error = CoordinatorError::kNone;

  bool accepted() const { return request_id != 0; }
};

// Thread-safe lifecycle coordinator for one debug-only, same-process slot.
// This class intentionally has no serializer callbacks. A future operation
// runner may commit a save only after all required sections have been
// checksummed and atomically published.
class Coordinator {
 public:
  Coordinator(uint64_t process_session_id, uint32_t title_id);

  BeginResult Begin(Operation operation);
  bool MarkQuiesced(uint64_t request_id);
  bool MarkTransferStarted(uint64_t request_id);
  bool MarkResuming(uint64_t request_id);
  bool CommitSave(uint64_t request_id, const SlotDescriptor& slot);
  bool CompleteRestore(uint64_t request_id);
  bool Fail(uint64_t request_id, CoordinatorError error,
            std::string message);

  CoordinatorStatus status() const;
  std::optional<SlotDescriptor> slot() const;

 private:
  bool Transition(uint64_t request_id, Phase expected, Phase next);
  bool IsCurrentRequest(uint64_t request_id) const;
  static bool IsActive(Phase phase);

  const uint64_t process_session_id_;
  const uint32_t title_id_;
  mutable std::mutex mutex_;
  uint64_t next_request_id_ = 1;
  CoordinatorStatus status_;
  std::optional<SlotDescriptor> slot_;
};

enum class QuiescenceResult {
  kReached,
  kBusy,
  kUnsupported,
  kFailed,
};

// Implementations must stop all guest-visible mutation at a documented
// boundary. A failed Enter must leave the emulator running and unchanged.
// Reaching this boundary never means an arbitrary mid-frame Vulkan snapshot.
class QuiescenceProvider {
 public:
  virtual ~QuiescenceProvider() = default;

  virtual QuiescenceResult Enter(Operation operation, uint64_t request_id,
                                 std::string* error_message) = 0;
  virtual void Leave(uint64_t request_id) noexcept = 0;
};

// Ensures a successfully entered quiescent boundary is released exactly once
// on success, failure, cancellation, or stack unwinding.
class QuiescenceLease {
 public:
  explicit QuiescenceLease(QuiescenceProvider& provider);
  QuiescenceLease(const QuiescenceLease&) = delete;
  QuiescenceLease& operator=(const QuiescenceLease&) = delete;
  ~QuiescenceLease();

  QuiescenceResult Enter(Operation operation, uint64_t request_id,
                         std::string* error_message);
  void Release() noexcept;

  bool active() const { return active_; }
  uint64_t request_id() const { return request_id_; }

 private:
  QuiescenceProvider& provider_;
  uint64_t request_id_ = 0;
  bool active_ = false;
};

}  // namespace xe::save_state

#endif  // XENIA_SAVE_STATE_H_
