#pragma once

// KEY1, the gamecode-based Blowfish encryption of DS cartridge commands and of the first 2 KiB of
// the secure area (GBATEK "DS Encryption by Gamecode/Idcode (KEY1)"). The 1048h-byte initial key
// table comes from the ARM7 BIOS (00000030h-00001077h).

#include <array>
#include <cstdint>
#include <vector>

namespace emugbds::inds {

class Key1 {
public:
    static constexpr uint32_t kTableOffset = 0x30u; // in the ARM7 BIOS
    static constexpr uint32_t kTableBytes = 0x1048u;

    // init_keycode(idcode, level, modulo). Returns false when `bios7` is too short for the table.
    bool Init(const std::vector<uint8_t>& bios7, uint32_t idcode, int level, uint32_t modulo) noexcept;
    // One 64-bit block, with `lo` the word at [ptr+0] and `hi` the word at [ptr+4].
    void Encrypt(uint32_t& lo, uint32_t& hi) const noexcept;
    void Decrypt(uint32_t& lo, uint32_t& hi) const noexcept;

private:
    void ApplyKeycode(uint32_t modulo) noexcept;

    std::array<uint32_t, kTableBytes / 4u> table_{};
    std::array<uint32_t, 3> keycode_{};
};

} // namespace emugbds::inds
