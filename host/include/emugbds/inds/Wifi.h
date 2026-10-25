#pragma once

// ARM7 wireless hardware stub (4800000h-480FFFFh, GBATEK "DS Wifi I/O Map"):
// register storage, the 8 KiB Wifi RAM, and the baseband chip's serial register
// file, so wifi initialisation completes. Serial ports are never busy.
// shortcut: no radio; nothing is ever sent or received and no wifi IRQs fire.

#include <array>
#include <cstdint>

namespace emugbds::inds {

class Wifi {
public:
    void Reset() noexcept;
    // address within 4800000h-480FFFFh, word aligned; mask selects byte lanes.
    uint32_t ReadWord(uint32_t address) const noexcept;
    void WriteWord(uint32_t address, uint32_t value, uint32_t mask) noexcept;

private:
    uint16_t Read16(uint32_t offset) const noexcept;
    void Write16(uint32_t offset, uint16_t value) noexcept;

    std::array<uint16_t, 0x800> registers_{}; // 4808000h-4808FFFh (mirrored)
    std::array<uint16_t, 0x1000> ram_{};      // 4804000h-4805FFFh
    std::array<uint8_t, 0x100> baseband_{};
    uint8_t baseband_read_ = 0;
};

} // namespace emugbds::inds
