#pragma once

// ARM7 SPI bus (SPICNT/SPIDATA at 40001C0h) and its three devices: power
// management, the firmware flash (backed by the loaded firmware image), and the
// touchscreen controller (TSC2046-style X/Y conversions). Transfers complete instantly.

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::inds {

class Spi {
public:
    static constexpr uint32_t kIrq = 1u << 23;

    void Reset() noexcept;
    // Word 40001C0h: SPICNT in the low half, SPIDATA in the high half.
    uint32_t ReadWord() const noexcept { return spicnt_ | (static_cast<uint32_t>(spidata_) << 16u); }
    // Returns true when a transfer completed with its IRQ enabled.
    bool WriteWord(uint32_t value, uint32_t mask, std::vector<uint8_t>& firmware);
    // Increments whenever a firmware flash write or erase changes the image.
    uint64_t firmware_revision() const noexcept { return firmware_revision_; }
    // Pen state as the touchscreen controller sees it (12-bit ADC values).
    void SetTouch(bool down, uint16_t adc_x, uint16_t adc_y) noexcept {
        pen_down_ = down;
        adc_x_ = adc_x;
        adc_y_ = adc_y;
    }

private:
    enum class Device : uint8_t { PowerManagement = 0, Firmware = 1, Touchscreen = 2, None = 3 };

    uint8_t Transfer(uint8_t out, std::vector<uint8_t>& firmware);
    uint8_t FirmwareTransfer(uint8_t out, std::vector<uint8_t>& firmware);
    uint8_t PowerTransfer(uint8_t out);
    uint8_t TouchscreenTransfer(uint8_t out) noexcept;
    void Deselect() noexcept;

    uint64_t firmware_revision_ = 0;
    uint16_t spicnt_ = 0;
    uint8_t spidata_ = 0;
    bool selected_ = false; // Chip select held from a previous transfer
    Device device_ = Device::None; // Device selected for the current command
    uint32_t position_ = 0; // Byte index within the current command

    // Firmware flash
    uint8_t flash_command_ = 0;
    uint32_t flash_address_ = 0;
    bool flash_write_enabled_ = false;
    // Power management
    uint8_t power_index_ = 0;
    std::array<uint8_t, 5> power_registers_{};
    // Touchscreen
    bool pen_down_ = false;
    uint16_t adc_x_ = 0;
    uint16_t adc_y_ = 0;
    uint16_t sample_ = 0;      // Last conversion result
    uint32_t sample_phase_ = 0; // Result bytes still to shift out (2 = high, 1 = low)
};

} // namespace emugbds::inds
