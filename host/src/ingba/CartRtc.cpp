#include "emugbds/ingba/CartRtc.h"

#include <ctime>

namespace emugbds::ingba {
namespace {

constexpr uint8_t k24Hour = 1u << 6;          // control register
constexpr uint8_t kControlWritable = 0x6Au;   // bits 1, 3, 5, 6 (bit 7, power-off, is read-only)
constexpr uint8_t kPm = 1u << 7;              // hour register (bit 6 on the DS)

uint8_t Bcd(int value) noexcept {
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t ReverseBits(uint8_t value) noexcept {
    uint8_t reversed = 0u;
    for (int i = 0; i < 8; ++i) {
        reversed = static_cast<uint8_t>((reversed << 1u) | ((value >> i) & 1u));
    }
    return reversed;
}

} // namespace

void CartRtc::Reset() noexcept {
    *this = CartRtc{};
}


void CartRtc::WritePins(uint8_t pins) noexcept {
    const uint8_t old_pins = pins_;
    pins_ = pins;
    if ((pins & kCs) == 0u) {
        selected_ = false;
        return;
    }
    if (!selected_) {
        selected_ = true; // CS rising edge starts a new command
        bit_index_ = 0u;
        byte_index_ = 0u;
        shift_ = 0u;
        register_ = Register::None;
        reading_ = false;
        return;
    }
    const bool old_clock = (old_pins & kSck) != 0u;
    const bool clock = (pins & kSck) != 0u;
    if (old_clock && !clock && reading_) {
        // Falling edge: the RTC drives the next parameter bit, read after the rising edge.
        sio_out_ = ((ParameterByte(byte_index_ - 1u) >> bit_index_) & 1u) != 0u;
    } else if (!old_clock && clock) {
        ClockIn((pins & kSio) != 0u);
    }
}


void CartRtc::ClockIn(bool bit) noexcept {
    // Rising edge: the RTC samples SIO, or (when reading) moves on to the next bit.
    if (bit) {
        shift_ = static_cast<uint8_t>(shift_ | (1u << bit_index_));
    }
    if (++bit_index_ < 8u) {
        return;
    }
    bit_index_ = 0u;
    if (byte_index_ == 0u) {
        StartCommand(shift_);
    } else if (!reading_ && register_ == Register::Control && byte_index_ == 1u) {
        control_ = static_cast<uint8_t>((control_ & ~kControlWritable) | (shift_ & kControlWritable));
    }
    ++byte_index_;
    shift_ = 0u;
}


void CartRtc::StartCommand(uint8_t command) noexcept {
    if ((command & 0x0Fu) != 0x06u && (command & 0xF0u) == 0x60u) {
        command = ReverseBits(command); // sent MSB first
    }
    if ((command & 0x0Fu) != 0x06u) {
        register_ = Register::None; // not a command: the rest of the transfer is ignored
        return;
    }
    // Command bits 4-6 (LSB-first numbering), in the DS RTC's order: GBATEK maps DS stat1 to force
    // reset, stat2 to control, and clkadjust to force IRQ; alarm1, alarm2 and free read FFh.
    static constexpr Register kRegisters[8] = {Register::ForceReset, Register::Unused, Register::DateTime,
        Register::ForceIrq, Register::Control, Register::Unused, Register::Time, Register::Unused};
    register_ = kRegisters[(command >> 4u) & 7u];
    reading_ = (command & 0x80u) != 0u;
    if (register_ == Register::ForceReset) {
        control_ = 0u; // strobed by any access; the date and time keep following the host clock
    } else if (reading_ && (register_ == Register::DateTime || register_ == Register::Time)) {
        LatchDateTime();
    }
}


uint8_t CartRtc::ParameterByte(uint32_t index) const noexcept {
    switch (register_) {
    case Register::Control: return index == 0u ? control_ : 0xFFu;
    case Register::DateTime: return index < 7u ? date_time_[index] : 0xFFu;
    case Register::Time: return index < 3u ? date_time_[4u + index] : 0xFFu;
    default: return 0xFFu;
    }
}


void CartRtc::LatchDateTime() noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    int hour = local.tm_hour;
    const bool pm = hour >= 12;
    if ((control_ & k24Hour) == 0u) {
        hour %= 12; // 12-hour mode: 12 o'clock reads as 00h
    }
    date_time_ = {Bcd(local.tm_year % 100), Bcd(local.tm_mon + 1), Bcd(local.tm_mday),
        static_cast<uint8_t>((local.tm_wday + 6) % 7), // 0 = Monday, as on the DS
        static_cast<uint8_t>(Bcd(hour) | (pm ? kPm : 0u)), Bcd(local.tm_min), Bcd(local.tm_sec)};
}

} // namespace emugbds::ingba
