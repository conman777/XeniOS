/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_BASE_GUEST_INTERRUPT_CLOCK_H_
#define XENIA_BASE_GUEST_INTERRUPT_CLOCK_H_

#include <cstdint>
#include <limits>

namespace xe {

// Converts virtual guest ticks to 100 ns interrupt-time units from a
// restorable anchor. Host uptime is deliberately not consulted.
class GuestInterruptClock {
 public:
  bool Reset(uint64_t tick_anchor, uint64_t interrupt_time_anchor,
             uint64_t tick_frequency) {
    if (!tick_frequency ||
        tick_frequency >
            std::numeric_limits<uint64_t>::max() / kUnitsPerSecond) {
      return false;
    }
    tick_anchor_ = tick_anchor;
    interrupt_time_anchor_ = interrupt_time_anchor;
    tick_frequency_ = tick_frequency;
    return true;
  }

  uint64_t Query(uint64_t guest_tick_count) const {
    const uint64_t delta =
        guest_tick_count > tick_anchor_ ? guest_tick_count - tick_anchor_ : 0;
    const uint64_t whole_seconds = delta / tick_frequency_;
    const uint64_t remaining_ticks = delta % tick_frequency_;
    if (whole_seconds >
        (std::numeric_limits<uint64_t>::max() - interrupt_time_anchor_) /
            kUnitsPerSecond) {
      return std::numeric_limits<uint64_t>::max();
    }
    const uint64_t whole_units = whole_seconds * kUnitsPerSecond;
    const uint64_t partial_units =
        remaining_ticks * kUnitsPerSecond / tick_frequency_;
    if (partial_units >
        std::numeric_limits<uint64_t>::max() - interrupt_time_anchor_ -
            whole_units) {
      return std::numeric_limits<uint64_t>::max();
    }
    return interrupt_time_anchor_ + whole_units + partial_units;
  }

 private:
  static constexpr uint64_t kUnitsPerSecond = 10000000;
  uint64_t tick_anchor_ = 0;
  uint64_t interrupt_time_anchor_ = 0;
  uint64_t tick_frequency_ = 1;
};

}  // namespace xe

#endif  // XENIA_BASE_GUEST_INTERRUPT_CLOCK_H_
