// SPDX-License-Identifier: MIT
#pragma once

#include "aoahid_share/tracker.hpp"

#include <atomic>
#include <cstdint>

namespace aoas {

// The tracker's range, published for a thread that must not touch the
// session: one writer, any number of readers, nobody waits. The writer makes
// the sequence odd while it stores, so a reader that sees an odd or a changed
// sequence reads again.
class PositionCell {
  public:
    struct Value {
        double x_lo{};
        double x_hi{};
        double y_lo{};
        double y_hi{};
    };

    void store(const Tracker& tracker) noexcept {
        const uint32_t before = sequence_.load(std::memory_order_relaxed);
        sequence_.store(before + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        x_lo_.store(tracker.x().lo, std::memory_order_relaxed);
        x_hi_.store(tracker.x().hi, std::memory_order_relaxed);
        y_lo_.store(tracker.y().lo, std::memory_order_relaxed);
        y_hi_.store(tracker.y().hi, std::memory_order_relaxed);
        sequence_.store(before + 2, std::memory_order_release);
    }

    [[nodiscard]] Value load() const noexcept {
        for (;;) {
            const uint32_t before = sequence_.load(std::memory_order_acquire);
            if ((before & 1U) != 0)
                continue;
            const Value value{
                x_lo_.load(std::memory_order_relaxed), x_hi_.load(std::memory_order_relaxed),
                y_lo_.load(std::memory_order_relaxed), y_hi_.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == before)
                return value;
        }
    }

  private:
    std::atomic<uint32_t> sequence_{};
    std::atomic<double> x_lo_{};
    std::atomic<double> x_hi_{};
    std::atomic<double> y_lo_{};
    std::atomic<double> y_hi_{};
};

} // namespace aoas
