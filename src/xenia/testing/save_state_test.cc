/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "third_party/catch/include/catch.hpp"
#include "xenia/save_state.h"

namespace xe::save_state::test {
namespace {

Sha256Digest Digest(uint8_t value) {
  Sha256Digest digest = {};
  digest.fill(value);
  return digest;
}

ContainerDirectory ValidDirectory() {
  ContainerDirectory directory;
  directory.header.title_id = 0x4D53085B;
  directory.header.process_session_id = UINT64_C(0x1020304050607080);
  directory.header.identity_sha256 = Digest(0x11);
  directory.header.payload_sha256 = Digest(0x22);
  directory.header.section_count = uint32_t(kRequiredSectionIds.size());

  uint64_t offset =
      kContainerHeaderSize +
      uint64_t(directory.header.section_count) * kSectionEntrySize;
  for (size_t index = 0; index < kRequiredSectionIds.size(); ++index) {
    SectionEntry section;
    section.id = kRequiredSectionIds[index];
    section.version = 1;
    section.flags = kSectionFlagRequired;
    section.offset = offset;
    section.size = 8;
    section.payload_sha256 = Digest(uint8_t(index + 1));
    directory.sections.push_back(section);
    offset += section.size;
  }
  directory.header.total_size = offset;
  return directory;
}

class TestQuiescenceProvider final : public QuiescenceProvider {
 public:
  QuiescenceResult enter_result = QuiescenceResult::kReached;
  uint32_t enter_count = 0;
  uint32_t leave_count = 0;
  uint64_t last_request_id = 0;

  QuiescenceResult Enter(Operation operation, uint64_t request_id,
                         std::string* error_message) override {
    (void)operation;
    ++enter_count;
    last_request_id = request_id;
    if (enter_result != QuiescenceResult::kReached && error_message) {
      *error_message = "boundary unavailable";
    }
    return enter_result;
  }

  void Leave(uint64_t request_id) noexcept override {
    ++leave_count;
    last_request_id = request_id;
  }
};

SlotDescriptor Slot(uint64_t generation = 1) {
  SlotDescriptor slot;
  slot.generation = generation;
  slot.process_session_id = UINT64_C(0x1020304050607080);
  slot.title_id = 0x4D53085B;
  slot.payload_sha256 = Digest(uint8_t(generation));
  return slot;
}

bool AdvanceSaveToResuming(Coordinator& coordinator, uint64_t request_id) {
  return coordinator.MarkQuiesced(request_id) &&
         coordinator.MarkTransferStarted(request_id) &&
         coordinator.MarkResuming(request_id);
}

}  // namespace

TEST_CASE("Save-state directory round-trips required subsystem metadata",
          "[save_state]") {
  ContainerDirectory source = ValidDirectory();
  std::vector<uint8_t> encoded;
  REQUIRE(EncodeContainerDirectory(source, &encoded).ok());

  ContainerDirectory decoded;
  REQUIRE(DecodeContainerDirectory(encoded.data(), encoded.size(),
                                   source.header.total_size, &decoded)
              .ok());
  CHECK(decoded.header.title_id == source.header.title_id);
  CHECK(decoded.header.process_session_id ==
        source.header.process_session_id);
  CHECK(decoded.sections.size() == kRequiredSectionIds.size());
  CHECK(decoded.sections.front().id == SectionId::kIdentity);
  CHECK(decoded.sections.back().id == SectionId::kVfs);
}

TEST_CASE("Save-state directory rejects incomplete and overlapping states",
          "[save_state]") {
  ContainerDirectory missing = ValidDirectory();
  missing.sections.pop_back();
  --missing.header.section_count;
  CHECK(ValidateContainerDirectory(missing, missing.header.total_size).error ==
        FormatError::kMissingRequiredSection);

  ContainerDirectory overlapping = ValidDirectory();
  overlapping.sections[1].offset = overlapping.sections[0].offset;
  CHECK(ValidateContainerDirectory(overlapping,
                                   overlapping.header.total_size)
            .error == FormatError::kOverlappingSections);
}

TEST_CASE("Coordinator refuses restore before a verified slot is committed",
          "[save_state]") {
  Coordinator coordinator(UINT64_C(0x1020304050607080), 0x4D53085B);
  BeginResult restore = coordinator.Begin(Operation::kRestore);
  CHECK_FALSE(restore.accepted());
  CHECK(restore.error == CoordinatorError::kNoSlot);

  BeginResult save = coordinator.Begin(Operation::kSave);
  REQUIRE(save.accepted());
  REQUIRE(AdvanceSaveToResuming(coordinator, save.request_id));
  REQUIRE(coordinator.CommitSave(save.request_id, Slot()));

  restore = coordinator.Begin(Operation::kRestore);
  CHECK(restore.accepted());
}

TEST_CASE("Failed replacement save preserves the last verified slot",
          "[save_state]") {
  Coordinator coordinator(UINT64_C(0x1020304050607080), 0x4D53085B);
  BeginResult first = coordinator.Begin(Operation::kSave);
  REQUIRE(first.accepted());
  REQUIRE(AdvanceSaveToResuming(coordinator, first.request_id));
  REQUIRE(coordinator.CommitSave(first.request_id, Slot(1)));

  BeginResult replacement = coordinator.Begin(Operation::kSave);
  REQUIRE(replacement.accepted());
  REQUIRE(coordinator.MarkQuiesced(replacement.request_id));
  REQUIRE(coordinator.Fail(replacement.request_id,
                           CoordinatorError::kSerializationFailed,
                           "memory section failed"));

  REQUIRE(coordinator.slot().has_value());
  CHECK(coordinator.slot()->generation == 1);
  CHECK(coordinator.status().phase == Phase::kFailed);
}

TEST_CASE("Coordinator rejects concurrent and stale transitions",
          "[save_state]") {
  Coordinator coordinator(UINT64_C(0x1020304050607080), 0x4D53085B);
  BeginResult save = coordinator.Begin(Operation::kSave);
  REQUIRE(save.accepted());
  CHECK(coordinator.Begin(Operation::kSave).error ==
        CoordinatorError::kBusy);
  CHECK_FALSE(coordinator.MarkQuiesced(save.request_id + 1));
  CHECK_FALSE(coordinator.MarkTransferStarted(save.request_id));
  CHECK(coordinator.MarkQuiesced(save.request_id));
}

TEST_CASE("Quiescence lease releases a reached boundary exactly once",
          "[save_state]") {
  TestQuiescenceProvider provider;
  {
    QuiescenceLease lease(provider);
    REQUIRE(lease.Enter(Operation::kSave, 7, nullptr) ==
            QuiescenceResult::kReached);
    CHECK(lease.active());
    lease.Release();
    lease.Release();
  }
  CHECK(provider.enter_count == 1);
  CHECK(provider.leave_count == 1);
  CHECK(provider.last_request_id == 7);

  provider.enter_result = QuiescenceResult::kUnsupported;
  {
    QuiescenceLease lease(provider);
    std::string error;
    CHECK(lease.Enter(Operation::kSave, 8, &error) ==
          QuiescenceResult::kUnsupported);
    CHECK_FALSE(lease.active());
    CHECK_FALSE(error.empty());
  }
  CHECK(provider.leave_count == 1);
}

}  // namespace xe::save_state::test
