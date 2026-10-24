#include "emugbds/inds/Rtc.h"

#include <ctime>

namespace emugbds::inds {
namespace {

constexpr uint16_t kData = 1u << 0;
constexpr uint16_t kClock = 1u << 1;
constexpr uint16_t kSelect = 1u << 2;
constexpr uint16_t kDataWrite = 1u << 4;
constexpr uint8_t kReset = 1u << 0;
constexpr uint8_t k24Hour = 1u << 1;
constexpr uint8_t kAlarm1Mode = 1u << 2; // Status 2: the INT1 parameter is the 3-byte alarm, not the frequency

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

void Rtc::Reset() noexcept {
    *this = Rtc{};
    status1_ = k24Hour;
}


uint16_t Rtc::Read() const noexcept {
    if ((lines_ & kDataWrite) != 0u) {
        return lines_;
    }
    return static_cast<uint16_t>((lines_ & ~kData) | (data_out_ ? kData : 0u)); // SIO is an input: read the RTC
}


void Rtc::Write(uint16_t value, uint16_t mask) noexcept {
    const uint16_t old_lines = lines_;
    lines_ = static_cast<uint16_t>((lines_ & ~mask) | (value & mask));
    if ((lines_ & kSelect) == 0u) {
        selected_ = false;
        return;
    }
    if (!selected_) {
        selected_ = true; // CS rising edge starts a new command.
        bit_index_ = 0u;
        byte_index_ = 0u;
        shift_ = 0u;
        register_ = Register::None;
        reading_ = false;
        return;
    }
    const bool old_clock = (old_lines & kClock) != 0u;
    const bool clock = (lines_ & kClock) != 0u;
    if (old_clock && !clock && reading_) {
        // Falling edge: the RTC drives the next parameter bit, read after the rising edge.
        const uint8_t* byte = ParameterByte(byte_index_ - 1u);
        data_out_ = byte != nullptr && ((*byte >> bit_index_) & 1u) != 0u;
    } else if (!old_clock && clock) {
        ClockIn((lines_ & kData) != 0u);
    }
}


void Rtc::ClockIn(bool bit) noexcept {
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
    } else if (!reading_) {
        StoreParameter(byte_index_ - 1u, shift_);
    }
    ++byte_index_;
    shift_ = 0u;
}


void Rtc::StartCommand(uint8_t command) noexcept {
    if ((command & 0x0Fu) != 0x06u && (command & 0xF0u) == 0x60u) {
        command = ReverseBits(command); // Sent MSB first
    }
    if ((command & 0x0Fu) != 0x06u) {
        register_ = Register::None; // Not a command: the rest of the transfer is ignored.
        return;
    }
    // Command bits 4-6 (LSB-first numbering) select the register.
    static constexpr Register kRegisters[8] = {Register::Status1, Register::Int1, Register::DateTime,
        Register::ClockAdjust, Register::Status2, Register::Alarm2, Register::Time, Register::Free};
    register_ = kRegisters[(command >> 4u) & 7u];
    reading_ = (command & 0x80u) != 0u;
    if (reading_ && (register_ == Register::DateTime || register_ == Register::Time)) {
        LatchDateTime();
    }
}


void Rtc::StoreParameter(uint32_t index, uint8_t value) noexcept {
    if (register_ == Register::Status1) {
        if ((value & kReset) != 0u) {
            status2_ = 0u;
            alarm1_ = {};
            alarm2_ = {};
            clock_adjust_ = 0u;
            free_ = 0u;
        }
        status1_ = static_cast<uint8_t>((status1_ & ~0x0Eu) | (value & 0x0Eu)); // 12/24h and general bits
        return;
    }
    if (register_ == Register::DateTime || register_ == Register::Time) {
        return; // shortcut: date/time writes are ignored and the clock follows the host; add an offset if games set it.
    }
    if (uint8_t* byte = ParameterByte(index)) {
        *byte = value;
    }
}


uint8_t* Rtc::ParameterByte(uint32_t index) noexcept {
    const bool int1_alarm = (status2_ & kAlarm1Mode) != 0u;
    switch (register_) {
    case Register::Status1: return index == 0u ? &status1_ : nullptr;
    case Register::Status2: return index == 0u ? &status2_ : nullptr;
    case Register::DateTime: return index < 7u ? &date_time_[index] : nullptr;
    case Register::Time: return index < 3u ? &date_time_[4u + index] : nullptr;
    case Register::Int1:
        return int1_alarm ? (index < 3u ? &alarm1_[index] : nullptr) : (index == 0u ? &alarm1_[2] : nullptr);
    case Register::Alarm2: return index < 3u ? &alarm2_[index] : nullptr;
    case Register::ClockAdjust: return index == 0u ? &clock_adjust_ : nullptr;
    case Register::Free: return index == 0u ? &free_ : nullptr;
    default: return nullptr;
    }
}


void Rtc::LatchDateTime() noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    int hour = local.tm_hour;
    const bool pm = hour >= 12;
    if ((status1_ & k24Hour) == 0u) {
        hour %= 12; // 12-hour mode: 12 o'clock reads as 00h
    }
    date_time_ = {Bcd(local.tm_year % 100), Bcd(local.tm_mon + 1), Bcd(local.tm_mday),
        static_cast<uint8_t>((local.tm_wday + 6) % 7), // 0 = Monday
        static_cast<uint8_t>(Bcd(hour) | (pm ? 0x40u : 0u)), Bcd(local.tm_min), Bcd(local.tm_sec)};
}

} // namespace emugbds::inds
