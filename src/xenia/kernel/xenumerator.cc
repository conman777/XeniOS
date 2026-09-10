/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xenumerator.h"

#include <limits>

#include "xenia/base/byte_stream.h"
#include "xenia/base/logging.h"

namespace xe {
namespace kernel {
namespace {

constexpr uint32_t kEnumeratorSaveSignature = 0x454E554D;
constexpr uint32_t kMaxSavedEnumeratorItems = 1024 * 1024;
constexpr uint32_t kMaxSavedEnumeratorBufferSize = 256 * 1024 * 1024;
constexpr uint32_t kMaxSavedEnumeratorStringLength = 1024 * 1024;

enum class SavedEnumeratorKind : uint32_t {
  kStaticUntyped = 1,
  kAchievement = 2,
  kTitle = 3,
  kUserStats = 4,
  kUserPlaylist = 5,
};

bool HasRemaining(const ByteStream* stream, size_t size) {
  return stream->offset() <= stream->data_length() &&
         size <= stream->data_length() - stream->offset();
}

bool ReadU16String(ByteStream* stream, std::u16string* value) {
  if (!HasRemaining(stream, sizeof(uint32_t))) {
    return false;
  }
  const uint32_t length = stream->Read<uint32_t>();
  if (length > kMaxSavedEnumeratorStringLength ||
      !HasRemaining(stream, size_t(length) * sizeof(char16_t))) {
    return false;
  }
  value->resize(length);
  if (length) {
    stream->Read(value->data(), size_t(length) * sizeof(char16_t));
  }
  return true;
}

template <typename T>
bool ReadPodVector(ByteStream* stream, std::vector<T>* values) {
  if (!HasRemaining(stream, sizeof(uint32_t))) {
    return false;
  }
  const uint32_t count = stream->Read<uint32_t>();
  if (count > kMaxSavedEnumeratorItems ||
      count > std::numeric_limits<size_t>::max() / sizeof(T) ||
      !HasRemaining(stream, size_t(count) * sizeof(T))) {
    return false;
  }
  values->resize(count);
  if (count) {
    stream->Read(values->data(), size_t(count) * sizeof(T));
  }
  return true;
}

template <typename T>
void WritePodVector(ByteStream* stream, const std::vector<T>& values) {
  stream->Write<uint32_t>(static_cast<uint32_t>(values.size()));
  if (!values.empty()) {
    stream->Write(values.data(), values.size() * sizeof(T));
  }
}

}  // namespace

XEnumerator::XEnumerator(KernelState* kernel_state, size_t items_per_enumerate,
                         size_t item_size)
    : XObject(kernel_state, kObjectType),
      items_per_enumerate_(items_per_enumerate),
      item_size_(item_size) {}

XEnumerator::~XEnumerator() = default;

bool XEnumerator::Save(ByteStream* stream) {
  if (items_per_enumerate_ > std::numeric_limits<uint32_t>::max() ||
      item_size_ > std::numeric_limits<uint32_t>::max() ||
      extra_size_ > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write<uint32_t>(kEnumeratorSaveSignature);
  stream->Write<uint32_t>(static_cast<uint32_t>(items_per_enumerate_));
  stream->Write<uint32_t>(static_cast<uint32_t>(item_size_));
  stream->Write<uint32_t>(static_cast<uint32_t>(extra_size_));

  if (auto* enumerator = dynamic_cast<XStaticUntypedEnumerator*>(this)) {
    if (enumerator->buffer_.size() > kMaxSavedEnumeratorBufferSize ||
        enumerator->item_count_ > kMaxSavedEnumeratorItems ||
        enumerator->current_item_ > enumerator->item_count_) {
      return false;
    }
    stream->Write<uint32_t>(
        static_cast<uint32_t>(SavedEnumeratorKind::kStaticUntyped));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->item_count_));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->current_item_));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->buffer_.size()));
    if (!enumerator->buffer_.empty()) {
      stream->Write(enumerator->buffer_.data(), enumerator->buffer_.size());
    }
    return true;
  }

  if (auto* enumerator = dynamic_cast<XAchievementEnumerator*>(this)) {
    if (enumerator->items_.size() > kMaxSavedEnumeratorItems ||
        enumerator->current_item_ > enumerator->items_.size()) {
      return false;
    }
    stream->Write<uint32_t>(
        static_cast<uint32_t>(SavedEnumeratorKind::kAchievement));
    stream->Write<uint32_t>(enumerator->flags_);
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->current_item_));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->items_.size()));
    for (const auto& item : enumerator->items_) {
      stream->Write<uint32_t>(item.id);
      stream->Write(item.label);
      stream->Write(item.description);
      stream->Write(item.unachieved);
      stream->Write<uint32_t>(item.image_id);
      stream->Write<uint32_t>(item.gamerscore);
      stream->Write<uint32_t>(item.unlock_time.high_part);
      stream->Write<uint32_t>(item.unlock_time.low_part);
      stream->Write<uint32_t>(item.flags);
    }
    return true;
  }

  if (auto* enumerator = dynamic_cast<XTitleEnumerator*>(this)) {
    if (enumerator->items_.size() > kMaxSavedEnumeratorItems ||
        enumerator->current_item_ > enumerator->items_.size()) {
      return false;
    }
    stream->Write<uint32_t>(
        static_cast<uint32_t>(SavedEnumeratorKind::kTitle));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->current_item_));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->items_.size()));
    for (const auto& item : enumerator->items_) {
      stream->Write(item.title_name);
      stream->Write<uint32_t>(item.id);
      stream->Write<uint32_t>(item.unlocked_achievements_count);
      stream->Write<uint32_t>(item.achievements_count);
      stream->Write<uint32_t>(item.title_earned_gamerscore);
      stream->Write<uint32_t>(item.gamerscore_amount);
      stream->Write<uint32_t>(item.flags);
      stream->Write<uint16_t>(item.online_unlocked_achievements);
      stream->Write<uint8_t>(item.all_avatar_awards.earned);
      stream->Write<uint8_t>(item.all_avatar_awards.possible);
      stream->Write<uint8_t>(item.male_avatar_awards.earned);
      stream->Write<uint8_t>(item.male_avatar_awards.possible);
      stream->Write<uint8_t>(item.female_avatar_awards.earned);
      stream->Write<uint8_t>(item.female_avatar_awards.possible);
    }
    return true;
  }

  if (auto* enumerator = dynamic_cast<XUserStatsEnumerator*>(this)) {
    if (enumerator->current_item_ > enumerator->items_.size()) {
      return false;
    }
    stream->Write<uint32_t>(
        static_cast<uint32_t>(SavedEnumeratorKind::kUserStats));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->current_item_));
    WritePodVector(stream, enumerator->items_);
    return true;
  }

  if (auto* enumerator =
          dynamic_cast<XMPCreateUserPlaylistEnumerator*>(this)) {
    if (enumerator->current_item_ > enumerator->items_.size()) {
      return false;
    }
    stream->Write<uint32_t>(
        static_cast<uint32_t>(SavedEnumeratorKind::kUserPlaylist));
    stream->Write<uint32_t>(static_cast<uint32_t>(enumerator->current_item_));
    WritePodVector(stream, enumerator->items_);
    return true;
  }

  XELOGE("Unknown enumerator implementation while saving handle {:08X}",
         handle());
  return false;
}

object_ref<XEnumerator> XEnumerator::Restore(KernelState* kernel_state,
                                             ByteStream* stream) {
  if (!HasRemaining(stream, 5 * sizeof(uint32_t))) {
    return nullptr;
  }

  // The common object record precedes the enumerator header. Read it into a
  // temporary concrete object after validating the fixed-size header.
  const size_t object_offset = stream->offset();
  stream->Read<uint32_t>();  // allocated_guest_object
  stream->Read<uint32_t>();  // guest_object_ptr
  const uint32_t handle_count = stream->Read<uint32_t>();
  if (handle_count > 64 * 1024 ||
      !HasRemaining(stream, size_t(handle_count) * sizeof(X_HANDLE) +
                                5 * sizeof(uint32_t))) {
    return nullptr;
  }
  stream->Advance(size_t(handle_count) * sizeof(X_HANDLE));

  if (stream->Read<uint32_t>() != kEnumeratorSaveSignature) {
    return nullptr;
  }
  const uint32_t items_per_enumerate = stream->Read<uint32_t>();
  const uint32_t item_size = stream->Read<uint32_t>();
  const uint32_t extra_size = stream->Read<uint32_t>();
  const auto kind = static_cast<SavedEnumeratorKind>(stream->Read<uint32_t>());
  if (items_per_enumerate > kMaxSavedEnumeratorItems ||
      item_size > kMaxSavedEnumeratorBufferSize ||
      extra_size > kMaxSavedEnumeratorBufferSize) {
    return nullptr;
  }

  XEnumerator* restored = nullptr;
  switch (kind) {
    case SavedEnumeratorKind::kStaticUntyped:
      restored =
          new XStaticUntypedEnumerator(nullptr, items_per_enumerate, item_size);
      break;
    case SavedEnumeratorKind::kAchievement:
      restored =
          new XAchievementEnumerator(nullptr, items_per_enumerate, 0);
      break;
    case SavedEnumeratorKind::kTitle:
      restored = new XTitleEnumerator(nullptr, items_per_enumerate);
      break;
    case SavedEnumeratorKind::kUserStats:
      restored = new XUserStatsEnumerator(nullptr, items_per_enumerate);
      break;
    case SavedEnumeratorKind::kUserPlaylist:
      restored =
          new XMPCreateUserPlaylistEnumerator(nullptr, items_per_enumerate);
      break;
    default:
      return nullptr;
  }

  restored->kernel_state_ = kernel_state;
  stream->set_offset(object_offset);
  if (!restored->RestoreObject(stream) ||
      stream->Read<uint32_t>() != kEnumeratorSaveSignature) {
    restored->Release();
    return nullptr;
  }
  restored->items_per_enumerate_ = stream->Read<uint32_t>();
  restored->item_size_ = stream->Read<uint32_t>();
  restored->extra_size_ = stream->Read<uint32_t>();
  stream->Read<uint32_t>();  // SavedEnumeratorKind, validated above.

  bool valid = true;
  switch (kind) {
    case SavedEnumeratorKind::kStaticUntyped: {
      auto* enumerator = static_cast<XStaticUntypedEnumerator*>(restored);
      if (!HasRemaining(stream, 3 * sizeof(uint32_t))) {
        valid = false;
        break;
      }
      enumerator->item_count_ = stream->Read<uint32_t>();
      enumerator->current_item_ = stream->Read<uint32_t>();
      const uint32_t buffer_size = stream->Read<uint32_t>();
      valid = enumerator->item_count_ <= kMaxSavedEnumeratorItems &&
              enumerator->current_item_ <= enumerator->item_count_ &&
              buffer_size <= kMaxSavedEnumeratorBufferSize &&
              HasRemaining(stream, buffer_size) &&
              (enumerator->item_size_ == 0
                   ? buffer_size == 0
                   : enumerator->item_count_ <=
                             std::numeric_limits<size_t>::max() /
                                 enumerator->item_size_ &&
                         buffer_size ==
                             enumerator->item_count_ * enumerator->item_size_);
      if (valid) {
        enumerator->buffer_.resize(buffer_size);
        if (buffer_size) {
          stream->Read(enumerator->buffer_.data(), buffer_size);
        }
      }
      break;
    }
    case SavedEnumeratorKind::kAchievement: {
      auto* enumerator = static_cast<XAchievementEnumerator*>(restored);
      if (!HasRemaining(stream, 3 * sizeof(uint32_t))) {
        valid = false;
        break;
      }
      enumerator->flags_ = stream->Read<uint32_t>();
      enumerator->current_item_ = stream->Read<uint32_t>();
      const uint32_t count = stream->Read<uint32_t>();
      valid = count <= kMaxSavedEnumeratorItems &&
              enumerator->current_item_ <= count;
      enumerator->items_.reserve(valid ? count : 0);
      for (uint32_t i = 0; valid && i < count; ++i) {
        if (!HasRemaining(stream, sizeof(uint32_t))) {
          valid = false;
          break;
        }
        const uint32_t id = stream->Read<uint32_t>();
        std::u16string label;
        std::u16string description;
        std::u16string unachieved;
        valid = ReadU16String(stream, &label) &&
                ReadU16String(stream, &description) &&
                ReadU16String(stream, &unachieved) &&
                HasRemaining(stream, 5 * sizeof(uint32_t));
        if (!valid) {
          break;
        }
        const uint32_t image_id = stream->Read<uint32_t>();
        const uint32_t gamerscore = stream->Read<uint32_t>();
        X_FILETIME unlock_time;
        unlock_time.high_part = stream->Read<uint32_t>();
        unlock_time.low_part = stream->Read<uint32_t>();
        const uint32_t flags = stream->Read<uint32_t>();
        enumerator->items_.emplace_back(
            id, std::move(label), std::move(description),
            std::move(unachieved), image_id, gamerscore, unlock_time, flags);
      }
      break;
    }
    case SavedEnumeratorKind::kTitle: {
      auto* enumerator = static_cast<XTitleEnumerator*>(restored);
      if (!HasRemaining(stream, 2 * sizeof(uint32_t))) {
        valid = false;
        break;
      }
      enumerator->current_item_ = stream->Read<uint32_t>();
      const uint32_t count = stream->Read<uint32_t>();
      valid = count <= kMaxSavedEnumeratorItems &&
              enumerator->current_item_ <= count;
      enumerator->items_.reserve(valid ? count : 0);
      for (uint32_t i = 0; valid && i < count; ++i) {
        xam::TitleInfo item{};
        valid = ReadU16String(stream, &item.title_name) &&
                HasRemaining(stream, 6 * sizeof(uint32_t) + sizeof(uint16_t) +
                                         6 * sizeof(uint8_t));
        if (!valid) {
          break;
        }
        item.id = stream->Read<uint32_t>();
        item.unlocked_achievements_count = stream->Read<uint32_t>();
        item.achievements_count = stream->Read<uint32_t>();
        item.title_earned_gamerscore = stream->Read<uint32_t>();
        item.gamerscore_amount = stream->Read<uint32_t>();
        item.flags = stream->Read<uint32_t>();
        item.online_unlocked_achievements = stream->Read<uint16_t>();
        item.all_avatar_awards.earned = stream->Read<uint8_t>();
        item.all_avatar_awards.possible = stream->Read<uint8_t>();
        item.male_avatar_awards.earned = stream->Read<uint8_t>();
        item.male_avatar_awards.possible = stream->Read<uint8_t>();
        item.female_avatar_awards.earned = stream->Read<uint8_t>();
        item.female_avatar_awards.possible = stream->Read<uint8_t>();
        enumerator->items_.push_back(std::move(item));
      }
      break;
    }
    case SavedEnumeratorKind::kUserStats: {
      auto* enumerator = static_cast<XUserStatsEnumerator*>(restored);
      if (!HasRemaining(stream, sizeof(uint32_t))) {
        valid = false;
        break;
      }
      enumerator->current_item_ = stream->Read<uint32_t>();
      valid = ReadPodVector(stream, &enumerator->items_) &&
              enumerator->current_item_ <= enumerator->items_.size();
      break;
    }
    case SavedEnumeratorKind::kUserPlaylist: {
      auto* enumerator =
          static_cast<XMPCreateUserPlaylistEnumerator*>(restored);
      if (!HasRemaining(stream, sizeof(uint32_t))) {
        valid = false;
        break;
      }
      enumerator->current_item_ = stream->Read<uint32_t>();
      valid = ReadPodVector(stream, &enumerator->items_) &&
              enumerator->current_item_ <= enumerator->items_.size();
      break;
    }
  }

  if (!valid) {
    XELOGE("Invalid saved enumerator payload at offset={}", stream->offset());
    // A restore failure is fatal to the enclosing restore transaction, which
    // retains the partially rebuilt object table for diagnostics.
    return nullptr;
  }
  return object_ref<XEnumerator>(restored);
}

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags, uint32_t extra_size,
                                 void** extra_buffer) {
  auto native_object = CreateNative(sizeof(X_KENUMERATOR) + extra_size);
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }
  auto guest_object = reinterpret_cast<X_KENUMERATOR*>(native_object);
  guest_object->app_id = app_id;
  guest_object->open_message = open_message;
  guest_object->close_message = close_message;
  guest_object->user_index = user_index;
  guest_object->items_per_enumerate =
      static_cast<uint32_t>(items_per_enumerate_);
  guest_object->flags = flags;
  if (extra_buffer) {
    *extra_buffer =
        !extra_buffer ? nullptr : &native_object[sizeof(X_KENUMERATOR)];
  }

  extra_size_ = extra_size;
  return X_STATUS_SUCCESS;
}

X_STATUS XEnumerator::Initialize(uint32_t user_index, uint32_t app_id,
                                 uint32_t open_message, uint32_t close_message,
                                 uint32_t flags) {
  return Initialize(user_index, app_id, open_message, close_message, flags, 0,
                    nullptr);
}

uint8_t* XStaticUntypedEnumerator::AppendItem() {
  size_t offset = buffer_.size();
  buffer_.resize(offset + item_size());
  item_count_++;
  return const_cast<uint8_t*>(&buffer_.data()[offset]);
}

uint32_t XStaticUntypedEnumerator::WriteItems(uint8_t* buffer_data,
                                              uint32_t buffer_size,
                                              uint32_t* written_count) {
  size_t count = std::min(item_count_ - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();
  size_t offset = current_item_ * item_size();
  std::memcpy(buffer_data, buffer_.data() + offset, size);

  current_item_ += count;

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t XAchievementEnumerator::WriteItems(uint8_t* buffer_data,
                                            uint32_t buffer_size,
                                            uint32_t* written_count) {
  size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();

  auto details = reinterpret_cast<xam::X_ACHIEVEMENT_DETAILS*>(buffer_data);
  size_t string_offset =
      items_per_enumerate() * sizeof(xam::X_ACHIEVEMENT_DETAILS);
  auto string_buffer =
      StringBuffer{&buffer_data[string_offset],
                   count * xam::X_ACHIEVEMENT_DETAILS::kStringBufferSize};
  for (size_t i = 0; i < count; ++i, ++current_item_) {
    const auto& item = items_[current_item_];
    details[i].id = item.id;
    details[i].label_ptr =
        !!(flags_ & 1) ? AppendString(string_buffer, item.label) : 0;
    details[i].description_ptr =
        !!(flags_ & 2) ? AppendString(string_buffer, item.description) : 0;
    details[i].unachieved_ptr =
        !!(flags_ & 4) ? AppendString(string_buffer, item.unachieved) : 0;
    details[i].image_id = item.image_id;
    details[i].gamerscore = item.gamerscore;
    details[i].unlock_time.high_part = item.unlock_time.high_part;
    details[i].unlock_time.low_part = item.unlock_time.low_part;
    details[i].flags = item.flags;
  }

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t XTitleEnumerator::WriteItems(uint8_t* buffer_data,
                                      uint32_t buffer_size,
                                      uint32_t* written_count) {
  size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();
  auto details = reinterpret_cast<XTITLE_PLAYED*>(buffer_data);

  for (size_t i = 0; i < count; ++i, ++current_item_) {
    const auto& item = items_[current_item_];
    details[i].base.title_id = item.id;
    details[i].base.achievements_count = item.achievements_count;
    details[i].base.achievements_unlocked = item.unlocked_achievements_count;
    details[i].base.gamerscore_total = item.gamerscore_amount;
    details[i].base.gamerscore_earned = item.title_earned_gamerscore;
    details[i].base.online_achievement_count =
        item.online_unlocked_achievements;
    details[i].base.all_avatar_awards.earned = item.all_avatar_awards.earned;
    details[i].base.all_avatar_awards.possible =
        item.all_avatar_awards.possible;
    details[i].base.male_avatar_awards.earned = item.male_avatar_awards.earned;
    details[i].base.male_avatar_awards.possible =
        item.male_avatar_awards.possible;
    details[i].base.female_avatar_awards.earned =
        item.female_avatar_awards.earned;
    details[i].base.female_avatar_awards.possible =
        item.female_avatar_awards.possible;
    details[i].base.flags = item.flags;

    // On console if title is played offline this field is set to 0.
    details[i].base.last_played = X_FILETIME((uint64_t)0);
    string_util::copy_and_swap_truncating((char16_t*)&details[i].title_name,
                                          item.title_name, 128);
  }

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

uint32_t XUserStatsEnumerator::WriteItems(uint8_t* buffer_data,
                                          uint32_t buffer_size,
                                          uint32_t* written_count) {
  size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  size_t size = count * item_size();

  return X_ERROR_SUCCESS;
}

uint32_t XMPCreateUserPlaylistEnumerator::WriteItems(uint8_t* buffer_data,
                                                     uint32_t buffer_size,
                                                     uint32_t* written_count) {
  // Fixed 545408C0 freezing at main menu.
  std::memset(buffer_data, 0, buffer_size);

  size_t count = std::min(items_.size() - current_item_, items_per_enumerate());
  if (!count) {
    return X_ERROR_NO_MORE_FILES;
  }

  xam::XMP_USER_PLAYLIST_INFO* results =
      reinterpret_cast<xam::XMP_USER_PLAYLIST_INFO*>(buffer_data);

  std::copy_n(items_.begin() + current_item_, count, results);

  if (written_count) {
    *written_count = static_cast<uint32_t>(count);
  }

  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe
