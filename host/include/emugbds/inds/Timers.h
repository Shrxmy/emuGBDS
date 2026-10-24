#pragma once

// One DS CPU's four timers (TM0-TM3 at 4000100h-400010Fh), clocked at 33.51 MHz
// with GBA-style prescalers, count-up cascade, and overflow IRQs.

#include <array>
#include <cstdint>

namespace emugbds::inds {

class Timers {
public:
    void Reset() noexcept;
    // offset = address - 4000100h (word aligned); mask selects the written byte lanes.
    uint32_t ReadWord(uint32_t offset) const noexcept;
    void WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept;
    // Returns the IF bits (3-6) of timers that overflowed with their IRQ enabled.
    uint32_t Advance(uint32_t cycles) noexcept;

private:
    struct Timer {
        uint16_t reload = 0;
        uint16_t counter = 0;
        uint16_t control = 0;
        uint32_t phase = 0; // Cycles accumulated toward the next prescaled tick.
    };

    uint32_t Tick(uint32_t index, uint32_t ticks) noexcept;

    std::array<Timer, 4> timers_{};
};

} // namespace emugbds::inds
