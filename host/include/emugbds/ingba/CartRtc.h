#pragma once

// GBA cartridge real-time clock (Seiko S3511, GBATEK "GBA Cart Real-Time Clock"),
// on the ROM's GPIO pins: bit 0 SCK, bit 1 SIO, bit 2 CS. The serial protocol is
// the DS RTC's (bytes LSB first; MSB-first commands accepted too), with the GBA
// register set: control (24-hour mode, power-off flag), date/time, time, and the
// force-reset and force-IRQ strobes; the alarm and free registers read FFh.
// The date and time come from the host clock.
// shortcut: date/time writes are ignored and the per-minute IRQ is not generated.

#include <array>
#include <cstdint>

namespace emugbds::ingba {

class CartRtc {
public:
    static constexpr uint8_t kSck = 1u << 0;
    static constexpr uint8_t kSio = 1u << 1;
    static constexpr uint8_t kCs = 1u << 2;

    void Reset() noexcept;
    // The GPIO data bits as the GBA drives them.
    void WritePins(uint8_t pins) noexcept;
    // SIO as driven by the RTC (meaningful while the GBA reads it).
    bool sio() const noexcept { return sio_out_; }

private:
    enum class Register : uint8_t { ForceReset, Control, DateTime, Time, ForceIrq, Unused, None };

    void ClockIn(bool bit) noexcept;
    void StartCommand(uint8_t command) noexcept;
    uint8_t ParameterByte(uint32_t index) const noexcept;
    void LatchDateTime() noexcept;

    uint8_t pins_ = 0;
    bool sio_out_ = false;
    bool selected_ = false;
    uint32_t bit_index_ = 0;
    uint32_t byte_index_ = 0; // 0 = command byte, then parameters
    uint8_t shift_ = 0;
    Register register_ = Register::None;
    bool reading_ = false;
    uint8_t control_ = 0x40u; // 24-hour mode, power-off flag clear: a cartridge with a good battery
    std::array<uint8_t, 7> date_time_{}; // year, month, day, day of week, hour, minute, second (BCD)
};

} // namespace emugbds::ingba
