#pragma once

// ARM7 real-time clock (Seiko S-35180), bit-banged through 4000138h: CS, /SCK,
// and SIO with per-line directions. Bytes go LSB first; MSB-first commands are
// accepted too. The date and time come from the host clock.

#include <array>
#include <cstdint>

namespace emugbds::inds {

class Rtc {
public:
    void Reset() noexcept;
    uint16_t Read() const noexcept;
    void Write(uint16_t value, uint16_t mask) noexcept;

private:
    enum class Register : uint8_t { Status1, Status2, DateTime, Time, Int1, Alarm2, ClockAdjust, Free, None };

    void ClockIn(bool bit) noexcept;
    void StartCommand(uint8_t command) noexcept;
    void StoreParameter(uint32_t index, uint8_t value) noexcept;
    uint8_t* ParameterByte(uint32_t index) noexcept;
    void LatchDateTime() noexcept;

    uint16_t lines_ = 0;    // Last written register value (data and direction bits)
    bool data_out_ = false; // SIO as driven by the RTC
    bool selected_ = false;
    uint32_t bit_index_ = 0;
    uint32_t byte_index_ = 0; // 0 = command byte, then parameters
    uint8_t shift_ = 0;
    Register register_ = Register::None;
    bool reading_ = false;

    uint8_t status1_ = 0;
    uint8_t status2_ = 0;
    std::array<uint8_t, 7> date_time_{}; // year, month, day, day of week, hour, minute, second (BCD)
    std::array<uint8_t, 3> alarm1_{};    // alarm1_[2] doubles as the INT1 frequency register
    std::array<uint8_t, 3> alarm2_{};
    uint8_t clock_adjust_ = 0;
    uint8_t free_ = 0;
};

} // namespace emugbds::inds
