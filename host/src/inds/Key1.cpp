#include "emugbds/inds/Key1.h"

namespace emugbds::inds {
namespace {

uint32_t Bswap(uint32_t value) noexcept {
    return (value >> 24u) | ((value >> 8u) & 0xFF00u) | ((value << 8u) & 0xFF0000u) | (value << 24u);
}

} // namespace

bool Key1::Init(const std::vector<uint8_t>& bios7, uint32_t idcode, int level, uint32_t modulo) noexcept {
    if (bios7.size() < kTableOffset + kTableBytes) {
        return false;
    }
    for (uint32_t i = 0u; i < table_.size(); ++i) {
        const uint8_t* bytes = &bios7[kTableOffset + i * 4u];
        table_[i] = bytes[0] | (bytes[1] << 8u) | (bytes[2] << 16u) | (static_cast<uint32_t>(bytes[3]) << 24u);
    }
    keycode_ = {idcode, idcode / 2u, idcode * 2u};
    if (level >= 1) {
        ApplyKeycode(modulo);
    }
    if (level >= 2) {
        ApplyKeycode(modulo);
    }
    keycode_[1] *= 2u;
    keycode_[2] /= 2u;
    if (level >= 3) {
        ApplyKeycode(modulo);
    }
    return true;
}


void Key1::Encrypt(uint32_t& lo, uint32_t& hi) const noexcept {
    uint32_t y = lo;
    uint32_t x = hi;
    for (uint32_t i = 0u; i <= 0x0Fu; ++i) {
        const uint32_t z = table_[i] ^ x;
        x = table_[0x012u + ((z >> 24u) & 0xFFu)];
        x = table_[0x112u + ((z >> 16u) & 0xFFu)] + x;
        x = table_[0x212u + ((z >> 8u) & 0xFFu)] ^ x;
        x = table_[0x312u + (z & 0xFFu)] + x;
        x = y ^ x;
        y = z;
    }
    lo = x ^ table_[0x10u];
    hi = y ^ table_[0x11u];
}


void Key1::Decrypt(uint32_t& lo, uint32_t& hi) const noexcept {
    uint32_t y = lo;
    uint32_t x = hi;
    for (uint32_t i = 0x11u; i >= 0x02u; --i) {
        const uint32_t z = table_[i] ^ x;
        x = table_[0x012u + ((z >> 24u) & 0xFFu)];
        x = table_[0x112u + ((z >> 16u) & 0xFFu)] + x;
        x = table_[0x212u + ((z >> 8u) & 0xFFu)] ^ x;
        x = table_[0x312u + (z & 0xFFu)] + x;
        x = y ^ x;
        y = z;
    }
    lo = x ^ table_[0x01u];
    hi = y ^ table_[0x00u];
}


void Key1::ApplyKeycode(uint32_t modulo) noexcept {
    Encrypt(keycode_[1], keycode_[2]);
    Encrypt(keycode_[0], keycode_[1]);
    for (uint32_t i = 0u; i < 0x48u; i += 4u) {
        // keycode is a 12-byte little-endian buffer read as words at (i MOD modulo).
        const uint32_t offset = i % modulo;
        uint32_t word = 0u;
        for (uint32_t byte = 0u; byte < 4u; ++byte) {
            const uint32_t at = offset + byte;
            word |= ((keycode_[at / 4u] >> ((at % 4u) * 8u)) & 0xFFu) << (byte * 8u);
        }
        table_[i / 4u] ^= Bswap(word);
    }
    uint32_t lo = 0u;
    uint32_t hi = 0u;
    for (uint32_t i = 0u; i < kTableBytes; i += 8u) {
        Encrypt(lo, hi);
        table_[i / 4u] = hi; // upper 32 bits first
        table_[i / 4u + 1u] = lo;
    }
}

} // namespace emugbds::inds
