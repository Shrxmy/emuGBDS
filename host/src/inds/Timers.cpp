#include "emugbds/inds/Timers.h"

namespace emugbds::inds {
namespace {

constexpr uint16_t kEnable = 0x0080u;
constexpr uint16_t kIrq = 0x0040u;
constexpr uint16_t kCountUp = 0x0004u;
constexpr uint32_t kPrescalerShifts[4] = {0u, 6u, 8u, 10u}; // F/1, F/64, F/256, F/1024

} // namespace

void Timers::Reset() noexcept {
    timers_ = {};
}


uint32_t Timers::ReadWord(uint32_t offset) const noexcept {
    const Timer& timer = timers_[(offset >> 2u) & 3u];
    return timer.counter | (static_cast<uint32_t>(timer.control) << 16u);
}


void Timers::WriteWord(uint32_t offset, uint32_t value, uint32_t mask) noexcept {
    Timer& timer = timers_[(offset >> 2u) & 3u];
    if ((mask & 0x0000FFFFu) != 0u) { // Writing the counter half sets the reload value.
        timer.reload = static_cast<uint16_t>((timer.reload & ~mask) | (value & mask));
    }
    if ((mask & 0xFFFF0000u) != 0u) {
        const uint16_t control_mask = static_cast<uint16_t>(mask >> 16u);
        const uint16_t control = static_cast<uint16_t>(
            ((timer.control & ~control_mask) | ((value >> 16u) & control_mask)) & 0x00C7u);
        if ((timer.control & kEnable) == 0u && (control & kEnable) != 0u) {
            timer.counter = timer.reload; // Starting a timer reloads it.
            timer.phase = 0u;
        }
        timer.control = control;
    }
}


uint32_t Timers::Tick(uint32_t index, uint32_t ticks) noexcept {
    Timer& timer = timers_[index];
    uint32_t irq = 0u;
    while (ticks != 0u) {
        const uint32_t until_overflow = 0x10000u - timer.counter;
        if (ticks < until_overflow) {
            timer.counter = static_cast<uint16_t>(timer.counter + ticks);
            break;
        }
        ticks -= until_overflow;
        timer.counter = timer.reload;
        if ((timer.control & kIrq) != 0u) {
            irq |= 1u << (3u + index);
        }
        if (index < 3u && (timers_[index + 1u].control & (kEnable | kCountUp)) == (kEnable | kCountUp)) {
            irq |= Tick(index + 1u, 1u);
        }
    }
    return irq;
}


uint32_t Timers::Advance(uint32_t cycles) noexcept {
    uint32_t irq = 0u;
    for (uint32_t index = 0u; index < timers_.size(); ++index) {
        Timer& timer = timers_[index];
        if ((timer.control & kEnable) == 0u || (index != 0u && (timer.control & kCountUp) != 0u)) {
            continue; // Cascaded timers advance only on the lower timer's overflow.
        }
        const uint32_t shift = kPrescalerShifts[timer.control & 3u];
        const uint64_t total = static_cast<uint64_t>(timer.phase) + cycles;
        timer.phase = static_cast<uint32_t>(total & ((1u << shift) - 1u));
        irq |= Tick(index, static_cast<uint32_t>(total >> shift));
    }
    return irq;
}

} // namespace emugbds::inds
