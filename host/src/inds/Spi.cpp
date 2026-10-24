#include "emugbds/inds/Spi.h"

namespace emugbds::inds {
namespace {

constexpr uint16_t kSpiCntWritable = 0xCF03u; // baudrate, device, size, hold, IRQ, enable
constexpr uint16_t kHold = 1u << 11;
constexpr uint16_t kIrqEnable = 1u << 14;
constexpr uint16_t kEnable = 1u << 15;
constexpr uint16_t kDeviceMask = 3u << 8;

// Flash instructions (ST M45PE20-compatible)
constexpr uint8_t kFlashWriteEnable = 0x06u;
constexpr uint8_t kFlashWriteDisable = 0x04u;
constexpr uint8_t kFlashReadId = 0x9Fu;
constexpr uint8_t kFlashReadStatus = 0x05u;
constexpr uint8_t kFlashRead = 0x03u;
constexpr uint8_t kFlashFastRead = 0x0Bu;
constexpr uint8_t kFlashPageWrite = 0x0Au;
constexpr uint8_t kFlashPageProgram = 0x02u;
constexpr uint8_t kFlashPageErase = 0xDBu;
constexpr uint8_t kFlashSectorErase = 0xD8u;

} // namespace

void Spi::Reset() noexcept {
    *this = Spi{};
    power_registers_[0] = 0x0Du; // Sound amplifier and both backlights on
    power_registers_[4] = 0x40u; // DS-Lite register 4: fixed upper bits, low brightness
}


bool Spi::WriteWord(uint32_t value, uint32_t mask, std::vector<uint8_t>& firmware) {
    if ((mask & 0xFFFFu) != 0u) {
        const uint16_t old_spicnt = spicnt_;
        spicnt_ = static_cast<uint16_t>((spicnt_ & ~(mask & kSpiCntWritable)) | (value & mask & kSpiCntWritable));
        if ((spicnt_ & kEnable) == 0u || ((spicnt_ ^ old_spicnt) & kDeviceMask) != 0u) {
            Deselect(); // Disabling the bus or switching devices ends the held command.
        }
    }
    if ((mask & 0x00FF0000u) == 0u || (spicnt_ & kEnable) == 0u) {
        return false; // Only a write to SPIDATA with the bus enabled starts a transfer.
    }
    spidata_ = Transfer(static_cast<uint8_t>(value >> 16u), firmware);
    if ((spicnt_ & kHold) == 0u) {
        Deselect(); // Chip select drops after this byte: the command ends.
    }
    return (spicnt_ & kIrqEnable) != 0u;
}


void Spi::Deselect() noexcept {
    if (selected_ && device_ == Device::Firmware &&
        (flash_command_ == kFlashPageWrite || flash_command_ == kFlashPageProgram) && position_ > 4u) {
        flash_write_enabled_ = false; // A completed page write resets WEL.
    }
    selected_ = false;
}


uint8_t Spi::Transfer(uint8_t out, std::vector<uint8_t>& firmware) {
    if (!selected_) {
        selected_ = true;
        position_ = 0u;
        device_ = static_cast<Device>((spicnt_ >> 8u) & 3u);
    }
    uint8_t in = 0u;
    switch (device_) {
    case Device::PowerManagement: in = PowerTransfer(out); break;
    case Device::Firmware: in = FirmwareTransfer(out, firmware); break;
    case Device::Touchscreen: in = TouchscreenTransfer(out); break;
    default: break; // Reserved device: nothing answers.
    }
    ++position_;
    return in;
}


uint8_t Spi::FirmwareTransfer(uint8_t out, std::vector<uint8_t>& firmware) {
    if (position_ == 0u) {
        flash_command_ = out;
        flash_address_ = 0u;
        if (out == kFlashWriteEnable) {
            flash_write_enabled_ = true;
        } else if (out == kFlashWriteDisable) {
            flash_write_enabled_ = false;
        }
        return 0u;
    }
    const auto at = [&](uint32_t address) -> uint8_t* {
        return firmware.empty() ? nullptr : &firmware[address % firmware.size()];
    };
    switch (flash_command_) {
    case kFlashReadStatus:
        return flash_write_enabled_ ? 0x02u : 0x00u; // WIP is never set: writes finish instantly
    case kFlashReadId: {
        // ST M45PE20-style JEDEC ID; the capacity byte follows the image size (12h = 256 KiB).
        uint8_t capacity = 0x12u;
        for (size_t size = firmware.size(); size > 0x40000u; size >>= 1u) {
            ++capacity;
        }
        const uint8_t id[3] = {0x20u, 0x40u, capacity};
        return position_ <= 3u ? id[position_ - 1u] : 0xFFu;
    }
    case kFlashRead:
    case kFlashFastRead:
    case kFlashPageWrite:
    case kFlashPageProgram:
    case kFlashPageErase:
    case kFlashSectorErase:
        if (position_ <= 3u) {
            flash_address_ = (flash_address_ << 8u) | out; // 3-byte address, MSB first
            if (position_ == 3u && flash_write_enabled_ &&
                (flash_command_ == kFlashPageErase || flash_command_ == kFlashSectorErase)) {
                const uint32_t span = flash_command_ == kFlashPageErase ? 0x100u : 0x10000u;
                const uint32_t base = flash_address_ & ~(span - 1u);
                for (uint32_t i = 0u; i < span; ++i) {
                    if (uint8_t* byte = at(base + i)) {
                        *byte = 0xFFu;
                    }
                }
                flash_write_enabled_ = false;
            }
            return 0u;
        }
        if (flash_command_ == kFlashFastRead && position_ == 4u) {
            return 0u; // Dummy byte
        }
        if (flash_command_ == kFlashRead || flash_command_ == kFlashFastRead) {
            const uint8_t* byte = at(flash_address_++);
            return byte != nullptr ? *byte : 0xFFu;
        }
        if (flash_write_enabled_ && (flash_command_ == kFlashPageWrite || flash_command_ == kFlashPageProgram)) {
            // Writes stay within the 256-byte page and only in memory: the firmware file is never modified.
            const uint32_t address = (flash_address_ & ~0xFFu) | ((flash_address_ + position_ - 4u) & 0xFFu);
            if (uint8_t* byte = at(address)) {
                *byte = flash_command_ == kFlashPageWrite ? out : static_cast<uint8_t>(*byte & out);
            }
        }
        return 0u;
    default:
        return 0u; // Deep power-down / release and unknown instructions
    }
}


uint8_t Spi::TouchscreenTransfer(uint8_t out) noexcept {
    // The 12-bit result follows a control byte: bits 11-5 in the next byte, bits 4-0 (then zeros) in the one after.
    uint8_t in = 0u;
    if (sample_phase_ == 2u) {
        in = static_cast<uint8_t>(sample_ >> 5u);
    } else if (sample_phase_ == 1u) {
        in = static_cast<uint8_t>(sample_ << 3u);
    }
    if (sample_phase_ != 0u) {
        --sample_phase_;
    }
    if ((out & 0x80u) != 0u) { // Start bit: a control byte begins a conversion
        const uint32_t channel = (out >> 4u) & 7u;
        // shortcut: only X (5) and Y (1) positions are measured; pressure, temperature, and AUX read 0.
        sample_ = !pen_down_ ? 0u : channel == 5u ? adc_x_ : channel == 1u ? adc_y_ : 0u;
        sample_phase_ = 2u;
    }
    return in;
}


uint8_t Spi::PowerTransfer(uint8_t out) {
    if (position_ == 0u) {
        power_index_ = out;
        return 0u;
    }
    const uint32_t index = power_index_ & 0x7Fu;
    if (index >= power_registers_.size()) {
        return 0u;
    }
    if ((power_index_ & 0x80u) != 0u) {
        return power_registers_[index]; // Register 1: battery good (0)
    }
    if (index != 1u) {
        power_registers_[index] = out;
    }
    return 0u;
}

} // namespace emugbds::inds
