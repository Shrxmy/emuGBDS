#include "emugbds/inds/Wifi.h"

namespace emugbds::inds {
namespace {

constexpr uint32_t kRamFirst = 0x4000u;
constexpr uint32_t kRamEnd = 0x6000u;
constexpr uint32_t kId = 0x000u;
constexpr uint32_t kBasebandControl = 0x158u;
constexpr uint32_t kBasebandWrite = 0x15Au;
constexpr uint32_t kBasebandRead = 0x15Cu;
constexpr uint32_t kBasebandBusy = 0x15Eu;
constexpr uint32_t kRfBusy = 0x180u;
constexpr uint16_t kChipId = 0x1440u; // original DS

} // namespace

void Wifi::Reset() noexcept {
    registers_ = {};
    ram_ = {};
    baseband_ = {};
    baseband_[0x00u] = 0x6Du; // read-only chip ID
    baseband_[0x5Du] = 0x01u; // read-only
    baseband_read_ = 0u;
}


uint16_t Wifi::Read16(uint32_t offset) const noexcept {
    offset &= 0x7FFFu; // 4808000h mirrors 4800000h
    if (offset >= kRamFirst && offset < kRamEnd) {
        return ram_[(offset - kRamFirst) / 2u];
    }
    if (offset >= kRamFirst) {
        return 0u;
    }
    switch (offset &= 0xFFFu) {
    case kId: return kChipId;
    case kBasebandRead: return baseband_read_;
    case kBasebandBusy:
    case kRfBusy: return 0u; // serial transfers finish at once
    default: return registers_[offset / 2u];
    }
}


void Wifi::Write16(uint32_t offset, uint16_t value) noexcept {
    offset &= 0x7FFFu;
    if (offset >= kRamFirst && offset < kRamEnd) {
        ram_[(offset - kRamFirst) / 2u] = value;
        return;
    }
    if (offset >= kRamFirst) {
        return;
    }
    offset &= 0xFFFu;
    registers_[offset / 2u] = value;
    if (offset == kBasebandControl) {
        const uint32_t index = value & 0xFFu;
        const uint32_t direction = value >> 12u;
        if (direction == 5u && index != 0x00u && index != 0x5Du) {
            baseband_[index] = static_cast<uint8_t>(registers_[kBasebandWrite / 2u]);
        } else if (direction == 6u) {
            baseband_read_ = baseband_[index];
        }
    }
}


uint32_t Wifi::ReadWord(uint32_t address) const noexcept {
    return Read16(address) | (static_cast<uint32_t>(Read16(address + 2u)) << 16u);
}


void Wifi::WriteWord(uint32_t address, uint32_t value, uint32_t mask) noexcept {
    // Only 16/32-bit writes reach the wifi bus; STRB is ignored.
    if ((mask & 0xFFFFu) == 0xFFFFu) {
        Write16(address, static_cast<uint16_t>(value));
    }
    if ((mask & 0xFFFF0000u) == 0xFFFF0000u) {
        Write16(address + 2u, static_cast<uint16_t>(value >> 16u));
    }
}

} // namespace emugbds::inds
